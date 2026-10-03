#pragma once
// Background level analyzer: the worker thread (docs/BACKGROUND_ANALYZER_DESIGN.md §2 "Worker",
// AN-D7, AN-D12). One std::thread at below-normal priority owns every sim::Job; the main thread
// only posts messages (a finished extraction, recorded attempts, level closed) and reads the
// published status (mutex copy).
//
//   level submitted  -> WorldBuilder::finish + sim::gameplayHash (here, never on the game thread)
//                    -> the same gameplay version kept from an earlier visit (10 minutes)? resume it
//                    -> analysis/<gdLevelId>-<gameplayHash>.json on this computer? "cached" (sent
//                       later when it was never sent)
//                    -> `analysis-cache`: GET /v1/levels/:gdId/sim-cache; found + status client /
//                       reviewed + solvedPercent >= 99 -> "cached analysis used", nothing simulated
//                    -> else a sim::Job with the recorded attempts
//   NOT allowed now  -> (review MEDIUM-8) while modes::simAllowedNow() is false (Record-Safe during
//                       an attempt, frame pressure) a submitted level waits in an ordered backlog
//                       (at most 4 visits, the oldest dropped): nothing of it is built, hashed or
//                       identified (WorldBuilder::finish, sim::gameplayHash, identity::compute all
//                       wait); attempts of a waiting visit attach to it, a level exit marks it closed
//   running          -> JobControl.mayRun = modes::simAllowedNow() (Record-Safe: no attempt
//                       active; never under frame pressure) && the job's level is open && no
//                       message waits; low CPU: 8 ms bursts with 8 ms rests (normal: 16 / 1).
//                       Recorded attempts are queued on the entry and handed to the job
//                       (Job::addRecordedAttempt verifies synchronously) only inside the same rule
//   errors           -> (review HIGH-4) every message, job slice and upload runs inside try/catch:
//                       an exception marks that entry Failed ("worker error: ...") or that upload
//                       failed, logs it, and the thread goes on
//   done             -> LevelSimResult (+ computedAt, build) -> toJsonString -> the local file ->
//                       `analysis-upload` + connected: POST /v1/me/level-sim (retry once after 30 s
//                       on a network failure); the server's status label goes to the HUD
//   level closed     -> the job pauses and is kept 10 minutes for another visit, then dropped
//
//   level families (docs/LEVEL_FAMILY_DESIGN.md §8, core/analyzer_identity.hpp), per EXACT level
//   version (sha256 level hash), right after the world is hashed:
//     identity/<levelHash>.json with a server answer younger than 24 h -> nothing is computed or
//       sent, the cached notice is queued again
//     else identity::compute(World, DecoSet, {levelHash, gdLevelId, copiedFromGdId, nameHint}) ->
//       toJson (<= 1 MB, else kept on this computer) -> `analysis-upload` + connected: POST
//       /v1/me/level-identity { sessionId, identity } once the telemetry session is open (the
//       server creates the level version at session start; waits up to 60 s for it, retries a
//       not-yet-known version / session twice) -> the answer's `notice` lines go to the main thread
//       (takeFamilyNotice, drained by Analyzer::onFrame -> hud::familyNotice while the level is
//       still open and `family-notice` is on), the family summary to Published::familyLine
//
// READ-ONLY RULE: the worker never touches the game - no cocos, no GD type, no Geode UI. It owns
// plain data (sim::World, RecordedAttempts) and talks to the network and its own files only.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../core/analyzer_recorder.hpp"
#include "../../core/analyzer_status.hpp"
#include "../../core/sim/job.hpp"
#include "Extract.hpp"

namespace gprl::analyzer::worker {

struct Config {
    bool cache = true;              // analysis-cache
    bool upload = true;             // analysis-upload
    bool debug = false;             // analysis-debug / debug-log
    int maxShiftTicks = 10;         // max-shift-ticks
    int subTickRefine = 1;          // solver-subtick: off 0, 1/8 tick 1, 1/64 tick 2
    std::string clientBuild;        // "gprl-geode 0.12.0+win"
};

/// What the network needs (client::uploadAuth + the open session id), pushed by the main thread.
struct Credentials {
    bool online = false;            // not local-only, API configured
    bool connected = false;         // a device token usable for the logged-in GD account
    std::string deviceToken;
    std::string apiBaseUrl;
    std::string sessionId;          // the open telemetry session ("" = none)
    // the level version that session belongs to: client::Status::sessionLevelHash, published by the
    // telemetry worker TOGETHER with sessionId / sessionGen (review MEDIUM-5: never the main thread's
    // current level, which can already be the next one while the status still describes the old
    // session). The server takes an upload only with a session of THIS device on THAT exact version
    // (review HIGH-1/2), so the worker remembers the session per level hash (core/analyzer_worker_
    // rules.hpp SessionMap: a newer generation wins, the oldest entry is evicted) and uses it even
    // after the level was left.
    std::string sessionLevelHash;
    uint32_t sessionGen = 0;        // client::Status::sessionGen of that session (0 = none)
};

struct AttemptIn {
    sim::RecordedAttempt attempt;
    AttemptMeta meta;
};

/// Main thread. Starts the thread once (idempotent).
void start();
/// Game exit: stop and join (at most ~1.5 s, then detached). Once stopping, the worker starts no
/// request; one already in flight may finish in the detached thread, which then returns without
/// touching any shared state (review MEDIUM-11).
void shutdown();
void setConfig(Config cfg);
void setCredentials(Credentials c);

/// A level visit's finished extraction, plus the attempts recorded before it finished.
void submitLevel(uint64_t visitId, extraction::Handover handover, std::vector<AttemptIn> attempts);
/// A recorded attempt of a visit whose level was already submitted.
void submitAttempt(uint64_t visitId, AttemptIn attempt);
/// The visit's level was left: its job pauses and is kept for 10 minutes.
void levelClosed(uint64_t visitId);

/// The worker's published state (copied under a mutex; read by the HUD / popup twice a second).
struct Published {
    bool running = false;           // the thread exists
    uint64_t visitId = 0;           // the visit this describes (the newest submitted one)
    int gdLevelId = 0;
    std::string gameplayHash;
    status::Stage stage = status::Stage::Off;
    status::WaitReason wait = status::WaitReason::None;
    sim::JobProgress progress;
    bool hasResult = false;
    double physicsPercent = 0.0, solvedPercent = 0.0, verifiedPercent = 0.0;
    bool hasVerification = false;
    int windows = 0;
    int unsupportedSpans = 0, unsolvedSpans = 0;
    int attemptsAdded = 0;
    int worldObjects = 0, gameplayObjects = 0, decorationObjects = 0;
    bool tooLarge = false;
    double finishMs = 0.0;          // WorldBuilder::finish + gameplayHash on the worker
    std::string cacheSource;        // "server" | "this computer" | ""
    std::string upload;             // HUD text of the upload state ("" = nothing yet)
    std::string failReason;
    int keptJobs = 0;               // jobs kept for other visits
    // level families: the Session tab's summary line ("Family: <name> - gameplay 99.7% - ...",
    // "" until the server answered) and the identity's state ("computing", "waiting for the
    // session", "sent", "cached answer (this computer)", "kept on this computer (...)", ...)
    std::string familyLine;
    std::string identityState;
};
Published published();

/// The level-enter notice of a level family (FA-D11), queued by the worker when the server's
/// answer (or the cached one) carries `notice` lines. Drained on the main thread by
/// Analyzer::onFrame, which shows it only while `visitId` is still the open visit.
struct FamilyNotice {
    uint64_t visitId = 0;
    std::vector<std::string> lines;
};
/// Cheap check (atomic) before taking: true while at least one notice waits.
bool familyNoticePending();
/// The oldest waiting notice (mutex), nullopt when none.
std::optional<FamilyNotice> takeFamilyNotice();

}  // namespace gprl::analyzer::worker
