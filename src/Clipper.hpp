#pragma once
// Clipper (v0.6.0, stream M3; MASTER §16, SPEC §28-§31, docs/CLIPPING.md): the rolling evidence
// buffer and what happens to a preserved attempt.
//
// What it does
//   - While the mod setting "Clipping" is ON (default OFF) and a level is open, the game picture
//     (and the game's own sound; the microphone only with its separate opt-in) is recorded into a
//     rolling, disk-backed buffer: the last `clip-buffer-seconds` (default 120) plus the whole open
//     attempt, bounded by `clip-disk-cap-mb` (src/clip/Capture, core/clip BufferBook).
//   - When a run is exceptional - the server's `preserveEvidence` hint in a telemetry batch
//     response, or, while the server sends none, the local rule "legit completion from 0 % of a
//     level that counts" (core/clip decidePreserve) - or when the player presses "Clip last
//     attempt", the attempt is PRESERVED: its segments are cut into one MP4 (stream copy, no
//     re-encode), hashed with SHA-256 and kept in <clip folder>/gprl-clips/pending.
//   - The player then chooses (src/clip/ClipPopup, shown on the end screen / the pause menu, and
//     always available in the GPRL menu > Account): Save to computer / Send to GPRL moderators /
//     Save + send / Do nothing. Closing the popup without choosing keeps the clip pending.
//   - Save: the file moves to <clip folder>/gprl-clips and a telemetry `clip_available` event
//     {clipId, attemptId, durationMs, sha256} is sent (never the media).
//   - Send: POST /v1/me/evidence/uploads, then PUT of the bytes (stream V2's endpoints), with
//     retries; the progress is shown in the Account tab.
//
// What it never does: upload anything without an explicit Send / Save + send choice (SPEC §28,
// §42); record outside a level; record the microphone without its own opt-in; touch files outside
// the clip folder / the mod save folder.
//
// Everything here runs on the main thread except the detached clip thread (cut + hash) and upload
// thread, whose results come back through queueInMainThread. The rules are pure and host-tested
// (core/clip, core/clip_flow; tests/clip_tests.cpp, tests/clip_flow_tests.cpp).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../core/clip.hpp"
#include "../core/clip_flow.hpp"
#include "clip/Capture.hpp"

namespace FMOD {
class System;
}

namespace gprl::clipper {

/// The clipping buffer is implemented (v0.6.0). Still off until the player enables it.
inline constexpr bool kImplemented = true;

/// Main thread, once at mod load: loads the clip index (pending choices, failed uploads).
void init();
/// Main thread: re-reads the clipping settings (called by settings::load()).
void applySettings();
/// Game exit (GameEvent Exiting): stop feeding the encoder, save the index. Never blocks.
void shutdown();

// ---- tracker notifications (main thread) ----

struct LevelInfo {
    std::string levelId;
    std::string levelName;
    std::string levelHash;
    bool localRatedDemon = false;   // GD's own data: a demon with stars (used while there is no server verdict)
};
void onLevelEnter(LevelInfo info, std::string const& sessionLocalId);
void onAttemptStart(std::string const& attemptId, int attemptNo, double fromPercent, bool practice, bool startPos);
/// `t` / `tick` = the attempt_end event's (what clip_available carries).
void onAttemptEnd(std::string const& attemptId, bool completed, double percent, bool legit, double t, int64_t tick);
void onLevelExit();

/// Main thread (queued by the telemetry worker): the server's `preserveEvidence` hint.
void onServerPreserveHint(std::vector<std::string> attemptIds, std::string reason);
/// Main thread (queued by the telemetry worker): `verificationRequests` of a batch / session-end
/// response (v0.9.0, owner decision 2026-10-01): the server judged a run of this session too good
/// for the attempts behind it. A top-right "!" notification tells the player, the attempt's clip
/// (game, microphone and desktop sound as separate tracks) is preserved and SENT without a popup;
/// each case id is handled once. Clipping off / footage gone: the notification says what to do.
void onVerificationRequested(std::vector<clip::VerificationRequest> requests);

// ---- frame hooks (src/ClipHooks.cpp) ----

/// GL thread, right before the frame is shown.
void onSwap();
/// Main thread, every FMODAudioEngine::update.
void onAudioTick(FMOD::System* system);
/// Opens the four-choice popup for the newest clip that waits for a choice. `automatic` = the
/// end screen is up / the pause menu opened: each clip is offered that way only ONCE (a player who
/// closed the popup is not asked again at every pause); the Account tab button passes false.
void offerPendingClip(bool automatic = true);

// ---- clips ----

/// Preserves the clip of an attempt still in the buffer. `rule`: "server_hint" |
/// "local_completion" | "manual". False when clipping is off, the attempt is unknown or it is
/// already preserved.
bool preserveAttempt(std::string const& attemptId, char const* rule);
/// "Clip last attempt" (GPRL menu > Account): the newest attempt of the open / last level.
bool preserveLastAttempt();
/// The player's choice for a clip (explicit UI action only).
void applyChoice(std::string const& clipId, clip::Choice choice);
void retryUpload(std::string const& clipId);
void openClipsFolder();
/// v0.10.0: sends a YouTube link as the evidence of a clip that was too long to upload
/// (POST /v1/me/evidence/links, private). The outcome lands in Status::linkMessage / the record.
void sendLink(std::string const& clipId, std::string const& url);

struct Status {
    clip::ClipConfig config;
    clip::CaptureStatus capture;
    std::string bufferLine;                 // "Clipping: recording 1280x720 h264_nvenc, buffer 118 s / 74 MB (cap 1024 MB)"
    bool bufferProblem = false;
    std::vector<clip::ClipRecord> clips;    // newest first
    std::string uploadClipId;               // the clip being uploaded, "" = none
    double uploadFraction = 0.0;
    clip::UploadGate gate;
    bool levelOpen = false;
    bool canClipLastAttempt = false;
    std::string clipsDir;
    // v0.10.0: verification clips too long to upload, waiting for a YouTube link (newest first)
    std::vector<clip::ClipRecord> linkNeeded;
    bool linkSending = false;               // a link is being sent right now
    std::string linkClipId;                 // the clip of the last link attempt
    std::string linkMessage;                // why the last link was refused ("" = none)
};
Status status();
/// The newest clip still waiting for the player's choice (Preparing without a choice, or Ready).
std::optional<clip::ClipRecord> pendingClip();
std::optional<clip::ClipRecord> findClip(std::string const& clipId);

}  // namespace gprl::clipper
