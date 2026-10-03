#pragma once
// GdOracle - the game-side facade of the timing-window solver (docs/SOLVER_DESIGN.md §2, §3.2,
// §3.7, §5). Phase 3 replaces the Phase 1 stub: the lockstep clone engine (CloneEngine, the
// frame-perfect-counter model) measures a LOCAL window for every player-1 jump press / release
// with hidden clones stepped in the game's own order, and this facade
//   - gates measuring (SOLVER_DESIGN §5): off / paused under Eclipse TPS bypass != 240, speedhack or
//     bot playback / unsupported on platformer levels; noclip and unknown mods measure,
//   - links each pending job to its telemetry input event (`bindLastInput` from the tracker, right
//     after client::push assigned the seq),
//   - turns a finished job into a `timing_window` event (core/fingerprint_build + solver/window_event),
//     pushes it, feeds the local CalibrationTracker (HUD / Session tab counts) and logs one line per
//     input (SOLVER_DESIGN §8) so the user's Geode log alone diagnoses every path,
//   - answers the popup / HUD with a status line and counters.
//
// It does NOT derive from IPhysicsOracle (SOLVER_DESIGN D2): a trial cannot be answered
// synchronously; the pure PassPlanner reproduces BoundarySearch as batched passes instead.
// Every function runs on the game thread.
#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "../../core/calibration.hpp"
#include "../../core/solver/trace.hpp"
#include "../../core/vocab.hpp"
#include "../Environment.hpp"

namespace gprl::solver::oracle {

/// Phase 3: the solver exists (Popup / HUD switch from the "not measured yet" text).
constexpr bool kImplemented = true;

struct Status {
    bool active = false;          // engine set up for the current level
    bool measuring = false;       // gate open
    std::string state;            // "measuring" | "paused: ..." | "off" | "unsupported: ..." | "failed: ..."
    // v0.5.1 (MASTER §13, §20): a bot / macro is playing - windows are still measured, tagged
    // `evidenceHint: level_only` (level analysis only, never the player); the environment event
    // carries bot = true for the server's own classification.
    bool levelOnly = false;
    std::string line;             // Session tab, first line
    std::string line2;            // Session tab, second line (counters)
    int emitted = 0, misses = 0, dropped = 0, dropMismatch = 0, dropInvalid = 0, dropPool = 0, dropBudget = 0;
    // v0.5.0: every drop / skip reason by name (docs/SOLVER_DESIGN.md §12), so the Session tab
    // says why an input got no window
    int dropNoPass = 0, dropReset = 0, dropLevelEnd = 0, dropHistory = 0, dropBlocked = 0, dropUnbound = 0, dropOther = 0;
    int notWindowable = 0;        // no_pass + reset + level_end (outside the coverage denominator)
    int skipped = 0, skippedDual = 0;
    int skippedDead = 0, skippedPaused = 0, skippedThrottle = 0, skippedFrame = 0, skippedJobs = 0, skippedOther = 0;
    int deferredSpawns = 0, deferredExpired = 0, clonesCreated = 0, poolSize = 0;
    std::string mismatchKinds;    // "rings 3, speed 2"
    double coverage = 0.0;        // % of windowable inputs that got a window
    int shadowSteps = 0, shadowMismatches = 0;
    double simMs = 0.0;
    double simPeakMs = 0.0;
    double stepCostUs = 0.0;
    int stepBudget = 0;
    bool throttled = false;
    int clones = 0, jobs = 0;
    size_t localSamples = 0;
    // v0.6.1 sequence windows (MASTER §5, docs/SOLVER_DESIGN.md §13): joint windows of neighbouring
    // inputs, measured only with spare solver time; level evidence only. line2 ends with
    // "| seq <emitted>/<measured> of <groups>" once a group was seen.
    bool sequencesOn = false;
    int seqGroups = 0, seqJobs = 0, seqMeasured = 0, seqEmitted = 0, seqUnsent = 0, seqTrivial = 0, seqDropped = 0, seqNotStarted = 0, seqWaiting = 0;
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §3.4 item 10, §6 item 5): timing_result statuses and the
    // sequence-adjusted jobs; line3 = "Results: <n> (ok a, low b, unresolved c, replay d, seq-dep e, no effect f) | SA ..."
    std::string line3;
    int results = 0, resultsSent = 0, resultsUnsent = 0;
    int statusCount[6] = {};
    bool saOn = false;
    int saCandidates = 0, saMeasured = 0, saWaiting = 0, saDropped = 0, saNotStarted = 0;
    bool replayBroken = false;
};

// ---- lifecycle (Hooks.cpp) ----
void setup(PlayLayer* pl);               // PlayLayer::setupHasCompleted (after the tracker)
void teardown();                         // PlayLayer::onQuit / PauseLayer::goEdit
void forgetLayer(PlayLayer* pl);         // Fields destructor safety net
void reset(PlayLayer* pl);               // after PlayLayer::resetLevel
void applySettings();                    // settings::load(); Record-Safe switched on mid-level -> CloneEngine::stopForVisit (no clone removed mid-level)
/// Gate re-evaluation (tracker::pollEnvironment: attempt start + every 0.5 s poll).
void refreshGate(TrustState trust, env::MenuState const& menus);

// ---- step loop ----
void stepBegin(GJBaseGameLayer* layer, float dt, bool halfTick);   // processCommands pre-hook
void playerSubUpdate(PlayerObject* p, float dt);                    // PlayerObject::update post-hook
/// PlayerObject::pushButton / releaseButton PRE-hook (before the button state changes).
void onPushButton(PlayerObject* p, int button, bool down);
/// tracker::onButton after client::push of the input event (player 1 jump only). `tSubTick` =
/// the event's sub-tick fraction (v0.7.0: the canonical actualMs includes it, RC-minor 6).
void bindLastInput(int64_t seq, double t, int64_t tick, std::string const& attemptId, bool practice, bool dropped, double tSubTick = 0.0);

// ---- deaths / triggers ----
/// PlayLayer::destroyPlayer BEFORE the original: true = consumed (a clone's death, or the
/// anti-cheat spike touched by a clone); the caller must return without calling the original.
bool claimDestroy(PlayLayer* pl, PlayerObject* player, GameObject* object);
/// After the original returned for a real player.
void onRealDestroy(PlayLayer* pl, PlayerObject* player, GameObject* object, bool wasDead, bool isDead);
void onRealPortal(PlayerObject* p, int objectId);
void onLevelComplete(PlayLayer* pl);
/// v0.15.0: PlayLayer::playEndAnimationToPos for the real player - the run's last physical step.
void onLevelEndAnimation(PlayLayer* pl);
void frameEnd(PlayLayer* pl, float dt);

// ---- clone identity for the side-effect guards (CloneHooks.cpp) ----
bool stepping();
bool isClone(PlayerObject* p);
PlayerObject* simClone();
void cloneInvalid(PlayerObject* p, char const* why);
// v0.8.0 live-state isolation (CloneHooks.cpp; docs/LIVE_ISOLATION_DESIGN.md §2.3): the stepped
// clone's activation overlay and the recorded half-tick flag of the step it replays.
uint8_t overlayGet(EnhancedGameObject* o);
void overlaySet(EnhancedGameObject* o, uint8_t logical);
uint8_t simSelfBit();
/// v0.8.3: the pair partner of the stepping clone (null outside a dual pair step).
PlayerObject* simPartner();
bool simHalfTick();
/// A clone reached a live-state writer (IsolationHooks.cpp tripwire) or an unisolated object type:
/// counted per attempt, the first few logged (docs/LIVE_ISOLATION_DESIGN.md §2.3 H9).
void noteTripwire(char const* what);
/// v0.8.1 (step 2): a gamemode portal's player part was replayed on the stepped clone
/// (core/solver/portal_model.hpp): counted per attempt / level, printed in the 5 s summary.
void notePortalModelled(int objectType);

/// core/geometry_hash low 32 bits at x (0 when no engine / no objects) - tracker state samples.
uint32_t geometryHashAt(float x);

/// v0.12.0 (docs/BACKGROUND_ANALYZER_DESIGN.md AN-D11): the background analyzer's read-only
/// extraction found a live-state difference; the clone engine behaves as on its own breach
/// (CloneEngine::tripIsolationBreaker). No-op while the engine is not active. Main thread, outside
/// the step loop.
void tripIsolationBreaker(char const* who);

/// The most recent finished job, for the middle-right HUD readout ("is it working?").
struct LastWindow {
    bool any = false;            // a job has finished on this level
    bool ok = false;             // emitted (false: dropped, see reason)
    bool down = true;            // press / release
    double widthMs = 0.0;
    double earlyMs = 0.0;        // earliest - actual (<= 0)
    double lateMs = 0.0;         // latest - actual (>= 0)
    bool boundedEarly = false, boundedLate = false;
    bool refined = false;        // CBF sub-tick resolution
    bool miss = false;           // the real player died on this input
    bool levelOnly = false;      // v0.5.1: measured while a bot played (evidenceHint level_only)
    std::string reason;          // drop reason when !ok
    double ageSeconds = 0.0;     // since the job finished
    // v0.7.0: the input's timing_result, once it arrives: "local 4.00 f / seq 10.75 f ok"
    int jobId = 0;
    std::string v2;
    // v0.15.0: a Ship input - the figure above is the LOCAL (frozen) window, `v2` the compensated one
    bool frozen = false;
};

// ---- display ----
Status status();
LastWindow lastWindow();
/// The most recent finished jobs, newest first (at most `max`), for the HUD history list.
std::vector<LastWindow> recentWindows(size_t max = 8);
void clearRecentWindows();
/// "Reset data" (Account tab): forget everything the player sees locally - the HUD history and
/// the local calibration tracker - so the screens read "no data" instead of stale counts. Called
/// from the HUD tick when client::resetGeneration() changes; the engine itself keeps running.
void clearLocalData();
bool hasLocalSamples();
CalibrationState localCalibration();

/// v0.7.0 debug view (docs/TIMING_SOLVER_V2.md §2.12, AUDIT §11; MOD item M9): the last traced
/// input exactly as its `GPRL trace:` lines printed it - header (input, type, gamemode, speed,
/// gravity, mini, local / sequence windows, hold) and the trajectories by role, world coordinates,
/// each with its death (killer id / rect, hitbox, later inputs) when it died. For an overlay that
/// only DRAWS (never touches physics). `serial` changes with every new trace; 0 = none on this
/// level (or the engine is not active). The reference stays valid until the next game-thread step.
trace::View const& lastTrace();

}  // namespace gprl::solver::oracle
