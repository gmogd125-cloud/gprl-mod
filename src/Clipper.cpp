#include "Clipper.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

#include "../core/crypto.hpp"
#include "../core/telemetry.hpp"
#include "Api.hpp"
#include "Settings.hpp"
#include "Telemetry.hpp"
#include "Tracker.hpp"
#include "clip/AudioTap.hpp"
#include "clip/ClipLinkPopup.hpp"
#include "clip/ClipPopup.hpp"
#include "clip/ClipUtil.hpp"
#include "clip/MicCapture.hpp"
#include "clip/Process.hpp"
#include "Hud.hpp"

using namespace geode::prelude;

namespace gprl::clipper {

namespace {

struct State {
    bool initialized = false;
    clip::ClipConfig config;
    clip::Paths paths;
    // the open (or last) level
    bool levelOpen = false;
    LevelInfo level;
    std::string sessionLocalId;
    // what the telemetry client said about THIS level session (sessionStatus()); kept after the
    // level is left, so a clip made from the menu still names the right server session
    uint32_t sessionGen = 0;               // client::sessionGeneration() of the level session, 0 = not begun
    std::string serverSessionId;           // telemetry session id while it is a Remote session, else ""
    std::optional<bool> serverLevelCounts; // the server's levelCounts verdict (Remote sessions only)
    bool serverHintsSupported = false;     // this level session's server sent `preserveEvidence` / `verificationRequests`
    std::set<std::string> hinted;          // attempt ids the server named
    std::set<std::string> verificationCases;    // v0.9.0: case ids already handled (each once)
    std::set<std::string> verificationAttempts; // v0.9.0: attempts whose clip is sent as soon as it is ready
    clip::AttemptBook attempts;
    std::string lastAttemptId;             // newest attempt started
    std::string lastEndedAttemptId;        // newest attempt that has ended
    // clips
    std::vector<clip::ClipRecord> records;   // oldest first
    uint32_t clipCounter = 0;
    std::set<std::string> offered;         // clips whose popup was already opened automatically
    // upload (one at a time)
    bool uploadActive = false;
    std::string uploadClipId;
    std::deque<std::string> uploadQueue;
    int64_t serverMaxBytes = 0;            // the last upload session's maxBytes (0 = not known yet)
    // v0.10.0: YouTube links
    bool linkSending = false;
    std::string linkClipId;
    std::string linkMessage;
    std::set<std::string> longRunWarned;   // level sessions already warned about runs over the upload cap
} s;

std::shared_ptr<std::atomic<double>> s_uploadFraction = std::make_shared<std::atomic<double>>(0.0);
std::atomic<bool> s_exiting{false};

int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

clip::ClipRecord* find(std::string const& clipId) {
    for (auto& r : s.records) {
        if (r.clipId == clipId) return &r;
    }
    return nullptr;
}

/// The newest clip whose upload failed (its file is still here): offered from the Account tab.
std::optional<clip::ClipRecord> failedUploadClip() {
    for (auto it = s.records.rbegin(); it != s.records.rend(); ++it) {
        if (it->state == clip::ClipState::UploadFailed) return *it;
    }
    return std::nullopt;
}

/// An attempt "Clip last attempt" may take: still tracked, and of the open (or last) level session.
bool clippable(std::string const& attemptId) {
    if (attemptId.empty()) return false;
    auto const* mark = s.attempts.find(attemptId);
    return mark && mark->sessionLocalId == s.sessionLocalId;
}

/// The telemetry client's status, but only when it describes THIS level session. The worker
/// publishes a session a moment after the main thread began it and shows the previous one's state
/// until then (Status::sessionGen), so without this check a clip could be stamped with another
/// level's server session id / levelCounts, or clip_available be sent on a stale telemetryRevision.
/// What a current status says is remembered for clips made after the session closed.
std::optional<client::Status> sessionStatus() {
    auto cs = client::status();
    if (s.sessionGen == 0 || cs.sessionGen != s.sessionGen) return std::nullopt;
    if (cs.mode == client::SessionMode::Remote) {
        s.serverSessionId = cs.sessionId;
        s.serverLevelCounts = cs.levelCounts;
    }
    else {
        // local-only / not connected / networking stopped: nothing of this session reaches a server
        s.serverSessionId.clear();
        s.serverLevelCounts.reset();
    }
    return cs;
}

clip::UploadGate gate() {
    auto auth = client::uploadAuth();
    clip::UploadGate g;
    g.connected = auth.connected;
    g.localOnly = auth.localOnly;
    g.maxBytes = s.serverMaxBytes > 0 ? s.serverMaxBytes : clip::kFlow.defaultMaxUploadBytes;
    return g;
}

void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info, float seconds = 3.5f) {
    Notification::create(text, icon, seconds)->show();
}

void persist() {
    if (s.paths.indexFile.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(s.paths.indexFile.parent_path(), ec);
    std::ofstream out(s.paths.indexFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        log::warn("GPRL clip: cannot write the clip index {}", clip::utf8(s.paths.indexFile));
        return;
    }
    out << clip::indexToText(s.records);
}

void deleteFile(clip::ClipRecord& r) {
    if (r.path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(clip::fromUtf8(r.path), ec);
    if (ec) log::warn("GPRL clip: could not delete {}: {}", r.path, ec.message());
    r.path.clear();
}

/// Pending file -> the clips folder, under a readable name.
void moveToClipsFolder(clip::ClipRecord& r) {
    if (r.path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(s.paths.clipsDir, ec);
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    std::string name = clip::clipFileName(r.levelName, r.levelId, r.percent, r.completed, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                          local.tm_hour, local.tm_min, local.tm_sec);
    auto target = s.paths.clipsDir / clip::fromUtf8(name);
    for (int n = 2; std::filesystem::exists(target, ec) && n < 100; ++n) {
        target = s.paths.clipsDir / clip::fromUtf8(name.substr(0, name.size() - 4) + fmt::format(" ({}).mp4", n));
    }
    auto from = clip::fromUtf8(r.path);
    std::filesystem::rename(from, target, ec);
    if (ec) {
        // another drive: copy, then remove the pending file
        ec.clear();
        std::filesystem::copy_file(from, target, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) std::filesystem::remove(from, ec);
    }
    if (ec) {
        log::warn("GPRL clip: could not move {} to the clips folder ({}); it stays at {}", r.clipId, ec.message(), r.path);
        return;
    }
    r.path = clip::utf8(target);
    log::info("GPRL clip: {} saved to {}", r.clipId, r.path);
}

/// telemetry clip_available {clipId, attemptId, durationMs, sha256} after a local save. Only in
/// the level session the attempt belongs to, and only when that session's server knows the kind.
void emitClipAvailable(clip::ClipRecord& r) {
    if (r.eventSent) return;
    std::string why;
    std::optional<client::Status> cs;
    if (!clip::validClipId(r.clipId) || !clip::validSha256Hex(r.sha256) || r.durationMs <= 0.0) why = "the clip record is incomplete";
    else if (!client::sessionOpen() || tracker::sessionLocalId() != r.sessionLocalId || s.sessionLocalId != r.sessionLocalId)
        why = "the level session of that attempt is closed";
    // only a status that describes this very session counts: a stale one (the previous level's)
    // could carry a newer telemetryRevision than the server of this session validates
    else if (!(cs = sessionStatus()) || cs->mode == client::SessionMode::None) why = "the telemetry session is not open yet";
    else if (cs->telemetryRevision < telemetry::kClipAvailableRevision)
        why = fmt::format("this server validates telemetry revision {} (clip_available needs {})", cs->telemetryRevision, telemetry::kClipAvailableRevision);
    if (!why.empty()) {
        log::info("GPRL clip: clip_available for {} not sent: {} (the record stays in clips.json)", r.clipId, why);
        return;
    }
    telemetry::Event e;
    e.attemptId = r.attemptId;
    e.t = std::max(0.0, r.endT);
    e.tick = std::max<int64_t>(0, r.endTick);
    telemetry::ClipAvailablePayload p;
    p.clipId = r.clipId;
    p.durationMs = std::max(1.0, std::round(r.durationMs));
    p.sha256 = r.sha256;
    e.payload = p;
    client::push(std::move(e));
    r.eventSent = true;
    log::info("GPRL clip: clip_available sent for {} (attempt {}, {:.0f} ms, sha256 {}...)", r.clipId, r.attemptId, r.durationMs, r.sha256.substr(0, 12));
}

void startUpload(std::string const& clipId);

/// Runs the actions the flow returned for `r` (core/clip_flow). StartUpload only ever appears for
/// an explicit Send / Save + send choice or a retry of one.
void run(std::vector<clip::Action> const& actions, clip::ClipRecord& r) {
    std::string clipId = r.clipId;
    bool upload = false;
    for (auto a : actions) {
        switch (a) {
            case clip::Action::MoveToClipsFolder: moveToClipsFolder(r); break;
            case clip::Action::EmitClipAvailable: emitClipAvailable(r); break;
            case clip::Action::StartUpload: upload = true; break;
            case clip::Action::DeleteFile: deleteFile(r); break;
            case clip::Action::Persist: break;
        }
    }
    persist();
    if (upload) startUpload(clipId);
}

// ---- cutting a clip (clip thread) ----

struct PrepareJob {
    std::string clipId;
    std::string attemptId;
    std::string title;
    double from = 0.0;
    double to = 0.0;
    std::filesystem::path ffmpeg;
    std::filesystem::path bufferRoot;
    std::filesystem::path pendingDir;
};

struct PrepareResult {
    bool ok = false;
    std::string error;
    std::string path;
    std::string sha256;
    int64_t size = 0;
    double durationMs = 0.0;
    int width = 0;
    int height = 0;
    bool game = false;
    bool mic = false;
    bool desktop = false;
    bool wholeAttempt = false;
    int segments = 0;
    double tookSeconds = 0.0;
};

/// Seconds a clip may last before it is over the server's upload cap at the current settings.
double uploadCapSeconds() {
    int tracks = (s.config.gameAudio ? 1 : 0) + (s.config.micAudio ? 1 : 0) + (s.config.desktopAudio ? 1 : 0);
    return clip::maxUploadSeconds(clip::spec(s.config.quality).videoKbps, tracks, clip::kClip.audioKbps, gate().maxBytes);
}

/// v0.10.0: a verification clip over the upload cap is saved and waits for a YouTube link.
bool handleVerificationTooLong(clip::ClipRecord& r, std::string const& what) {
    std::string clipId = r.clipId;
    std::string size = clip::formatBytes(r.sizeBytes);
    std::string cap = clip::formatBytes(gate().maxBytes);
    std::string capTime = clip::formatDuration(uploadCapSeconds() * 1000.0);
    r.needsLink = true;
    s.offered.insert(clipId);
    applyChoice(clipId, clip::Choice::SaveLocal);   // keeps the file in the clips folder, sends clip_available
    if (auto* again = find(clipId)) {
        again->needsLink = true;
        persist();
    }
    log::info("GPRL clip: {} is {} - over the upload cap of {} ({} at {}): saved for a YouTube link", clipId, size, cap, capTime,
              clip::spec(s.config.quality).label);
    hud::verificationToast("Verify your run (" + what + "): the clip is " + size + ", too long to upload (max " + capTime
                               + "). It was SAVED - upload it to YouTube (Unlisted or Public) and send the link: GPRL menu > Account > YouTube link",
                           14.f);
    return true;
}

void onPrepared(std::string const& clipId, PrepareResult const& res) {
    auto* r = find(clipId);
    if (!r) {
        // the record is gone: never leave an unreferenced clip on disk
        std::error_code ec;
        if (!res.path.empty()) std::filesystem::remove(clip::fromUtf8(res.path), ec);
        return;
    }
    if (res.ok) {
        r->path = res.path;
        r->sha256 = res.sha256;
        r->sizeBytes = res.size;
        r->durationMs = res.durationMs;
        r->width = res.width;
        r->height = res.height;
        r->hasGameAudio = res.game;
        r->hasMicAudio = res.mic;
        r->hasDesktopAudio = res.desktop;
        r->wholeAttempt = res.wholeAttempt;
        log::info("GPRL clip: {} ready: {} {} {}x{} from {} segments, game sound {}, microphone {}, desktop sound {}, whole attempt {}, sha256 {}... (took {:.1f} s)", clipId,
                  clip::formatDuration(res.durationMs), clip::formatBytes(res.size), res.width, res.height, res.segments, res.game ? "yes" : "no",
                  res.mic ? "yes" : "no", res.desktop ? "yes" : "no", res.wholeAttempt ? "yes" : "NO (the buffer no longer held its start)",
                  res.sha256.substr(0, 12), res.tookSeconds);
    }
    else {
        log::warn("GPRL clip: {} could not be made: {}", clipId, res.error);
        notify("GPRL: the clip could not be made (see the Geode log)", NotificationIcon::Error);
    }
    bool hadChoice = r->choiceMade;
    auto actions = clip::onPrepared(*r, res.ok, res.error, gate());
    run(actions, *r);
    // v0.10.0: a verification clip that turned out too long to upload is saved and waits for a link
    if (res.ok) {
        if (auto* again = find(clipId); again && again->rule == "verification" && again->state == clip::ClipState::Ready && clip::overUploadCap(*again, gate()))
            handleVerificationTooLong(*again, "this run");
    }
    if (res.ok && hadChoice) {
        if (auto* again = find(clipId)) {
            log::info("GPRL clip: {} - the choice made while it was prepared is applied: {}", clipId, clip::statusLine(*again, 0.0));
            // "Send" was chosen early and cannot be sent now: the clip waits for a choice again
            // (the popup is already closed, so say where it is)
            if (again->state == clip::ClipState::Ready && !again->error.empty())
                notify("GPRL: clip " + again->error + " - choose again in the GPRL menu > Account", NotificationIcon::Warning, 6.f);
        }
    }
    // too many clips waiting for a choice: the oldest are dropped (bounded disk use)
    auto dropped = clip::trimIndex(s.records);
    for (auto& d : dropped) {
        log::info("GPRL clip: {} discarded (more than {} clips were waiting for a choice)", d.clipId, clip::kFlow.maxPendingClips);
        deleteFile(d);
    }
    if (!dropped.empty()) persist();
}

void prepareThread(PrepareJob job) {
    PrepareResult res;
    double started = clip::now();
    std::error_code ec;
    auto work = job.bufferRoot / "work" / job.clipId;
    // blocks until the segment holding the range end is on disk (the tail after the attempt end)
    auto src = clip::Capture::get().snapshot(job.from, job.to, work);
    clip::Capture::get().unpin(job.clipId);   // the hard links keep the footage from here on
    if (!src.error.empty()) res.error = src.error;
    else if (job.ffmpeg.empty()) res.error = "ffmpeg.exe was not found";
    else {
        std::filesystem::create_directories(job.pendingDir, ec);
        auto out = job.pendingDir / (job.clipId + ".mp4");
        clip::MuxInputs in;
        in.concatFile = clip::utf8(src.concatFile);
        in.gameWav = src.gameWav.empty() ? std::string() : clip::utf8(src.gameWav);
        in.micWav = src.micWav.empty() ? std::string() : clip::utf8(src.micWav);
        in.desktopWav = src.desktopWav.empty() ? std::string() : clip::utf8(src.desktopWav);
        in.output = clip::utf8(out);
        in.clipId = job.clipId;
        in.attemptId = job.attemptId;
        in.title = job.title;
        auto args = clip::muxArgs(in, clip::kClip.audioKbps);
        GPRL_DEBUG("GPRL clip: ffmpeg {}", args);
        clip::ProcessOptions opts;
        opts.logFile = work / "mux.log";
        opts.lowPriority = true;   // never competes with the game
        auto proc = clip::Process::start(job.ffmpeg, clip::widen(args), opts);
        std::optional<unsigned long> code;
        if (proc) {
            code = proc->wait(clip::kClip.muxTimeoutMs);
            if (!code) proc->kill();
        }
        auto size = std::filesystem::file_size(out, ec);
        bool muxed = code && *code == 0 && !ec && size > 0;
        if (!muxed) {
            res.error = fmt::format("ffmpeg could not write the clip (exit {}): {}", code ? static_cast<long>(*code) : -1L, clip::readTail(opts.logFile, 500));
            std::filesystem::remove(out, ec);
        }
        else if (!clip::hashFile(out, res.sha256, res.size)) {
            res.error = "the clip file could not be read back for hashing";
            std::filesystem::remove(out, ec);
        }
        else {
            res.ok = true;
            res.path = clip::utf8(out);
            res.durationMs = src.duration * 1000.0;
            res.width = src.size.width;
            res.height = src.size.height;
            res.game = !src.gameWav.empty();
            res.mic = !src.micWav.empty();
            res.desktop = !src.desktopWav.empty();
            res.wholeAttempt = src.coversStart;
            res.segments = src.segments;
        }
    }
    std::filesystem::remove_all(work, ec);
    res.tookSeconds = clip::now() - started;
    Loader::get()->queueInMainThread([clipId = job.clipId, res] { onPrepared(clipId, res); });
}

// ---- upload (upload thread; explicit player choice only) ----

struct UploadOutcome {
    bool ok = false;
    bool already = false;
    bool reconnect = false;
    std::string error;
    std::string uploadId;
    int tries = 0;
    int64_t maxBytes = 0;
};

void startNextUpload() {
    while (!s.uploadActive && !s.uploadQueue.empty()) {
        std::string next = s.uploadQueue.front();
        s.uploadQueue.pop_front();
        startUpload(next);
    }
}

void onUploadFinished(std::string const& clipId, UploadOutcome const& out) {
    s.uploadActive = false;
    s.uploadClipId.clear();
    if (out.maxBytes > 0) s.serverMaxBytes = out.maxBytes;
    if (auto* r = find(clipId)) {
        r->uploadTries += out.tries;
        if (!out.uploadId.empty()) r->uploadId = out.uploadId;
        auto actions = clip::onUploadResult(*r, out.ok, out.error);
        run(actions, *r);
        if (out.ok) {
            log::info("GPRL clip: {} uploaded{} ({} tr{}, upload id {})", clipId, out.already ? " (the server already had it)" : "", out.tries,
                      out.tries == 1 ? "y" : "ies", out.uploadId.empty() ? "-" : out.uploadId);
            notify("GPRL: clip sent to the GPRL moderators", NotificationIcon::Success);
        }
        else {
            log::warn("GPRL clip: upload of {} failed after {} tr{}: {}", clipId, out.tries, out.tries == 1 ? "y" : "ies", out.error);
            notify("GPRL: the clip was not sent - " + out.error, NotificationIcon::Error, 5.f);
        }
    }
    startNextUpload();
}

void uploadThread(clip::ClipRecord record, client::UploadAuth auth, std::shared_ptr<std::atomic<double>> fraction) {
    UploadOutcome out;
    std::string clipId = record.clipId;
    auto finish = [&] { Loader::get()->queueInMainThread([clipId, out] { onUploadFinished(clipId, out); }); };

    std::vector<uint8_t> bytes;
    {
        std::ifstream in(clip::fromUtf8(record.path), std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            auto size = static_cast<std::streamoff>(in.tellg());
            in.seekg(0);
            if (size > 0) {
                bytes.resize(static_cast<size_t>(size));
                in.read(reinterpret_cast<char*>(bytes.data()), size);
                if (in.gcount() != size) bytes.clear();
            }
        }
    }
    if (bytes.empty()) {
        out.error = "the clip file cannot be read";
        return finish();
    }
    // evidence integrity (SPEC §31): what is sent is what was hashed when the clip was made
    if (crypto::toHex(crypto::sha256(bytes.data(), bytes.size())) != record.sha256) {
        out.error = "the clip file changed on disk after it was made, so it is not sent";
        return finish();
    }

    api::Config cfg{auth.apiBaseUrl, clip::kFlow.sessionTimeoutSeconds};
    std::string last;
    for (int attempt = 1; attempt <= clip::kFlow.uploadAttempts && !s_exiting.load(); ++attempt) {
        for (int waited = 0; waited < clip::backoffMs(attempt) && !s_exiting.load(); waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        out.tries = attempt;
        fraction->store(0.0);
        auto session = api::createEvidenceUpload(cfg, auth.deviceToken, record, auth.clientBuild);
        auto step = clip::classifySessionResponse(session.status, session.code);
        if (step.alreadyUploaded) {
            out.ok = true;
            out.already = true;
            break;
        }
        if (step.verdict == clip::UploadVerdict::Fatal) {
            out.error = step.message;
            out.reconnect = step.reconnect;
            break;
        }
        if (step.verdict == clip::UploadVerdict::Retry) {
            last = step.message;
            log::info("GPRL clip: upload of {} try {}/{}: {}", clipId, attempt, clip::kFlow.uploadAttempts, last);
            continue;
        }
        if (!session.parsed) {
            out.error = "the server's upload answer is not usable";
            log::warn("GPRL clip: upload of {}: {}", clipId, session.error);
            break;
        }
        out.uploadId = session.session.uploadId;
        out.maxBytes = session.session.maxBytes;
        if (out.maxBytes > 0 && static_cast<int64_t>(bytes.size()) > out.maxBytes) {
            out.error = "the clip is " + clip::formatBytes(static_cast<int64_t>(bytes.size())) + ", the server accepts at most " + clip::formatBytes(out.maxBytes);
            break;
        }
        auto plan = clip::planPut(auth.apiBaseUrl, session.session);
        if (!plan.ok) {
            out.error = plan.error;
            break;
        }
        log::info("GPRL clip: upload of {} try {}/{}: sending {} to the {} (upload id {})", clipId, attempt, clip::kFlow.uploadAttempts,
                  clip::formatBytes(static_cast<int64_t>(bytes.size())), plan.sendBearer ? "GPRL API" : "evidence storage", out.uploadId);
        auto put = api::putEvidence(plan, auth.deviceToken, record.sha256, bytes, clip::kFlow.putTimeoutSeconds, fraction.get());
        auto pstep = clip::classifyPutResponse(put.status, put.code, put.sha256, record.sha256);
        if (pstep.verdict == clip::UploadVerdict::Done) {
            out.ok = true;
            out.already = pstep.alreadyUploaded;
            fraction->store(1.0);
            break;
        }
        if (pstep.verdict == clip::UploadVerdict::Fatal) {
            out.error = pstep.message;
            out.reconnect = pstep.reconnect;
            break;
        }
        last = pstep.message;
        log::info("GPRL clip: upload of {} try {}/{}: {}", clipId, attempt, clip::kFlow.uploadAttempts, last);
    }
    if (!out.ok && out.error.empty()) out.error = last.empty() ? std::string("the upload did not finish") : last;
    finish();
}

void startUpload(std::string const& clipId) {
    auto* r = find(clipId);
    if (!r || r->state != clip::ClipState::Uploading) return;
    if (s.uploadActive) {
        if (std::find(s.uploadQueue.begin(), s.uploadQueue.end(), clipId) == s.uploadQueue.end()) s.uploadQueue.push_back(clipId);
        return;
    }
    auto auth = client::uploadAuth();
    if (!auth.connected || auth.localOnly || auth.deviceToken.empty()) {
        // the gate said yes a moment ago (flow), so this is a disconnect in between
        UploadOutcome out;
        out.error = "not connected: press Connect in the GPRL menu first";
        s.uploadActive = true;
        onUploadFinished(clipId, out);
        return;
    }
    s.uploadActive = true;
    s.uploadClipId = clipId;
    s_uploadFraction->store(0.0);
    log::info("GPRL clip: uploading {} ({}; the player chose \"{}\")", clipId, clip::formatBytes(r->sizeBytes), clip::label(r->choice));
    std::thread(uploadThread, *r, std::move(auth), s_uploadFraction).detach();
}

/// A modal popup is fine right now: in a menu, paused, or on the end screen - never mid-run.
bool safeToPrompt() {
    auto* pl = PlayLayer::get();
    return !pl || pl->m_isPaused || pl->m_hasCompletedLevel;
}

std::string bufferLine(clip::ClipConfig const& cfg, clip::CaptureStatus const& cap, bool& problem) {
    problem = false;
    if (!cfg.enabled) return "Clipping: off (mod settings > Clipping; " + clip::costLine(cfg) + ")";
    if (!cap.problem.empty()) {
        problem = cap.problem.rfind("checking", 0) != 0;
        return "Clipping: NOT recording - " + cap.problem;
    }
    std::string held = fmt::format("buffer {:.0f} s, {} (cap {} MB){}", cap.recordedSeconds, clip::formatBytes(cap.liveBytes), cfg.diskCapMb,
                                   cap.capLimited ? ", CAP REACHED" : "");
    if (cap.droppedFrames > 0) held += fmt::format(", {} frames dropped", cap.droppedFrames);
    if (cap.recording)
        return fmt::format("Clipping: recording {}x{} {} fps {} - {}{}", cap.output.width, cap.output.height, clip::kClip.fps, clip::name(cap.encoder), held,
                           cfg.micAudio ? ", MICROPHONE ON" : "");
    if (cap.wanted) return "Clipping: on, starting the recording... (" + held + ")";
    return "Clipping: on, records while a level is open (" + held + ")";
}

}  // namespace

// v0.9.0 (defined below, used by onAttemptEnd)
bool adoptForVerification(std::string const& attemptId);

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

void applySettings() {
    auto const& st = settings::get();
    clip::ClipConfig cfg;
    cfg.enabled = st.clipping && st.enabled;   // the mod's master switch off = nothing is recorded
    cfg.bufferSeconds = st.clipBufferSeconds;
    cfg.diskCapMb = st.clipDiskCapMb;
    cfg.quality = clip::parseQuality(st.clipQuality);
    cfg.gameAudio = st.clipGameAudio;
    cfg.micAudio = st.clipMic;
    cfg.desktopAudio = st.clipDesktopAudio;
    cfg = clip::sanitized(cfg);
    auto paths = clip::resolvePaths(st.ffmpegPath, st.clipFolder);
    bool changed = paths.ffmpeg != s.paths.ffmpeg || paths.root != s.paths.root || cfg.enabled != s.config.enabled;
    s.config = cfg;
    s.paths = paths;
    if (cfg.enabled && changed) {
        log::info("GPRL clip: ffmpeg {}{}; clip folder {}; microphone track {}; desktop sound track {}", paths.ffmpeg.empty() ? std::string("NOT FOUND") : clip::utf8(paths.ffmpeg),
                  paths.ffmpegSource.empty() ? std::string() : " (" + paths.ffmpegSource + ")", clip::utf8(paths.root), cfg.micAudio ? "ON (opt-in)" : "off",
                  cfg.desktopAudio ? "ON" : "off");
    }
    clip::Capture::get().applySettings(clip::CaptureSettings{cfg, paths.ffmpeg, paths.bufferRoot});
    // Without ffmpeg nothing can be recorded: no sound tap and no recording device either.
    bool canRecord = cfg.enabled && !paths.ffmpeg.empty();
    clip::AudioTap::get().setEnabled(canRecord && cfg.gameAudio);
    clip::MicCapture::get().setEnabled(canRecord && cfg.micAudio);
    clip::MicCapture::desktop().setEnabled(canRecord && cfg.desktopAudio);
    // The microphone / desktop sound is only KEPT while a level is open (what it hears in a menu is
    // dropped as it arrives): a clip never holds sound from outside a level, and none waits in memory either.
    clip::MicCapture::get().setStoring(canRecord && s.levelOpen);
    clip::MicCapture::desktop().setStoring(canRecord && s.levelOpen);
    clip::Capture::get().setWanted(cfg.enabled && s.levelOpen);
    if (!cfg.enabled) clip::Capture::get().setOpenAttemptStart(std::nullopt);
}

void init() {
    if (s.initialized) return;
    s.initialized = true;
    if (s.paths.indexFile.empty()) applySettings();
    std::error_code ec;
    {
        std::ifstream in(s.paths.indexFile, std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            s.records = clip::indexFromText(text);
        }
    }
    for (auto& r : s.records) {
        bool exists = !r.path.empty() && std::filesystem::is_regular_file(clip::fromUtf8(r.path), ec);
        clip::reconcileAfterRestart(r, exists);
    }
    auto dropped = clip::trimIndex(s.records);
    for (auto& d : dropped) deleteFile(d);
    // Prepared files no record refers to (a crash between the mux and the index write). Matched by
    // the clip id in the file name, and only this mod's own "<clipId>.mp4" files are ever removed.
    std::set<std::string> referenced;
    for (auto const& r : s.records) {
        if (!r.path.empty()) referenced.insert(r.clipId);
    }
    if (std::filesystem::is_directory(s.paths.pendingDir, ec)) {
        for (auto const& entry : std::filesystem::directory_iterator(s.paths.pendingDir, ec)) {
            std::string stem = clip::utf8(entry.path().stem());
            bool ours = entry.path().extension() == ".mp4" && stem.rfind("clip-", 0) == 0 && clip::validClipId(stem);
            if (ours && !referenced.count(stem)) {
                log::info("GPRL clip: removing the unreferenced pending clip {}", stem);
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
    if (!s.records.empty()) persist();
    int waiting = 0;
    for (auto const& r : s.records) waiting += clip::awaitingChoice(r) ? 1 : 0;
    log::info("GPRL clip: clipping buffer {} ({}, {}); {} clip record(s), {} waiting for a choice", s.config.enabled ? "ON" : "off", clip::kClip.version,
              clip::kFlow.version, s.records.size(), waiting);
}

void shutdown() {
    s_exiting.store(true);
    clip::Capture::get().shutdown();
    if (s.initialized) persist();
}

// ---------------------------------------------------------------------------
// tracker notifications
// ---------------------------------------------------------------------------

void onLevelEnter(LevelInfo info, std::string const& sessionLocalId) {
    s.levelOpen = true;
    s.level = std::move(info);
    s.sessionLocalId = sessionLocalId;
    s.sessionGen = 0;   // the tracker begins the telemetry session right after this call (onAttemptStart reads it)
    s.serverSessionId.clear();
    s.serverLevelCounts.reset();
    s.serverHintsSupported = false;
    s.hinted.clear();
    clip::Capture::get().setWanted(s.config.enabled);
    clip::MicCapture::get().setStoring(s.config.enabled && !s.paths.ffmpeg.empty());
    clip::MicCapture::desktop().setStoring(s.config.enabled && !s.paths.ffmpeg.empty());
}

void onAttemptStart(std::string const& attemptId, int attemptNo, double fromPercent, bool practice, bool startPos) {
    clip::AttemptMark mark;
    mark.attemptId = attemptId;
    mark.sessionLocalId = s.sessionLocalId;
    mark.attemptNo = attemptNo;
    mark.start = clip::now();
    mark.fromPercent = fromPercent;
    mark.practice = practice;
    mark.startPos = startPos;
    double start = mark.start;
    s.attempts.started(std::move(mark));
    s.lastAttemptId = attemptId;
    // the telemetry session of this level session (begun by the tracker before its first attempt)
    s.sessionGen = client::sessionOpen() ? client::sessionGeneration() : 0;
    sessionStatus();
    if (s.config.enabled) clip::Capture::get().setOpenAttemptStart(start);
}

void onAttemptEnd(std::string const& attemptId, bool completed, double percent, bool legit, double t, int64_t tick) {
    if (!s.attempts.ended(attemptId, clip::now(), percent, completed, legit, t, tick)) return;
    s.lastEndedAttemptId = attemptId;
    // Released LAST (every return path): a preserved attempt is pinned first, so the writer thread's
    // housekeeping can never drop the start of a run longer than the rolling window in between.
    struct ReleaseOpenAttempt {
        ~ReleaseOpenAttempt() { clip::Capture::get().setOpenAttemptStart(std::nullopt); }
    } release;
    // the session is still open here (the tracker ends it after the last attempt): remember its
    // server session id / levelCounts for a clip made later
    auto cs = sessionStatus();
    if (!s.config.enabled || s_exiting.load()) return;   // game exit: nothing is cut any more
    auto const* mark = s.attempts.find(attemptId);
    if (!mark) return;
    auto cap = clip::Capture::get().status();
    clip::PreserveFacts f;
    f.clippingEnabled = true;
    f.recording = cap.everRecorded && cap.recordedSeconds > 0.0;
    f.serverHintsSupported = s.serverHintsSupported;
    f.serverHinted = s.hinted.count(attemptId) > 0;
    f.completed = completed;
    f.practice = mark->practice;
    f.fromStart = mark->fromPercent <= 0.0 && !mark->startPos;
    f.legit = legit;
    if (cs && cs->mode == client::SessionMode::Remote) f.serverLevelCounts = cs->levelCounts;
    f.localRatedDemon = s.level.localRatedDemon;
    auto decision = clip::decidePreserve(f);
    if (decision.preserve) preserveAttempt(attemptId, decision.rule);
    else if (completed) log::info("GPRL clip: completion of attempt {} not preserved: {}", attemptId, decision.why);
    // v0.9.0: the server asked for this run while it was still running: send it now that it ended
    if (s.verificationAttempts.count(attemptId)) adoptForVerification(attemptId);
    // v0.10.0 (owner decision 2026-10-01): a run longer than the mod can upload - say so once per
    // level session, so the player knows a verification of it goes through a YouTube link
    if (mark->end > mark->start && (completed || percent >= 50.0)) {
        double cap = uploadCapSeconds();
        double ran = mark->end - mark->start;
        if (cap > 0.0 && ran > cap && s.longRunWarned.insert(s.sessionLocalId).second) {
            hud::verificationToast(fmt::format("Runs over {} (at {}) are too long for GPRL to upload. If a run needs verification it is saved instead: upload it to YouTube and send the link (GPRL menu > Account).",
                                               clip::formatDuration(cap * 1000.0), clip::spec(s.config.quality).label), 12.f);
        }
    }
    else GPRL_DEBUG("GPRL clip: attempt {} not preserved: {}", attemptId, decision.why);
    // The server decides (it sent preserveEvidence before): send the batch with this completion's
    // attempt_end now, so a hint in its ack arrives while the end screen is still up.
    if (completed && s.serverHintsSupported && !decision.preserve) client::requestFlush();
}

void onLevelExit() {
    s.attempts.closeOpen(clip::now());
    s.levelOpen = false;
    clip::Capture::get().setOpenAttemptStart(std::nullopt);
    clip::Capture::get().setWanted(false);
    clip::MicCapture::get().setStoring(false);   // nothing of the microphone is kept outside a level
    clip::MicCapture::desktop().setStoring(false);
}

/// v0.9.0: the clip of `attemptId` (preserved by any rule) becomes a verification clip: rule
/// "verification" (sent whatever the level is) and the Send choice, applied now when it is Ready
/// or as soon as it is prepared. False when no live record exists for the attempt.
bool adoptForVerification(std::string const& attemptId) {
    bool found = false;
    for (auto& r : s.records) {
        if (r.attemptId != attemptId) continue;
        if (r.state == clip::ClipState::Failed || r.state == clip::ClipState::Discarded) continue;
        found = true;
        if (r.rule != "verification") {
            r.rule = "verification";
            persist();
        }
        if (r.state == clip::ClipState::Ready && !r.choiceMade) {
            s.offered.insert(r.clipId);   // no popup for a clip the server asked for
            if (clip::overUploadCap(r, gate())) {
                handleVerificationTooLong(r, "this run");
                break;
            }
            applyChoice(r.clipId, clip::Choice::Send);
        }
        else if (r.state == clip::ClipState::Preparing && !r.choiceMade) {
            s.offered.insert(r.clipId);
            auto actions = clip::onChoice(r, clip::Choice::Send, gate());
            run(actions, r);
            log::info("GPRL clip: {} will be sent for verification as soon as it is ready", r.clipId);
        }
        else if (r.state == clip::ClipState::UploadFailed) retryUpload(r.clipId);
        break;
    }
    return found;
}

void onLinkFinished(std::string const& clipId, std::string const& url, api::EvidenceLinkResult const& res) {
    s.linkSending = false;
    s.linkClipId = clipId;
    if (res.ok) {
        s.linkMessage.clear();
        if (auto* r = find(clipId)) {
            r->linkUrl = res.canonicalUrl.empty() ? url : res.canonicalUrl;
            r->linkSent = true;
            persist();
        }
        log::info("GPRL clip: {} - YouTube link sent ({}{})", clipId, res.canonicalUrl.empty() ? url : res.canonicalUrl,
                  res.title.empty() ? "" : ", \"" + res.title + "\"");
        notify("GPRL: YouTube link sent to the GPRL moderators", NotificationIcon::Success, 4.f);
        return;
    }
    std::string why;
    if (res.status == 400) why = clip::linkProblemText(res.detailUrl, res.message);
    else if (res.status == 401) why = "not connected - press Connect in the GPRL menu first";
    else if (res.status == 0) why = res.error.empty() ? std::string("the GPRL server could not be reached") : res.error;
    else if (res.status == 404 || res.status == 405 || res.status == 501) why = "this GPRL server does not accept links yet";
    else why = res.message.empty() ? (res.error.empty() ? "the link was not accepted" : res.error) : res.message;
    s.linkMessage = "Not sent: " + why;
    log::warn("GPRL clip: {} - YouTube link not accepted (HTTP {} {}): {}", clipId, res.status, res.code, why);
    notify("GPRL: the link was not sent - " + why, NotificationIcon::Warning, 7.f);
}

void sendLink(std::string const& clipId, std::string const& url) {
    auto* r = find(clipId);
    if (!r) return;
    if (s.linkSending) {
        notify("GPRL: a link is already being sent", NotificationIcon::Info);
        return;
    }
    if (clip::youtubeVideoId(url).empty()) {
        s.linkClipId = clipId;
        s.linkMessage = "Not sent: that is not a link to a YouTube video";
        notify("GPRL: that is not a link to a YouTube video", NotificationIcon::Warning, 5.f);
        return;
    }
    auto auth = client::uploadAuth();
    if (auth.localOnly || !auth.connected) {
        s.linkClipId = clipId;
        s.linkMessage = auth.localOnly ? "Not sent: local-only mode is on (or the API is not set)" : "Not sent: not connected - press Connect in the GPRL menu first";
        notify("GPRL: " + s.linkMessage, NotificationIcon::Warning, 5.f);
        return;
    }
    s.linkSending = true;
    s.linkClipId = clipId;
    s.linkMessage.clear();
    log::info("GPRL clip: {} - sending a YouTube link as its evidence", clipId);
    std::thread([record = *r, auth, url] {
        api::Config cfg{auth.apiBaseUrl, clip::kFlow.sessionTimeoutSeconds};
        auto res = api::postEvidenceLink(cfg, auth.deviceToken, record, url, auth.clientBuild);
        Loader::get()->queueInMainThread([clipId = record.clipId, url, res] { onLinkFinished(clipId, url, res); });
    }).detach();
}

void onVerificationRequested(std::vector<clip::VerificationRequest> requests) {
    if (!s.serverHintsSupported) log::info("GPRL clip: the server reviews runs in this session (the local completion rule is off)");
    s.serverHintsSupported = true;
    for (auto& q : requests) {
        if (!s.verificationCases.insert(q.caseId).second) continue;   // each case once
        std::string what = q.reason.empty() ? std::string("this run") : q.reason;
        log::info("GPRL clip: the server asks to verify attempt {} ({}; case {}{})", q.attemptId, what, q.caseId,
                  q.requestId.empty() ? "" : ", request " + q.requestId);
        if (!s.config.enabled) {
            hud::verificationToast("Verify your run (" + what + "): Clipping is OFF - turn it on in the GPRL mod settings", 12.f);
            continue;
        }
        auto const* mark = s.attempts.find(q.attemptId);
        if (!mark) {
            hud::verificationToast("Verify your run (" + what + "): its footage is no longer in the clip buffer", 10.f);
            continue;
        }
        s.hinted.insert(q.attemptId);
        s.verificationAttempts.insert(q.attemptId);
        if (mark->end == 0.0) {
            hud::verificationToast("Verify this run (" + what + "): GPRL sends the clip when the attempt ends", 8.f);
            continue;   // decided when it ends (onAttemptEnd)
        }
        preserveAttempt(q.attemptId, "verification");   // false when a clip of it exists already: adopted below
        if (adoptForVerification(q.attemptId))
            hud::verificationToast("Verify this run (" + what + "): GPRL is sending the clip to the moderators", 8.f);
        else
            hud::verificationToast("Verify your run (" + what + "): the clip could not be made - see the Geode log", 10.f);
    }
}

void onServerPreserveHint(std::vector<std::string> attemptIds, std::string reason) {
    if (!s.serverHintsSupported) log::info("GPRL clip: the server decides which runs need evidence in this session (the local completion rule is off)");
    s.serverHintsSupported = true;
    for (auto& id : attemptIds) {
        auto const* mark = s.attempts.find(id);
        if (!mark) {
            log::info("GPRL clip: the server asked for evidence of attempt {}, which is no longer tracked (left the buffer)", id);
            continue;
        }
        s.hinted.insert(id);
        if (mark->end == 0.0) continue;   // still running: decided when it ends (onAttemptEnd)
        if (!preserveAttempt(id, "server_hint")) continue;
        if (safeToPrompt()) offerPendingClip(true);   // end screen / paused / in a menu
        else
            notify(reason.empty() ? std::string("GPRL kept a clip of this run - pause to choose what happens to it")
                                  : "GPRL kept a clip of this run (" + reason + ") - pause to choose what happens to it",
                   NotificationIcon::Info, 5.f);
    }
}

// ---------------------------------------------------------------------------
// frame hooks
// ---------------------------------------------------------------------------

void onSwap() { clip::Capture::get().onFrame(); }

void onAudioTick(FMOD::System* system) { clip::AudioTap::get().tick(system); }

void offerPendingClip(bool automatic) {
    if (clip::ClipPopup::isOpen()) return;
    auto pending = pendingClip();
    // The Account tab's "Clip choice" also reaches a clip whose upload failed: the popup then
    // offers Send (= retry), Save, Save + send and Do nothing for it. Never opened on its own.
    if (!pending && !automatic) pending = failedUploadClip();
    if (!pending) return;
    if (automatic && !s.offered.insert(pending->clipId).second) return;   // already offered once
    s.offered.insert(pending->clipId);
    clip::ClipPopup::open(pending->clipId);
}

// ---------------------------------------------------------------------------
// clips
// ---------------------------------------------------------------------------

bool preserveAttempt(std::string const& attemptId, char const* rule) {
    if (!s.config.enabled) return false;
    auto const* mark = s.attempts.find(attemptId);
    if (!mark) {
        log::info("GPRL clip: attempt {} cannot be preserved: it is no longer tracked", attemptId);
        return false;
    }
    if (mark->sessionLocalId != s.sessionLocalId) {
        // s.level / the server session describe the open (or last) level only: a clip of an older
        // level session would be stamped with another level's id, hash and session
        log::info("GPRL clip: attempt {} cannot be preserved: it belongs to an earlier level session", attemptId);
        return false;
    }
    for (auto const& r : s.records) {
        if (r.attemptId == attemptId && r.state != clip::ClipState::Failed && r.state != clip::ClipState::Discarded) return false;   // already preserved
    }
    double t = clip::now();
    auto range = clip::preserveRange(*mark, t);
    sessionStatus();   // refreshes s.serverSessionId / s.serverLevelCounts while the session is open

    clip::ClipRecord r;
    r.clipId = clip::makeClipId(attemptId, unixMs(), ++s.clipCounter);
    r.attemptId = attemptId;
    r.sessionLocalId = mark->sessionLocalId;
    r.serverSessionId = s.serverSessionId;
    r.levelId = s.level.levelId;
    r.levelName = s.level.levelName;
    r.levelHash = s.level.levelHash;
    r.attemptNo = mark->attemptNo;
    r.percent = mark->percent;
    r.completed = mark->completed;
    r.levelCounts = s.serverLevelCounts ? *s.serverLevelCounts : s.level.localRatedDemon;
    r.rule = rule;
    r.state = clip::ClipState::Preparing;
    r.createdAtMs = unixMs();
    r.endT = mark->endT;
    r.endTick = mark->endTick;

    PrepareJob job;
    job.clipId = r.clipId;
    job.attemptId = attemptId;
    job.title = clip::safeText(fmt::format("GPRL {} {} percent attempt {}", r.levelName, r.completed ? 100 : static_cast<int>(r.percent), r.attemptNo));
    job.from = range.from;
    job.to = range.to;
    job.ffmpeg = s.paths.ffmpeg;
    job.bufferRoot = s.paths.bufferRoot;
    job.pendingDir = s.paths.pendingDir;

    clip::Capture::get().pin(r.clipId, range.from, range.to);
    log::info("GPRL clip: preserving attempt {} as {} (rule {}; level {} \"{}\", {}{}%, {:.1f} s of wall time; nothing is uploaded unless you choose Send)",
              attemptId, r.clipId, rule, r.levelId, r.levelName, r.completed ? "COMPLETE " : "", r.completed ? 100 : static_cast<int>(r.percent),
              range.to - range.from);
    s.records.push_back(std::move(r));
    persist();
    std::thread(prepareThread, std::move(job)).detach();
    return true;
}

bool preserveLastAttempt() {
    if (!s.config.enabled) {
        notify("GPRL: Clipping is off (turn it on in the mod settings)", NotificationIcon::Warning);
        return false;
    }
    // the newest attempt that has ENDED (the run you just had); the open one when none has ended yet.
    // Only attempts of the open (or last) level session: see preserveAttempt.
    std::string target = clippable(s.lastEndedAttemptId) ? s.lastEndedAttemptId : s.lastAttemptId;
    if (!clippable(target)) {
        notify("GPRL: no attempt to clip yet - play a level first", NotificationIcon::Warning);
        return false;
    }
    // nothing in the buffer (no ffmpeg, no encoder, the disk is full, the recording never started):
    // say why instead of making a clip that can only fail
    auto cap = clip::Capture::get().status();
    if (!cap.everRecorded || cap.recordedSeconds <= 0.0) {
        notify("GPRL: nothing has been recorded yet" + (cap.problem.empty() ? std::string() : " - " + cap.problem), NotificationIcon::Warning, 6.f);
        return false;
    }
    if (!preserveAttempt(target, "manual")) {
        notify("GPRL: that attempt already has a clip", NotificationIcon::Info);
        offerPendingClip(false);
        return false;
    }
    offerPendingClip(false);
    return true;
}

void applyChoice(std::string const& clipId, clip::Choice choice) {
    auto* r = find(clipId);
    if (!r) return;
    auto before = r->state;
    auto actions = clip::onChoice(*r, choice, gate());
    log::info("GPRL clip: {} - the player chose \"{}\" (state {} -> {}{})", clipId, clip::label(choice), clip::name(before), clip::name(r->state),
              r->error.empty() ? "" : "; " + r->error);
    auto state = r->state;
    bool saved = r->savedLocal;
    std::string error = r->error;
    run(actions, *r);
    if (actions.empty()) persist();
    if (state == clip::ClipState::Discarded) notify("GPRL: clip discarded", NotificationIcon::Info);
    else if (state == clip::ClipState::Saved) notify(error.empty() ? std::string("GPRL: clip saved on this computer") : "GPRL: clip " + error, NotificationIcon::Success, 4.f);
    else if (state == clip::ClipState::Uploading) notify(saved ? "GPRL: clip saved; sending it to the GPRL moderators..." : "GPRL: sending the clip to the GPRL moderators...");
    else if (state == clip::ClipState::Preparing) notify(std::string("GPRL: \"") + clip::label(choice) + "\" will be applied when the clip is ready");
    else if (!error.empty()) notify("GPRL: " + error, NotificationIcon::Warning, 5.f);
}

void retryUpload(std::string const& clipId) {
    auto* r = find(clipId);
    if (!r) return;
    auto actions = clip::onRetryUpload(*r, gate());
    log::info("GPRL clip: {} - retry upload requested (state {}{})", clipId, clip::name(r->state), r->error.empty() ? "" : "; " + r->error);
    std::string error = r->state == clip::ClipState::Uploading ? std::string() : r->error;
    run(actions, *r);
    if (!error.empty()) notify("GPRL: " + error, NotificationIcon::Warning, 5.f);
}

void openClipsFolder() {
    std::error_code ec;
    std::filesystem::create_directories(s.paths.clipsDir, ec);
    ShellExecuteW(nullptr, L"open", s.paths.clipsDir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

Status status() {
    Status st;
    st.config = s.config;
    st.capture = clip::Capture::get().status();
    st.bufferLine = bufferLine(s.config, st.capture, st.bufferProblem);
    st.clips.assign(s.records.rbegin(), s.records.rend());
    st.uploadClipId = s.uploadActive ? s.uploadClipId : std::string();
    st.uploadFraction = s_uploadFraction->load();
    st.gate = gate();
    st.levelOpen = s.levelOpen;
    st.canClipLastAttempt = s.config.enabled && (clippable(s.lastEndedAttemptId) || clippable(s.lastAttemptId));
    st.clipsDir = clip::utf8(s.paths.clipsDir);
    for (auto it = s.records.rbegin(); it != s.records.rend(); ++it) {
        if (it->needsLink && !it->linkSent && !it->path.empty()) st.linkNeeded.push_back(*it);
    }
    st.linkSending = s.linkSending;
    st.linkClipId = s.linkClipId;
    st.linkMessage = s.linkMessage;
    return st;
}

std::optional<clip::ClipRecord> pendingClip() {
    for (auto it = s.records.rbegin(); it != s.records.rend(); ++it) {
        if (it->state == clip::ClipState::Ready || (it->state == clip::ClipState::Preparing && !it->choiceMade)) return *it;
    }
    return std::nullopt;
}

std::optional<clip::ClipRecord> findClip(std::string const& clipId) {
    if (auto* r = find(clipId)) return *r;
    return std::nullopt;
}

}  // namespace gprl::clipper
