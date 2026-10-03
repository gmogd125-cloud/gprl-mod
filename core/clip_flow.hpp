#pragma once
// Clip choice flow, upload contract, hashing and the clip index (stream M3, geode v0.6.0;
// MASTER §16, SPEC §28-§31, docs/CLIPPING.md). Pure C++20: no Geode / cocos / Windows includes
// (compiled by the mod AND by tests/clip_flow_tests.cpp). The constants live in FlowParams.
//
//   ClipRecord      one preserved attempt: where its file is, its SHA-256, what the player chose
//   flow            the state machine behind the four-choice popup (Save to computer / Send to GPRL
//                   moderators / Save + send / Do nothing). It returns ACTIONS for the mod to run;
//                   StartUpload is only ever returned for an explicit Send / Save + send choice
//                   (or a retry of one): nothing uploads on its own (SPEC §28, §42)
//   upload          the client side of stream V2's endpoints (POST /v1/me/evidence/uploads, then
//                   PUT): request body, response parsing, where the bearer token may go, retry rules
//   hashFile        streaming SHA-256 of the clip file (the evidence record of SPEC §31)
//   index           the clip records as JSON (<save dir>/clips.json), so a pending choice or a
//                   failed upload survives a restart
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"

namespace gprl::clip {

struct FlowParams {
    char const* version = "gprl-clip-flow/1";
    // ---- pending choices ----
    int maxPendingClips = 3;              // prepared clips waiting for a choice; the oldest is discarded beyond this
    float popupInputGuardSeconds = 1.0f;  // the four buttons ignore input this long after the popup opened
    float popupTickSeconds = 0.25f;       // how often the popup re-reads the clip's state
    int maxIndexRecords = 20;             // finished records kept for the Account tab
    // ---- upload (stream V2: POST /v1/me/evidence/uploads, then PUT) ----
    char const* uploadsPath = "/v1/me/evidence/uploads";
    char const* linksPath = "/v1/me/evidence/links";     // v0.10.0: YouTube link evidence (V1_EVIDENCE_LINKS_PATH)
    int64_t defaultMaxUploadBytes = 95ll * 1024 * 1024;   // until the server states its own maxBytes
    int uploadAttempts = 3;               // per Send / Retry click
    int backoffMs[3] = {2000, 6000, 15000};
    int sessionTimeoutSeconds = 30;
    int putTimeoutSeconds = 900;
    size_t maxHeaderCount = 16;           // headers the upload session may ask the PUT to carry
    size_t maxHeaderLength = 1024;
    // ---- the server's preserveEvidence hint (contracts.ts V1PreserveEvidenceHint) ----
    size_t maxPreserveAttempts = 8;       // V1_PRESERVE_EVIDENCE_MAX_ATTEMPTS
    size_t maxPreserveReasonLength = 160;
    size_t maxAttemptIdLength = 512;      // TELEMETRY_LIMITS.maxStringLength
    // ---- hashing ----
    size_t hashChunkBytes = 1024 * 1024;
    // ---- ids ----
    size_t clipIdMin = 8;
    size_t clipIdMax = 64;
};
inline constexpr FlowParams kFlow{};

// ---- the player's choice (SPEC §28) ----

/// Nothing deletes the preserved clip.
enum class Choice : uint8_t { Nothing, SaveLocal, Send, SaveAndSend };
char const* name(Choice c);          // "nothing" | "save" | "send" | "save_send"
char const* label(Choice c);         // the popup button text
bool parse(std::string_view s, Choice& out);
constexpr bool wantsSave(Choice c) { return c == Choice::SaveLocal || c == Choice::SaveAndSend; }
constexpr bool wantsSend(Choice c) { return c == Choice::Send || c == Choice::SaveAndSend; }

enum class ClipState : uint8_t {
    Preparing,      // the attempt's segments are being cut into one file and hashed
    Ready,          // waiting for the player's choice
    Saved,          // kept on this computer (final unless an upload follows)
    Uploading,
    Uploaded,
    UploadFailed,   // the file is still here; Retry or Discard
    Discarded,      // "Do nothing", or too many pending clips: the file is gone
    Failed,         // the clip could not be made
};
char const* name(ClipState s);
bool parse(std::string_view s, ClipState& out);
constexpr bool finished(ClipState s) {
    return s == ClipState::Saved || s == ClipState::Uploaded || s == ClipState::Discarded || s == ClipState::Failed;
}

struct ClipRecord {
    std::string clipId;
    std::string attemptId;          // telemetry attemptId
    std::string sessionLocalId;     // the tracker's level-session id
    std::string serverSessionId;    // telemetry session id when the attempt ran in a Remote session, else ""
    std::string levelId;
    std::string levelName;
    std::string levelHash;
    int attemptNo = 0;
    double percent = 0.0;
    bool completed = false;
    bool levelCounts = true;        // the level counted when the clip was made (server verdict, else GD's own data)
    std::string rule;               // why it was preserved: "server_hint" | "local_completion" | "manual"
    ClipState state = ClipState::Preparing;
    bool choiceMade = false;        // a choice was made (possibly while still Preparing)
    Choice choice = Choice::Nothing;
    std::string path;               // the clip file (pending or saved), "" once deleted
    std::string sha256;             // hex, of the file at `path` when it was made
    int64_t sizeBytes = 0;
    double durationMs = 0.0;
    int width = 0;
    int height = 0;
    bool hasGameAudio = false;
    bool hasMicAudio = false;
    bool hasDesktopAudio = false;   // v0.9.0: the computer's own sound (WASAPI loopback), its own track
    bool needsLink = false;         // v0.10.0: a verification clip over the upload cap: saved, waits for a YouTube link
    std::string linkUrl;            // v0.10.0: the YouTube link the player sent for it ("" = none yet)
    bool linkSent = false;          // v0.10.0: the server accepted the link
    bool wholeAttempt = true;       // false: the buffer no longer held the start of the attempt
    bool savedLocal = false;        // the file is in the clips folder by the player's choice
    bool eventSent = false;         // telemetry clip_available was pushed
    bool uploaded = false;
    std::string uploadId;
    int uploadTries = 0;            // upload sessions requested so far
    std::string error;              // last failure, shown in the popup / Account tab
    int64_t createdAtMs = 0;        // unix ms
    double endT = 0.0;              // the attempt_end event's t / tick (clip_available carries them)
    int64_t endTick = 0;
};

// ---- flow ----

enum class Action : uint8_t {
    MoveToClipsFolder,    // pending file -> the clips folder (the player chose to keep it)
    EmitClipAvailable,    // telemetry clip_available {clipId, attemptId, durationMs, sha256}
    StartUpload,          // POST the upload session, PUT the bytes (explicit choice only)
    DeleteFile,           // remove the clip file
    Persist,              // write the index
};
char const* name(Action a);

/// What the upload needs to be allowed at all.
struct UploadGate {
    bool connected = false;       // a usable device token for the logged-in GD account
    bool localOnly = false;       // local-only mode / API not configured: never network
    int64_t maxBytes = kFlow.defaultMaxUploadBytes;
};
/// "" when the clip may be sent now; otherwise why not (shown next to the disabled Send buttons).
std::string uploadBlockReason(ClipRecord const& r, UploadGate const& gate);

/// The clip file is ready (`ok`) or could not be made. Applies a choice made while Preparing.
std::vector<Action> onPrepared(ClipRecord& r, bool ok, std::string const& error, UploadGate const& gate);
/// The player picked one of the four choices. Legal while Preparing (applied once prepared) and
/// Ready. On UploadFailed (the popup offers the four choices again): Send / SaveAndSend retry
/// (SaveAndSend also saves a clip that was only being sent), SaveLocal keeps the file and gives
/// up the upload, Nothing gives up the upload (a saved clip stays, an unsaved one is deleted).
/// Anything else: no-op.
std::vector<Action> onChoice(ClipRecord& r, Choice c, UploadGate const& gate);
/// The upload finished.
std::vector<Action> onUploadResult(ClipRecord& r, bool ok, std::string const& error);
/// "Retry upload" on an UploadFailed record (the earlier choice already said Send).
std::vector<Action> onRetryUpload(ClipRecord& r, UploadGate const& gate);
/// After a restart: an upload / preparation that was cut off, a file that is gone.
void reconcileAfterRestart(ClipRecord& r, bool fileExists);

/// True while the record still needs the player (the popup / Account tab offers it).
bool awaitingChoice(ClipRecord const& r);

// ---- upload contract (shared/src/api/contracts.ts V1EvidenceUpload*) ----

/// POST /v1/me/evidence/uploads body. Always `visibility: "private"`: what the mod sends is for
/// the assigned moderators only (SPEC §29); a public version is a separate website action.
json::Value uploadRequestJson(ClipRecord const& r, std::string const& clientBuild);

struct UploadSession {
    std::string uploadId;
    std::string uploadUrl;        // absolute https URL, or a path starting with '/' on the API
    std::string expiresAt;
    int64_t maxBytes = 0;         // 0 = not stated
    std::vector<std::pair<std::string, std::string>> headers;   // the PUT must carry these
};
bool parseUploadSession(json::Value const& body, UploadSession& out, std::string* err = nullptr);

struct PutPlan {
    bool ok = false;
    std::string url;
    bool sendBearer = false;      // only when the URL is on the API's own origin
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;
};
/// Where the bytes go and with which headers. The device token is sent ONLY to the API origin
/// (a presigned storage URL on another host never sees it), plain http is refused except for a
/// local API, and the session's headers cannot set Authorization / Cookie / Host / Content-Length.
PutPlan planPut(std::string const& apiBaseUrl, UploadSession const& session);
/// "https://host[:port]" of a URL, lowercased; "" when it is not http(s).
std::string originOf(std::string_view url);

enum class UploadVerdict : uint8_t { Done, Retry, Fatal };
struct UploadStep {
    UploadVerdict verdict = UploadVerdict::Fatal;
    std::string message;          // for the player (no server internals)
    bool reconnect = false;       // 401: the device token was rejected
    bool alreadyUploaded = false; // Done because the server already holds this clip (409 evidence_exists)
};
/// POST /v1/me/evidence/uploads answered `status` (0 = transport failure) with ApiErrorBody `code`.
UploadStep classifySessionResponse(int status, std::string const& code);
/// The PUT answered. `serverSha256` = the hash the server echoed ("" when it sent none).
UploadStep classifyPutResponse(int status, std::string const& code, std::string const& serverSha256, std::string const& ourSha256);
/// Wait before try `tryIndex` (1-based; the first try does not wait).
int backoffMs(int tryIndex);

// ---- the server's hint ----

/// `V1TelemetryBatchResponse.preserveEvidence` as the mod reads it. Untrusted input: at most
/// `maxPreserveAttempts` non-empty string ids are taken, the reason is reduced to printable ASCII
/// and capped. `known` = the member is PRESENT in the ack (whatever its value): the server
/// evaluates SPEC §27 itself and the mod's local rule is off for the session.
struct PreserveHint {
    bool known = false;
    std::vector<std::string> attemptIds;
    std::string reason;
};
PreserveHint parsePreserveHint(json::Value const& ackBody);

/// v0.9.0 (owner decision 2026-10-01, docs/SECURITY.md §10.6): `verificationRequests` of the batch
/// / session-end responses (contracts.ts V1VerificationRequestHint). The server judged a run of
/// this session too good for the attempts behind it: the mod preserves the attempt's clip and
/// SENDS it (the one upload that happens without a popup choice). Untrusted input: at most
/// `maxPreserveAttempts` entries, ids capped, the reason reduced to printable ASCII, one entry per
/// case id. `known` = the member is PRESENT (even `[]`): the server reviews runs itself and the
/// mod's local completion rule is off for the session.
struct VerificationRequest {
    std::string requestId;   // site verification_requests.id ("" = no moderator queue row yet)
    std::string caseId;      // the review case: stable while it is open (the mod handles each once)
    std::string attemptId;   // telemetry attemptId of the run (this session)
    std::string reason;      // public, factual ("completion on attempt 3"); may be empty
    int progressStart = 0;
    int progressEnd = 100;
};
struct VerificationHint {
    bool known = false;
    std::vector<VerificationRequest> requests;
};
VerificationHint parseVerificationHint(json::Value const& ackBody);

/// The clip's audio tracks in the mux order (core/clip muxArgs: Game, Microphone, Desktop):
/// `V1EvidenceUploadRequest.audioTracks`, one entry per track the clip has.
json::Value audioTracksJson(ClipRecord const& r);

// ---- v0.10.0: YouTube links for runs too long to upload (docs/CLIPPING.md §9b) ----

/// The prepared clip is over the server's upload cap: it can only be verified through a link.
bool overUploadCap(ClipRecord const& r, UploadGate const& gate);
/// How long a clip may be before it is over `maxBytes`, at a constant video bitrate with
/// `audioTracks` AAC tracks of `audioKbps` (core/clip kClip.audioKbps). Seconds.
double maxUploadSeconds(int videoKbps, int audioTracks, int audioKbps, int64_t maxBytes);
/// The 11-character YouTube video id of a pasted link (watch?v=, youtu.be/, shorts/, live/,
/// embed/, with or without https://), "" when it is not a link to one YouTube video. Local
/// pre-check only: the server parses and checks the link itself.
std::string youtubeVideoId(std::string_view url);
/// POST /v1/me/evidence/links body (contracts.ts V1EvidenceLinkRequest): always private.
json::Value linkRequestJson(ClipRecord const& r, std::string const& url, std::string const& clientBuild);
/// What a 400 of that route means for the player (`details.url`: not_youtube | youtube_private |
/// youtube_unavailable), in words; the server's message when the detail is unknown.
std::string linkProblemText(std::string const& detailUrl, std::string const& serverMessage);

// ---- hashing ----

/// Streaming SHA-256 of a file (hex) and its size. False when it cannot be read or `cancel` is set.
bool hashFile(std::filesystem::path const& path, std::string& hexOut, int64_t& sizeOut, std::atomic<bool> const* cancel = nullptr);

// ---- ids and names ----

/// "clip-<unix ms, hex>-<8 hex of SHA-256(attemptId | ms | counter)>": unique per preserved attempt,
/// no player name in it.
std::string makeClipId(std::string const& attemptId, int64_t unixMs, uint32_t counter);
/// [A-Za-z0-9_-]{8,64} (validate.ts clip_available.clipId).
bool validClipId(std::string_view id);
/// 64 lowercase hex characters.
bool validSha256Hex(std::string_view hex);
/// "GPRL <level name> 100pct 2026-09-30 01-02-03.mp4": Windows-safe, ASCII, at most 120 characters.
std::string clipFileName(std::string_view levelName, std::string_view levelId, double percent, bool completed, int year, int month, int day,
                         int hour, int minute, int second);

// ---- index ----

json::Value recordToJson(ClipRecord const& r);
bool recordFromJson(json::Value const& v, ClipRecord& out);
std::string indexToText(std::vector<ClipRecord> const& records);
std::vector<ClipRecord> indexFromText(std::string_view text);
/// Keeps every record that still needs the player, and the newest `FlowParams::maxIndexRecords`
/// finished ones. Returns the clips dropped because more than `maxPendingClips` wait for a choice
/// (oldest first; the caller deletes their files).
std::vector<ClipRecord> trimIndex(std::vector<ClipRecord>& records);

// ---- text ----

std::string formatBytes(int64_t bytes);            // "78.2 MB"
std::string formatDuration(double milliseconds);   // "1:58"
/// One Account-tab line for a record: "Clip: Bloodbath 100% 1:58 78.2 MB - uploading 43%".
std::string statusLine(ClipRecord const& r, double uploadFraction);

}  // namespace gprl::clip
