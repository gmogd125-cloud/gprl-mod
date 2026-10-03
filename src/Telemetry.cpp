#include "Telemetry.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "../core/display.hpp"
#include "../core/live_recalc.hpp"
#include "../core/ringbuffer.hpp"
#include "Clipper.hpp"
#include "Connect.hpp"
#include "LocalStore.hpp"
#include "Settings.hpp"
#include "analyzer/Modes.hpp"
#include "solver/GdOracle.hpp"
#include "Hud.hpp"

using namespace geode::prelude;

namespace gprl::client {

namespace {

using Clock = std::chrono::steady_clock;

/// "input 180, timing_window 96, ..." for the batch log lines (SOLVER_DESIGN §8).
std::string kindCounts(telemetry::Batch const& batch) {
    int counts[telemetry::kEventKindCount] = {};
    for (auto const& e : batch.events) ++counts[static_cast<int>(e.kind())];
    std::string out;
    for (int i = 0; i < telemetry::kEventKindCount; ++i) {
        if (!counts[i]) continue;
        if (!out.empty()) out += ", ";
        out += fmt::format("{} {}", telemetry::kindName(static_cast<telemetry::EventKind>(i)), counts[i]);
    }
    return out;
}

constexpr size_t kFlushEvents = 500;                       // batch when this many events wait
constexpr auto kFlushInterval = std::chrono::seconds(2);   // or when the oldest waited this long
constexpr auto kWorkerTick = std::chrono::milliseconds(100);
constexpr int kSendRetries = 3;
constexpr int kMaxSessionReopens = 2;                            // per level session, then spool + stop
constexpr auto kExitWorkerWait = std::chrono::milliseconds(2500);   // game exit: longest wait for the worker
constexpr char const* kSessionInvalidCode = "session_invalid";   // ApiErrorBody code (api/src/errors.ts)
constexpr char const* kSavedToken = "device-token";
constexpr char const* kSavedPlayerId = "player-id";
constexpr char const* kSavedUsername = "username";
constexpr char const* kSavedDisplayName = "display-name";
constexpr char const* kSavedGdAccount = "gd-account-id";         // GD account the token was issued for (v0.2.0+)
constexpr char const* kSavedIdentityVerified = "identity-verified";
constexpr char const* kRejectedToken = "the server rejected the device token (401); press Connect again";
// site data cache (in-game menu): per-endpoint TTL, and never more than one attempt per 10 s
constexpr auto kRanksTtl = std::chrono::minutes(10);
constexpr auto kBoardTtl = std::chrono::seconds(60);
constexpr auto kProfileTtl = std::chrono::seconds(60);
constexpr auto kCoverageTtl = std::chrono::seconds(60);   // v0.5.1: GET /v1/levels/:id/analysis (public, cached 60 s)
constexpr auto kSiteMinInterval = std::chrono::seconds(10);
constexpr int kBoardLimit = 25;
// Live updates (core/live_recalc.hpp): the session end's own recalculation runs on the server AFTER
// its answer (waitUntil), so the calibration is fetched right after the answer and once more a
// few seconds later
constexpr auto kEndRefreshDelay = std::chrono::seconds(8);
// v0.12.2 Patreon plans: GET /v1/me/entitlements every 10 minutes (a transport failure retries after
// 2). The link itself only exists after Enter Patreon code, which fetches the plan at once.
constexpr auto kEntitlementsInterval = std::chrono::minutes(10);
constexpr auto kEntitlementsRetry = std::chrono::minutes(2);

// ---- settings copy the worker may read (updated on the main thread under a mutex) ----
struct Config {
    bool enabled = true;
    std::string apiBaseUrl;
    bool localOnly = false;
    bool placeholder = true;
    std::string clientBuild;
    bool liveRecalc = true;   // setting `live-recalc` (core/live_recalc.hpp)
};
std::mutex s_cfgMx;
Config s_cfg;

Config configCopy() {
    std::lock_guard lock(s_cfgMx);
    return s_cfg;
}

// ---- ring: game thread -> worker ----
struct Item {
    uint32_t gen = 0;   // session generation the event belongs to
    telemetry::Event event;
};
SpscRing<Item, 4096> s_ring;
std::atomic<int64_t> s_seq{-1};      // last assigned event seq (main thread)
std::atomic<uint32_t> s_gen{0};      // current session generation (main thread bumps on begin)
std::atomic<bool> s_sessionOpenMain{false};
std::atomic<uint32_t> s_resetGen{0};   // bumps on every successful Reset data (client::resetGeneration)

// ---- commands: main thread -> worker ----
struct Command {
    // LiveRecalc: posted by the worker itself when core/live_recalc says one is due (never by the
    // main thread); handled in order with the session commands and re-checked when handled
    enum class Kind { Begin, End, Flush, Connect, WebLogin, Disconnect, Reset, Site, LiveRecalc, PatreonConnect, PatreonSync, PatreonConfirm, Shutdown } kind = Kind::Flush;
    uint32_t gen = 0;
    SessionStart start;
    SessionEndReason reason = SessionEndReason::LevelExit;
    std::optional<int64_t> reportedAttempts;   // Kind::End (v0.5.1): the GD save's attempt count, untrusted
    api::ConnectRequest connect;
    SiteKind site = SiteKind::Ranks;   // Kind::Site
    std::string siteUsername;          // Kind::Site: Profile = the username, Coverage = the GD level id
    std::string patreonCode;           // Kind::PatreonConfirm: the normalised one-time code (never logged)
};
std::mutex s_cmdMx;
std::condition_variable s_cv;
std::deque<Command> s_cmds;

void postCommand(Command c) {
    {
        std::lock_guard lock(s_cmdMx);
        s_cmds.push_back(std::move(c));
    }
    s_cv.notify_one();
}

// ---- shared status ----
std::mutex s_statusMx;
Status s_status;
std::string s_deviceToken;   // in memory; persisted through Mod::setSavedValue on the main thread
int64_t s_tokenGdAccount = 0;   // GD account the token was issued for (0 = none / v0.1.x link-code token)
std::optional<CalibrationState> s_serverCalibration;
CalibrationDisplay s_serverDisplay;   // with s_serverCalibration; default = locked

template <typename F>
void withStatus(F&& f) {
    std::lock_guard lock(s_statusMx);
    f(s_status);
}

std::string deviceTokenCopy() {
    std::lock_guard lock(s_statusMx);
    return s_deviceToken;
}

int64_t tokenGdAccountCopy() {
    std::lock_guard lock(s_statusMx);
    return s_tokenGdAccount;
}

/// MAIN THREAD: the GD account logged in right now (0 = logged out).
int64_t currentGdAccountMain() {
    auto* am = GJAccountManager::get();
    return am ? static_cast<int64_t>(am->m_accountID) : 0;
}

// ---- site data (public reads for the menu) ----
std::mutex s_siteMx;
SiteData s_site;   // under s_siteMx

int64_t steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

SiteFetchState& siteState(SiteData& d, SiteKind kind) {
    switch (kind) {
        case SiteKind::Ranks: return d.ranksState;
        case SiteKind::Board: return d.boardState;
        case SiteKind::Profile: return d.profileState;
        case SiteKind::Coverage: return d.coverageState;
    }
    return d.ranksState;
}

/// Session tab text for the server's level rating decision (core/display keeps it ASCII).
std::string levelRatingText(api::LevelRating const& r) {
    std::string text = r.isDemon ? "rated demon" : (r.stars > 0 ? "rated, not a demon" : "unrated");
    if (r.isDemon && !r.demonDifficulty.empty()) text += ": " + r.demonDifficulty;
    if (r.stars > 0) text += fmt::format(", {} star{}", r.stars, r.stars == 1 ? "" : "s");
    text += " (" + (r.source.empty() ? std::string("none") : r.source) + ")";
    return display::asciiDash(text);
}

// ---- v0.14.6: why a session does not feed the rating ----

/// Worker thread: the server answered `ratable: false` for the session it just opened. Says why,
/// top right, once per game launch and reason (owner report 2026-10-03: a friend with Mega Hack
/// played twenty sessions that were all stored unrated and saw only "0 % calibrating").
void unratedNotice(std::string const& reason) {
    Loader::get()->queueInMainThread([reason] {
        static std::set<std::string> shown;
        std::string text = display::unratedSessionNotice(reason);
        if (shown.insert(text).second) hud::notify(text, hud::ToastKind::Warning, 10.f);
    });
}

// ---- v0.12.2 Patreon: main-thread outcomes (queued by the worker) ----

void patreonNotify(std::string const& text, NotificationIcon icon, float seconds = 5.f) { hud::notify(text, hud::toastKindOf(icon), seconds); }   // v0.14.5: top right

/// Why a Connect Patreon / Sync Patreon request failed, in the player's words (the server's
/// ApiErrorBody message is public-safe by contract and wins when there is one).
std::string patreonErrorText(int status, std::string const& code, std::string const& message, std::string const& error) {
    if (code == "unavailable" || status == 503) return "Patreon memberships are not open yet";
    if (status == 429) return "used a moment ago - try again in a minute";
    if (status == 404 && message.empty()) return "this GPRL server has no Patreon support yet";
    if (!message.empty()) return display::asciiDash(message);
    return error.empty() ? std::string("no answer") : error;
}

/// MAIN THREAD: opens Patreon's authorize page (only a URL that passed authorizeUrlAllowed on the
/// worker; never logged) or says why not.
void onPatreonConnectResult(api::PatreonConnectResult const& res, bool urlAllowed) {
    if (res.ok && urlAllowed) {
        web::openLinkInBrowser(res.authorizeUrl);
        patreonNotify("Opened Patreon in your browser: allow GPRL there, then press Enter Patreon code", NotificationIcon::Success, 6.f);
        return;
    }
    if (res.ok) {
        patreonNotify("GPRL: refused to open a link that is not Patreon's sign-in page", NotificationIcon::Error);
        return;
    }
    patreonNotify("GPRL: Connect Patreon failed - " + patreonErrorText(res.status, res.code, res.message, res.error), NotificationIcon::Error);
}

/// MAIN THREAD: the outcome of Sync Patreon (the entitlement itself arrives through Status).
void onPatreonSyncResult(api::PatreonSyncResult const& res, entitlements::Entitlement const& after, bool entitlementOk) {
    if (!res.ok) {
        patreonNotify("GPRL: Sync Patreon failed - " + patreonErrorText(res.status, res.code, res.message, res.error), NotificationIcon::Error);
        return;
    }
    if (!res.available) {
        patreonNotify("GPRL: Patreon memberships are not open yet", NotificationIcon::Info);
        return;
    }
    if (!res.connected) {
        patreonNotify("GPRL: no Patreon account is connected yet - press Connect Patreon, then Enter Patreon code", NotificationIcon::Info);
        return;
    }
    std::string text = "GPRL: Patreon synchronized - " + (entitlementOk ? entitlements::planLine(after) : std::string("plan not fetched yet"));
    if (!res.lastError.empty()) text += " (" + display::asciiDash(res.lastError) + ")";
    patreonNotify(text, res.lastError.empty() ? NotificationIcon::Success : NotificationIcon::Warning);
}

/// MAIN THREAD: the outcome of Enter Patreon code (the popup shows the same text from Status).
void onPatreonConfirmResult(bool ok, std::string const& text) {
    patreonNotify("GPRL: " + text, ok ? NotificationIcon::Success : NotificationIcon::Error, ok ? 4.f : 6.f);
}

// ---- worker ----
std::thread s_worker;
std::atomic<bool> s_running{false};
std::atomic<bool> s_exiting{false};   // game exit in progress: no network, spool instead
std::mutex s_doneMx;
std::condition_variable s_doneCv;
bool s_workerDone = false;            // under s_doneMx

// Static-destruction safety net ONLY. If the process ends without GameEvent Exiting (a crash, an
// ExitProcess from elsewhere) Windows has already killed the worker thread, and a still-joinable
// std::thread would call std::terminate in its destructor. Declared after s_worker in the same TU,
// so it is destroyed first. It never joins and never networks: pending events are lost on that
// path, which is exactly why flushing happens on GameEvent Exiting instead (main.cpp).
struct WorkerReaper {
    ~WorkerReaper() {
        if (s_worker.joinable()) s_worker.detach();
    }
};
WorkerReaper s_workerReaper;

struct Session {
    bool open = false;
    uint32_t gen = 0;
    SessionMode mode = SessionMode::None;
    bool levelCounts = false;      // Remote only: the server's V1CreateSessionResponse.levelCounts (live updates gate)
    std::string id;
    std::string nonce;
    std::string keyBytes;          // Remote only; wiped at end
    int64_t nextSeq = 0;
    int64_t lastSentSeq = -1;
    std::vector<telemetry::Event> pending;
    Clock::time_point oldestPending{};
    SessionStart start;
    // ---- session_invalid handling (reopenAfterInvalid) ----
    std::optional<telemetry::Event> lastEnvironment;   // latest environment event the server accepted in this session
    int reopens = 0;                                   // replacement sessions opened for this level so far
    bool reopened = false;                             // this session replaced one the server invalidated
    std::set<std::string> startedAttempts;             // attempts whose attempt_start went to this replacement session
    std::string orphanSessionId;                       // identity the orphaned events are spooled under
    std::string orphanNonce;
    int64_t orphanNextSeq = 0;
};

enum class SendOutcome { Done, Requeued };   // Requeued: the batch went back into `pending` under a new session

class Worker {
public:
    void run() {
        std::vector<Item> held;   // events of a session generation whose Begin has not been processed yet
        while (true) {
            std::deque<Command> cmds;
            {
                std::unique_lock lock(s_cmdMx);
                s_cv.wait_for(lock, kWorkerTick, [] { return !s_cmds.empty(); });
                cmds.swap(s_cmds);
            }
            bool stop = false;
            for (auto& c : cmds) {
                drainRing(held);
                if (c.kind == Command::Kind::Shutdown) { stop = true; break; }
                handle(std::move(c), held);
            }
            drainRing(held);
            if (stop) break;
            maybeFlush(false);
            maybeEndRefresh();
            queueLiveRecalc();
            maybeRefreshEntitlements();
            publishCounters();
        }
        // Shutdown = game exit (GameEvent Exiting, s_exiting set): keep the evidence without
        // networking. flush() spools every pending batch synchronously (send() never networks
        // while exiting); the remote session is NOT ended here - the server closes abandoned
        // sessions (Telemetry.hpp "Game exit").
        if (m_session.open) {
            flush(true);
            if (m_session.mode == SessionMode::Remote)
                log::info("GPRL: game exit - session {} left open for the server to close (last accepted batch seq {})", m_session.id,
                          m_session.lastSentSeq);
            m_session = Session{};   // wipes the session key
        }
    }

private:
    Session m_session;
    uint32_t m_discardGen = 0;   // session generation dropped by a Reset data: its late events are discarded, never spooled
    int64_t m_batchesSent = 0, m_batchesSpooled = 0, m_batchesFailed = 0, m_eventsSent = 0;
    int64_t m_lastOkMs = 0, m_lastFailMs = 0;   // LIVE tag: last accepted / last failed batch (steady ms)
    live::LiveRecalcSchedule m_live;   // live updates: POST /v1/me/recalc every 45 s (core/live_recalc.hpp)
    int64_t m_endRefreshAtMs = 0;      // steady ms of the follow-up calibration fetch after a session end; 0 = none
    // v0.12.2 Patreon plans: steady ms of the next GET /v1/me/entitlements (0 = at the first tick
    // with a token: a saved token fetches its plan when the game starts)
    int64_t m_entNextMs = 0;
    std::string m_entLastLoggedError;  // one WARN per distinct failure

    void publishCounters() {
        int64_t pending = static_cast<int64_t>(m_session.pending.size());
        withStatus([&](Status& s) {
            s.batchesSent = m_batchesSent;
            s.batchesSpooled = m_batchesSpooled;
            s.batchesFailed = m_batchesFailed;
            s.lastBatchOkMs = m_lastOkMs;
            s.lastBatchFailMs = m_lastFailMs;
            s.eventsSent = m_eventsSent;
            s.eventsPending = pending;
            s.droppedEvents = static_cast<int64_t>(s_ring.dropped());
            s.sessionOpen = m_session.open;
            s.mode = m_session.mode;
            s.sessionId = m_session.id;
            s.sessionGen = m_session.open ? m_session.gen : 0;   // which beginSession() this describes
            // v0.12.0 analyzer (review MEDIUM-5): the level version THIS session belongs to, published
            // together with its id and generation so the analyzer never pairs a new level's hash
            // with the previous level's session (src/analyzer/Worker.cpp setCredentials)
            s.sessionLevelHash = m_session.open ? m_session.start.levelHash : std::string();
        });
    }

    void setError(std::string msg) {
        log::warn("GPRL: {}", msg);
        withStatus([&](Status& s) { s.lastError = std::move(msg); });
    }

    void drainRing(std::vector<Item>& held) {
        Item item;
        while (s_ring.tryPop(item)) {
            if (m_discardGen != 0 && item.gen == m_discardGen) continue;   // the level a Reset data cut short: measured, not kept
            if (m_session.open && item.gen == m_session.gen) accept(std::move(item.event));
            else if (item.gen > m_session.gen) held.push_back(std::move(item));
            else {
                // an event of an already ended session generation (should not happen: End drains first)
                GPRL_DEBUG("GPRL telemetry: dropping late event seq {} of generation {}", item.event.seq, item.gen);
            }
        }
        if (m_session.open && !held.empty()) {
            std::vector<Item> keep;
            for (auto& h : held) {
                if (m_discardGen != 0 && h.gen == m_discardGen) continue;
                if (h.gen == m_session.gen) accept(std::move(h.event));
                else keep.push_back(std::move(h));
            }
            held.swap(keep);
        }
    }

    void accept(telemetry::Event e) {
        if (m_session.pending.empty()) m_session.oldestPending = Clock::now();
        m_session.pending.push_back(std::move(e));
    }

    void handle(Command c, std::vector<Item>& held) {
        switch (c.kind) {
            case Command::Kind::Begin: begin(std::move(c.start), c.gen, held); break;
            case Command::Kind::End: end(c.reason, c.reportedAttempts); break;
            case Command::Kind::Flush: maybeFlush(true); break;
            case Command::Kind::Connect: connect(c.connect); break;
            case Command::Kind::WebLogin: webLogin(); break;
            case Command::Kind::Disconnect: doDisconnect("Disconnected"); break;
            case Command::Kind::Reset: resetData(); break;
            case Command::Kind::Site: fetchSite(c.site, c.siteUsername); break;
            case Command::Kind::LiveRecalc: liveRecalc(); break;
            case Command::Kind::PatreonConnect: patreonConnect(); break;
            case Command::Kind::PatreonSync: patreonSync(); break;
            case Command::Kind::PatreonConfirm: patreonConfirm(std::move(c.patreonCode)); break;
            case Command::Kind::Shutdown: break;
        }
    }

    // ---- session ----

    void begin(SessionStart start, uint32_t gen, std::vector<Item>& held) {
        if (m_session.open) end(SessionEndReason::LevelExit);
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        Session s;
        s.open = true;
        s.gen = gen;
        s.start = std::move(start);
        // Local / Unsent batches are only spooled: the mod's own revision. A Remote session takes
        // the server's (absent on older servers = 1: no clip_available, see Status).
        int telemetryRevision = telemetry::kTelemetryRevision;
        // the token only counts for the GD account it was issued for (core/identity)
        auto connection = identity::connectionState(!token.empty(), tokenGdAccountCopy(), s.start.gdAccountId);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (cfg.localOnly || cfg.placeholder || !cfg.enabled) {
            s.mode = SessionMode::Local;
            s.id = fmt::format("local-{}", now);
            s.nonce = "local";
        }
        else if (!identity::tokenUsable(connection)) {
            s.mode = SessionMode::Unsent;
            s.id = fmt::format("unsent-{}", now);
            s.nonce = "not-connected";
            if (!token.empty())
                log::info("GPRL: saved device token not used for this level ({}); spooling as unsent", identity::name(connection));
        }
        else if (s_exiting.load()) {
            // a level entered while the game is closing: no network on the exit path
            s.mode = SessionMode::Unsent;
            s.id = fmt::format("unsent-{}", now);
            s.nonce = "game-exit";
        }
        else {
            api::Config api{cfg.apiBaseUrl};
            auto res = api::createSession(api, token, s.start.levelId, s.start.levelHash, cfg.clientBuild, s.start.modList, s.start.integrity,
                                          s.start.hints);
            if (res.ok) {
                s.mode = SessionMode::Remote;
                s.levelCounts = res.levelCounts;
                s.id = res.sessionId;
                s.nonce = res.nonce;
                s.keyBytes = res.sessionKeyBytes;
                s.nextSeq = res.seq0;
                telemetryRevision = res.telemetryRevision;
                withStatus([&](Status& st) {
                    st.ratable = res.ratable;
                    st.ratableReason = res.ratableReason;
                    st.levelCounts = res.levelCounts;
                    st.levelCountsReason = res.levelCountsReason;
                    st.levelRatingText = levelRatingText(res.levelRating);
                    st.lastError.clear();
                });
                // "level counts" is the server's verdict (MASTER C6): the HUD shows "Not a rated
                // demon: not counted" from it, so the log names the decision and its source
                log::info("GPRL: session {} opened for level {} (telemetry revision {}; ratable {}{}; level counts {}{}; level rating {}; hints sent: stars {}, demon {}, {}, name {})",
                          s.id, s.start.levelId, res.telemetryRevision, res.ratable, res.ratableReason.empty() ? "" : ": " + res.ratableReason, res.levelCounts,
                          res.levelCountsReason.empty() ? "" : ": " + display::asciiDash(res.levelCountsReason), levelRatingText(res.levelRating),
                          s.start.hints.stars ? std::to_string(*s.start.hints.stars) : "-", s.start.hints.isDemon ? (*s.start.hints.isDemon ? "yes" : "no") : "-",
                          s.start.hints.demonDifficulty.empty() ? "-" : s.start.hints.demonDifficulty, s.start.hints.name.empty() ? "-" : "\"" + s.start.hints.name + "\"");
                if (!res.ratable) unratedNotice(res.ratableReason);
            }
            else {
                s.mode = SessionMode::Unsent;
                s.id = fmt::format("unsent-{}", now);
                s.nonce = "session-failed";
                setError("session could not be opened, spooling as unsent: " + res.error);
                if (res.status == 401) invalidateToken(kRejectedToken);
            }
        }
        m_session = std::move(s);
        m_live.sessionStarted(steadyMs());   // live updates: the first one comes one interval after the start
        withStatus([&](Status& st) { st.telemetryRevision = telemetryRevision; });
        publishCounters();
        // events pushed for this generation before the Begin command was processed
        drainRing(held);
        if (m_session.mode == SessionMode::Remote) fetchCalibration();
    }

    void end(SessionEndReason reason, std::optional<int64_t> reportedAttempts = std::nullopt) {
        if (!m_session.open) return;
        flush(true);
        m_live.sessionEnded();   // the session end recalculates on the server: no live one for this session any more
        bool ended = endRemote(reason, 15, reportedAttempts);
        log::info("GPRL: session {} ended ({}), {} batches sent, {} spooled, {} failed", m_session.id, name(reason), m_batchesSent,
                  m_batchesSpooled, m_batchesFailed);
        m_session = Session{};   // wipes the session key
        publishCounters();
        if (ended) {
            // Live updates: the server recalculates at the session end - fetch the calibration at
            // once (like before, the HUD / menu follow the server) and once more shortly after,
            // because that recalculation runs after the end answer (waitUntil)
            fetchCalibration();
            m_endRefreshAtMs = steadyMs() + std::chrono::duration_cast<std::chrono::milliseconds>(kEndRefreshDelay).count();
        }
    }

    /// True when the server acknowledged the end of a Remote session.
    bool endRemote(SessionEndReason reason, int timeoutSeconds, std::optional<int64_t> reportedAttempts = std::nullopt) {
        if (m_session.mode != SessionMode::Remote) return false;
        if (s_exiting.load()) return false;   // no network on the exit path; the server closes the session
        Config cfg = configCopy();
        api::Config api{cfg.apiBaseUrl, timeoutSeconds};
        auto res = api::endSession(api, deviceTokenCopy(), m_session.id, reason, m_session.lastSentSeq, reportedAttempts);
        if (!res.ok) {
            setError("session end failed: " + res.error);
            return false;
        }
        GPRL_DEBUG("GPRL: session end acknowledged ({} events stored, recalculation queued {}, reported GD attempts {})", res.eventCount,
                   res.recalculationQueued, reportedAttempts ? std::to_string(*reportedAttempts) : "not sent");
        if (res.verificationKnown && !res.verificationRequests.empty()) {
            log::info("GPRL: session end ack asks to verify {} run(s) of the session", res.verificationRequests.size());
            Loader::get()->queueInMainThread([requests = res.verificationRequests]() mutable {
                clipper::onVerificationRequested(std::move(requests));
            });
        }
        return true;
    }

    // ---- live updates (owner decision 2026-10-01; core/live_recalc.hpp, docs/contracts/calibration.md "Live updates") ----

    live::LiveRecalcGate liveGate() const {
        Config cfg = configCopy();
        live::LiveRecalcGate g;
        g.enabled = cfg.liveRecalc && cfg.enabled && !cfg.localOnly && !cfg.placeholder && !cfg.apiBaseUrl.empty() && !s_exiting.load();
        g.sessionOpen = m_session.open;
        g.remote = m_session.mode == SessionMode::Remote;
        g.levelCounts = m_session.levelCounts;
        return g;
    }

    /// Run loop: posts Command::Kind::LiveRecalc when one is due (handled in order with the
    /// session commands already queued, and re-checked there).
    void queueLiveRecalc() {
        int64_t now = steadyMs();
        // cheap checks first: this runs on every 100 ms worker tick
        if (m_live.queued() || m_live.pendingWindows() <= 0 || now < m_live.nextAttemptMs()) return;
        if (!m_live.due(now, liveGate())) return;
        m_live.markQueued();
        postCommand(Command{Command::Kind::LiveRecalc});
    }

    /// POST /v1/me/recalc and store the answer exactly like fetchCalibration (server calibration,
    /// display, private figure): the HUD line and the Profile tab update mid-session.
    void liveRecalc() {
        if (!m_live.queued()) return;   // a session end / start handled before it cancelled it
        std::string token = deviceTokenCopy();
        auto gate = liveGate();
        // v0.8.4: every level feeds calibration, so the live update runs on any level
        if (!gate.enabled || !gate.sessionOpen || !gate.remote || token.empty()) {
            m_live.dropQueued();   // the session ended / the setting changed since it was queued
            return;
        }
        Config cfg = configCopy();
        int64_t windows = m_live.pendingWindows();
        auto res = api::liveRecalc(api::Config{cfg.apiBaseUrl}, token);
        if (!res.ok) {
            if (m_live.onError(steadyMs()))
                log::warn("GPRL: live recalc failed ({}); next try in {} min, the HUD keeps the last values", res.error,
                          live::effectiveBackoffMs(m_live.params()) / 60000);
            else GPRL_DEBUG("GPRL: live recalc failed again: {}", res.error);
            if (res.status == 401) invalidateToken(kRejectedToken);
            return;
        }
        storeCalibration(res.calibration);
        m_live.onAnswer(steadyMs(), res.answer.recalculated);
        log::info("GPRL: {}", live::liveRecalcLogLine(res.answer, res.calibration.overall, res.calibration.display));
        GPRL_DEBUG("GPRL: live recalc covered {} sent window event(s); server computedAt {}, next allowed {}", windows,
                   res.answer.computedAt.empty() ? "-" : res.answer.computedAt, res.answer.nextAllowedAt);
    }

    /// Run loop: the follow-up calibration fetch a few seconds after a session end.
    void maybeEndRefresh() {
        if (m_endRefreshAtMs == 0 || steadyMs() < m_endRefreshAtMs) return;
        m_endRefreshAtMs = 0;
        fetchCalibration();
    }

    // ---- batches ----

    void maybeFlush(bool force) {
        if (!m_session.open || m_session.pending.empty()) return;
        bool due = force || m_session.pending.size() >= kFlushEvents || Clock::now() - m_session.oldestPending >= kFlushInterval;
        if (due) flush(force);
    }

    void flush(bool everything) {
        while (m_session.open && !m_session.pending.empty()) {
            size_t take = std::min(m_session.pending.size(), telemetry::kMaxEventsPerBatch);
            std::vector<telemetry::Event> events(std::make_move_iterator(m_session.pending.begin()),
                                                 std::make_move_iterator(m_session.pending.begin() + take));
            m_session.pending.erase(m_session.pending.begin(), m_session.pending.begin() + take);
            if (m_session.reopened) {
                // a replacement session only takes events it can accept (core splitForReopenedSession)
                auto split = telemetry::splitForReopenedSession(std::move(events), m_session.startedAttempts);
                spoolOrphans(std::move(split.orphaned));
                events = std::move(split.send);
            }
            if (!events.empty()) {
                telemetry::Batch batch;
                batch.sessionId = m_session.id;
                batch.seq = m_session.nextSeq;
                batch.nonce = m_session.nonce;
                batch.clientBuild = configCopy().clientBuild;
                batch.events = std::move(events);
                // keep the body under the size limit by halving the batch until it fits
                std::string body = telemetry::canonicalBody(batch);
                while (body.size() > telemetry::kMaxBatchBytes && batch.events.size() > 1) {
                    size_t half = batch.events.size() / 2;
                    // put the second half back in front of the remaining pending events
                    m_session.pending.insert(m_session.pending.begin(), std::make_move_iterator(batch.events.begin() + half),
                                             std::make_move_iterator(batch.events.end()));
                    batch.events.resize(half);
                    body = telemetry::canonicalBody(batch);
                }
                if (!m_session.pending.empty()) m_session.oldestPending = Clock::now();
                if (send(batch, body) == SendOutcome::Done) ++m_session.nextSeq;
            }
            if (!everything && m_session.pending.size() < kFlushEvents) break;
        }
    }

    void spool(char const* kind, int status, std::string const& signature, telemetry::Batch const& batch) {
        localstore::appendBatch(kind, status, signature, telemetry::serializeBatch(batch, false));
        ++m_batchesSpooled;
        GPRL_DEBUG("GPRL telemetry: spooled batch seq {} ({} events: {}) as {}", batch.seq, batch.events.size(), kindCounts(batch), kind);
    }

    /// Events of an attempt that started in a session the server invalidated: kept on disk as
    /// "unsent" under that session's identity (never sent anywhere, never re-labelled).
    void spoolOrphans(std::vector<telemetry::Event> events) {
        if (events.empty()) return;
        telemetry::Batch b;
        b.sessionId = m_session.orphanSessionId;
        b.seq = m_session.orphanNextSeq++;
        b.nonce = m_session.orphanNonce;
        b.clientBuild = configCopy().clientBuild;
        b.events = std::move(events);
        spool("unsent", 0, "", b);
        GPRL_DEBUG("GPRL telemetry: {} events of attempts from invalidated session {} kept on disk", b.events.size(), b.sessionId);
    }

    /// No more network for this level session: later batches are spooled as "unsent" under the
    /// same identity (seq continues). The session key is wiped.
    void stopNetworking(std::string why) {
        setError(std::move(why));
        std::fill(m_session.keyBytes.begin(), m_session.keyBytes.end(), '\0');
        m_session.keyBytes.clear();
        m_session.mode = SessionMode::Unsent;
        publishCounters();
    }

    /// `session_invalid` on a batch: the server ended the session, does not know it, or can no
    /// longer verify its key (secret rotated). Opens a replacement session for the same level and
    /// puts the batch back in front of `pending` (preceded by the last environment event the server
    /// had accepted, which the new session needs), so it is re-sent from the new session's seq0.
    /// From then on flush() filters with splitForReopenedSession: events of attempts that started
    /// in the invalidated session are orphans (spooled, never re-labelled into the new session),
    /// events from the next attempt_start on go to the new session. The invalidated session is not
    /// ended by the client (already closed / unknown / unverifiable). After kMaxSessionReopens the
    /// batch is spooled as failed and networking stops for this level (spool + stop).
    SendOutcome reopenAfterInvalid(telemetry::Batch const& batch, std::string const& signature, int status) {
        if (m_session.reopens >= kMaxSessionReopens) {
            spool("failed", status, signature, batch);
            ++m_batchesFailed;
        m_lastFailMs = steadyMs();
            stopNetworking(fmt::format("session {} invalidated again after {} reopen(s); spooling the rest of this level", m_session.id,
                                       m_session.reopens));
            return SendOutcome::Done;
        }
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        SessionStart start = m_session.start;
        if (m_session.lastEnvironment) {
            // report the current integrity, not the level-entry one
            if (auto* env = std::get_if<telemetry::EnvironmentPayload>(&m_session.lastEnvironment->payload)) start.integrity.state = env->integrity;
        }
        api::SessionResult created;
        if (!token.empty()) {
            api::Config api{cfg.apiBaseUrl};
            created = api::createSession(api, token, start.levelId, start.levelHash, cfg.clientBuild, start.modList, start.integrity);
        }
        else created.error = "not connected";

        std::vector<telemetry::Event> requeue;
        bool batchStartsWithEnvironment = !batch.events.empty() && batch.events.front().kind() == telemetry::EventKind::Environment;
        if (m_session.lastEnvironment && !batchStartsWithEnvironment) requeue.push_back(*m_session.lastEnvironment);
        requeue.insert(requeue.end(), batch.events.begin(), batch.events.end());
        m_session.pending.insert(m_session.pending.begin(), std::make_move_iterator(requeue.begin()), std::make_move_iterator(requeue.end()));

        std::string oldId = m_session.id;
        std::string oldNonce = m_session.nonce;
        std::fill(m_session.keyBytes.begin(), m_session.keyBytes.end(), '\0');
        ++m_session.reopens;
        m_session.lastSentSeq = -1;
        if (created.ok) {
            m_session.id = created.sessionId;
            m_session.levelCounts = created.levelCounts;
            m_session.nonce = created.nonce;
            m_session.keyBytes = created.sessionKeyBytes;
            m_session.nextSeq = created.seq0;
            m_session.reopened = true;
            m_session.startedAttempts.clear();
            m_session.orphanSessionId = oldId;
            m_session.orphanNonce = oldNonce;
            m_session.orphanNextSeq = batch.seq;   // the rejected seq and later were never accepted under the old id
            withStatus([&](Status& st) {
                st.ratable = created.ratable;
                st.ratableReason = created.ratableReason;
                st.telemetryRevision = created.telemetryRevision;
            });
            if (!created.ratable) unratedNotice(created.ratableReason);
            setError(fmt::format("session {} invalidated by the server (HTTP {}); reopened as {}", oldId, status, m_session.id));
        }
        else {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            m_session.keyBytes.clear();
            m_session.mode = SessionMode::Unsent;
            m_session.id = fmt::format("unsent-{}", now);
            m_session.nonce = "session-invalid";
            m_session.nextSeq = 0;
            m_session.reopened = false;
            setError(fmt::format("session {} invalidated (HTTP {}) and no replacement could be opened ({}); spooling as unsent", oldId, status,
                                 created.error));
            if (created.status == 401) invalidateToken(kRejectedToken);
        }
        publishCounters();
        return SendOutcome::Requeued;
    }

    SendOutcome send(telemetry::Batch const& batch, std::string const& canonicalBody) {
        std::string err;
        if (!telemetry::validateBatch(batch, &err)) {
            // never send what the server would reject; keep it as evidence of the bug
            setError("batch failed local validation (" + err + "), spooled as failed");
            spool("failed", 0, "", batch);
            ++m_batchesFailed;
        m_lastFailMs = steadyMs();
            return SendOutcome::Done;
        }
        if (m_session.mode == SessionMode::Local) { spool("local", 0, "", batch); return SendOutcome::Done; }
        if (m_session.mode == SessionMode::Unsent) {
            spool("unsent", 0, "", batch);
            m_lastFailMs = steadyMs();   // kept on disk, not sent: not LIVE
            return SendOutcome::Done;
        }
        std::string signature = api::signBody(m_session.keyBytes, canonicalBody);
        if (s_exiting.load()) {
            // game exit: never network. Kept signed with the session key so the record stays verifiable.
            spool("unsent", 0, signature, batch);
            return SendOutcome::Done;
        }
        Config cfg = configCopy();
        api::Config api{cfg.apiBaseUrl};
        std::string token = deviceTokenCopy();
        int lastStatus = 0;
        for (int attempt = 1; attempt <= kSendRetries; ++attempt) {
            auto res = api::postBatch(api, token, batch, m_session.keyBytes, &signature);
            lastStatus = res.status;
            if (res.ok && res.accepted) {
                ++m_batchesSent;
                m_lastOkMs = steadyMs();
                m_eventsSent += static_cast<int64_t>(batch.events.size());
                m_session.lastSentSeq = batch.seq;
                int64_t windows = 0;   // live updates: timing evidence the server now holds
                for (auto const& e : batch.events) {
                    if (e.kind() == telemetry::EventKind::Environment) m_session.lastEnvironment = e;
                    if (e.kind() == telemetry::EventKind::TimingWindow || e.kind() == telemetry::EventKind::TimingResult) ++windows;
                }
                m_live.windowsSent(windows);
                if (res.nextSeq > batch.seq) m_session.nextSeq = res.nextSeq - 1;   // ++ happens in flush()
                GPRL_DEBUG("GPRL telemetry: batch seq {} sent ({} events: {})", batch.seq, res.eventCount, kindCounts(batch));
                withStatus([](Status& s) { s.lastError.clear(); });
                // v0.6.0 (MASTER §16): the server's preserveEvidence hint goes to the clipper on the
                // main thread. It only PRESERVES a clip for the player's choice; nothing uploads.
                if (res.preserveKnown) {
                    log::info("GPRL: batch seq {} ack carries preserveEvidence ({} attempt(s){})", batch.seq, res.preserveAttemptIds.size(),
                              res.preserveReason.empty() ? "" : ": " + res.preserveReason);
                    Loader::get()->queueInMainThread([ids = res.preserveAttemptIds, reason = res.preserveReason]() mutable {
                        clipper::onServerPreserveHint(std::move(ids), std::move(reason));
                    });
                }
                // v0.9.0 (owner decision 2026-10-01): the server reviews runs; a run it names is
                // preserved AND sent by the clipper (the "!" notification says so).
                if (res.verificationKnown) {
                    if (!res.verificationRequests.empty())
                        log::info("GPRL: batch seq {} ack asks to verify {} run(s) of this session", batch.seq, res.verificationRequests.size());
                    Loader::get()->queueInMainThread([requests = res.verificationRequests]() mutable {
                        clipper::onVerificationRequested(std::move(requests));
                    });
                }
                return SendOutcome::Done;
            }
            if (res.ok && !res.accepted) {
                setError(fmt::format("batch seq {} not accepted by the server", batch.seq));
                break;
            }
            if (res.code == kSessionInvalidCode) return reopenAfterInvalid(batch, signature, res.status);
            if (res.status == 401) {
                invalidateToken(kRejectedToken);
                spool("failed", res.status, signature, batch);
                ++m_batchesFailed;
        m_lastFailMs = steadyMs();
                stopNetworking("device token rejected (401); the rest of this level is spooled as unsent");
                return SendOutcome::Done;
            }
            if (!res.retryable || attempt == kSendRetries) {
                setError(res.error);
                break;
            }
            // transient: back off but stay responsive to shutdown
            int waitMs = attempt == 1 ? 1000 : 3000;
            for (int i = 0; i < waitMs / 50 && s_running.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!s_running.load()) break;
        }
        spool("failed", lastStatus, signature, batch);
        ++m_batchesFailed;
        m_lastFailMs = steadyMs();
        return SendOutcome::Done;
    }

    // ---- connect (GD account via Argon) / web login / disconnect ----

    void connect(api::ConnectRequest const& rq) {
        Config cfg = configCopy();
        api::ConnectResult res;
        if (cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty())
            res.error = "local-only mode is on or the API base URL is not set (placeholder or invalid); change it in the mod settings first";
        else if (s_exiting.load()) res.error = "the game is closing";
        else {
            api::Config api{cfg.apiBaseUrl, 30};   // the server also asks Argon: allow for it
            res = api::connect(api, rq, cfg.clientBuild);
        }
        if (!res.ok) {
            log::warn("GPRL: connect failed: {}", res.error);
            withStatus([&](Status& s) {
                s.connecting = false;
                s.connectMessage = "Connect failed: " + res.error;
            });
            Loader::get()->queueInMainThread([res, accountId = rq.accountId] { connect::onServerResult(res, accountId); });
            return;
        }
        {
            std::lock_guard lock(s_statusMx);
            s_deviceToken = res.deviceToken;
            s_tokenGdAccount = rq.accountId;
            s_serverCalibration.reset();   // may be a different player than before
            s_serverDisplay = CalibrationDisplay{};
            s_status.serverCalibration = false;
            s_status.username = res.username;
            s_status.displayName = res.displayName;
            s_status.playerId = res.playerId;
            s_status.identityVerified = res.identityVerified;
            s_status.tokenGdAccountId = rq.accountId;
            s_status.connecting = false;
            s_status.connectMessage = "Connected as " + res.displayName;
            s_status.lastError.clear();
        }
        Loader::get()->queueInMainThread([res, accountId = rq.accountId] {
            auto mod = Mod::get();
            mod->setSavedValue<std::string>(kSavedToken, res.deviceToken);
            mod->setSavedValue<int64_t>(kSavedGdAccount, accountId);
            mod->setSavedValue<std::string>(kSavedPlayerId, res.playerId);
            mod->setSavedValue<std::string>(kSavedUsername, res.username);
            mod->setSavedValue<std::string>(kSavedDisplayName, res.displayName);
            mod->setSavedValue<bool>(kSavedIdentityVerified, res.identityVerified);
            log::info("GPRL: connected as {} (GPRL user {}, GD account {}, verified {})", res.displayName, res.username, accountId,
                      res.identityVerified);
            connect::onServerResult(res, accountId);
        });
        fetchCalibration();
        // v0.12.2: the plan of the player this token belongs to (may differ from the one before)
        clearEntitlement();
        fetchEntitlements("connect");
    }

    void webLogin() {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        api::WebLoginResult res;
        if (cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty()) res.error = "local-only mode is on or the API base URL is not set";
        else if (token.empty()) res.error = "not connected";
        else if (s_exiting.load()) res.error = "the game is closing";
        else res = api::webLogin(api::Config{cfg.apiBaseUrl}, token);
        withStatus([](Status& s) { s.webLoginPending = false; });
        if (!res.ok) log::warn("GPRL: web login failed: {}", res.error);
        if (res.status == 401) invalidateToken(kRejectedToken);
        Loader::get()->queueInMainThread([res] { connect::onWebLoginResult(res); });
    }

    /// Account tab "Reset data" (docs/contracts/reset.md). The server removes every session of the
    /// player, an open one included: the worker does not /end it (the row is gone) and the next
    /// batch of this level, if any, is answered session_invalid and takes the replacement-session
    /// path like any server-side end. Local: the spool is cleared and the cached calibration
    /// forgotten, then fetched again (0 / 700). The saved device token stays.
    void resetData() {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        api::ResetResult res;
        if (cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty()) res.error = "local-only mode is on or the API base URL is not set";
        else if (token.empty()) res.error = "not connected";
        else if (s_exiting.load()) res.error = "the game is closing";
        else res = api::resetData(api::Config{cfg.apiBaseUrl, 30}, token);
        if (res.ok) {
            // The server removed this level's session with the rest: drop it locally WITHOUT /end
            // (the row is gone; an /end or another batch would only earn session_invalid and a
            // diagnostics flag). What this level still measures is discarded, not spooled; the
            // next level entry opens a fresh session (beginSession bumps the generation).
            if (m_session.open) {
                m_discardGen = m_session.gen;
                log::info("GPRL: reset - session {} dropped locally with {} pending events (the server removed it)", m_session.id, m_session.pending.size());
                m_session = Session{};   // wipes the session key
                m_live.sessionEnded();
            }
            localstore::clear();
            {
                std::lock_guard lock(s_statusMx);
                s_serverCalibration.reset();
                s_serverDisplay = CalibrationDisplay{};
                s_status.serverCalibration = false;
            }
            s_resetGen.fetch_add(1);
            publishCounters();
        }
        withStatus([&](Status& s) {
            s.resetPending = false;
            s.connectMessage = res.ok ? fmt::format("Data reset: {} sessions and {} timing samples removed - calibration starts over", res.sessions, res.timingSamples)
                                      : "Reset failed: " + res.error;
        });
        if (!res.ok) log::warn("GPRL: reset failed: {}", res.error);
        if (res.status == 401) invalidateToken(kRejectedToken);
        Loader::get()->queueInMainThread([res] { connect::onResetResult(res); });
        if (res.ok) fetchCalibration();
    }

    void doDisconnect(std::string message) {
        {
            std::lock_guard lock(s_statusMx);
            s_deviceToken.clear();
            s_tokenGdAccount = 0;
            s_status.username.clear();
            s_status.displayName.clear();
            s_status.playerId.clear();
            s_status.identityVerified = false;
            s_status.tokenGdAccountId = 0;
            s_status.connectMessage = std::move(message);
            s_serverCalibration.reset();
            s_serverDisplay = CalibrationDisplay{};
            s_status.serverCalibration = false;
        }
        clearEntitlement();   // v0.12.2: no token, no plan (Free; analysis speeds back to low / normal)
        Loader::get()->queueInMainThread([] {
            auto mod = Mod::get();
            mod->setSavedValue<std::string>(kSavedToken, "");
            mod->setSavedValue<int64_t>(kSavedGdAccount, 0);
            mod->setSavedValue<std::string>(kSavedPlayerId, "");
            mod->setSavedValue<std::string>(kSavedUsername, "");
            mod->setSavedValue<std::string>(kSavedDisplayName, "");
            mod->setSavedValue<bool>(kSavedIdentityVerified, false);
        });
    }

    void invalidateToken(std::string why) {
        setError(std::move(why));
        doDisconnect("Device token rejected by the server; press Connect again");
    }

    /// Public site reads for the menu (no token). Stores the answer (or the error) under s_siteMx.
    void fetchSite(SiteKind kind, std::string const& username) {
        Config cfg = configCopy();
        std::string error;
        bool offline = cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty();
        if (offline) error = cfg.localOnly ? "local-only mode is on: online data is not fetched" : "API base URL not set (placeholder or invalid)";
        else if (s_exiting.load()) error = "the game is closing";
        api::Config api{cfg.apiBaseUrl, 10};
        api::RanksResult ranks;
        api::LeaderboardResult board;
        api::ProfileResult profile;
        api::LevelAnalysisResult coverage;
        if (error.empty()) {
            switch (kind) {
                case SiteKind::Ranks:
                    ranks = api::getRanks(api);
                    if (!ranks.ok) error = ranks.error;
                    break;
                case SiteKind::Board:
                    board = api::getLeaderboard(api, kBoardLimit);
                    if (!board.ok) error = board.error;
                    break;
                case SiteKind::Profile:
                    profile = api::getPlayer(api, username);
                    if (!profile.ok) error = profile.error;
                    break;
                case SiteKind::Coverage:
                    coverage = api::getLevelAnalysis(api, username);
                    if (!coverage.ok) error = coverage.error;
                    else {
                        GPRL_DEBUG("GPRL site: level {} analysis: {} (status {}, observed {:.1f}%, analysis {})", username,
                                   display::coverageLine(coverage.coverage), coverage.coverage.status, coverage.coverage.observedPercent,
                                   coverage.coverage.hasAnalysis ? "present" : "none");
                    }
                    break;
            }
        }
        if (!error.empty()) GPRL_DEBUG("GPRL site: fetch {} failed: {}", static_cast<int>(kind), error);
        std::lock_guard lock(s_siteMx);
        s_site.online = !offline;
        auto& st = siteState(s_site, kind);
        st.inFlight = false;
        st.lastAttemptMs = steadyMs();
        ++st.generation;
        if (!error.empty()) {
            st.error = error;
            if (kind == SiteKind::Profile) {
                s_site.profileUsername = username;
                s_site.profileNotFound = profile.notFound;
                if (profile.notFound) {
                    s_site.profile = ranks::Profile{};
                    st.loaded = false;
                }
            }
            else if (kind == SiteKind::Coverage) {
                s_site.coverageLevelId = username;
                s_site.coverageNotFound = coverage.notFound;
                if (coverage.notFound) {
                    s_site.coverage = display::LevelCoverage{};
                    st.loaded = false;
                }
            }
            return;
        }
        st.error.clear();
        st.loaded = true;
        st.fetchedAtMs = st.lastAttemptMs;
        switch (kind) {
            case SiteKind::Ranks: s_site.ranks = std::move(ranks.list); break;
            case SiteKind::Board: s_site.board = std::move(board.board); break;
            case SiteKind::Profile:
                s_site.profile = std::move(profile.profile);
                s_site.profileUsername = username;
                s_site.profileNotFound = false;
                break;
            case SiteKind::Coverage:
                s_site.coverage = std::move(coverage.coverage);
                s_site.coverageLevelId = username;
                s_site.coverageNotFound = false;
                break;
        }
    }

    void fetchCalibration() {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        if (token.empty() || cfg.placeholder || s_exiting.load()) return;
        api::Config api{cfg.apiBaseUrl};
        auto res = api::getCalibration(api, token);
        if (!res.ok) {
            GPRL_DEBUG("GPRL: calibration fetch failed: {}", res.error);
            return;
        }
        storeCalibration(res);
    }

    // ---- v0.12.2 Patreon plans (docs/contracts/patreon.md) ----

    /// Local-only / placeholder API / no token / game exit: no plan requests.
    bool entitlementsOffline(Config const& cfg, std::string const& token) const {
        return token.empty() || cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty() || s_exiting.load();
    }

    /// Forget the plan (Disconnect, a new Connect): Free, and the analyzer back to low / normal.
    void clearEntitlement() {
        m_entNextMs = 0;
        m_entLastLoggedError.clear();
        withStatus([](Status& s) {
            s.entitlement = entitlements::Entitlement{};
            s.entitlementPending = false;
            s.entitlementError.clear();
        });
        analyzer::modes::setSpeedAllowance(sim::modes::SpeedAllowance::Normal);
    }

    /// Run loop: the 10-minute refresh (and the first fetch when the worker starts with a token).
    void maybeRefreshEntitlements() {
        int64_t now = steadyMs();
        if (now < m_entNextMs) return;   // the cheap check: this runs on every 100 ms tick
        Config cfg = configCopy();
        if (entitlementsOffline(cfg, deviceTokenCopy())) {
            // nothing to ask now; Connect fetches at once, otherwise look again after the interval
            m_entNextMs = now + std::chrono::duration_cast<std::chrono::milliseconds>(kEntitlementsRetry).count();
            return;
        }
        fetchEntitlements("refresh");
    }

    /// GET /v1/me/entitlements -> Status::entitlement + the analyzer's speed allowance. A failure
    /// keeps the last answer (a short outage must not drop a member to Free mid-session); it never
    /// disconnects the device.
    void fetchEntitlements(char const* why) {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        int64_t now = steadyMs();
        m_entNextMs = now + std::chrono::duration_cast<std::chrono::milliseconds>(kEntitlementsInterval).count();
        if (entitlementsOffline(cfg, token)) return;
        withStatus([](Status& s) { s.entitlementPending = true; });
        auto res = api::fetchEntitlements(api::Config{cfg.apiBaseUrl}, token);
        if (!res.ok) {
            std::string err = res.status == 404 ? std::string("this GPRL server has no plans yet") : (res.error.empty() ? std::string("unusable answer") : res.error);
            if (err != m_entLastLoggedError) {
                log::warn("GPRL: plan (GET /v1/me/entitlements, {}) failed: {}", why, err);
                m_entLastLoggedError = err;
            }
            else GPRL_DEBUG("GPRL: plan fetch failed again: {}", err);
            if (res.status == 0) m_entNextMs = now + std::chrono::duration_cast<std::chrono::milliseconds>(kEntitlementsRetry).count();
            withStatus([&](Status& s) {
                s.entitlementPending = false;
                s.entitlementError = err;
            });
            return;
        }
        m_entLastLoggedError.clear();
        auto const& e = res.entitlement;
        bool changed = false;
        withStatus([&](Status& s) {
            changed = !s.entitlement.valid || s.entitlement.plan != e.plan || s.entitlement.status != e.status || s.entitlement.trial != e.trial ||
                      s.entitlement.patreonConnected != e.patreonConnected;
            s.entitlement = e;
            s.entitlementPending = false;
            s.entitlementError.clear();
        });
        if (changed)
            log::info("GPRL: {} (status {}, Patreon {}, background analysis up to {}) [{}]", entitlements::planLine(e), entitlements::statusId(e.status),
                      e.patreonConnected ? "connected" : "not connected", sim::modes::name(e.backgroundAnalysis), why);
        else GPRL_DEBUG("GPRL: plan unchanged ({}) [{}]", entitlements::planId(e.plan), why);
        analyzer::modes::setSpeedAllowance(e.backgroundAnalysis);
    }

    /// Account tab "Connect Patreon": the authorize URL, checked here, opened on the main thread.
    void patreonConnect() {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        api::PatreonConnectResult res;
        if (cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty()) res.error = "local-only mode is on or the API base URL is not set";
        else if (token.empty()) res.error = "not connected to GPRL: press Connect first";
        else if (s_exiting.load()) res.error = "the game is closing";
        else res = api::patreonConnect(api::Config{cfg.apiBaseUrl}, token);
        bool urlAllowed = res.ok && entitlements::authorizeUrlAllowed(res.authorizeUrl);
        if (res.ok && !urlAllowed) log::warn("GPRL: refused to open a Patreon link that is not https://www.patreon.com/oauth2/authorize? ({} characters)", res.authorizeUrl.size());
        else if (!res.ok) log::warn("GPRL: Connect Patreon failed: {}", res.error);
        // the link only exists after Enter Patreon code (patreonConfirm fetches the plan then)
        else log::info("GPRL: Connect Patreon - opening Patreon's authorize page (valid until {})", res.expiresAt.empty() ? "-" : res.expiresAt);
        withStatus([](Status& s) { s.patreonConnectPending = false; });
        Loader::get()->queueInMainThread([res, urlAllowed] { onPatreonConnectResult(res, urlAllowed); });
    }

    /// Account tab "Sync Patreon": the server asks Patreon now, then the plan is fetched again.
    void patreonSync() {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        api::PatreonSyncResult res;
        if (cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty()) res.error = "local-only mode is on or the API base URL is not set";
        else if (token.empty()) res.error = "not connected to GPRL: press Connect first";
        else if (s_exiting.load()) res.error = "the game is closing";
        else res = api::patreonSync(api::Config{cfg.apiBaseUrl}, token);
        if (!res.ok) log::warn("GPRL: Sync Patreon failed: {}", res.error);
        else log::info("GPRL: Sync Patreon - connected {}, plan {}{}", res.connected, res.plan.empty() ? "-" : res.plan,
                       res.lastError.empty() ? "" : " (" + res.lastError + ")");
        // after a sync (and after a refused one: the plan may have changed anyway) the plan is read again
        if (!entitlementsOffline(cfg, token)) fetchEntitlements("after Sync Patreon");
        entitlements::Entitlement after;
        bool entitlementOk = false;
        withStatus([&](Status& s) {
            s.patreonSyncPending = false;
            after = s.entitlement;
            entitlementOk = s.entitlement.valid && s.entitlementError.empty();
        });
        Loader::get()->queueInMainThread([res, after, entitlementOk] { onPatreonSyncResult(res, after, entitlementOk); });
    }

    /// Enter Patreon code (security review): POST /v1/me/patreon/confirm with this device's token.
    /// The code is used for this one request and never logged; a success fetches the plan at once.
    void patreonConfirm(std::string code) {
        Config cfg = configCopy();
        std::string token = deviceTokenCopy();
        std::string text;
        bool ok = false;
        if (cfg.placeholder || cfg.localOnly || cfg.apiBaseUrl.empty()) text = "Patreon could not be connected: local-only mode is on or the API base URL is not set";
        else if (token.empty()) text = "Patreon could not be connected: press Connect (GPRL) first";
        else if (s_exiting.load()) text = "the game is closing";
        else if (entitlements::normalizePatreonCode(code) != code) text = entitlements::kPatreonCodeHint;   // never sent
        else {
            auto res = api::patreonConfirm(api::Config{cfg.apiBaseUrl}, token, code);
            ok = res.ok && res.connected;
            if (ok) {
                text = entitlements::confirmSuccessText(res.plan);
                log::info("GPRL: Patreon code confirmed - {}", text);
            }
            else {
                // the log names the status, error.code and details.reason only (machine words; a
                // server message could echo the code, so it is never logged)
                text = res.ok ? std::string("Patreon could not be connected, try again") : res.playerMessage;
                auto word = [](std::string const& s) {
                    bool plain = !s.empty() && s.size() <= 40 &&
                                 std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
                    return plain ? s : std::string("-");
                };
                log::warn("GPRL: Patreon code refused (HTTP {}, {} / {})", res.status, word(res.code), word(res.reason));
            }
        }
        std::fill(code.begin(), code.end(), '\0');
        code.clear();
        withStatus([&](Status& s) {
            s.patreonConfirmPending = false;
            s.patreonConfirmOk = ok;
            s.patreonConfirmText = text;
            ++s.patreonConfirmGen;
        });
        if (ok) fetchEntitlements("after the Patreon code");
        Loader::get()->queueInMainThread([ok, text] { onPatreonConfirmResult(ok, text); });
    }

    /// The server's calibration answer (GET /v1/me/calibration, or the same body from POST
    /// /v1/me/recalc) becomes what the HUD and the menu show: the calibration, the display state
    /// and the player's own private figure.
    void storeCalibration(api::CalibrationResult const& res) {
        std::lock_guard lock(s_statusMx);
        s_serverCalibration = res.overall;
        s_serverDisplay = res.display;
        s_status.serverCalibration = true;
        s_status.serverCalibrationAt = res.updatedAt;
        s_status.serverAlgorithmVersion = res.algorithmVersion;
    }
};

}  // namespace

void applySettings() {
    auto const& s = settings::get();
    {
        std::lock_guard lock(s_cfgMx);
        s_cfg.enabled = s.enabled;
        s_cfg.apiBaseUrl = s.apiBaseUrl;
        s_cfg.localOnly = s.localOnly;
        s_cfg.placeholder = settings::apiIsPlaceholder();
        s_cfg.clientBuild = settings::clientBuild();
        s_cfg.liveRecalc = s.liveRecalc;
    }
    withStatus([&](Status& st) {
        st.enabled = s.enabled;
        st.localOnly = settings::effectiveLocalOnly();
        st.apiPlaceholder = settings::apiIsPlaceholder();
        st.apiBaseUrl = s.apiBaseUrl;
    });
}

void init() {
    if (s_running.exchange(true)) return;
    auto mod = Mod::get();
    {
        std::lock_guard lock(s_statusMx);
        s_deviceToken = mod->getSavedValue<std::string>(kSavedToken, "");
        // absent for v0.1.x link-code tokens: 0 -> ConnectionState::LegacyToken, never used
        s_tokenGdAccount = mod->getSavedValue<int64_t>(kSavedGdAccount, 0);
        s_status.tokenGdAccountId = s_tokenGdAccount;
        s_status.playerId = mod->getSavedValue<std::string>(kSavedPlayerId, "");
        s_status.username = mod->getSavedValue<std::string>(kSavedUsername, "");
        s_status.displayName = mod->getSavedValue<std::string>(kSavedDisplayName, "");
        if (s_status.displayName.empty()) s_status.displayName = s_status.username;
        s_status.identityVerified = mod->getSavedValue<bool>(kSavedIdentityVerified, false);
    }
    applySettings();
    s_worker = std::thread([] {
        Worker w;
        w.run();
        {
            std::lock_guard lock(s_doneMx);
            s_workerDone = true;
        }
        s_doneCv.notify_all();
    });
}

void shutdown() {
    if (!s_running.load()) return;
    s_exiting.store(true);           // from here on the worker never networks (spools instead)
    s_sessionOpenMain.store(false);  // late onQuit / endSession calls become no-ops
    postCommand(Command{Command::Kind::Shutdown});
    s_running.store(false);
    s_cv.notify_all();
    // The worker drains the ring (the attempt_end the tracker just pushed included), spools what is
    // pending and returns. A blocking request already in flight cannot be interrupted (Api.hpp), so
    // the exit waits at most kExitWorkerWait for it; after that the worker is detached and whatever
    // it still holds is lost (logged).
    bool done = false;
    {
        std::unique_lock lock(s_doneMx);
        done = s_doneCv.wait_for(lock, kExitWorkerWait, [] { return s_workerDone; });
    }
    if (!s_worker.joinable()) return;
    if (done) s_worker.join();
    else {
        log::warn("GPRL: telemetry worker still inside a network request at game exit; its pending events are not spooled");
        s_worker.detach();
    }
}

void beginSession(SessionStart start) {
    start.gdAccountId = currentGdAccountMain();
    uint32_t gen = s_gen.fetch_add(1) + 1;
    s_seq.store(-1);
    s_sessionOpenMain.store(true);
    Command c{Command::Kind::Begin};
    c.gen = gen;
    c.start = std::move(start);
    postCommand(std::move(c));
}

void endSession(SessionEndReason reason, std::optional<int64_t> reportedAttempts) {
    if (!s_sessionOpenMain.exchange(false)) return;
    Command c{Command::Kind::End};
    c.reason = reason;
    c.reportedAttempts = reportedAttempts;
    postCommand(std::move(c));
}

bool sessionOpen() { return s_sessionOpenMain.load(); }

int64_t steadyNowMs() { return steadyMs(); }
uint32_t sessionGeneration() { return s_gen.load(); }

void push(telemetry::Event event) {
    if (!s_running.load()) return;
    event.seq = s_seq.fetch_add(1) + 1;
    Item item;
    item.gen = s_gen.load();
    item.event = std::move(event);
    if (!s_ring.tryPush(std::move(item))) {
        // the ring is full: the worker is stalled (network?) - the drop is counted and reported
        return;
    }
}

int64_t lastAssignedSeq() { return s_seq.load(); }
int64_t droppedEvents() { return static_cast<int64_t>(s_ring.dropped()); }

void requestFlush() { postCommand(Command{Command::Kind::Flush}); }

void setConnecting(std::string message) {
    withStatus([&](Status& s) {
        s.connecting = true;
        s.connectMessage = std::move(message);
    });
}

void setConnectProgress(std::string message) {
    withStatus([&](Status& s) {
        if (s.connecting) s.connectMessage = std::move(message);
    });
}

void connectFailed(std::string message) {
    withStatus([&](Status& s) {
        s.connecting = false;
        s.connectMessage = std::move(message);
    });
}

void requestConnect(api::ConnectRequest request) {
    Command c{Command::Kind::Connect};
    c.connect = std::move(request);
    withStatus([](Status& s) {
        s.connecting = true;
        s.connectMessage = "Connecting... (asking the GPRL server)";
    });
    postCommand(std::move(c));
}

void requestWebLogin() {
    withStatus([](Status& s) { s.webLoginPending = true; });
    postCommand(Command{Command::Kind::WebLogin});
}

void requestReset() {
    withStatus([](Status& s) { s.resetPending = true; });
    postCommand(Command{Command::Kind::Reset});
}

uint32_t resetGeneration() { return s_resetGen.load(); }

void requestPatreonConnect() {
    bool already = false;
    withStatus([&](Status& s) {
        already = s.patreonConnectPending;
        s.patreonConnectPending = true;
    });
    if (!already) postCommand(Command{Command::Kind::PatreonConnect});
}

void requestPatreonSync() {
    bool already = false;
    withStatus([&](Status& s) {
        already = s.patreonSyncPending;
        s.patreonSyncPending = true;
    });
    if (!already) postCommand(Command{Command::Kind::PatreonSync});
}

void requestPatreonConfirm(std::string code) {
    if (entitlements::normalizePatreonCode(code) != code) return;   // only a well-formed code is ever sent
    bool already = false;
    withStatus([&](Status& s) {
        already = s.patreonConfirmPending;
        s.patreonConfirmPending = true;
    });
    if (already) return;
    Command c{Command::Kind::PatreonConfirm};
    c.patreonCode = std::move(code);
    postCommand(std::move(c));
}

void disconnect() {
    postCommand(Command{Command::Kind::Disconnect});
    // the cached profile belongs to the player that just disconnected
    std::lock_guard lock(s_siteMx);
    s_site.profile = ranks::Profile{};
    s_site.profileUsername.clear();
    s_site.profileNotFound = false;
    s_site.profileState = SiteFetchState{};
}

void requestSiteData(SiteKind kind, std::string const& username) {
    if (!s_running.load() || s_exiting.load()) return;
    if ((kind == SiteKind::Profile || kind == SiteKind::Coverage) && username.empty()) return;
    int64_t now = steadyMs();
    {
        std::lock_guard lock(s_siteMx);
        auto& st = siteState(s_site, kind);
        if (st.inFlight) return;
        bool sameProfile = kind != SiteKind::Profile || s_site.profileUsername == username;
        if (!sameProfile) {
            // another player than the cached one: start over (the old profile is not shown)
            s_site.profile = ranks::Profile{};
            s_site.profileUsername = username;
            s_site.profileNotFound = false;
            st = SiteFetchState{};
        }
        if (kind == SiteKind::Coverage && s_site.coverageLevelId != username) {
            // another level than the cached one: start over (the old coverage is not shown)
            s_site.coverage = display::LevelCoverage{};
            s_site.coverageLevelId = username;
            s_site.coverageNotFound = false;
            st = SiteFetchState{};
        }
        auto ttl = kind == SiteKind::Ranks ? kRanksTtl : kind == SiteKind::Board ? kBoardTtl : kind == SiteKind::Coverage ? kCoverageTtl : kProfileTtl;
        auto ttlMs = std::chrono::duration_cast<std::chrono::milliseconds>(ttl).count();
        auto minMs = std::chrono::duration_cast<std::chrono::milliseconds>(kSiteMinInterval).count();
        if (st.loaded && now - st.fetchedAtMs < ttlMs) return;
        if (st.lastAttemptMs != 0 && now - st.lastAttemptMs < minMs) return;   // after an error (or anything): one try per 10 s
        st.inFlight = true;
        st.lastAttemptMs = now;
    }
    Command c{Command::Kind::Site};
    c.site = kind;
    c.siteUsername = username;
    postCommand(std::move(c));
}

SiteData siteData() {
    std::lock_guard lock(s_siteMx);
    return s_site;
}

Status status() {
    int64_t current = currentGdAccountMain();
    std::lock_guard lock(s_statusMx);
    Status st = s_status;
    st.currentGdAccountId = current;
    st.connection = identity::connectionState(!s_deviceToken.empty(), s_tokenGdAccount, current);
    st.connected = identity::tokenUsable(st.connection);
    return st;
}

UploadAuth uploadAuth() {
    int64_t current = currentGdAccountMain();
    Config cfg = configCopy();
    std::lock_guard lock(s_statusMx);
    UploadAuth a;
    a.connected = identity::tokenUsable(identity::connectionState(!s_deviceToken.empty(), s_tokenGdAccount, current));
    a.localOnly = cfg.localOnly || cfg.placeholder || !cfg.enabled || cfg.apiBaseUrl.empty();
    if (a.connected && !a.localOnly) a.deviceToken = s_deviceToken;
    a.apiBaseUrl = cfg.apiBaseUrl;
    a.clientBuild = cfg.clientBuild;
    return a;
}

CalibrationState displayCalibration() {
    int64_t current = currentGdAccountMain();
    std::lock_guard lock(s_statusMx);
    bool usable = identity::tokenUsable(identity::connectionState(!s_deviceToken.empty(), s_tokenGdAccount, current));
    if (s_serverCalibration && usable) return *s_serverCalibration;
    // Local picture (v0.4.0): the solver's own CalibrationTracker (main thread, solver/GdOracle)
    // once it has samples; before that the empty calibration: 0 %, 0/700, LOCKED. It never unlocks
    // anything by itself (core/calibration.hpp: no fitted L locally).
    if (solver::oracle::hasLocalSamples()) return solver::oracle::localCalibration();
    CalibrationState empty;
    empty.effectiveSamples = {0.0, CalibrationParams{}.overallEffectiveSamples};
    empty.gamemodes = {0.0, static_cast<double>(CalibrationParams{}.minGamemodes)};
    return empty;
}

bool serverCalibrationKnown() {
    std::lock_guard lock(s_statusMx);
    return s_serverCalibration.has_value();
}

CalibrationDisplay displayRating() {
    int64_t current = currentGdAccountMain();
    std::lock_guard lock(s_statusMx);
    bool usable = identity::tokenUsable(identity::connectionState(!s_deviceToken.empty(), s_tokenGdAccount, current));
    if (s_serverCalibration && usable) return s_serverDisplay;
    return CalibrationDisplay{};   // locked: no local figure ever (SPEC §10)
}

}  // namespace gprl::client
