#pragma once
// Blocking HTTP calls to the GPRL /v1 API (shared/src/api/contracts.ts, ARCHITECTURE §4).
//
// Every function here BLOCKS and is only ever called from the telemetry worker thread
// (Telemetry.cpp), never from the game thread (SPEC §46). Transport is geode::utils::web
// (WebRequest::postSync / getSync, which run the request on Geode's async runtime and wait for it
// on the calling thread). JSON goes through core/json so the request bodies are exactly the
// canonical bytes the signature covers.
//
// Error bodies follow ApiErrorBody (common.ts): { error: { code, message, details? } }.
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../core/analyzer_identity.hpp"
#include "../core/calibration.hpp"
#include "../core/clip_flow.hpp"
#include "../core/display.hpp"
#include "../core/entitlements.hpp"
#include "../core/json.hpp"
#include "../core/live_recalc.hpp"
#include "../core/ranks.hpp"
#include "../core/telemetry.hpp"
#include "../core/vocab.hpp"

namespace gprl::api {

struct Config {
    std::string baseUrl;         // e.g. https://gprl-api.example.workers.dev (no trailing slash)
    int timeoutSeconds = 15;
};

/// Raw transport result. `code` is the ApiErrorBody code when the server sent one.
struct Response {
    bool ok = false;             // 2xx and the body parsed (or was empty)
    int status = 0;              // HTTP status, 0 = transport failure
    std::string error;           // human readable
    std::string code;            // ApiErrorBody.error.code when present
    std::string reason;          // ApiErrorBody.error.details.reason when present (e.g. "gd_identity_invalid")
    std::string detailUrl;       // ApiErrorBody.error.details.url when present (evidence links: youtube_private ...)
    std::string message;         // ApiErrorBody.error.message when present
    json::Value body;
    int retryAfterSeconds = -1;  // a non-2xx answer's Retry-After header as delta-seconds; -1 = none / not seconds
    bool retryable() const { return status == 0 || status == 408 || status == 429 || status >= 500; }
};

Response post(Config const& cfg, std::string const& path, std::string const& body, std::string const& bearer,
              std::string const& signatureHex = {});
Response get(Config const& cfg, std::string const& path, std::string const& bearer);

// ---- typed endpoints ----

/// POST /v1/client/connect (no bearer; the Argon token is the credential, ARCHITECTURE §4 v0.2.0)
///   { accountId, userId, username, argonToken, clientBuild, modList }
///   -> { deviceToken, playerId, username, displayName, identityVerified }
/// Reconnecting (same GD account) always returns a NEW device token for the same player.
struct ConnectRequest {
    int64_t accountId = 0;       // GJAccountManager::m_accountID
    int64_t userId = 0;          // GameManager::m_playerUserID
    std::string username;        // GJAccountManager::m_username (the server trusts Argon's copy)
    std::string argonToken;      // argon::startAuth() result
    std::vector<telemetry::EnvironmentMod> modList;
};
struct ConnectResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string code;            // ApiErrorBody code, e.g. "unauthorized"
    std::string reason;          // ApiErrorBody details.reason, e.g. "gd_identity_invalid" (V1_IDENTITY_ERROR_REASONS)
    std::string deviceToken;
    std::string playerId;
    std::string username;
    std::string displayName;
    bool identityVerified = false;
};
ConnectResult connect(Config const& cfg, ConnectRequest const& rq, std::string const& clientBuild);

/// POST /v1/client/web-login (Bearer deviceToken, body {}) -> { url, expiresAt, code }: a
/// one-time, short-lived sign-in for this player, as a link ("Open my profile": the caller
/// validates `url` against the site origin, core/identity profileUrlAllowed, before opening it)
/// and as the same code in its display form XXXXX-XXXXX-XXXXX-XXXXX ("Website code": the caller
/// validates it with core/identity formatWebLoginCode before showing it). Servers from before
/// v0.2.1 send no `loginCode` (empty here; the caller falls back to the url's fragment). Neither
/// value is ever logged.
struct WebLoginResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string code;            // ApiErrorBody code on failure (NOT the sign-in code)
    std::string url;
    std::string expiresAt;
    std::string loginCode;       // V1WebLoginResponse.code: the one-time sign-in code, display form
};
WebLoginResult webLogin(Config const& cfg, std::string const& deviceToken);

struct IntegrityReport {
    Integrity state = Integrity::Unknown;
    std::string gdHash = "unknown";
    std::string geodeHash = "unknown";
    std::string gprlHash = "unknown";
};

/// The optional `level*` hints of V1CreateSessionRequest (v0.5.1, MASTER C6 / docs/RATING.md
/// "Which levels count"): what GD shows the client (GJGameLevel::m_stars, m_demon,
/// m_demonDifficulty, m_levelName). The SERVER decides `levelCounts` from the GD servers; a hint
/// can never upgrade a level (docs/SECURITY.md §5). Absent members are not sent.
struct LevelHints {
    std::optional<int> stars;             // 0-10
    std::optional<bool> isDemon;
    std::string demonDifficulty;          // easy | medium | hard | insane | extreme, "" = not sent
    std::string name;                     // core/display levelNameHint, "" = not sent
    // level families (docs/LEVEL_FAMILY_DESIGN.md §1): GJGameLevel::m_originalLevel, GD level key 30,
    // the level this one was copied from. Sent as `originalLevelId` (a string of digits) when > 0; the
    // server stores it on the level version as a matching HINT only (it never decides a match).
    int64_t originalLevelId = 0;
};

/// V1LevelRating: what the server decided `levelCounts` from (shown in the Session tab).
struct LevelRating {
    int stars = 0;
    bool isDemon = false;
    std::string demonDifficulty;          // "" = null
    std::string source = "none";          // gd_api | manual | client_claim | none
};

struct SessionResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string sessionId;
    std::string sessionKeyBytes;   // decoded from the base64 the server sends; memory only
    std::string nonce;
    int64_t seq0 = 0;
    bool ratable = false;
    std::string ratableReason;
    // v0.5.1: "Which levels count". Servers from before it send neither: counts = true then.
    bool levelCounts = true;
    std::string levelCountsReason;
    LevelRating levelRating;
    // v0.6.0: V1CreateSessionResponse.telemetryRevision (absent = 1). The mod only sends event kinds
    // the server's validator knows (clip_available needs revision 2, core/telemetry.hpp).
    int telemetryRevision = 1;
};
/// POST /v1/client/sessions (Bearer deviceToken)
SessionResult createSession(Config const& cfg, std::string const& deviceToken, std::string const& levelId, std::string const& levelHash,
                            std::string const& clientBuild, std::vector<telemetry::EnvironmentMod> const& mods, IntegrityReport const& integrity,
                            LevelHints const& hints = {});

struct BatchResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string code;
    bool accepted = false;
    int64_t nextSeq = -1;
    int eventCount = 0;
    bool retryable = false;
    // v0.6.0: V1TelemetryBatchResponse.preserveEvidence (contracts.ts V1PreserveEvidenceHint).
    // `preserveKnown` = the member is present at all (even null / an empty list): the server
    // evaluates SPEC §27 itself, so the clipper's local rule is off for the session.
    bool preserveKnown = false;
    std::vector<std::string> preserveAttemptIds;
    std::string preserveReason;
    // v0.9.0: V1TelemetryBatchResponse.verificationRequests (clip_flow parseVerificationHint).
    bool verificationKnown = false;
    std::vector<clip::VerificationRequest> verificationRequests;
};
/// POST /v1/telemetry/batches with X-GPRL-Signature = hex(HMAC-SHA256(sessionKey, canonical body)).
BatchResult postBatch(Config const& cfg, std::string const& deviceToken, telemetry::Batch const& batch, std::string const& sessionKeyBytes,
                      std::string* signatureHexOut = nullptr);

struct EndResult {
    bool ok = false;
    int status = 0;
    std::string error;
    int64_t eventCount = 0;
    bool recalculationQueued = false;
    // v0.9.0: V1EndSessionResponse.verificationRequests (the last batch's run review may land here).
    bool verificationKnown = false;
    std::vector<clip::VerificationRequest> verificationRequests;
};
/// POST /v1/client/sessions/:id/end { reason, lastSeq, reportedAttempts? }. `reportedAttempts` =
/// the GD save's attempt count when the level was left (v0.5.1, MASTER §10; untrusted context,
/// omitted when unknown).
EndResult endSession(Config const& cfg, std::string const& deviceToken, std::string const& sessionId, SessionEndReason reason, int64_t lastSeq,
                     std::optional<int64_t> reportedAttempts = std::nullopt);

struct CalibrationResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string algorithmVersion;
    std::string updatedAt;
    CalibrationState overall;
    /// docs/RANKS.md "Visibility thresholds" (v0.4.3): what the server lets the mod show
    /// (displayState locked / visible / ranked, the sigma/s figures when not locked, rankHint).
    /// Absent fields parse as locked (core/calibration.hpp calibrationDisplayFromJson).
    CalibrationDisplay display;
};
/// GET /v1/me/calibration (Bearer deviceToken): the SPEC §10 screen; a sigma/s figure only when
/// the server's displayState says so.
CalibrationResult getCalibration(Config const& cfg, std::string const& deviceToken);

struct LiveRecalcResult {
    bool ok = false;
    int status = 0;
    std::string error;
    /// The GET /v1/me/calibration part of the answer, read by the same parser (ok/status/error
    /// mirror the outer ones).
    CalibrationResult calibration;
    /// recalculated / computedAt / nextAllowedAt / skipped / ratableSamples (core/live_recalc).
    live::LiveRecalcAnswer answer;
};
/// POST /v1/me/recalc { "reason": "live" } (Bearer deviceToken; docs/contracts/calibration.md
/// "Live updates"): the server recalculates the player's own rating now (at most once per 30 s per
/// player; inside that it answers the stored calibration with recalculated false). The body never
/// carries a rating.
LiveRecalcResult liveRecalc(Config const& cfg, std::string const& deviceToken);

struct ResetResult {
    bool ok = false;
    int status = 0;
    std::string error;
    int64_t sessions = 0;        // sessions the server removed
    int64_t timingSamples = 0;   // timing samples the server removed
};
/// POST /v1/me/reset { confirm: "RESET" } (Bearer deviceToken; docs/contracts/reset.md): the player's
/// own progress is removed on the server; identity and device token stay.
ResetResult resetData(Config const& cfg, std::string const& deviceToken);

// ---- public site API (v0.3.0 in-game menu; shared/src/api/site.ts) ----
// No Authorization header: these are the website's public reads. Worker thread only, like the
// rest of this file; the popup reads the cached results through client::siteData().

struct RanksResult {
    bool ok = false;
    int status = 0;
    std::string error;
    ranks::RankList list;
};
/// GET /api/ranks -> the rank ladder (core/ranks parseRankList accepts both response shapes).
RanksResult getRanks(Config const& cfg);

struct LeaderboardResult {
    bool ok = false;
    int status = 0;
    std::string error;
    ranks::Leaderboard board;
};
/// GET /api/leaderboard?limit=N (overall, lifetime).
LeaderboardResult getLeaderboard(Config const& cfg, int limit);

struct ProfileResult {
    bool ok = false;
    int status = 0;
    std::string error;
    bool notFound = false;   // 404: no GPRL player with that username (yet)
    ranks::Profile profile;
};
/// GET /api/players/<username> (path segment percent-encoded).
ProfileResult getPlayer(Config const& cfg, std::string const& username);

struct LevelAnalysisResult {
    bool ok = false;
    int status = 0;
    std::string error;
    bool notFound = false;   // 404: the server has never seen this level
    display::LevelCoverage coverage;
};
/// GET /v1/levels/<id>/analysis (public, no auth; `id` = the GD level id): the level-analysis
/// coverage block of MASTER §20 for the Session / Profile tabs (core/display parseLevelCoverage).
LevelAnalysisResult getLevelAnalysis(Config const& cfg, std::string const& levelId);

// ---- evidence upload (v0.6.0, stream M3 client of stream V2's endpoints; core/clip_flow) ----
// Called from the clipper's upload thread, never the game thread and never the telemetry worker
// (a 90 MB PUT must not hold up telemetry batches). Only ever reached through the player's own
// "Send" / "Save + send" choice.

struct EvidenceSessionResult {
    int status = 0;                 // HTTP status, 0 = transport failure
    std::string error;
    std::string code;               // ApiErrorBody code
    bool parsed = false;            // 2xx and the body is a usable upload session
    clip::UploadSession session;
};
/// POST /v1/me/evidence/uploads (Bearer deviceToken) with core/clip_flow uploadRequestJson.
EvidenceSessionResult createEvidenceUpload(Config const& cfg, std::string const& deviceToken, clip::ClipRecord const& record,
                                           std::string const& clientBuild);

/// v0.10.0: POST /v1/me/evidence/links (Bearer deviceToken) with core/clip_flow linkRequestJson.
struct EvidenceLinkResult {
    bool ok = false;                // 2xx, or 409 evidence_exists (the server already has this link)
    int status = 0;
    std::string code;
    std::string detailUrl;          // not_youtube | youtube_private | youtube_unavailable on a 400
    std::string message;            // the server's message
    std::string error;              // transport / other failure, human readable
    std::string canonicalUrl;       // the link as the server stored it
    std::string title;              // the video's title as YouTube reported it
};
EvidenceLinkResult postEvidenceLink(Config const& cfg, std::string const& deviceToken, clip::ClipRecord const& record, std::string const& url,
                                    std::string const& clientBuild);

struct EvidencePutResult {
    int status = 0;
    std::string error;
    std::string code;
    std::string sha256;             // the hash the server echoed ("" when it sent none)
};
/// PUT of the clip bytes to a core/clip_flow PutPlan. `bearer` is empty unless the plan says the
/// URL is on the API origin. `uploadedFraction` (0..1) is updated while the transfer runs.
EvidencePutResult putEvidence(clip::PutPlan const& plan, std::string const& bearer, std::string const& sha256Hex, std::vector<uint8_t> bytes,
                              int timeoutSeconds, std::atomic<double>* uploadedFraction);

// ---- v0.12.0 background level analyzer (docs/BACKGROUND_ANALYZER_DESIGN.md AN-D7,
// shared/src/api/contracts.ts V1LevelSimCacheResponse / V1LevelSimUploadRequest / Response).
// Called from the analyzer's worker thread only (src/analyzer/Worker.cpp), never the game thread.

struct LevelSimCacheResult {
    bool ok = false;               // 2xx with a parseable body
    int status = 0;
    std::string error;
    bool found = false;
    std::string resultStatus;      // "client" | "reviewed" (found only)
    double physicsPercent = 0.0;
    double solvedPercent = 0.0;
    bool hasVerified = false;
    double verifiedPercent = 0.0;
    int64_t windowsCount = 0;
    std::string computedAt;
};
/// GET /v1/levels/:gdId/sim-cache?gameplayHash=&analyzerVersion=&simVersion= (public, no auth).
LevelSimCacheResult levelSimCache(Config const& cfg, int64_t gdLevelId, std::string const& gameplayHash, std::string const& analyzerVersion,
                                  std::string const& simVersion);

struct LevelSimUploadResult {
    bool ok = false;               // 2xx
    int status = 0;                // HTTP status, 0 = transport failure
    std::string error;
    std::string code;              // ApiErrorBody code on failure
    bool stored = false;
    bool replaced = false;
    std::string reason;            // why not stored (the current result is better)
    std::string statusLabel;       // V1LevelSimUploadResponse.status.label ("Physics Verified", "Partial", ...)
    int64_t windowsStored = 0;
    bool retryable = false;        // transport failure / 408 / 5xx (429 is the worker's own clock)
    int retryAfterSeconds = -1;    // 429: the server's Retry-After (seconds), -1 = none
};
/// POST /v1/me/level-sim (Bearer deviceToken) { sessionId?, result }. `resultJson` must already be
/// the LevelSimResult JSON object text (core/sim/analysis toJson); `sessionId` "" = not sent. The
/// body is core/analyzer_worker_rules.hpp levelSimUploadBody (the worker's size check uses the same).
LevelSimUploadResult uploadLevelSim(Config const& cfg, std::string const& deviceToken, std::string const& resultJson, std::string const& sessionId);

// ---- level families (docs/LEVEL_FAMILY_DESIGN.md §7-§8, docs/contracts/level-family.md,
// shared/src/api/contracts.ts V1LevelIdentityRequest / V1LevelIdentityResponse). Called from the
// analyzer's worker thread only (src/analyzer/Worker.cpp), never the game thread.

struct LevelIdentityUploadResult {
    bool ok = false;                 // 2xx with a parseable V1LevelIdentityResponse
    int status = 0;                  // HTTP status, 0 = transport failure
    std::string error;
    std::string code;                // ApiErrorBody code on failure (bad_request, not_found, unavailable, ...)
    std::string reason;              // ApiErrorBody details.reason (too_large, invalid_identity, unknown_level_version,
                                     // unknown_session, session_level_mismatch)
    bool retryable = false;          // transport failure / 408 / 429 / 5xx
    int retryAfterSeconds = -1;      // 429: the server's Retry-After (seconds), -1 = none
    analyzer::family::FamilyAnswer answer;   // the parsed answer (ok only)
    std::string answerText;          // the answer as JSON text (kept on disk next to the identity)
};
/// POST /v1/me/level-identity (Bearer deviceToken) { sessionId?, identity }. `identityJson` must
/// already be identity::toJson's text; `sessionId` "" = not sent (core/analyzer_identity uploadBody).
LevelIdentityUploadResult uploadLevelIdentity(Config const& cfg, std::string const& deviceToken, std::string const& identityJson, std::string const& sessionId);

// ---- v0.12.2 Patreon plans (shared/src/entitlements/api.ts, docs/contracts/patreon.md) ----
// Telemetry worker only. The answer is server authoritative; the mod keeps it in memory only.
// No Patreon token, secret or e-mail is ever in these answers; the authorize URL and the
// patreonName are never logged.

struct EntitlementsResult {
    bool ok = false;               // 2xx with an object body
    int status = 0;
    std::string error;
    std::string code;
    entitlements::Entitlement entitlement;   // Free (valid false) unless ok
};
/// GET /v1/me/entitlements (Bearer deviceToken) -> V1EntitlementsResponse (core/entitlements parse).
EntitlementsResult fetchEntitlements(Config const& cfg, std::string const& deviceToken);

struct PatreonConnectResult {
    bool ok = false;               // 2xx with an authorizeUrl
    int status = 0;
    std::string error;             // transport / unusable answer, human readable
    std::string code;              // ApiErrorBody code (e.g. unavailable when Patreon is not configured)
    std::string message;           // ApiErrorBody message (public-safe, shown to the player)
    std::string authorizeUrl;      // never logged; the caller checks entitlements::authorizeUrlAllowed
    std::string expiresAt;
};
/// POST /v1/me/patreon/connect {"returnTo":"mod"} (Bearer deviceToken) -> V1PatreonConnectResponse.
PatreonConnectResult patreonConnect(Config const& cfg, std::string const& deviceToken);

struct PatreonSyncResult {
    bool ok = false;               // 2xx
    int status = 0;
    std::string error;
    std::string code;
    std::string message;           // ApiErrorBody message on failure
    int retryAfterSeconds = -1;    // 429 (the sync is rate limited)
    bool connected = false;        // V1PatreonStatusResponse.connected
    bool available = true;         // V1PatreonStatusResponse.available
    std::string plan;              // plan id ("pro"); the mod shows its own label for it
    std::string lastError;         // V1PatreonStatusResponse.lastError (public-safe)
};
/// POST /v1/me/patreon/sync (Bearer deviceToken, body {}) -> V1PatreonStatusResponse.
PatreonSyncResult patreonSync(Config const& cfg, std::string const& deviceToken);

/// Security review: the link confirmation (shared/src/entitlements/api.ts V1PatreonConfirmRequest).
/// 200 = a V1PatreonStatusResponse; failures are ApiErrorBody with the kind in `error.details.reason`
/// (PATREON_CONFIRM_ERRORS: link_mismatch / already_linked 409, ticket_expired 410, ticket_invalid
/// 404, too_many_attempts 429, invalid_body 400). The code is never logged (post() logs the path and
/// the body size only).
struct PatreonConfirmResult {
    bool ok = false;               // 2xx
    int status = 0;
    std::string error;             // transport / server error (not logged by the caller: a message could echo the code)
    std::string code;              // ApiErrorBody error.code (conflict, not_found, ...)
    std::string reason;            // ApiErrorBody error.details.reason (the kind)
    std::string playerMessage;     // failure: entitlements::confirmErrorTextFromBody of the answer
    bool connected = false;        // V1PatreonStatusResponse.connected
    std::string plan;              // plan id; the mod shows its own label for it
};
/// POST /v1/me/patreon/confirm {"code":"<CODE>"} (Bearer deviceToken). `code` must be the
/// entitlements::normalizePatreonCode form.
PatreonConfirmResult patreonConfirm(Config const& cfg, std::string const& deviceToken, std::string const& code);

/// Percent-encodes a URL path segment (unreserved characters kept).
std::string encodePathSegment(std::string const& segment);

/// Sign a canonical body with a session key (hex HMAC-SHA256), exposed for the spool records.
std::string signBody(std::string const& sessionKeyBytes, std::string const& canonicalBody);

}  // namespace gprl::api
