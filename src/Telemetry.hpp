#pragma once
// Telemetry client (ARCHITECTURE §4, SPEC §46): the ring buffer the game thread pushes events
// into, the background worker that batches, signs and sends them, and the session flow
//   connect (once per GD account)  ->  session per level  ->  batches (seq, nonce, HMAC signature)  ->  end
//
// Threads
//   game thread : push(), beginSession(), endSession(), requestConnect(), status() (mutex, cheap)
//   worker      : std::thread; wakes every 100 ms; the only caller of Api (blocking HTTP) and
//                 LocalStore. Never touches cocos / Geode UI; results reach the main thread through
//                 the Status struct and queueInMainThread (saved values, settings).
//
// Modes of a session (decided when it starts):
//   Remote  connected (token saved for the GD account logged in now, core/identity) + API
//           configured: POST /v1/client/sessions, batches signed with the session key
//   Unsent  API configured but not connected - no token, a v0.1.x link-code token, or a token of
//           another GD account - (or session creation failed): batches spooled as
//           "unsent" JSONL (LocalStore) - evidence is never dropped
//   Local   local-only setting or placeholder API URL: batches spooled as "local", nothing networks
//
// session_invalid (a batch answered with ApiErrorBody code `session_invalid`: the server ended
// the session, does not know it, or its key no longer verifies after a secret rotation):
//   the worker opens a REPLACEMENT session for the same level and re-sends the rejected batch from
//   the new seq0, preceded by the last environment event the server had accepted. The new session
//   knows none of the old attempts, so events of the attempt that was in flight (it started in the
//   old session) are spooled as "unsent" under the old session's identity and never re-labelled;
//   everything from the next attempt_start on goes to the new session (core/telemetry
//   splitForReopenedSession). The old session is not ended by the client. After 2 reopens per level
//   (kMaxSessionReopens) the batch is spooled as "failed" and the rest of the level as "unsent"
//   (spool + stop). A 401 on a batch disconnects the device and also spools the rest of the level.
//
// Live updates (owner decision 2026-10-01, core/live_recalc.hpp; setting `live-recalc`): while a
//   Remote session on a counting level has sent timing_window / timing_result events since the last
//   one, the worker posts itself Command::Kind::LiveRecalc every 45 s of wall clock -> POST
//   /v1/me/recalc, whose answer is stored like GET /v1/me/calibration (HUD + Profile tab follow the
//   server mid-session; one INFO line "GPRL: live recalc -> ..."). Errors: one WARN per session, 5 min
//   back-off. After a session end the calibration is fetched at once and again 8 s later.
//
// Game exit (shutdown(), called on Geode's GameEvent Exiting, main.cpp): no network on the exit
//   path. The worker drains the ring, spools every pending batch synchronously ("unsent", signed,
//   under the remote session's identity) and does NOT call /end: closing the abandoned remote
//   session is the server's job. The main thread waits at most 2.5 s (kExitWorkerWait) for a
//   request already in flight; after that the worker is detached and its pending events are lost
//   (logged). No flush runs from a static destructor (Windows kills the worker before those run).
//
// Secrets: the device token is stored with Mod::setSavedValue ("device-token", with the GD account
// id it was issued for in "gd-account-id"); the Argon token is never stored by GPRL (Argon keeps
// its own cache) and only travels in the one /v1/client/connect request; the session key
// exists only in this worker's memory and is wiped when the session ends (ARCHITECTURE §3).
// STUB (SPEC §24): the saved device token is NOT encrypted yet - Geode's saved values are plain
// JSON in the mod's save dir. An AES-GCM / ChaCha20-Poly1305 wrapper with a per-install key is a
// later phase; documented in README.
#include <cstdint>
#include <string>
#include <vector>

#include "../core/calibration.hpp"
#include "../core/entitlements.hpp"
#include "../core/identity.hpp"
#include "../core/ranks.hpp"
#include "../core/telemetry.hpp"
#include "../core/vocab.hpp"
#include "Api.hpp"

namespace gprl::client {

enum class SessionMode : uint8_t { None, Local, Unsent, Remote };
constexpr char const* name(SessionMode m) {
    switch (m) {
        case SessionMode::None: return "none";
        case SessionMode::Local: return "local";
        case SessionMode::Unsent: return "unsent";
        case SessionMode::Remote: return "remote";
    }
    return "none";
}

struct SessionStart {
    std::string levelId;
    std::string levelName;
    std::string levelHash;
    std::vector<telemetry::EnvironmentMod> modList;
    api::IntegrityReport integrity;
    api::LevelHints hints;     // v0.5.1: what GD shows of the level (stars / demon / difficulty / name), never trusted
    int64_t gdAccountId = 0;   // filled by beginSession (main thread): the GD account logged in now
};

/// Snapshot of the client state for the HUD / popup. Copy returned under a mutex.
struct Status {
    bool enabled = true;
    identity::ConnectionState connection = identity::ConnectionState::NotConnected;
    bool connected = false;           // identity::tokenUsable(connection)
    std::string username;             // GPRL username (may be a placeholder when the GD name was taken)
    std::string displayName;          // the GD name, case preserved
    std::string playerId;
    bool identityVerified = false;    // server: Argon verified the GD account (false only on dev servers)
    int64_t tokenGdAccountId = 0;     // GD account the saved token was issued for (0 = none / v0.1.x token)
    int64_t currentGdAccountId = 0;   // GD account logged in right now (read on the main thread)
    bool connecting = false;          // Argon auth or POST /v1/client/connect in progress
    std::string connectMessage;       // progress while connecting, else the last outcome
    bool webLoginPending = false;     // POST /v1/client/web-login in progress
    bool resetPending = false;        // POST /v1/me/reset in progress (Account tab "Reset data")
    bool localOnly = false;           // effective (setting or placeholder API)
    bool apiPlaceholder = true;
    std::string apiBaseUrl;
    bool sessionOpen = false;
    SessionMode mode = SessionMode::None;
    std::string sessionId;
    bool ratable = false;
    std::string ratableReason;
    // v0.5.1 "Which levels count" (V1CreateSessionResponse, Remote sessions only; true otherwise):
    // false = the HUD / Session tab show "Not a rated demon: not counted" (core/display).
    bool levelCounts = true;
    std::string levelCountsReason;
    std::string levelRatingText;      // "rated demon: extreme, 10 stars (gd_api)" for the Session tab
    // v0.6.0: which additive revision of gprl.telemetry/1 the open session accepts - the server's
    // V1CreateSessionResponse.telemetryRevision for Remote sessions (absent = 1), the mod's own for
    // Local / Unsent sessions (spooled only). The clipper sends clip_available only at revision >= 2.
    int telemetryRevision = 1;
    // v0.6.0: the generation (sessionGeneration()) of the session the fields above describe, 0 =
    // none. The worker publishes a session a moment after the main thread began it and keeps the
    // previous one's state until then, so a caller that ties data to ONE level session (the
    // clipper: server session id, levelCounts, telemetryRevision) compares this first.
    uint32_t sessionGen = 0;
    // v0.12.0 background analyzer (review MEDIUM-5): SessionStart::levelHash of the session the
    // fields above describe ("" = none), written in the same publish as sessionId / sessionGen. The
    // analyzer maps (sessionLevelHash -> sessionId) from this pair only, never from the main
    // thread's current level (which may already be the next level while this is still the old one).
    std::string sessionLevelHash;
    int64_t batchesSent = 0;
    int64_t batchesSpooled = 0;
    int64_t batchesFailed = 0;
    int64_t eventsSent = 0;
    int64_t eventsPending = 0;
    int64_t droppedEvents = 0;        // ring buffer refusals (reported in environment events too)
    // v0.12.0 LIVE tag (owner request 2026-10-02): steady-clock ms of the last batch the server
    // ACCEPTED and of the last batch that failed / was spooled instead of sent (0 = none this run).
    // core/display liveNow decides the tag from these; telemetry::steadyNowMs is the same clock.
    int64_t lastBatchOkMs = 0;
    int64_t lastBatchFailMs = 0;
    std::string lastError;
    bool serverCalibration = false;   // GET /v1/me/calibration answered
    std::string serverCalibrationAt;
    std::string serverAlgorithmVersion;
    // v0.12.2 Patreon plan (GET /v1/me/entitlements, core/entitlements.hpp; docs/contracts/patreon.md):
    // the SERVER's answer for the saved device token's player, in memory only (never a setting or a
    // file). Free (valid false) until the first answer and again after Disconnect.
    entitlements::Entitlement entitlement;
    bool entitlementPending = false;   // a GET /v1/me/entitlements is queued / in flight
    std::string entitlementError;      // why the last fetch failed ("" = it answered)
    bool patreonConnectPending = false;   // POST /v1/me/patreon/connect in flight (Connect Patreon)
    bool patreonSyncPending = false;      // POST /v1/me/patreon/sync in flight (Sync Patreon)
    // security review: the link confirmation (Enter Patreon code -> POST /v1/me/patreon/confirm)
    bool patreonConfirmPending = false;
    uint32_t patreonConfirmGen = 0;       // bumps on every confirm outcome (the code popup shows only newer ones)
    bool patreonConfirmOk = false;        // the newest outcome
    std::string patreonConfirmText;       // "Patreon connected: GPRL Pro" / entitlements::confirmErrorText
};

/// Main thread, once at mod load.
void init();
/// Main thread, once at game exit (GameEvent Exiting): spool pending events, stop the worker,
/// never network (see "Game exit" above). Blocks at most kExitWorkerWait.
void shutdown();
/// Main thread: re-reads the settings the worker needs (called by settings::load()).
void applySettings();

/// Main thread. A new session; any open one is flushed and ended first (reason level_exit).
void beginSession(SessionStart start);
/// `reportedAttempts` (v0.5.1, MASTER §10): the GD save's attempt count when the level was left,
/// sent with POST /v1/client/sessions/:id/end as untrusted context; nullopt = unknown (not sent).
void endSession(SessionEndReason reason, std::optional<int64_t> reportedAttempts = std::nullopt);
bool sessionOpen();
/// The steady clock of Status::lastBatchOkMs / lastBatchFailMs (any thread).
int64_t steadyNowMs();
/// Main thread: the generation the newest beginSession() gave its session (Status::sessionGen).
uint32_t sessionGeneration();

/// Main thread. Assigns the event's `seq` (strictly increasing within the session) and hands it
/// to the worker. Never blocks; a full ring refuses the event and counts a drop.
void push(telemetry::Event event);
int64_t lastAssignedSeq();
int64_t droppedEvents();

void requestFlush();

/// Connect (src/Connect.cpp drives it). Main thread: marks the connection as in progress (Argon
/// auth runs first, on Geode's async runtime); any thread: progress text; main thread: failure
/// before the server was asked (Argon error, no GD login).
void setConnecting(std::string message);
void setConnectProgress(std::string message);
void connectFailed(std::string message);
/// Main thread. POST /v1/client/connect on the worker; the result arrives in Status and through
/// connect::onServerResult (main thread).
void requestConnect(api::ConnectRequest request);
/// Main thread. POST /v1/client/web-login on the worker; the result arrives through
/// connect::onWebLoginResult (main thread).
void requestWebLogin();
/// Main thread. Forget the device token locally (the server keeps the device until it is revoked
/// on the website or the token is replaced by a later Connect).
void disconnect();
/// Main thread. POST /v1/me/reset on the worker (docs/contracts/reset.md): the server removes the
/// player's own progress; the worker then clears the local spool and the cached calibration and
/// fetches the (empty) calibration again. The outcome arrives through connect::onResetResult.
/// An open level session is dropped locally (no /end: the server removed it) and what the level
/// still measures is discarded; the next level entry opens a fresh session.
void requestReset();
/// Any thread. Bumps on every successful Reset data: the HUD's recent-window list and the local
/// calibration tracker clear their state when they see it change (they are not reset from here).
uint32_t resetGeneration();

// ---- v0.12.2 Patreon plans (docs/contracts/patreon.md) ----
//
// GET /v1/me/entitlements runs on the worker after a successful Connect, when the worker starts
// with a saved token, every 10 minutes (kEntitlementsInterval), after Sync Patreon and after a
// successful Patreon code (the link only exists once the code is confirmed). The answer goes to
// Status::entitlement and analyzer::modes::setSpeedAllowance (the `analysis-cpu` cap). Disconnect
// forgets it (Free). Nothing is written to disk.
//
// Linking (security review): Connect Patreon opens Patreon's authorize page; after the player allows
// GPRL there, the GPRL page shows a one-time 8-character code; Enter Patreon code sends it with this
// device's token (POST /v1/me/patreon/confirm), so the link can only land on this GPRL account.

/// Main thread. Account tab "Connect Patreon": POST /v1/me/patreon/connect {"returnTo":"mod"} on the
/// worker; the authorize URL (checked by entitlements::authorizeUrlAllowed) is opened with
/// geode::utils::web::openLinkInBrowser on the main thread, the outcome is a notification.
void requestPatreonConnect();
/// Main thread. Account tab "Sync Patreon": POST /v1/me/patreon/sync, then GET /v1/me/entitlements.
void requestPatreonSync();
/// Main thread. "Enter Patreon code" popup: `code` must be entitlements::normalizePatreonCode's form
/// (else nothing is sent). POST /v1/me/patreon/confirm on the worker; the outcome lands in
/// Status::patreonConfirm* and as a notification, and a success fetches the plan again. The code is
/// held only for the request and never logged.
void requestPatreonConfirm(std::string code);

// ---- public site data for the in-game menu (v0.3.0): /api/ranks, /api/leaderboard, /api/players/<name> ----
//
// The popup asks for a kind on every 0.5 s tick of the tab that shows it (requestSiteData); the
// worker fetches at most once per TTL (ranks 10 min, board 60 s, profile 60 s, level coverage
// 60 s) and never more than once per kSiteMinInterval (10 s) per endpoint after a failure.
// Results are delivered through siteData() (mutex copy), like Status. No auth: these are the
// website's public reads. Coverage (v0.5.1): GET /v1/levels/<gd id>/analysis for the level being
// played (or the last one), the MASTER §20 block for the Session / Profile tabs.

enum class SiteKind : uint8_t { Ranks, Board, Profile, Coverage };

struct SiteFetchState {
    bool loaded = false;       // a successful answer is cached
    bool inFlight = false;     // the worker is fetching
    std::string error;         // last failure (kept until the next success)
    uint32_t generation = 0;   // bumps on every stored answer (the popup rebuilds lists only then)
    int64_t fetchedAtMs = 0;   // steady clock, 0 = never
    int64_t lastAttemptMs = 0;
};

struct SiteData {
    bool online = false;       // false in local-only mode / placeholder API: nothing is fetched
    SiteFetchState ranksState, boardState, profileState;
    ranks::RankList ranks;
    ranks::Leaderboard board;
    ranks::Profile profile;
    std::string profileUsername;   // the username `profile` / profileState belong to
    bool profileNotFound = false;  // 404 for profileUsername
    // v0.5.1: level-analysis coverage of one level (GET /v1/levels/<id>/analysis)
    SiteFetchState coverageState;
    display::LevelCoverage coverage;
    std::string coverageLevelId;   // the GD level id `coverage` / coverageState belong to
    bool coverageNotFound = false; // 404: the server has never seen the level
};

/// Main thread. Fetches `kind` on the worker when the cache is stale (see above); cheap when it
/// is not. Profile: `username` names the player (the connected GPRL username). Coverage: `username`
/// carries the GD level id.
void requestSiteData(SiteKind kind, std::string const& username = {});
/// Copy of the cached site data (any thread).
SiteData siteData();

/// MAIN THREAD ONLY (reads GJAccountManager for the connection state).
Status status();

/// What an evidence upload needs (v0.6.0, src/Clipper.cpp). MAIN THREAD ONLY. The token is only
/// filled while it is usable for the logged-in GD account and the mod may network; the caller
/// hands it to its upload thread and never logs or stores it.
struct UploadAuth {
    bool connected = false;
    bool localOnly = false;       // local-only setting, placeholder API or the mod disabled: never network
    std::string deviceToken;
    std::string apiBaseUrl;
    std::string clientBuild;
};
UploadAuth uploadAuth();
/// MAIN THREAD. The calibration the HUD shows: the server's authoritative state when it has been
/// fetched and the token belongs to the logged-in GD account, otherwise the local tracker's (which has no samples until the Phase 3 solver exists).
CalibrationState displayCalibration();
bool serverCalibrationKnown();
/// MAIN THREAD. What the server lets the HUD / Profile tab show of the rating (docs/RANKS.md
/// "Visibility thresholds"): the last GET /v1/me/calibration display fields while the token
/// belongs to the logged-in GD account, otherwise locked. Never unlocks anything locally.
CalibrationDisplay displayRating();

}  // namespace gprl::client
