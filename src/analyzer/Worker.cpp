// READ-ONLY RULE: the worker never touches the game. No cocos node, no GD object, no Geode UI is
// reachable from this file; it owns plain data (sim::World, RecordedAttempts, LevelSimResult) and
// talks only to the network (Api.cpp, blocking calls are fine on this thread) and its own files.
#include "Worker.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include <utility>

#include "../../core/analyzer_identity.hpp"
#include "../../core/analyzer_worker_rules.hpp"
#include "../../core/identity/identity.hpp"
#include "../../core/sim/analysis.hpp"
#include "../../core/sim/gameplay_hash.hpp"
#include "../Api.hpp"
#include "Cache.hpp"
#include "Modes.hpp"

// last: <Windows.h> (SetThreadPriority) must not see the std::min / std::max uses above
#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace gprl::analyzer::worker {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kKeepClosed = std::chrono::minutes(10);     // owner rule: a left level's job waits 10 minutes
constexpr auto kRetryDelay = std::chrono::seconds(30);     // one retry after a network failure
// a re-verified result is re-sent at most this often while the level is open (review LOW: never
// faster than the server's one-stored-upload-per-10-minutes rule, which would only answer 429)
constexpr auto kResendAfter = std::chrono::minutes(10);
constexpr double kCacheMinSolved = 99.0;                   // a server result this complete replaces the simulation
constexpr size_t kMaxUploadBytes = 2u * 1024u * 1024u;     // LEVEL_SIM_LIMITS.maxBodyBytes (the whole body, envelope included)
constexpr int kMaxUploadTries = 2;
constexpr int kMaxRateLimitedTries = 6;                     // 429: one stored upload per device + version per 10 min
constexpr size_t kBacklogLevels = 4;                        // review MEDIUM-8: extractions waiting for the simulator
// level families: the server creates the level version at session start, so the identity waits
// for the open telemetry session (at most this long), and a not-yet-known version / session is
// retried a few times before the identity stays on this computer for the next visit
constexpr auto kIdentitySessionRetry = std::chrono::seconds(15);
constexpr int kIdentitySessionTries = 3;
constexpr int kIdentityNetworkTries = 2;

namespace family = gprl::analyzer::family;

struct Msg {
    enum class Kind : uint8_t { Level, Attempt, Closed } kind = Kind::Level;
    uint64_t visitId = 0;
    std::optional<extraction::Handover> handover;
    std::vector<AttemptIn> attempts;
};

struct Entry {
    std::string key;
    int gdLevelId = 0;
    std::string gameplayHash;
    std::string levelHash;          // the exact version (its session authorises the upload)
    int rateLimitedTries = 0;       // 429 answers (one stored upload per device + version per 10 min)
    std::unique_ptr<sim::Job> job;
    bool open = true;
    Clock::time_point closedAt{};
    status::Stage stage = status::Stage::Waiting;
    std::string cacheSource;
    std::string failReason;
    std::string upload;
    // result
    bool hasResult = false;
    double physicsPercent = 0.0, solvedPercent = 0.0, verifiedPercent = 0.0;
    bool hasVerification = false;
    int windows = 0, unsupportedSpans = 0, unsolvedSpans = 0;
    bool resultHandled = false;
    bool resultDirty = false;       // re-verified since the last hand-over
    bool reverifyNeeded = false;    // attempts added to a finished job
    Clock::time_point handledAt{};
    std::string resultText;
    bool uploadPending = false;
    Clock::time_point retryAt{};
    int uploadTries = 0;
    int attemptsAdded = 0;          // recorded attempts given to this analysis (queued or fed)
    // review MEDIUM-8: recorded attempts wait here and reach the job (Job::addRecordedAttempt
    // verifies synchronously) only inside runJob's rule (modes::simAllowedNow, level open)
    std::deque<AttemptIn> queuedAttempts;
    // world facts
    int worldObjects = 0, gameplayObjects = 0, decorationObjects = 0;
    bool tooLarge = false;
    double finishMs = 0.0;
    std::vector<float> startPosXs;
    sim::JobProgress progress;
};

/// Level families: one EXACT level version (sha256 level hash) seen this run (kept like the jobs:
/// 10 minutes after the level was left, unless an upload still waits).
struct IdentityEntry {
    std::string levelHash;
    int gdLevelId = 0;
    uint64_t visitId = 0;           // the newest visit of this version
    bool open = true;
    Clock::time_point closedAt{};
    Clock::time_point readyAt{};    // when the identity was computed (the session wait counts from here)
    std::string json;               // identity::toJson text ("" when a fresh cached answer made the compute unnecessary)
    int sections = 0;
    double computeMs = 0.0;
    bool uploadPending = false;
    Clock::time_point retryAt{};
    int sessionTries = 0;           // unknown version / session answers
    int networkTries = 0;
    int rateLimitedTries = 0;       // 429 answers
    bool answered = false;
    family::FamilyAnswer answer;
    std::string line;               // Published::familyLine
    std::string state;              // Published::identityState
};

// ---- shared (s_mx) ----
std::mutex s_mx;
std::condition_variable s_cv;
std::deque<Msg> s_inbox;
Config s_cfg;
Credentials s_creds;
rules::SessionMap s_sessions{64};   // level hash -> the session of this device on it (review MEDIUM-5)
Published s_pub;
std::deque<FamilyNotice> s_notices;
std::atomic<bool> s_noticePending{false};
std::atomic<bool> s_inboxPending{false};
std::atomic<bool> s_stop{false};
// review MEDIUM-11: set by shutdown() when it gives up waiting (a request in flight at game exit):
// the thread then returns without touching any shared state (statics may already be destroyed)
std::atomic<bool> s_detached{false};
std::thread s_thread;
std::mutex s_doneMx;
std::condition_variable s_doneCv;
bool s_done = false;

// Static-destruction safety net only (like the telemetry worker): a still-joinable std::thread would
// call std::terminate when the process ends without GameEvent Exiting.
struct Reaper {
    ~Reaper() {
        if (s_thread.joinable()) {
            s_detached.store(true);
            s_stop.store(true);
            s_thread.detach();
        }
    }
};
Reaper s_reaper;

// ---- worker thread only ----
// The thread's own state lives on ITS stack (loop()), reached through `w`: a thread detached at game
// exit never races the static destructors over it (review MEDIUM-11).
struct Local {
    std::map<std::string, Entry> entries;
    std::unordered_map<uint64_t, std::string> visitKey;
    std::map<std::string, IdentityEntry> identities;            // by level hash (level families)
    std::unordered_map<uint64_t, std::string> visitIdentity;    // visit -> level hash
    rules::LevelBacklog<Msg> backlog{kBacklogLevels};           // review MEDIUM-8
    uint64_t currentVisit = 0;
    bool restDue = false;
    double restMs = 0.0;
};
Local* w = nullptr;

bool stopping() { return s_stop.load(); }

double nowMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }

int64_t unixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string isoNow() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef GEODE_IS_WINDOWS
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[40];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return b;
}

Config cfgCopy() {
    std::lock_guard lock(s_mx);
    return s_cfg;
}

Credentials credsCopy() {
    std::lock_guard lock(s_mx);
    return s_creds;
}

/// The session this device opened on the exact level version `levelHash` ("" = none seen this run).
/// Filled by setCredentials from the telemetry worker's published (hash, id, generation) triple; an
/// ended session still identifies the version (the server accepts it).
std::string sessionFor(std::string const& levelHash) {
    if (levelHash.empty()) return {};
    std::lock_guard lock(s_mx);
    return s_sessions.find(levelHash);
}

bool debugOn() { return cfgCopy().debug || modes::analysisDebug(); }

char const* whatOf(std::exception const& e) { return e.what() ? e.what() : "?"; }

Entry* current() {
    if (!w->currentVisit) return nullptr;
    auto k = w->visitKey.find(w->currentVisit);
    if (k == w->visitKey.end()) return nullptr;
    auto it = w->entries.find(k->second);
    return it == w->entries.end() ? nullptr : &it->second;
}

int resolveStartPos(Entry const& e, AttemptMeta const& m) {
    if (!m.hasStartPos) return -1;
    int best = -1;
    float bestD = 1.0f;
    for (size_t i = 0; i < e.startPosXs.size(); ++i) {
        float d = std::fabs(e.startPosXs[i] - m.startPosX);
        if (d <= bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

/// A recorded attempt for an analysis: QUEUED (review MEDIUM-8), handed to the job by feedAttempts.
void addAttempt(Entry& e, AttemptIn a) {
    if (!e.job) return;   // a cached / failed result: nothing to verify against
    a.attempt.startPosIndex = resolveStartPos(e, a.meta);
    ++e.attemptsAdded;
    e.queuedAttempts.push_back(std::move(a));
}

/// Hands the queued attempts to the job, one at a time, only while the simulator may run (each
/// Job::addRecordedAttempt can verify a whole attempt synchronously). False = some still wait.
bool feedAttempts(Entry& e) {
    while (!e.queuedAttempts.empty()) {
        if (stopping() || s_inboxPending.load() || !e.open || !e.job || !modes::simAllowedNow()) return false;
        AttemptIn a = std::move(e.queuedAttempts.front());
        e.queuedAttempts.pop_front();
        bool wasDone = e.job->done();
        int ticks = static_cast<int>(a.attempt.ticks.size());
        int index = a.attempt.attemptIndex;
        e.job->addRecordedAttempt(std::move(a.attempt));
        if (wasDone) e.reverifyNeeded = true;
        if (debugOn())
            log::info("GPRL analyzer: recorded attempt {} ({} ticks{}) added to the analysis of level {}", index, ticks, a.meta.truncated ? ", truncated" : "",
                      e.gdLevelId);
    }
    return true;
}

/// Review HIGH-4: an exception out of the job / its result ends this analysis, never the thread.
void failEntry(Entry& e, std::string const& what) {
    e.stage = status::Stage::Failed;
    e.failReason = "worker error: " + what;
    e.job.reset();
    e.queuedAttempts.clear();
    e.reverifyNeeded = false;
    e.resultDirty = false;
    e.resultHandled = true;
    log::error("GPRL analyzer: the analysis of level {} stopped - {}", e.gdLevelId, e.failReason);
}

void coverageFromJson(Entry& e, json::Value const& r) {
    auto const& c = r["coverage"];
    e.hasResult = true;
    e.physicsPercent = c.getNumber("physicsPercent");
    e.solvedPercent = c.getNumber("solvedPercent");
    e.hasVerification = c.getBool("hasVerification");
    e.verifiedPercent = c.getNumber("verifiedPercent");
    e.unsupportedSpans = static_cast<int>(c["unsupported"].asArray().size());
    e.unsolvedSpans = static_cast<int>(c["unsolved"].asArray().size());
    e.windows = static_cast<int>(r["windows"].asArray().size());
}

// ---- level families ----

std::string ageText(int64_t ms) {
    if (ms < 60'000) return fmt::format("{} s ago", ms / 1000);
    if (ms < 3'600'000) return fmt::format("{} min ago", ms / 60'000);
    return fmt::format("{:.1f} h ago", static_cast<double>(ms) / 3'600'000.0);
}

/// The answer's notice lines go to the main thread (shown there only while the visit is still open).
void queueNotice(IdentityEntry const& e) {
    if (!e.answered || !e.answer.hasNotice || e.answer.notice.empty() || !e.open) return;
    FamilyNotice n;
    n.visitId = e.visitId;
    n.lines = e.answer.notice;
    {
        std::lock_guard lock(s_mx);
        if (s_notices.size() >= 8) s_notices.pop_front();
        s_notices.push_back(std::move(n));
    }
    s_noticePending.store(true);
}

/// The cache record of an entry: the identity JSON with (or without) the server answer.
void storeIdentityRecord(IdentityEntry const& e, std::string const& answerText, int64_t answeredAtMs) {
    cache::storeIdentity(e.levelHash, family::cacheRecordText(e.levelHash, e.gdLevelId, e.json, answerText, unixMs(), answeredAtMs, e.state));
}

/// Right after the world is finished and hashed: the exact version's identity (FA-D2), the cache
/// rule (24 h), the upload request. `world` is still the worker's here (the job takes it later).
/// Runs only while modes::simAllowedNow() (handleLevel's rule, review MEDIUM-8).
void handleIdentity(uint64_t visitId, sim::World const& world, extraction::Handover& h) {
    std::string const& levelHash = world.levelHash;
    if (levelHash.empty()) {
        if (debugOn()) log::info("GPRL analyzer: level identity skipped - no level hash for level {}", world.gdLevelId);
        identity::DecoSet dropped;
        std::swap(dropped, h.deco);
        return;
    }
    w->visitIdentity[visitId] = levelHash;
    auto it = w->identities.find(levelHash);
    if (it != w->identities.end()) {
        auto& e = it->second;
        e.open = true;
        e.visitId = visitId;
        // review LOW: a revisit inside the 10-minute keep sends what waited for a session / a later visit
        if (!e.uploadPending && !e.json.empty() && rules::rearmOnRevisit(e.state)) {
            e.uploadPending = true;
            e.retryAt = Clock::now();
            e.sessionTries = 0;
            e.networkTries = 0;
            e.rateLimitedTries = 0;
            if (debugOn()) log::info("GPRL analyzer: level identity of level {} was \"{}\" - sent again with this visit's session", e.gdLevelId, e.state);
            e.state = "not sent yet";
        }
        if (debugOn()) log::info("GPRL analyzer: level identity of this exact version kept from an earlier visit ({})", e.state);
        queueNotice(e);   // a revisit re-shows the notice
        identity::DecoSet dropped;
        std::swap(dropped, h.deco);
        return;
    }
    IdentityEntry e;
    e.levelHash = levelHash;
    e.gdLevelId = world.gdLevelId;
    e.visitId = visitId;
    e.readyAt = Clock::now();
    // 1. this computer: a server answer younger than 24 h means nothing is computed or sent
    if (auto stored = cache::loadIdentity(levelHash)) {
        family::CacheRecord rec;
        if (family::parseCacheRecord(*stored, rec) && family::answerFresh(rec, unixMs())) {
            e.answered = true;
            e.answer = rec.answer;
            e.line = family::familyLine(rec.answer);
            e.state = "cached answer (this computer, " + ageText(unixMs() - rec.answeredAtMs) + ")";
            log::info("GPRL analyzer: level identity of level {} answered {} - family \"{}\" ({}, {}), gameplay {} / presentation {}; nothing sent (24 h rule), notice {} line(s)",
                      e.gdLevelId, ageText(unixMs() - rec.answeredAtMs), e.answer.name, e.answer.relationshipLabel, e.answer.confidenceLabel,
                      family::matchPercent(e.answer.gameplaySimilarity), family::matchPercent(e.answer.presentationSimilarity), e.answer.notice.size());
            queueNotice(e);
            identity::DecoSet dropped;
            std::swap(dropped, h.deco);
            w->identities.emplace(levelHash, std::move(e));
            return;
        }
    }
    // 2. compute (pure core, worker thread)
    auto t0 = Clock::now();
    identity::IdentityInput in;
    in.levelHash = levelHash;
    in.gdLevelId = world.gdLevelId;
    in.copiedFromGdId = h.copiedFromGdId;
    in.nameHint = h.nameHint;
    identity::LevelIdentity id = identity::compute(world, h.deco, in);
    e.json = json::stringify(identity::toJson(id));
    e.sections = static_cast<int>(id.sections.size());
    e.computeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    size_t const triggers = h.deco.triggers.size();
    {
        identity::DecoSet dropped;   // the decoration set is not needed after the compute
        std::swap(dropped, h.deco);
    }
    log::info("GPRL analyzer: level identity - level {} version {}: gameplay fp {} ({}), presentation fp {} ({}), {} sections ({}), {} StartPos, {} gameplay / {} "
              "decoration objects, {} triggers, copied from {}, name hint \"{}\", {} bytes (estimate {}), {:.1f} ms on the worker",
              id.gdLevelId, levelHash.substr(0, 16), id.gameplayFingerprint, identity::kGameplayFingerprintVersion, id.presentationFingerprint,
              identity::kPresentationVersion, id.sections.size(), identity::kSectionVersion, id.startPos.size(), id.gameplayObjects, id.decorationObjects,
              triggers, id.copiedFromGdId, id.nameHint, e.json.size(), family::estimateBodyBytes(e.sections, static_cast<int>(id.startPos.size())),
              e.computeMs);
    e.uploadPending = true;
    e.retryAt = Clock::now();
    e.state = "not sent yet";
    storeIdentityRecord(e, {}, 0);
    w->identities.emplace(levelHash, std::move(e));
}

/// One identity upload attempt (review HIGH-4: the caller catches; review MEDIUM-11: no request
/// once stopping, nothing touched after a request that returned while stopping).
void uploadIdentity(IdentityEntry& e, Clock::time_point now) {
    auto cfg = cfgCopy();
    auto cr = credsCopy();
    auto keep = [&](std::string why) {
        e.uploadPending = false;
        e.state = std::move(why);
        storeIdentityRecord(e, {}, 0);
    };
    if (!cfg.upload) {
        keep("kept on this computer (sending is off)");
        return;
    }
    if (!cr.online) {
        keep("kept on this computer (local-only)");
        return;
    }
    if (!cr.connected || cr.deviceToken.empty()) {
        keep("kept on this computer (not connected)");
        return;
    }
    std::string const sid = sessionFor(e.levelHash);
    if (!family::bodyWithinLimit(family::uploadBody(sid, e.json))) {
        log::warn("GPRL analyzer: the level identity of level {} is {} bytes ({} sections), over the server's 1 MB limit; kept on this computer", e.gdLevelId,
                  e.json.size(), e.sections);
        keep("too large to send");
        return;
    }
    if (sid.empty()) {
        // the server takes an identity only with this device's session on this exact version
        // (it also creates the version): wait while the level is open, else keep it for a visit
        if (e.open) {
            e.state = "waiting for the session";
            return;
        }
        keep("kept on this computer (no session on this level yet)");
        return;
    }
    if (stopping()) return;
    api::Config ac{cr.apiBaseUrl, 30};
    auto r = api::uploadLevelIdentity(ac, cr.deviceToken, e.json, sid);
    if (stopping()) return;   // game exit while the request ran: touch nothing shared
    if (r.ok) {
        e.uploadPending = false;
        e.answered = true;
        e.answer = r.answer;
        e.line = family::familyLine(r.answer);
        e.state = "sent";
        storeIdentityRecord(e, r.answerText, unixMs());
        auto const& a = e.answer;
        log::info("GPRL analyzer: level identity of level {} sent - family \"{}\" (id {}, {} member(s), canonical GD id {}), {} / {}, gameplay {} / presentation {}, "
                  "transfer mechanical {} / visual {}, matched GD id {}, familiarity mechanical {:.2f} / visual {:.2f} ({} full, {} practice, {} StartPos attempts), "
                  "notice {} line(s){}",
                  e.gdLevelId, a.name, a.familyId, a.memberCount, a.canonicalGdLevelId < 0 ? std::string("none") : std::to_string(a.canonicalGdLevelId),
                  a.relationshipLabel, a.confidenceLabel, family::matchPercent(a.gameplaySimilarity), family::matchPercent(a.presentationSimilarity),
                  a.mechanicalTransfer, a.visualTransfer, a.matchedGdLevelId < 0 ? std::string("none") : std::to_string(a.matchedGdLevelId), a.mechanical,
                  a.visual, a.fullAttempts, a.practiceAttempts, a.startposAttempts, a.notice.size(),
                  a.hasStartposRange ? fmt::format(", StartPos coverage {:.0f}-{:.0f}%", a.startposFrom, a.startposTo) : std::string());
        queueNotice(e);
        return;
    }
    if (r.status == 429 && ++e.rateLimitedTries <= kMaxRateLimitedTries) {
        // review LOW: the server's Retry-After (clamped), else 10 minutes
        int secs = rules::rateLimitDelaySeconds(r.retryAfterSeconds);
        e.retryAt = now + std::chrono::seconds(secs);
        e.state = fmt::format("rate limited, retry in {} min", (secs + 59) / 60);
        if (debugOn()) log::info("GPRL analyzer: level identity of level {} rate limited by the server; retrying in {} s", e.gdLevelId, secs);
        return;
    }
    bool sessionNotSettled = (r.status == 404 || r.status == 409 || r.status == 422)
        && (r.reason == "unknown_level_version" || r.reason == "unknown_session" || r.reason == "session_level_mismatch");
    if (sessionNotSettled && ++e.sessionTries < kIdentitySessionTries) {
        e.retryAt = now + kIdentitySessionRetry;
        e.state = "waiting for the session (retry)";
        if (debugOn()) log::info("GPRL analyzer: level identity of level {} not accepted yet ({}); retrying in 15 s", e.gdLevelId, r.reason);
        return;
    }
    if (r.retryable && r.status != 429 && ++e.networkTries < kIdentityNetworkTries) {
        e.retryAt = now + kRetryDelay;
        e.state = "send failed, retry in 30 s";
        log::warn("GPRL analyzer: sending the level identity of level {} failed ({}); retrying once in 30 s", e.gdLevelId, r.error);
        return;
    }
    std::string why;
    if (r.status == 404 && r.reason == "unknown_level_version") why = "not sent: the server does not know this level version yet (sent next visit)";
    else if (r.status == 503 || r.code == "unavailable") why = "not sent: the server has no level families yet";
    else if (r.status == 413 || r.reason == "too_large") why = "too large to send";
    else if (r.reason == "invalid_identity") why = "refused by the server (invalid identity)";
    else if (r.status == 403) why = "refused by the server (" + (r.reason.empty() ? std::string("session") : r.reason) + ")";
    else if (r.status == 429) why = "not sent: rate limited, sent on a later visit";
    else why = "send failed (" + (r.code.empty() ? std::to_string(r.status) : r.code) + ")";
    log::warn("GPRL analyzer: sending the level identity of level {} failed ({}); it stays on this computer and is sent on a later visit", e.gdLevelId, r.error);
    keep(why);
}

void processIdentityUploads() {
    auto now = Clock::now();
    for (auto& [hash, e] : w->identities) {
        (void)hash;
        if (stopping()) return;
        if (!e.uploadPending || now < e.retryAt || e.json.empty()) continue;
        try {
            uploadIdentity(e, now);
        } catch (std::exception const& ex) {
            e.uploadPending = false;
            e.state = "send failed (worker error)";
            log::error("GPRL analyzer: sending the level identity of level {} failed with an error ({}); it stays on this computer", e.gdLevelId, whatOf(ex));
        } catch (...) {
            e.uploadPending = false;
            e.state = "send failed (worker error)";
            log::error("GPRL analyzer: sending the level identity of level {} failed with an unknown error; it stays on this computer", e.gdLevelId);
        }
    }
}

/// A finished extraction (only while modes::simAllowedNow(): review MEDIUM-8 - the world build, the
/// gameplay hash and the identity are all CPU work the Record-Safe rule covers).
void handleLevel(uint64_t visitId, Msg& m) {
    auto t0 = Clock::now();
    auto& h = *m.handover;
    sim::World world = h.builder.finish(h.params);
    auto const counters = h.builder.lastCounters();
    world.gameplayHash = sim::gameplayHash(world);
    double finishMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::string key = fmt::format("{}:{}", world.gdLevelId, world.gameplayHash);
    w->visitKey[visitId] = key;
    w->currentVisit = visitId;
    // level families: the exact version's identity, before the job may take the world
    handleIdentity(visitId, world, h);
    log::info("GPRL analyzer: level {} gameplay version {} ({}) - {} gameplay objects, {} decoration, {} unsupported spans, {} StartPos{}{}; world built and hashed in "
              "{:.1f} ms on the worker, {:.1f} ms of slices on the game thread",
              world.gdLevelId, world.gameplayHash, sim::kGameplayHashVersion, world.gameplayObjects, world.decorationObjects, world.unsupported.size(),
              world.startPositions.size(), world.tooLarge ? ", TOO LARGE (Partial)" : "",
              counters.startPositionIgnored ? ", m_startPosition disagreed with most objects (positions as read)" : "", finishMs, h.mainThreadMs);
    if (debugOn()) {
        std::string mechanics;
        for (size_t i = 0; i < world.unsupported.size() && i < 12; ++i) {
            auto const& s = world.unsupported[i];
            mechanics += fmt::format("{}{} [{:.0f}, {:.0f})", mechanics.empty() ? "" : ", ", s.mechanic, s.x0, s.x1);
        }
        if (!mechanics.empty()) log::info("GPRL analyzer: unsupported spans: {}{}", mechanics, world.unsupported.size() > 12 ? " ..." : "");
    }
    auto it = w->entries.find(key);
    if (it != w->entries.end()) {
        auto& e = it->second;
        e.open = true;
        // review LOW: a revisit inside the 10-minute keep sends what waited for a session / a later visit
        if (!e.uploadPending && !e.resultText.empty() && rules::rearmOnRevisit(e.upload)) {
            if (debugOn()) log::info("GPRL analyzer: the analysis of level {} was \"{}\" - sent again with this visit's session", e.gdLevelId, e.upload);
            e.uploadPending = true;
            e.retryAt = Clock::now();
            e.uploadTries = 0;
            e.upload = "not sent yet";
        }
        log::info("GPRL analyzer: resumed the kept analysis of this gameplay version ({})", e.job ? sim::name(e.job->progress().phase) : "cached");
        for (auto& a : m.attempts) addAttempt(e, std::move(a));
        return;
    }
    Entry e;
    e.key = key;
    e.gdLevelId = world.gdLevelId;
    e.gameplayHash = world.gameplayHash;
    e.levelHash = world.levelHash;
    e.worldObjects = h.arrayObjects;
    e.gameplayObjects = world.gameplayObjects;
    e.decorationObjects = world.decorationObjects;
    e.tooLarge = world.tooLarge;
    e.finishMs = finishMs;
    for (auto const& sp : world.startPositions) e.startPosXs.push_back(sp.x);

    // 1. this computer (AN-D7: one expensive analysis per gameplay version AND analyzer version, Cache.hpp)
    if (auto d = cache::load(e.gdLevelId, e.gameplayHash)) {
        e.stage = status::Stage::Cached;
        e.cacheSource = "this computer";
        e.resultText = std::move(d->resultText);
        coverageFromJson(e, d->result);
        e.resultHandled = true;
        if (!d->uploaded) {
            e.uploadPending = true;
            e.retryAt = Clock::now();
            e.upload = "not sent yet";
        }
        else e.upload = d->uploadStatus;
        log::info("GPRL analyzer: cached analysis used (this computer, {}) - solved {:.1f}%, physics {:.1f}%{}", cache::pathText(cache::dir()), e.solvedPercent,
                  e.physicsPercent, d->uploaded ? "" : "; it was never sent and is sent now");
        w->entries.emplace(key, std::move(e));
        return;
    }
    // 2. the server's trusted cache (review MEDIUM-11: never a request once stopping)
    auto cfg = cfgCopy();
    auto cr = credsCopy();
    if (cfg.cache && cr.online && !stopping()) {
        api::Config ac{cr.apiBaseUrl, 15};
        auto r = api::levelSimCache(ac, e.gdLevelId, e.gameplayHash, sim::kAnalyzerVersion, sim::kSimVersion);
        if (stopping()) return;   // game exit while the request ran: touch nothing shared
        if (r.ok && r.found && r.solvedPercent >= kCacheMinSolved && (r.resultStatus == "client" || r.resultStatus == "reviewed")) {
            e.stage = status::Stage::Cached;
            e.cacheSource = "server";
            e.hasResult = true;
            e.physicsPercent = r.physicsPercent;
            e.solvedPercent = r.solvedPercent;
            e.hasVerification = r.hasVerified;
            e.verifiedPercent = r.verifiedPercent;
            e.windows = static_cast<int>(r.windowsCount);
            e.resultHandled = true;
            log::info("GPRL analyzer: cached analysis used (server, {} {}) - solved {:.1f}%, physics {:.1f}%, {} windows; nothing is simulated", r.resultStatus,
                      r.computedAt, r.solvedPercent, r.physicsPercent, r.windowsCount);
            w->entries.emplace(key, std::move(e));
            return;
        }
        if (debugOn()) {
            log::info("GPRL analyzer: server cache: {}", !r.ok ? "no answer (" + r.error + ")"
                                                         : !r.found ? std::string("no analysis of this gameplay version yet")
                                                                    : fmt::format("found ({}), solved only {:.1f}% - simulating", r.resultStatus, r.solvedPercent));
        }
    }
    // 3. simulate
    sim::JobConfig jc;
    jc.maxShiftTicks = std::clamp(cfg.maxShiftTicks, 1, 10);
    jc.subTickRefine = std::clamp(cfg.subTickRefine, 0, 2);
    e.job = std::make_unique<sim::Job>(std::move(world), jc);
    e.stage = status::Stage::Running;
    e.progress = e.job->progress();
    for (auto& a : m.attempts) addAttempt(e, std::move(a));
    log::info("GPRL analyzer: simulation queued - {} recorded attempt(s), max shift {} ticks, sub-tick refine {}, budget {:.0f} min", e.attemptsAdded,
              jc.maxShiftTicks, jc.subTickRefine, jc.wallBudgetMs / 60000.0);
    w->entries.emplace(key, std::move(e));
}

/// handleLevel threw (review HIGH-4): the visit shows a failed analysis instead of "queued" forever.
void failVisit(uint64_t visitId, std::string const& what) {
    std::string key = fmt::format("failed:{}", visitId);
    Entry e;
    e.key = key;
    e.stage = status::Stage::Failed;
    e.failReason = "worker error: " + what;
    e.resultHandled = true;
    w->visitKey[visitId] = key;
    w->currentVisit = visitId;
    w->entries.insert_or_assign(key, std::move(e));
    log::error("GPRL analyzer: building the analysis of level visit {} failed ({}); this visit is not analysed", visitId, what);
}

void handleAttempt(Msg& m) {
    auto k = w->visitKey.find(m.visitId);
    if (k == w->visitKey.end()) return;
    auto it = w->entries.find(k->second);
    if (it == w->entries.end()) return;
    for (auto& a : m.attempts) addAttempt(it->second, std::move(a));
}

void handleClosed(uint64_t visitId) {
    if (auto vi = w->visitIdentity.find(visitId); vi != w->visitIdentity.end()) {
        if (auto it = w->identities.find(vi->second); it != w->identities.end() && it->second.visitId == visitId) {
            it->second.open = false;
            it->second.closedAt = Clock::now();
        }
        w->visitIdentity.erase(vi);
    }
    auto k = w->visitKey.find(visitId);
    if (w->currentVisit == visitId) w->currentVisit = 0;
    if (k == w->visitKey.end()) return;
    auto it = w->entries.find(k->second);
    w->visitKey.erase(k);
    if (it == w->entries.end()) return;
    it->second.open = false;
    it->second.closedAt = Clock::now();
    if (it->second.job && !it->second.job->done())
        log::info("GPRL analyzer: level left - the analysis pauses ({}) and is kept 10 minutes for another visit", it->second.job->progress().line);
}

/// Inbox messages, in order (review MEDIUM-8: a level waits in the backlog; its attempts and its
/// exit attach to it). Review HIGH-4: one message's exception never stops the others.
void dispatch(std::deque<Msg>& msgs) {
    for (auto& m : msgs) {
        uint64_t const visit = m.visitId;
        try {
            switch (m.kind) {
                case Msg::Kind::Level: {
                    if (uint64_t dropped = w->backlog.push(visit, std::move(m))) {
                        log::warn("GPRL analyzer: {} level visits wait for the simulator to be allowed (Record-Safe during an attempt / a busy game); the "
                                  "oldest (visit {}) is dropped unanalysed",
                                  kBacklogLevels + 1, dropped);
                    }
                    break;
                }
                case Msg::Kind::Attempt:
                    if (auto* item = w->backlog.find(visit)) {
                        for (auto& a : m.attempts) item->payload.attempts.push_back(std::move(a));
                    }
                    else handleAttempt(m);
                    break;
                case Msg::Kind::Closed:
                    if (!w->backlog.markClosed(visit)) handleClosed(visit);
                    break;
            }
        } catch (std::exception const& ex) {
            log::error("GPRL analyzer: a message of visit {} failed ({}); ignored", visit, whatOf(ex));
        } catch (...) {
            log::error("GPRL analyzer: a message of visit {} failed (unknown error); ignored", visit);
        }
    }
}

/// The waiting levels, oldest first, only while the simulator may run (review MEDIUM-8).
void drainBacklog() {
    while (!w->backlog.empty()) {
        if (stopping() || !modes::simAllowedNow()) return;
        auto item = w->backlog.take();
        if (!item.payload.handover) continue;
        try {
            handleLevel(item.visitId, item.payload);
        } catch (std::exception const& ex) {
            failVisit(item.visitId, whatOf(ex));
        } catch (...) {
            failVisit(item.visitId, "unknown error");
        }
        if (stopping()) return;
        if (item.closed) handleClosed(item.visitId);
    }
}

void expire() {
    auto now = Clock::now();
    for (auto it = w->entries.begin(); it != w->entries.end();) {
        auto& e = it->second;
        bool idle = !e.open && now - e.closedAt > kKeepClosed && !e.uploadPending && !e.resultDirty;
        if (idle) {
            if (debugOn()) log::info("GPRL analyzer: dropped the kept analysis of level {} (10 minutes after the level was left)", e.gdLevelId);
            it = w->entries.erase(it);
        }
        else ++it;
    }
    for (auto it = w->identities.begin(); it != w->identities.end();) {
        auto& e = it->second;
        if (!e.open && now - e.closedAt > kKeepClosed && !e.uploadPending) it = w->identities.erase(it);
        else ++it;
    }
}

void handleResult(Entry& e) {
    auto const& res = e.job->result();
    auto phase = e.job->progress().phase;
    e.resultHandled = true;
    e.resultDirty = false;
    e.handledAt = Clock::now();
    if (phase == sim::JobPhase::Failed) {
        e.stage = status::Stage::Failed;
        e.failReason = res.debug.empty() ? std::string("job failed") : res.debug.back();
        log::warn("GPRL analyzer: the simulation of level {} failed ({})", e.gdLevelId, e.failReason);
        return;
    }
    auto cfg = cfgCopy();
    sim::LevelSimResult r = res;
    r.computedAt = isoNow();
    r.build = cfg.clientBuild;
    r.budget.mode = sim::modes::name(modes::config().mode);
    r.budget.recordSafe = modes::config().recordSafe;
    e.resultText = sim::toJsonString(r);
    e.stage = status::Stage::Done;
    e.hasResult = true;
    e.physicsPercent = r.coverage.physicsPercent;
    e.solvedPercent = r.coverage.solvedPercent;
    e.hasVerification = r.hasVerification && r.coverage.hasVerification;
    e.verifiedPercent = r.coverage.verifiedPercent;
    e.windows = static_cast<int>(r.windows.size());
    e.unsupportedSpans = static_cast<int>(r.coverage.unsupported.size());
    e.unsolvedSpans = static_cast<int>(r.coverage.unsolved.size());
    cache::store(e.gdLevelId, e.gameplayHash, e.resultText, false, "");
    e.uploadPending = true;
    e.uploadTries = 0;
    e.retryAt = Clock::now();
    log::info("GPRL analyzer: analysis done - level {} version {}: physics {:.1f}%, solved {:.1f}%, {}, {} windows, {} sections, {} unsupported / {} unsolved spans, "
              "cpu {:.1f} s, {} ticks, {} trials{} ({} bytes)",
              e.gdLevelId, e.gameplayHash, r.coverage.physicsPercent, r.coverage.solvedPercent,
              e.hasVerification ? fmt::format("verified {:.1f}% over {} attempt(s)", r.coverage.verifiedPercent, r.verification.attempts) : std::string("no recorded attempt verified"),
              r.windows.size(), r.sections.size(), e.unsupportedSpans, e.unsolvedSpans, r.budget.cpuMs / 1000.0, r.budget.ticksSimulated, r.budget.trials,
              r.budget.budgetExhausted ? ", wall budget exhausted (Partial)" : "", e.resultText.size());
}

/// One result upload attempt (review HIGH-4: the caller catches; review MEDIUM-11: no request once
/// stopping, nothing touched after a request that returned while stopping).
void uploadResult(Entry& e, Clock::time_point now) {
    auto cfg = cfgCopy();
    auto cr = credsCopy();
    if (!cfg.upload) {
        e.uploadPending = false;
        e.upload = "kept on this computer (sending is off)";
        return;
    }
    if (!cr.online) {
        e.uploadPending = false;
        e.upload = "kept on this computer (local-only)";
        return;
    }
    if (!cr.connected || cr.deviceToken.empty()) {
        e.uploadPending = false;
        e.upload = "kept on this computer (not connected)";
        return;
    }
    std::string const sid = sessionFor(e.levelHash);
    // review LOW: the server's 2 MB limit is on the whole body, the {sessionId, result} envelope included
    size_t const bodyBytes = rules::levelSimUploadBodyBytes(sid, e.resultText);
    if (bodyBytes > kMaxUploadBytes) {
        e.uploadPending = false;
        e.upload = "too large to send";
        log::warn("GPRL analyzer: the analysis of level {} is {} bytes to send, over the server's 2 MB limit; kept on this computer", e.gdLevelId, bodyBytes);
        return;
    }
    if (sid.empty()) {
        // the server takes a result only with this device's session on this exact version
        if (e.open) {
            e.upload = "waiting for the session";
            return;
        }
        e.uploadPending = false;
        e.upload = "kept on this computer (no session on this level yet)";
        return;
    }
    if (stopping()) return;
    api::Config ac{cr.apiBaseUrl, 30};
    auto r = api::uploadLevelSim(ac, cr.deviceToken, e.resultText, sid);
    if (stopping()) return;   // game exit while the request ran: touch nothing shared
    if (r.status == 429 && ++e.rateLimitedTries <= kMaxRateLimitedTries) {
        // one stored upload per device + level version per 10 minutes: send the newer result later,
        // when the server says (Retry-After, clamped to 1-60 min), else in 10 minutes (review LOW)
        int secs = rules::rateLimitDelaySeconds(r.retryAfterSeconds);
        e.retryAt = now + std::chrono::seconds(secs);
        e.upload = fmt::format("sent recently, the newer result follows in {} min", (secs + 59) / 60);
        if (debugOn()) log::info("GPRL analyzer: analysis of level {} rate limited by the server; retrying in {} s", e.gdLevelId, secs);
        return;
    }
    ++e.uploadTries;
    if (r.ok) {
        e.uploadPending = false;
        e.upload = r.stored ? "sent (" + (r.statusLabel.empty() ? std::string("stored") : r.statusLabel) + ")"
                            : "sent, not stored (" + (r.reason.empty() ? std::string("the server has a better one") : r.reason) + ")";
        cache::store(e.gdLevelId, e.gameplayHash, e.resultText, true, e.upload);
        log::info("GPRL analyzer: analysis of level {} sent - stored {}, replaced {}, {} windows stored, server status {}{}", e.gdLevelId, r.stored ? "yes" : "no",
                  r.replaced ? "yes" : "no", r.windowsStored, r.statusLabel.empty() ? "?" : r.statusLabel, r.reason.empty() ? "" : " (" + r.reason + ")");
    }
    else if (r.retryable && e.uploadTries < kMaxUploadTries) {
        e.retryAt = now + kRetryDelay;
        e.upload = "send failed, retry in 30 s";
        log::warn("GPRL analyzer: sending the analysis of level {} failed ({}); retrying once in 30 s", e.gdLevelId, r.error);
    }
    else {
        e.uploadPending = false;
        e.upload = r.status == 404   ? std::string("not sent: the server does not know this level version yet (sent next visit)")
                   : r.status == 403 ? "refused by the server (" + (r.reason.empty() ? std::string("session") : r.reason) + ")"
                   : r.status == 409 ? std::string("not sent: the session is on another version of this level")
                   : r.status == 429 ? std::string("not sent: rate limited, sent on a later visit")
                                     : "send failed (" + (r.code.empty() ? std::to_string(r.status) : r.code) + ")";
        log::warn("GPRL analyzer: sending the analysis of level {} failed ({}); it stays on this computer and is sent on a later visit", e.gdLevelId, r.error);
    }
}

void processUploads() {
    auto now = Clock::now();
    for (auto& [key, e] : w->entries) {
        (void)key;
        if (stopping()) return;
        if (!e.uploadPending || now < e.retryAt || e.resultText.empty()) continue;
        try {
            uploadResult(e, now);
        } catch (std::exception const& ex) {
            e.uploadPending = false;
            e.upload = "send failed (worker error)";
            log::error("GPRL analyzer: sending the analysis of level {} failed with an error ({}); it stays on this computer", e.gdLevelId, whatOf(ex));
        } catch (...) {
            e.uploadPending = false;
            e.upload = "send failed (worker error)";
            log::error("GPRL analyzer: sending the analysis of level {} failed with an unknown error; it stays on this computer", e.gdLevelId);
        }
    }
}

void publish(Entry const* e) {
    Published p;
    p.running = true;
    if (e) {
        p.visitId = w->currentVisit;
        p.gdLevelId = e->gdLevelId;
        p.gameplayHash = e->gameplayHash;
        p.stage = e->stage;
        p.progress = e->progress;
        p.hasResult = e->hasResult;
        p.physicsPercent = e->physicsPercent;
        p.solvedPercent = e->solvedPercent;
        p.verifiedPercent = e->verifiedPercent;
        p.hasVerification = e->hasVerification;
        p.windows = e->windows;
        p.unsupportedSpans = e->unsupportedSpans;
        p.unsolvedSpans = e->unsolvedSpans;
        p.attemptsAdded = e->attemptsAdded;
        p.worldObjects = e->worldObjects;
        p.gameplayObjects = e->gameplayObjects;
        p.decorationObjects = e->decorationObjects;
        p.tooLarge = e->tooLarge;
        p.finishMs = e->finishMs;
        p.cacheSource = e->cacheSource;
        p.upload = e->upload;
        p.failReason = e->failReason;
    }
    if (w->currentVisit) {
        if (auto vi = w->visitIdentity.find(w->currentVisit); vi != w->visitIdentity.end()) {
            if (auto it = w->identities.find(vi->second); it != w->identities.end()) {
                p.familyLine = it->second.line;
                p.identityState = it->second.state;
            }
        }
    }
    p.keptJobs = static_cast<int>(w->entries.size());
    std::lock_guard lock(s_mx);
    s_pub = std::move(p);
}

void runJob(Entry& e) {
    // low: at most half of one core (8 ms work / 8 ms rest), normal: 16 / 1, v0.12.2 fast: 32 / 1 and
    // fastest: 64 / 1 (only when the server's plan allows them, modes::config resolves that) - every
    // tier at below-normal priority, and never under frame pressure or (Record-Safe) during an attempt
    auto const burst = sim::modes::workerBurst(modes::config().cpu);
    double const burstMs = burst.burstMs;
    double const restMs = burst.restMs;
    double const start = nowMs();
    double lastPublish = start;
    sim::JobControl ctl;
    ctl.nowMs = [] { return nowMs(); };
    ctl.mayRun = [&]() -> bool {
        if (stopping() || s_inboxPending.load()) return false;
        if (!e.open || !modes::simAllowedNow()) return false;
        double t = nowMs();
        if (t - lastPublish >= 250.0) {
            lastPublish = t;
            e.progress = e.job->progress();
            publish(&e);
        }
        // AN-D12: work in bursts with rests (the job counts the rest as paused time)
        if (t - start >= burstMs) {
            w->restDue = true;
            w->restMs = restMs;
            return false;
        }
        return true;
    };
    if (debugOn()) ctl.log = [](std::string const& line) { log::info("GPRL analyzer: job: {}", line); };
    e.job->run(ctl);
    e.progress = e.job->progress();
}

/// The current entry's turn (called only while its level is open and simAllowedNow()): queued
/// attempts first, then the job (or a finished job's re-verify). True = more work right away.
bool work(Entry& e) {
    if (!feedAttempts(e)) return false;   // the rule said no / a message waits: the loop comes back
    if (!e.job) return false;
    if (!e.job->done()) {
        runJob(e);
        return e.job && !e.job->done() && modes::simAllowedNow();
    }
    if (e.reverifyNeeded) {
        e.reverifyNeeded = false;
        runJob(e);   // a finished job re-verifies with the new attempts and returns
        e.resultDirty = e.resultHandled;
    }
    return false;
}

/// One round of the loop (review HIGH-4: every part catches its own exceptions; this returns
/// whether the job wants to continue after a short rest).
bool iteration() {
    std::deque<Msg> msgs;
    {
        std::lock_guard lock(s_mx);
        msgs.swap(s_inbox);
        s_inboxPending.store(false);
    }
    dispatch(msgs);
    msgs.clear();
    if (stopping()) return false;
    drainBacklog();
    if (stopping()) return false;
    expire();
    Entry* cur = current();
    bool runnable = false;
    if (cur && cur->job && cur->open && modes::simAllowedNow()) {
        try {
            runnable = work(*cur);
        } catch (std::exception const& ex) {
            failEntry(*cur, whatOf(ex));
        } catch (...) {
            failEntry(*cur, "unknown error");
        }
    }
    if (stopping()) return false;
    if (cur && cur->job && cur->job->done() && !cur->resultHandled) {
        try {
            handleResult(*cur);
        } catch (std::exception const& ex) {
            failEntry(*cur, whatOf(ex));
        } catch (...) {
            failEntry(*cur, "unknown error");
        }
    }
    for (auto& [key, e] : w->entries) {
        (void)key;
        if (!(e.resultDirty && e.job && (!e.open || Clock::now() - e.handledAt >= kResendAfter))) continue;
        try {
            handleResult(e);
        } catch (std::exception const& ex) {
            failEntry(e, whatOf(ex));
        } catch (...) {
            failEntry(e, "unknown error");
        }
    }
    processUploads();
    if (stopping()) return false;
    processIdentityUploads();
    if (stopping()) return false;
    publish(cur);
    return runnable;
}

void loop() {
    Local local;
    w = &local;
#ifdef GEODE_IS_WINDOWS
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    log::info("GPRL analyzer: worker thread started (below-normal priority)");
    cache::enforceLimit();   // review MEDIUM-10: the store's cap once per run, on this thread
    while (!stopping()) {
        bool runnable = false;
        try {
            runnable = iteration();
        } catch (std::exception const& ex) {
            if (stopping()) break;
            log::error("GPRL analyzer: worker round failed ({}); the thread goes on", whatOf(ex));
        } catch (...) {
            if (stopping()) break;
            log::error("GPRL analyzer: worker round failed (unknown error); the thread goes on");
        }
        if (stopping()) break;
        std::unique_lock lock(s_mx);
        if (runnable) {
            // the burst's rest (AN-D12; a short yield when the job returned for another reason),
            // interruptible by a new message or the exit
            double rest = local.restDue ? local.restMs : 0.5;
            local.restDue = false;
            s_cv.wait_for(lock, std::chrono::duration<double, std::milli>(rest), [] { return s_stop.load() || !s_inbox.empty(); });
        }
        else {
            local.restDue = false;
            s_cv.wait_for(lock, std::chrono::milliseconds(250), [] { return s_stop.load() || !s_inbox.empty(); });
        }
    }
    if (s_detached.load()) {
        // shutdown() gave up on this thread (a request was in flight at game exit): the statics may
        // be gone - the thread's own state is dropped with this frame, nothing shared is touched
        w = nullptr;
        return;
    }
    {
        std::lock_guard lock(s_mx);
        s_pub = Published{};
        s_notices.clear();
    }
    s_noticePending.store(false);
    w = nullptr;
}

void post(Msg m) {
    {
        std::lock_guard lock(s_mx);
        s_inbox.push_back(std::move(m));
        s_inboxPending.store(true);
    }
    s_cv.notify_all();
}

}  // namespace

void start() {
    if (s_thread.joinable()) return;
    s_stop.store(false);
    s_detached.store(false);
    {
        std::lock_guard lock(s_doneMx);
        s_done = false;
    }
    s_thread = std::thread([] {
        try {
            loop();
        } catch (...) {
            // review HIGH-4: nothing may std::terminate the game from this thread
            w = nullptr;
        }
        if (s_detached.load()) return;
        {
            std::lock_guard lock(s_doneMx);
            s_done = true;
        }
        s_doneCv.notify_all();
    });
}

void shutdown() {
    if (!s_thread.joinable()) return;
    s_stop.store(true);
    s_cv.notify_all();
    bool done = false;
    {
        std::unique_lock lock(s_doneMx);
        done = s_doneCv.wait_for(lock, std::chrono::milliseconds(1500), [] { return s_done; });
    }
    if (done) s_thread.join();
    else {
        // review MEDIUM-11: once s_stop is set the worker starts no request; one already in flight
        // ends in the detached thread, which then returns without touching anything shared
        s_detached.store(true);
        log::warn("GPRL analyzer: worker still busy (a network request in flight) at game exit; detached - it returns as soon as the request ends");
        s_thread.detach();
    }
}

void setConfig(Config cfg) {
    std::lock_guard lock(s_mx);
    s_cfg = std::move(cfg);
}

void setCredentials(Credentials c) {
    std::lock_guard lock(s_mx);
    // review MEDIUM-5: only the (level hash, session id, generation) triple the telemetry worker
    // published together; a newer generation wins, the oldest mapping is evicted (rules::SessionMap)
    if (!c.sessionId.empty() && !c.sessionLevelHash.empty()) s_sessions.note(c.sessionLevelHash, c.sessionId, c.sessionGen);
    s_creds = std::move(c);
}

void submitLevel(uint64_t visitId, extraction::Handover handover, std::vector<AttemptIn> attempts) {
    Msg m;
    m.kind = Msg::Kind::Level;
    m.visitId = visitId;
    m.handover = std::move(handover);
    m.attempts = std::move(attempts);
    post(std::move(m));
}

void submitAttempt(uint64_t visitId, AttemptIn attempt) {
    Msg m;
    m.kind = Msg::Kind::Attempt;
    m.visitId = visitId;
    m.attempts.push_back(std::move(attempt));
    post(std::move(m));
}

void levelClosed(uint64_t visitId) {
    Msg m;
    m.kind = Msg::Kind::Closed;
    m.visitId = visitId;
    post(std::move(m));
}

Published published() {
    std::lock_guard lock(s_mx);
    Published p = s_pub;
    p.running = s_thread.joinable() && !s_stop.load();
    return p;
}

bool familyNoticePending() { return s_noticePending.load(std::memory_order_relaxed); }

std::optional<FamilyNotice> takeFamilyNotice() {
    std::lock_guard lock(s_mx);
    if (s_notices.empty()) {
        s_noticePending.store(false);
        return std::nullopt;
    }
    FamilyNotice n = std::move(s_notices.front());
    s_notices.pop_front();
    if (s_notices.empty()) s_noticePending.store(false);
    return n;
}

}  // namespace gprl::analyzer::worker
