#pragma once
// The lockstep clone engine (docs/SOLVER_DESIGN.md §1, §3, §4, §6, §12): the frame-perfect-counter
// Engine subset (shadow clone, history ring, jobs with shifted inputs, convergence / resync,
// horizon, anti-cheat spike ignore, portal / dual guards) with the auto-bot, manual tags, macro
// player, physics lock and HUD dropped, and the search driven by core/solver/pass_planner.
//
// Time axis: a "frame" is one 240 Hz physics tick. GD runs a tick as two half ticks when a queued
// input lands inside it (m_isBetweenSteps), so every processCommands call is one step of 1 or 0.5
// frames (0 when physics did not run). Steps index the history ring; frames are the unit of
// shifts, windows and the look-ahead. PlayerObject::update(dt) takes dt in 1/60 s units.
//
// v0.5.0 (docs/SOLVER_DESIGN.md §12): the one-shot activation flags of orbs / pads / portals are
// isolated per clone in the LOGICAL form of core/solver/activation.hpp (the game selects the slot by
// the player's unique id, so a clone must see the real player 1's slot in its own), the real
// player's speed changes (queued by the layer, never a collision) are mirrored onto every clone
// step, and every budget lives in core/solver/budget.hpp (kBudget, versioned): 24 jobs, a warm
// pool + rationed clone creation, an adaptive per-frame clone-step budget from the measured cost,
// deferred spawning of a pass's shifts when the pool is short (they catch up from the ring).
//
// Every function runs on the game thread inside the game's own step loop (D1: clones are stepped
// in lockstep with the real game, never deferred). Nothing here touches the network or the disk.
#include <Geode/Geode.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../core/snapshot.hpp"
#include "../../core/solver/activation.hpp"
#include "../../core/solver/settle.hpp"
#include "../../core/solver/budget.hpp"
#include "../../core/solver/cluster.hpp"
#include "../../core/solver/compensation.hpp"
#include "../../core/solver/control_card.hpp"
#include "../../core/solver/dual_rules.hpp"
#include "../../core/solver/isolation_guard.hpp"
#include "../../core/solver/live_state.hpp"
#include "../../core/solver/parity.hpp"
#include "../../core/solver/pass_planner.hpp"
#include "../../core/solver/result_ledger.hpp"
#include "../../core/solver/sequence.hpp"
#include "../../core/solver/sequence_adjusted.hpp"
#include "../../core/solver/ship_control.hpp"
#include "../../core/solver/timing_result_event.hpp"
#include "../../core/solver/timing_status.hpp"
#include "../../core/solver/trace.hpp"
#include "CloneState.hpp"

namespace gprl::clone {

// v0.7.0 (budget/3, docs/TIMING_SOLVER_V2.md §3.2): 2048 steps (8.5 s at one step per tick); the
// sequence-adjusted jobs replay up to ~3 s old snapshots. One definition in core/solver/budget.hpp.
constexpr int kHistorySteps = solver::budget::kHistorySteps;
constexpr int kMaxClones = solver::budget::kBudget.maxClones;
constexpr int kMaxJobs = solver::budget::kBudget.maxJobs;
constexpr int kMaxCloneStepsPerFrame = solver::budget::kBudget.maxStepsPerFrame;   // hard cap; the adaptive budget is usually lower
constexpr int kExtensionStepsPerFrame = solver::budget::kBudget.extensionStepsPerFrame;
// A clone more than kCatchUpLagSteps behind the real timeline (a refinement pass spawned from
// ~0.5 s old snapshots, a deferred spawn, or one the per-frame budget left behind) advances at
// most kCatchUpStepsPerStep steps per real step, so a 15-clone refinement pass (~2000 clone steps)
// is spread over a few rendered frames instead of one hitch. Not in FPC (its probe bursts).
constexpr int kCatchUpLagSteps = solver::budget::kBudget.catchUpLagSteps;
constexpr int kCatchUpStepsPerStep = solver::budget::kBudget.catchUpStepsPerStep;
constexpr uint32_t kNoInputId = 0xffffffffu;

/// v0.14.0 lockstep compensation (docs/SHIP_SOLVER.md §4.3): which modes are CONNECTED-CONTROL
/// modes (their sequence windows come from the lockstep compensation planner, never from the
/// delayed replay) and the load caps. DEV DEFAULTS: ship only (wave / ufo / swing are candidates,
/// SHIP_SOLVER §10). Runs only while `measure-sequence-adjusted` is on.
struct CompensationConfig {
    bool enabled = true;
    bool ship = true;
    bool wave = false;
    bool ufo = false;
    bool swing = false;
    int maxJobs = 24;                // compensation jobs WITH trials alive at once (more: sa_not_measured_budget); = the local job cap
    int parallelClones = 6;          // trials of one job running at once
    bool phase = true;               // v0.15.0 (docs/SHIP_SOLVER.md §11.3): the PHASE search of every complete control (on its release)
};

struct EngineConfig {
    bool subtick = false;            // Click Between Frames active: sub-tick refinement passes
    double resolutionFrames = 0.125; // 1/8 tick (or 1/64)
    int maxRefinePasses = 1;
    int maxShiftTicks = 10;
    double horizonSeconds = 0.5;     // the look-ahead the jobs plan with (the settle maximum while settle.enabled)
    solver::settle::SettleConfig settle;   // v0.11.0: the settled look-ahead (core/solver/settle.hpp)
    int verbosity = 1;               // 0 summaries only, 1 + per-input window lines, 2 + passes / spawns / details
    int traceMaxTicks = 0;           // v0.7.0 `solver-trace-max-ticks`: trace windows at most this wide (0 = off)
    bool isolationEveryStep = false; // v0.8.2 `isolation-check` = every clone step (or debug log): the live-state invariant per clone step
    bool dualShadow = true;          // v0.8.3 `solver-dual`: shadow only (a P1+P2 pair shadow in dual sections) | off (no clone steps in dual)
    CompensationConfig compensation; // v0.14.0 (docs/SHIP_SOLVER.md §4.3)
};

struct InputEvent {
    double t = 0.0;            // frames (fractional with Click Between Frames)
    bool down = true;
    uint32_t id = 0;
    double levelTime = 0.0;    // m_levelTime when it happened
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §2.7, §3.3): what the SA scheduler and the cluster links need
    int64_t seq = -1;          // telemetry seq of the input event (bindJob), -1 = unbound
    int jobId = 0;             // its local job (0 = none: skipped)
    bool localDone = false;    // the local job finished (emitted or dropped)
    bool noEffect = false;     // its local window was `no_effect` (every shift re-joined)
};

enum class CloneStatus : uint8_t { Idle, Running, Dead, Alive, Invalid, Cancelled, Limit };

/// Sequence trial (v0.6.1, docs/SOLVER_DESIGN.md §13): one input of the group at its own shifted
/// frame. A local job's clone has none of these (its single shifted input lives in the Clone).
struct MovedInput {
    uint32_t id = 0;            // the logged input this copy replaces
    bool down = true;
    double frame = 0.0;         // frame the shifted copy is applied at
    bool pending = true;
    double applied = solver::kNaN;   // where simStep actually applied it
};

struct Clone {
    PlayerObject* obj = nullptr;
    CloneStatus state = CloneStatus::Idle;
    int job = -1;
    int pass = 0;
    double shift = 0.0;         // nominal shift (frames)
    double inputFrame = 0.0;    // frame the shifted input is applied at
    bool inputPending = false;
    double appliedFrame = 0.0;  // where simStep actually applied it (NaN until applied)
    int stepDone = -1;
    double frameDone = 0.0;     // frame reached after the last step
    int deathStep = -1;
    double deathFrame = 0.0;
    int deathObjId = -1;        // object that killed the clone (-1: none / level boundary)
    float deathObjX = 0.f;
    int converged = 0;
    bool resynced = false;      // Alive by matching the control (not just by outliving the horizon)
    solver::settle::SettleState settle;   // v0.11.0: on the ground after its input (or flying long enough): a settled pass
    bool extension = false;     // stepped with synthetic ticks in the death pause
    std::string invalidReason;
    std::vector<MovedInput> moved;   // sequence trials only: the group's inputs at their shifted frames
    // v0.7.0 death attribution (docs/TIMING_SOLVER_V2.md §2.3): simStep's apply counts every logged
    // input that is NOT moved in this trial, is not the measured input, and was recorded after
    // `attributeAfterFrame` (the measured input's frame; +inf for the shadow / controls)
    double attributeAfterFrame = 1e300;
    int laterFixed = 0;
    double rejoinStartFrame = solver::kNaN;   // first frame of the converged streak
    float deathX = 0.f;                       // the clone's x at the death
    // debug trace (§2.12, only while solver-trace-max-ticks > 0): steps of this trial + its death
    bool tracing = false;
    solver::trace::TraceClone trace;
    // clone-local activation flags in the LOGICAL form (core/solver/activation.hpp): objects with
    // a nonzero logical value only
    std::unordered_map<EnhancedGameObject*, uint8_t> flags;   // v0.8.0: the trial's activation OVERLAY (never written into the game)
    bool player2 = false;       // v0.8.3: the P2 clone of a dual pair (activation bit kOther, m_isSecondPlayer; docs/LIVE_ISOLATION_DESIGN.md §2.7)
    bool compNoted = false;     // v0.14.0: this finished local copy was fed to the job's compensation planner
};

/// A shift of a pass that never got a clone (limit / no history / pool / cancelled): reported NotTested.
struct Untested {
    double shift = 0.0;
    std::string reason;
};

struct Job {
    int id = 0;
    uint32_t inputId = 0;       // input this job measures (replaced by the clone's shifted copy)
    int64_t inputSeq = -1;      // telemetry seq of the input event (bindJob)
    bool bound = false;
    std::string attemptId;
    double eventT = 0.0;        // the input event's t (seconds since the attempt start)
    int64_t tick = 0;
    bool practice = false;
    double t = 0.0;             // frame of the input
    int step = 0;               // step the input was applied in
    double levelTime = 0.0;     // m_levelTime at the input
    bool down = true;
    bool subtick = false;
    double horizonFrame = 0.0;
    double earlyLimitFrames = solver::kNaN;
    double lateLimitFrames = solver::kNaN;
    // fingerprint context
    PlayerStateSnapshot pre;
    std::string geometryHash;
    bool ceilingTouch = false;
    double prevGapFrames = solver::kNaN;
    double nextGapFrames = solver::kNaN;
    double pressLevelTime = solver::kNaN;    // releases: their press's m_levelTime
    double pressFrame = solver::kNaN;
    double releaseFrame = solver::kNaN;      // presses: their release's frame once logged
    int firstPortalId = 0;
    // search state
    solver::PassPlanner planner;
    int pass = 0;
    std::vector<double> passShifts;
    std::vector<double> pendingShifts;   // shifts of the current pass still waiting for a clone (deferred spawn)
    int spawnStep = 0;                   // step the current pass was planned at (deadline for the deferred shifts)
    std::vector<int> clones;
    std::vector<Untested> untested;
    bool spawned = false;                // the pass is planned and its control clone exists (or could not)
    bool spawnComplete = true;
    bool historyMissingEarly = false;
    float maxDrift = 0.f;
    std::string mismatch;       // first control-vs-real difference, empty while the control matched exactly
    bool controlDiedWithReal = false;
    bool extension = false;
    std::string dropReason;     // set when the job can never emit (finalised as dropped)
    double createdMs = 0.0;
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §2.2, §2.10, §2.12)
    int attemptInputIndex = 0;       // 1-based index in this attempt's input log ("Input #47 of this attempt")
    double percent = 0.0;            // PlayLayer::getCurrentPercent() in the pushButton pre-hook
    double subTickMs = 0.0;          // the input event's tSubTick x 1000/240 (bindJob)
    double engineSubTickMs = 0.0;    // v0.7.1 (Fable D11): frac(t) x 1000/240 - the source of actualMs
    bool halfTick = false;           // landed on a half tick without Click Between Frames
    solver::LimitKind earlyLimitKind = solver::LimitKind::History;
    bool cutByLevelEnd = false;
    int controlSims = 0;             // control clones this job ran (one per pass)
    std::vector<solver::trace::TraceClone> traces;   // pass-0 trajectories while tracing is on
    double missDeathFrame = solver::kNaN;   // v0.7.1 (Fable D6): the real / would-be death its control died with
    // v0.14.0 (docs/SHIP_SOLVER.md §4.3): a connected-control input (ship): its sequence window is the
    // lockstep compensation planner's (CompJob), never the delayed replay's
    bool connected = false;
    bool compCreated = false;        // a CompJob was created (or refused by the job cap) for this job
    solver::parity::Record parity;   // v0.15.0: the first divergence of this job's control from the real run
};

/// A real / would-be death of player 1 this attempt (v0.7.1: with its x for the object -1 match, Fable D6).
struct DeathMark {
    double frame = 0.0;
    int obj = -1;
    float x = 0.f;
};

struct Snapshot {
    int step = -1;
    float dt = 0.f;            // delta the real player was stepped with (known once the step ended)
    bool dtKnown = false;
    bool halfTick = false;
    double frame = 0.0;        // frames elapsed at the start of the step
    double frames = 1.0;       // 1 or 0.5 (0 when physics did not run)
    double totalTime = 0.0;    // player m_totalTime during the step
    double levelTime = 0.0;
    PlayerState state;
    std::vector<std::pair<EnhancedGameObject*, uint8_t>> flags;   // logical activation flags (nonzero only)
    bool dual = false;         // v0.8.3: the level was in dual mode at this step (state2 holds the real player 2)
    PlayerState state2;
};

/// What finalize hands to the oracle facade (GdOracle builds the event from it).
struct JobResult {
    int jobId = 0;
    int64_t inputSeq = -1;
    std::string attemptId;
    double eventT = 0.0;
    int64_t tick = 0;
    bool practice = false;
    bool down = true;
    double t = 0.0;
    int step = 0;
    double levelTime = 0.0;
    PlayerStateSnapshot pre;
    std::string geometryHash;
    bool ceilingTouch = false;
    std::optional<double> prevGapMs;
    std::optional<double> nextGapMs;
    std::optional<double> holdMs;
    double pressMs = solver::kNaN;   // releases: the press's actual ms on the same axis as actualMs
    int firstPortalId = 0;
    solver::WindowResult window;
    bool refined = false;
    bool miss = false;
    bool extension = false;
    int passes = 0;
    std::string detail;
    std::string mismatch;
    bool ok = false;                 // window valid and control exact (or died with the real player)
    std::string dropReason;          // mismatch | invalid | no_pass | pool | budget | level_end | reset | unbound | blocked | history lost
    bool wouldBeDeathAfter = false;  // a would-be death within 0.75 s after the input
    float maxDrift = 0.f;
    // v0.7.0: the log line names the input by its attempt index, the job id in brackets (RC-minor 4)
    int attemptInputIndex = 0;
    double subTickMs = 0.0;          // the tracker's sub-tick (the input event's tSubTick x 1000/240)
    double engineSubTickMs = 0.0;    // v0.7.1 (Fable D11): actualMs = t x 1000 + engineSubTickMs (the shifts' origin)
    std::string invalidWhy;          // `DROPPED invalid` names the reason (teleport / dual portal / ...)
    bool missDownstream = false;     // v0.7.1 (Fable D6): an earlier job of a died run - no timing_window
};

/// A finished `timing_result` (docs/TIMING_SOLVER_V2.md §4.1): the payload is built and checked by
/// the engine (core/solver/timing_result_event); the facade pushes it (telemetry revision >= 4)
/// and logs the `GPRL timing:` / `GPRL timing json:` lines.
struct TimingResultOut {
    int jobId = 0;
    std::string attemptId;
    double eventT = 0.0;
    int64_t tick = 0;
    bool down = true;
    int attemptInputIndex = 0;
    solver::TimingResultBuild build;
    std::string line;                // the `GPRL timing:` line without the prefix
    double localWidthMs = solver::kNaN;
    double seqWidthMs = solver::kNaN;
    // v0.15.0 (docs/SHIP_SOLVER.md §11.7): a connected-control (Ship) input; `card` = the lines of
    // its control's card when this result completed it (press + release both reported), with the
    // explicit fields of the press and of the release
    bool connected = false;
    std::vector<std::string> card;
    std::string cardFields[2];
};

/// Every counter the 5 s summary and the Session tab print (docs/SOLVER_DESIGN.md §8, §12).
struct Counters {
    int inputs = 0;          // P1 jump inputs logged
    int jobs = 0;            // measurements started
    int emitted = 0;
    int misses = 0;
    // dropped: the job existed and could not emit
    int dropped = 0;
    int dropMismatch = 0;
    int dropInvalid = 0;
    int dropNoPass = 0;      // miss without a passing shift in range: the death was not caused by this input
    int dropPool = 0;
    int dropBudget = 0;
    int dropHistory = 0;
    int dropBlocked = 0;
    int dropUnbound = 0;
    int dropReset = 0;       // restart / teardown before the horizon
    int dropLevelEnd = 0;
    int dropOther = 0;
    std::map<std::string, int> mismatchKinds;   // "rings", "speed", "position", ... (core/solver/diagnostics.hpp)
    // skipped: no job was created
    int skipped = 0;
    int skippedDead = 0;     // player dead / level not running (mashed in the death pause, before the start)
    int skippedDual = 0;
    int skippedPaused = 0;
    int skippedThrottle = 0;
    int skippedFrame = 0;    // rendered dt above budget::slowFrameDt
    int skippedJobs = 0;
    int skippedOther = 0;    // platformer, snapshot ring failed
    // deferred spawning
    int deferredSpawns = 0;  // shifts that got their clone one or more steps after the pass was planned
    int deferredExpired = 0; // shifts reported NotTested (pool) at the spawn deadline
    int clonesCreated = 0;
    int shadowSteps = 0;
    int shadowMismatches = 0;
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §6 items 5, 6, 10)
    int skippedReplay = 0;           // the replay breaker stopped measuring for the rest of the attempt
    int skippedIsolation = 0;        // v0.8.2: the isolation breaker (a live-state breach) stopped measuring for the rest of the attempt
    int settled = 0;                 // v0.11.0: shifted copies that passed by settling (on the ground after the input / flying long enough)
    int unsettled = 0;               // v0.11.0: shifted copies alive but never settled at the look-ahead's end (not tested)
    int pairShadowSteps = 0;         // v0.8.3: steps of the P1+P2 pair shadow in dual sections
    int pairShadowMismatches = 0;    // v0.8.3: pair shadow steps where P1 or P2 differed from the real pair
    int placeWhole = 0, placeHalf = 0, placeSub = 0;   // input placement: whole tick / half tick / sub-tick
    int statusCount[solver::status::kTimingStatusCount] = {};   // timing_result statuses
    int edgeSelf = 0, edgeDownstream = 0, edgeExtension = 0;   // local fail edges by cause
    int results = 0;                 // timing_results built
    int resultsSent = 0;             // pushed to the telemetry session (the sink said so)
    int resultsFallback = 0;         // builder failed: unresolved / payload_invalid emitted instead
    // v0.7.1 (the Fable review)
    int missDownstream = 0;          // D6: earlier jobs of a died run (timing_result only, no timing_window)
    int speedChanges = 0;            // D7: real speed changes seen (consecutive ring snapshots)
    int speedFlagged = 0;            // D7: timing_results labelled speed_change_in_lookahead
};

/// v0.7.0 sequence-adjusted jobs (docs/TIMING_SOLVER_V2.md §3.3, §6 item 5).
struct SACounters {
    int candidates = 0;              // local jobs whose SA window needs simulations
    int immediate = 0;               // SA decided without a simulation (isolated / self edges / no follower)
    int measured = 0;                // candidates whose SA job finished with both controls exact
    int undecidedSides = 0;
    int jobs = 0;
    int dropControl = 0, dropNegative = 0, dropExpired = 0, dropReset = 0, dropInvalid = 0;
    int dropDeath = 0;               // v0.7.1: a real / would-be death came inside a running job's span (its control died with it)
    int notStartedBudget = 0;        // queue full / trial budget
    int notStartedWide = 0;          // local window wider than priorityMaxLocalWidthTicks
    int notStartedNoNegative = 0;    // no lockstep death to prove the delayed replay with
    int notStartedExpired = 0;       // waited until its history left the ring
    int notStartedDeath = 0;         // a real / would-be death inside the job's span
    int trials = 0;
    int cloneSteps = 0;
    int dropped() const { return dropControl + dropNegative + dropExpired + dropReset + dropInvalid + dropDeath; }
    int notStarted() const { return notStartedBudget + notStartedWide + notStartedNoNegative + notStartedExpired + notStartedDeath; }
};

// ---- sequence jobs (v0.6.1, docs/SOLVER_DESIGN.md §13; the search is core/solver/sequence) ----
//
// A sequence job measures the JOINT window of 2-3 neighbouring inputs whose local windows are
// already emitted. It cannot run in lockstep (the second input does not exist yet when the first
// happens), so its clones are DELAYED replays from the history ring: every trial starts at the
// job's base snapshot and replays the logged inputs with the group's inputs moved. What makes a
// delayed replay trustworthy is proven per job, not assumed:
//   control first  the unshifted replay must match the ring's recorded real states step by step
//                  up to the look-ahead (the world it runs through still reproduces the real run),
//   negative       one death the local solver saw in lockstep (a member shifted to its first
//                  failing shift) must happen again (hazards are still live back there),
//   control last   the unshifted replay again, after the samples (nothing changed meanwhile).
// Any of the three failing drops the job with the reason in the log. Lowest priority: a sequence
// clone only steps with what the local jobs left of the frame's step budget (SequenceConfig).

/// A finished local window the sequence solver can build on (kept for the last few inputs).
struct LocalDone {
    uint32_t inputId = 0;
    int64_t inputSeq = -1;
    std::string attemptId;
    double eventT = 0.0;
    int64_t tick = 0;
    double t = 0.0;              // frame of the input
    int step = 0;
    bool down = true;
    bool subtick = false;
    double x = 0.0;              // player x at the input (the "same place" key)
    solver::LocalWindowInfo info;
};

enum class SeqRole : uint8_t { Control, Negative, Sample };

struct SeqTrial {
    int clone = -1;
    SeqRole role = SeqRole::Sample;
    int pointId = -1;            // lattice point (samples)
    int converged = 0;           // consecutive steps equal to the real run once every moved input is applied
    int steps = 0;
    int compared = 0;            // control: steps compared with the ring
    // v0.7.0 SA samples (docs/TIMING_SOLVER_V2.md §3.3): the planner's trial id and its own look-ahead
    int saTrialId = -1;
    double lookAheadFrame = 0.0;
    int saMember = -1;           // the planner member the trial belongs to (debug traces)
    int saAdaptation = 0;        // SAAdaptation: 0 local, 1 pair, 2 chain2, 3 chain3 (4..6 comp1..3 are the lockstep planner's)
    double saShift = 0.0;
};

enum class SeqPhase : uint8_t { ControlFirst, Negative, Sampling, ControlLast };

/// v0.7.0: M4 joint-share jobs (`gprl-clone-seq/1`, setting measure-joint-share, default off) and
/// sequence-adjusted jobs (`gprl-clone-sa/1`, measure-sequence-adjusted) share the delayed-replay
/// machinery (V2-D5): SA first, joint jobs only while no SA candidate waits.
enum class SeqKind : uint8_t { JointShare, SequenceAdjusted };

/// One input whose SA window needs delayed replays (docs/TIMING_SOLVER_V2.md §3.3).
struct SACandidate {
    int jobId = 0;               // the result ledger key
    uint32_t inputId = 0;
    int index = 0;               // attemptInputIndex
    double t = 0.0;
    int step = 0;
    bool down = true;
    bool subtick = false;
    double earlyLimitFrames = solver::kNaN;
    solver::LimitKind earlyLimitKind = solver::LimitKind::History;
    double lateLimitFrames = solver::kNaN;   // v0.7.1 (MOD verifier, D1): the local planner's late limit (NaN = range)
    std::vector<solver::ShiftOutcome> outcomes;
    double widthFrames = 0.0;    // local window width (queue priority: narrowest first)
    int64_t order = 0;
    bool wantsPairWalk = false;  // a press followed by a release: the pair walk runs if that release is effective
    double x = 0.0;              // v0.7.1 (Fable D10): player x at the input - the level-visit position bucket
};

/// A lockstep death a job can replay as its negative control (a member's, or a recent input's).
struct SANegative {
    bool valid = false;
    uint32_t inputId = 0;
    bool down = true;
    double frame = 0.0;          // the shifted frame the input is replayed at
    double shift = 0.0;
    int objectId = -1;
    double deathFrame = 0.0;
};

struct SeqJob {
    int id = 0;
    SeqKind kind = SeqKind::JointShare;
    std::vector<LocalDone> members;
    solver::SequencePlanner planner;
    // SequenceAdjusted jobs
    solver::SAPlanner sa;
    std::vector<SACandidate> saMembers;
    SANegative saNegative;
    SeqPhase phase = SeqPhase::ControlFirst;
    bool subtick = false;
    double horizonFrame = 0.0;
    double baseFrame = 0.0;      // earliest frame any trial changes something at
    int baseStep = -1;           // snapshot every trial starts from (-1 until the job starts)
    int negMember = -1;          // member whose known death is replayed
    int64_t positionKey = 0;
    int queuedStep = 0;
    int startedStep = 0;
    double startedMs = 0.0;
    std::vector<SeqTrial> running;
    bool controlLastSpawned = false;
    bool controlLastDone = false;
    int controlSteps[2] = {0, 0};   // steps compared with the ring by the first / last control
    int trials = 0;
    int cloneSteps = 0;
    std::string negText;
    // v0.7.0 debug traces (§2.12 second block): the SA trials of traced members, (member, trial)
    std::vector<std::pair<int, solver::trace::SATraced>> saTraced;
};

// ---- v0.14.0 lockstep compensation (docs/SHIP_SOLVER.md §4.3; core/solver/compensation.hpp) ----

/// One running compensation trial: its clone and what the planner needs back from it.
struct CompTrialRun {
    int clone = -1;
    int trialId = -1;
    double assessFrame = 0.0;
    double devFromFrame = 0.0;
    double lookAheadFrame = 0.0;
    int converged = 0;               // consecutive steps equal to the recorded run with every moved input applied
    std::vector<solver::comp::DevSample> dev;   // deviation from the recorded run per step (y, y velocity)
    int adaptation = 0;              // SAAdaptation value (traces)
    double shift = 0.0;
    std::vector<double> offsets;
    size_t logChecked = 0;           // m_log size at the last crossing check (F2)
    solver::rejoin::Tracker rejoin;  // v0.15.0 (core/solver/rejoin.hpp): fed after every moved input was applied
};

/// The compensation job of one connected-control local job (same lifetime as its ledger entry).
struct CompJob {
    int jobId = 0;                   // the local job / ledger key
    uint32_t inputId = 0;
    int index = 0;                   // attemptInputIndex
    double t = 0.0;
    int step = 0;
    bool down = true;
    solver::comp::CompPlanner planner;
    std::vector<CompTrialRun> running;
    int control = -1;                // the delayed control's clone (-1 = none yet / finished)
    int controlBaseStep = -1;
    int controlCompared = 0;         // live steps the control matched
    bool controlDone = false;        // the control reproduced a real death inside the span (no more proof needed)
    bool localDone = false;          // the local pass resolved (finalizeLocal fed)
    int trials = 0;
    int cloneSteps = 0;
    double startedMs = 0.0;
    double lookAheadMax = 0.0;
    std::string fail;                // why the job was dropped (control mismatch, expired)
    std::vector<solver::trace::SATraced> saTraces;   // traced trials (solver-trace-max-ticks)
    // v0.15.0 (docs/SHIP_SOLVER.md §11.3): a PHASE job measures the whole hold of the control that
    // ends with the job's release (member = the press, the release moves rigidly with it); its
    // result goes to ResultEntry::phase, never to the sequence window
    bool phase = false;
    solver::parity::Record parity;   // the first divergence of the delayed control
};

struct CompCounters {
    int jobs = 0, trials = 0, cloneSteps = 0, controls = 0;
    int passes = 0, compensated = 0, fails = 0, invalid = 0;
    int decided = 0, undecided = 0;
    int controlMismatch = 0, cutByRestart = 0, discarded = 0;
    int notStartedWide = 0, notStartedBudget = 0;
    int crossed = 0;                 // trials ended because a later input crossed a moved one (F2)
    // v0.15.0 (controls/1): how the passes re-joined, the trials that survived without re-joining,
    // the trials alive at the level end, the phase jobs
    int rejoinExact = 0, rejoinApprox = 0, rejoinParallel = 0, noRejoin = 0, levelEnd = 0;
    int phaseJobs = 0, phaseDecided = 0;
};

/// What a finished sequence job hands to the facade (GdOracle builds the event from it).
struct SeqResult {
    int jobId = 0;
    std::vector<int64_t> inputSeqs;
    std::string attemptId;
    double eventT = 0.0;         // the FIRST input's event t / tick
    int64_t tick = 0;
    solver::SequenceResult result;
};

/// The facade's answer: 1 = the event was pushed, 0 = measured but not sent (the server does not
/// know the kind yet / no open session), -1 = refused (invalid payload, telemetry ring full).
struct SeqEmit {
    int status = -1;
    std::string text;            // for the job's log line
};

struct SeqCounters {
    int groups = 0;              // neighbouring inputs with finished local windows (pairs + triples)
    int pairs = 0;
    int triples = 0;
    int jobs = 0;                // jobs started
    int measured = 0;            // jobs that produced a valid result
    int emitted = 0;
    int unsent = 0;
    int refused = 0;
    int trivial = 0;             // nothing to simulate (a single-tick window, order alone decides)
    // dropped: the job started and produced nothing
    int dropControl = 0;         // the delayed replay did not reproduce the real run
    int dropNegative = 0;        // a known death did not reproduce
    int dropInvalid = 0;         // too few / too many invalid samples
    int dropExpired = 0;         // the history ring moved past the job's first snapshot
    int dropReset = 0;
    int dropLevelEnd = 0;
    // not started
    int skipAttemptCap = 0;
    int skipPosition = 0;        // this place was already measured on this level visit
    int skipNoNegative = 0;      // no member has a known death next to its window
    int skipDeath = 0;           // a (would-be) death inside the look-ahead
    int skipQueue = 0;
    int skipExpired = 0;         // waited for an idle solver until its history left the ring
    int skipReset = 0;           // still waiting when the attempt restarted / the level ended
    int skipInvalid = 0;
    int trials = 0;              // clone trials run (controls included)
    int cloneSteps = 0;
    int dropped() const { return dropControl + dropNegative + dropInvalid + dropExpired + dropReset + dropLevelEnd; }
    int notStarted() const { return skipAttemptCap + skipPosition + skipNoNegative + skipDeath + skipQueue + skipExpired + skipReset + skipInvalid; }
};

/// v0.7.0 result ledger entry (docs/TIMING_SOLVER_V2.md §3.2 result_ledger): everything the one
/// `timing_result` of a bound job needs, filled as the job and its SA job progress.
struct ResultEntry {
    // identity (bindJob)
    int64_t inputSeq = -1;
    std::string attemptId;
    double eventT = 0.0;
    int64_t tick = 0;
    bool down = true;
    uint32_t inputId = 0;
    double t = 0.0;
    int attemptInputIndex = 1;
    double x = 0.0;
    double percent = 0.0;
    double subTickMs = 0.0;
    double engineSubTickMs = 0.0;    // v0.7.1 (Fable D11)
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool flipped = false;            // gravity at the input (trace header)
    bool mini = false;
    bool subtick = false;
    bool halfTick = false;
    double pressMs = solver::kNaN;
    int64_t pressSeq = -1;
    double horizonFrame = 0.0;
    // the local window
    bool haveLocal = false;
    solver::WindowResult window;
    std::vector<solver::ShiftOutcome> outcomes;
    bool miss = false;                           // the player's miss (v0.7.1: the attributed one, Fable D6)
    bool missDownstream = false;                 // v0.7.1 (Fable D6): an earlier job of a died run
    bool extension = false;
    bool refined = false;
    bool widthBelowResolution = false;
    std::vector<solver::status::Reason> ended;   // why the job has no (usable) window
    int localTrials = 0;
    int controlSims = 0;
    bool stateReplayValid = true;
    bool replayBroken = false;                   // the replay breaker tripped while this input was being measured
    // the sequence-adjusted window
    bool saRan = false;                          // `sa` holds a result (immediate or from a job)
    solver::SAResult sa;
    solver::status::Reason saWhy = solver::status::Reason::SaNotMeasuredBudget;   // why not, when !saRan
    int saTrials = 0;
    int saControls = 0;
    double saLookAheadFrame = solver::kNaN;      // v0.7.1 (Fable D7): the SA job's look-ahead (its trials' span)
    // debug traces (solver-trace-max-ticks)
    bool traced = false;
    std::vector<solver::trace::TraceClone> traces;
    std::vector<solver::trace::SATraced> saTraces;   // the SA job's trials of this input (second block)
    bool compDone = false;                       // v0.14.0: the compensation job finished (its result / reason is stored above)
    // v0.15.0 (docs/SHIP_SOLVER.md §11): the Ship control facts of a connected-control input
    bool connected = false;                      // measured in a connected-control mode (ship): the result carries its control
    bool compPending = false;                    // its compensation job is still running
    bool phasePending = false;                   // its phase job is still running (releases)
    bool phaseRan = false;
    solver::SAResult phase;                      // the phase search's result (releases)
    solver::parity::Record parity;               // the first divergence of a replay that left the real run
};

class CloneEngine {
public:
    static CloneEngine& get();

    /// Level entry. False when clones could not be created or the snapshot self-test failed
    /// (`status()` says why); the engine then stays inactive for the level.
    bool setup(PlayLayer* pl, EngineConfig const& cfg);
    void teardown();
    /// The layer is dying without onQuit (Fields destructor): only forget pointers.
    void forgetLayer(PlayLayer* pl);
    /// resetLevel: finalise what the death pause resolved, abort the rest, restart the timeline.
    void reset();
    void configure(EngineConfig const& cfg);
    /// v0.12.0 review fix (docs/BACKGROUND_ANALYZER_DESIGN.md §5): `analysis-mode` / `record-safe`
    /// changed mid-level so that hidden clones are no longer allowed. The gate closes and every open
    /// result ends exactly as on teardown (jobs aborted, misses settled, sequences / SA aborted,
    /// results flushed), every clone and both shadows are freed - but NO PlayerObject is removed
    /// while the level runs: the idle clones stay hidden in the object layer until the normal
    /// teardown (onQuit / goEdit) or forgetLayer. The engine stays off for the rest of the visit
    /// (no step, no shadow, no input, no pool growth, the gate never re-opens); the next setup()
    /// starts clean.
    void stopForVisit(std::string const& why);
    bool offForVisit() const { return m_offForVisit; }
    /// Gate (SOLVER_DESIGN §5): while false no jobs are created; the ring keeps running.
    void setMeasuring(bool on, std::string reason);
    bool measuring() const { return m_measuring; }
    std::string const& pauseReason() const { return m_pauseReason; }

    void stepBegin(float dt, bool halfTick);   // GJBaseGameLayer::processCommands (before)
    void playerSubUpdate(float dt);            // player 1 PlayerObject::update (inside CBF's splitting)
    /// Player 1 jump press / release BEFORE it is applied (pushButton pre-hook). `pre` = the portable
    /// snapshot taken at the same moment, `percent` = PlayLayer::getCurrentPercent() there.
    /// Returns the pending job id (0 = no job, logged why).
    int input(bool down, PlayerStateSnapshot pre, bool ceilingTouch, double percent = 0.0);
    /// The telemetry input event was pushed: links the pending job to its seq. `tSubTick` = the
    /// event's sub-tick fraction [0,1) (0 without Click Between Frames).
    void bindJob(int jobId, int64_t seq, double eventT, int64_t tick, std::string attemptId, bool practice, double tSubTick = 0.0);
    void cancelJob(int jobId, char const* why);
    /// PlayLayer::destroyPlayer of the real player 1 after the original returned.
    void onRealDeath(GameObject* by, bool wouldBe);
    void onLevelComplete();
    /// v0.15.0: the level's end animation starts (PlayLayer::playEndAnimationToPos for the real
    /// player): from here on the game moves the player, not physics. Only notes the step; the open
    /// measurements are finalised at the next step boundary (finishAtLevelEnd).
    void noteLevelEnd();
    bool levelEnded() const { return m_levelEnded || m_endPending; }
    void frameEnd(float dt);                   // PlayLayer::postUpdate
    /// The real player 1 touched a portal (fingerprint portalTransition).
    void onRealPortal(int objectId);

    PlayLayer* layer() const { return m_pl; }
    bool active() const { return m_pl != nullptr; }
    bool stepping() const { return m_sim != nullptr; }
    // v0.8.0 live-state isolation (docs/LIVE_ISOLATION_DESIGN.md §2.3-§2.4): the hooks in
    // IsolationHooks.cpp answer GD's activation gates for the stepped clone from its overlay. Nothing
    // in the live level objects is written any more (the v0.5.0-v0.7.x slot swap is gone).
    uint8_t overlayGet(EnhancedGameObject* o) const;
    void overlaySet(EnhancedGameObject* o, uint8_t logical);
    uint8_t simSelfBit() const { return m_simSelfBit; }   // kSelf, or kOther while the P2 clone of a pair steps (v0.8.3)
    /// v0.8.3: the pair partner of the stepping clone (null outside a pair step): the gravity link
    /// and the portal gravity rule act on it instead of a real player (docs/LIVE_ISOLATION_DESIGN.md §2.7).
    PlayerObject* simPartner() const { return m_simPartner; }
    bool simHalfTick() const { return m_simHalfTick; }
    void noteTripwire(char const* what);
    /// v0.8.1 (step 2): a gamemode portal was replayed on the stepped clone (portal_model.hpp).
    void notePortalModelled(int objectType);
    int portalsModelled() const { return m_portalModelLevel; }
    /// v0.8.2 (step 3, docs/LIVE_ISOLATION_DESIGN.md §3): the live-state invariant guard (mode,
    /// counters, the attempt's breaker).
    solver::isolation::IsolationGuard const& isolationGuard() const { return m_guard; }
    bool isolationBreached() const { return m_guard.breached(); }
    /// v0.12.0 (docs/BACKGROUND_ANALYZER_DESIGN.md AN-D11): another analysis block of the mod (the
    /// background analyzer's read-only extraction, `who`) found a live-state difference. The engine
    /// behaves exactly as on its own breach: running clones aborted, every open result of the
    /// attempt ends live_mutation_detected, the breaker stays on until the restart, the check runs
    /// per clone step for the rest of the level visit. Never call it from inside a clone step.
    void tripIsolationBreaker(char const* who);
    PlayerObject* simClone() const { return m_sim; }
    bool isClone(PlayerObject* p) const { return p && (m_index.contains(p) || p == m_shadow.obj || p == m_shadow2.obj); }
    void cloneDied(PlayerObject* p, GameObject* by);
    void cloneInvalid(PlayerObject* p, char const* why);
    void noteAnticheatTouch();

    /// core/geometry_hash over the level objects around x (hex + uint32; empty / 0 when inactive).
    uint32_t geometryHashAt(float x, std::string* hex) const;

    /// Returns true when the window was emitted (counted as emitted; false = dropped).
    using Sink = std::function<bool(JobResult const&)>;
    void setSink(Sink sink) { m_sink = std::move(sink); }

    Counters const& counters() const { return m_counters; }
    Counters const& attemptCounters() const { return m_attempt; }
    double simMsPerFrame() const { return m_simEma; }
    double simPeakMs() const { return m_simPeakMs; }
    double stepCostUs() const { return m_stepCostUs; }
    int stepBudget() const { return m_stepBudget; }
    bool throttled() const { return m_throttled; }
    int runningClones() const;
    int openJobs() const { return static_cast<int>(m_jobs.size()); }
    int poolSize() const { return static_cast<int>(m_clones.size()); }
    int poolWarmed() const { return m_poolWarmed; }
    bool realUsesSlot2() const { return m_realUsesSlot2; }
    int realPlayerUid() const { return m_realUid; }
    int firstCloneUid() const { return m_firstCloneUid; }
    bool ringOk() const { return m_ringOk; }
    std::string const& setupError() const { return m_setupError; }
    int step() const { return m_step; }
    double frame() const { return m_frame; }
    EngineConfig const& config() const { return m_cfg; }
    /// Not-windowable inputs so far (no_pass + reset + level_end): the coverage denominator excludes them.
    int notWindowable() const { return m_counters.dropNoPass + m_counters.dropReset + m_counters.dropLevelEnd + m_counters.missDownstream; }
    double coveragePercent() const;

    // ---- sequence jobs (v0.6.1) ----
    using SeqSink = std::function<SeqEmit(SeqResult const&)>;
    void setSeqSink(SeqSink sink) { m_seqSink = std::move(sink); }
    /// Setting `measure-joint-share` (v0.7.0; `measure-sequences` in v0.6.x, default now off: the
    /// M4 joint jobs are superseded by the sequence-adjusted jobs and run only while none waits).
    void setSequences(bool on);
    bool sequencesOn() const { return m_seqOn; }
    SeqCounters const& seqCounters() const { return m_seqCounters; }
    int seqWaiting() const { return static_cast<int>(m_seqQueue.size()); }
    bool seqRunning() const { return m_seqActive; }

    // ---- v0.7.0: timing_result + sequence-adjusted windows (docs/TIMING_SOLVER_V2.md) ----
    /// Called once per bound job with its final `timing_result`; returns true when the event was
    /// pushed to the telemetry session (false: logged only, `NOT SENT` / refused).
    using ResultSink = std::function<bool(TimingResultOut const&)>;
    void setResultSink(ResultSink sink) { m_resultSink = std::move(sink); }
    /// Setting `measure-sequence-adjusted`: off = no SA job runs (results carry sa_not_measured).
    void setSequenceAdjusted(bool on);
    bool sequenceAdjustedOn() const { return m_saOn; }
    SACounters const& saCounters() const { return m_saCounters; }
    int saWaiting() const { return static_cast<int>(m_saCands.size()); }
    bool replayBroken() const { return m_replayBroken; }
    int openResults() const { return static_cast<int>(m_ledger.openCount()); }
    /// Approximate ring memory (the Snapshot array; the vectors inside grow on demand).
    double ringMegabytes() const;
    /// The telemetry seq of the input logged by the latest input() call when it got no job (a
    /// skipped input): a release's hold names its press by this seq.
    void noteLastInputSeq(int64_t seq);
    /// The last traced input (docs/TIMING_SOLVER_V2.md §2.12): what the `GPRL trace:` lines printed,
    /// for the in-game overlay (setting `solver-trace-overlay`). serial 0 = nothing traced yet.
    solver::trace::View const& lastTrace() const { return m_lastTrace; }

private:
    PlayerObject* makeClone();
    void freeClone(Clone& c);
    /// An idle clone, or a new one when the pool and the per-frame creation ration allow it
    /// (`forControl`: a pass's control is never rationed). -1 = none right now.
    int allocClone(bool forControl);
    /// Always a NEW clone (never an idle one), under the same pool / ration rules. -1 = none.
    /// The warm pool must use this: allocClone returns an existing idle clone, so a warm-up loop
    /// on allocClone never grows the pool once one clone is idle (v0.5.0-v0.6.1 froze GD there).
    int createClone(bool forControl);
    void captureFlags(float x, std::vector<std::pair<EnhancedGameObject*, uint8_t>>& out);
    void rangeFor(float x, size_t& a, size_t& b) const;
    bool selfTest(PlayerObject* p1, std::string& why);
    void trySpawn(Job& job);
    /// 1 = a clone was spawned, 0 = no clone available right now (the shift stays pending),
    /// -1 = reported NotTested (no history for that shift).
    int spawnShift(Job& job, double s, bool isControl);
    bool prunedByDeath(Job const& job, double s) const;
    /// Fable D5: core/solver/pass_planner hitPruneLimit over the job's pass-0 clones on one side
    /// (NaN = never prune that side now).
    double hitPruneLimitOf(Job const& job, bool late) const;
    void advance(int target, bool compareControl);
    int simStep(Clone& c, Job const& job, int k);
    Clone* cloneFor(PlayerObject* p);
    bool statesMatch(PlayerObject* a, PlayerObject* b, std::string& why) const;
    /// Before a clone that finished step k is compared with the real player: the real player may
    /// already carry the speed of step k + 1 (the layer applies a queued speed change before
    /// processCommands). Gives the clone that speed when its own is the one step k ran with.
    void mirrorNextStepSpeed(Clone& c);
    void shadowStep(int k, bool compare);
    void shadowResync(int k);
    // v0.8.3 dual pair shadow (docs/LIVE_ISOLATION_DESIGN.md §2.7): P1 + P2 clones stepped in GD's dual
    // order and compared with BOTH real players every step; no jobs in dual yet (shadow only)
    void pairShadowStep(int k, bool compare);
    void pairResync(int k);
    int simStepPair(Clone& c1, Clone& c2, int k);
    void simBegin(Clone& c, bool halfTick, PlayerObject* partner);
    bool jobResolved(Job const& job) const;
    void finalizePass(Job& job);
    void finalize(Job& job);
    /// The sink / counters / ledger part of finalize (v0.7.1: also for a settled miss, Fable D6).
    void emitFinalized(Job& job, JobResult& r);
    /// Fable D6: attribute each death's pending MISS jobs (core/solver/miss_attribution) once no
    /// unresolved job could still die with it; `force` = the restart / level end / teardown.
    void settleMisses(bool force);
    void dropJob(Job& job, std::string reason);
    void countDrop(std::string const& reason, std::string const& mismatch);
    void runJobs(int target, bool compareControl);
    void extensionSteps();
    void applyLateNeighbour(double frame, bool down);
    void dropUnbound();
    void abortJobs(char const* why);
    Job* findJob(int id);
    bool frameOf(int k, double& frame) const;
    void slog(int level, std::string const& line) const;
    std::string summaryLine() const;

    // sequence jobs
    void noteLocalDone(Job const& job, JobResult const& r);
    void considerGroups(uint32_t inputId);
    void queueGroup(std::vector<LocalDone> members);
    void runSequence(int target);
    bool startSequence(SeqJob& job);
    /// 1 spawned, 0 no clone right now, -1 the job's base snapshot left the ring. `clone`: an index
    /// seqAllocClone() already returned (-1 = allocate here).
    int spawnSeqTrial(SeqJob& job, SeqRole role, solver::SequenceTrial const* point, int clone = -1, solver::SATrial const* saTrial = nullptr);
    int seqAllocClone();
    /// Clone steps the sequence job may run right now (core/solver/sequence stepAllowance).
    int seqAllowance() const;
    /// True when the trial resolved (the clone is freed, the job's state advanced or `fail` set).
    bool seqAfterStep(SeqJob& job, SeqTrial& trial, bool physicsRan, std::string& fail, std::string& failKind);
    void finishSequence(SeqJob& job);
    void dropSequence(SeqJob& job, char const* kind, std::string const& why);
    void abortSequences(char const* kind);
    std::string seqName(SeqJob const& job) const;
    std::string seqSummaryLine() const;

    // v0.7.0: results, SA candidates / jobs, marks, traces (CloneEngine.cpp "v2" section)
    InputEvent* logEntry(uint32_t inputId);
    int logIndex(uint32_t inputId) const;
    void resultOpen(Job const& job);
    void resultLocal(Job& job, JobResult const& r);
    void resultDrop(Job const& job, std::string const& reason);
    void setClusterLink(int index, solver::cluster::ConnectInput const& in);
    void drainResults();
    void flushResults(solver::status::Reason local, solver::status::Reason sa);
    void emitResult(int jobId, ResultEntry& e);
    bool forcedBreakBetween(double a, double b) const;
    void saConsider(Job const& job, ResultEntry& e);
    solver::SAContext saContext(int firstIndex, int lastIndex, double maxFrame) const;
    solver::SAInput saInput(SACandidate const& c) const;
    bool saReady(SACandidate const& c) const;
    void saGiveUp(SACandidate const& c, solver::status::Reason why, int& counter);
    void saAbortAll(solver::status::Reason why);
    bool saStart();
    SANegative saNegativeFor(std::vector<SACandidate> const& members, double baseFrame, double horizonFrame) const;
    void saMembersDone(SeqJob& job, bool ok, solver::status::Reason why, bool proofFailed);
    // v0.14.0 lockstep compensation (docs/SHIP_SOLVER.md §4.3; CloneEngine.cpp "compensation" section)
    bool connectedModeOf(PlayerObject* p) const;
    solver::comp::CompConfig compConfigFor() const;
    CompJob* findComp(int jobId);
    bool phaseConsider(Job& job, ResultEntry& e);
    void compLevelEnd(int target);
    void finishAtLevelEnd(int target);
    CompJob* compCreate(Job& job);
    void compNoteInput(InputEvent const& le);
    void compNoteClones(Job& job);
    void compConsider(Job& job, ResultEntry& e);
    void compDiscard(int jobId);
    void compFree(CompJob& job);
    int spawnCompControl(CompJob& job);
    int spawnCompTrial(CompJob& job, int clone, solver::comp::CompTrial const& t);
    bool compTrialCrossed(Clone const& c) const;
    void compAfterStep(CompJob& job, CompTrialRun& run);
    void compTrialDone(CompJob& job, CompTrialRun& run, solver::comp::CompOutcome o);
    void compFinish(CompJob& job, bool ok, solver::status::Reason why, bool proofFailed);
    void compAbortAll(solver::status::Reason why);
    void runCompensation(int target);
    std::string compSummary() const;
    solver::SequenceConfig const& seqCfg(SeqKind kind) const;
    void noteShadowMismatch(double frame, bool realDeath);
    void onReplayStep(bool mismatch, std::string const& why);
    std::string resultLine(ResultEntry const& e, solver::TimingResultBuild const& b, int jobId) const;
    void traceRecord(Clone& c);
    void traceEmit(ResultEntry const& e, solver::TimingResultBuild const& b);
    // v0.8.2 live-state invariant check (docs/LIVE_ISOLATION_DESIGN.md §3; CloneEngine.cpp "isolation" section)
    void isoCapture(solver::live::Snapshot& out);
    /// Capture before a block (`step` = a clone step in every-clone-step mode). False = not checked now.
    bool isoBegin(solver::live::Snapshot& before, double& t0, bool step = false);
    /// Capture after it and compare; a difference marks the breach (isoBreach) for isoFinishBreach.
    void isoEnd(solver::live::Snapshot const& before, double t0, char const* block, Clone* trial);
    void isoBreach(std::vector<solver::live::Diff> const& diffs, int total, char const* block, Clone* trial);
    /// End of the block: abort every open sample / result of the attempt after a breach (§3.3).
    void isoFinishBreach();

    PlayLayer* m_pl = nullptr;
    PlayerObject* m_sim = nullptr;
    Clone* m_simClone = nullptr;        // the Clone record of m_sim while it steps (overlay owner)
    bool m_simHalfTick = false;         // the recorded half-tick flag of the step being replayed (postCollision hook)
    bool m_checkpointPauseLogged = false;
    int m_tripwiresAttempt = 0;         // isolation tripwires hit this attempt (first 6 logged)
    int m_tripwiresLevel = 0;
    int m_portalModelAttempt = 0;       // v0.8.1: gamemode portals replayed on clones this attempt (first 3 logged at verbosity 2)
    int m_portalModelLevel = 0;
    solver::isolation::IsolationGuard m_guard;   // v0.8.2 (§3.2): the invariant check's state machine
    solver::live::Snapshot m_isoBefore, m_isoAfter, m_isoStepBefore;
    bool m_isoFallbackLogged = false;
    bool m_isoAbortPending = false;     // a breach was found inside a block; the abort runs at the block's end
    std::string m_isoBreachLine;        // the summary line of the pending breach
    EngineConfig m_cfg;
    Sink m_sink;
    std::vector<Clone> m_clones;
    std::unordered_map<PlayerObject*, int> m_index;
    std::vector<Snapshot> m_hist;
    std::vector<EnhancedGameObject*> m_act;   // orbs, pads, portals: activation flags isolated per clone
    std::vector<float> m_actX;
    std::vector<GameObject*> m_geo;           // every non-decoration object, by x (geometry hash)
    std::vector<float> m_geoX;
    std::vector<InputEvent> m_log;
    std::vector<Job> m_jobs;
    bool m_platformer = false;
    bool m_snapAhead = false;
    bool m_throttled = false;
    bool m_frameSlow = false;
    bool m_measuring = false;
    bool m_offForVisit = false;         // stopForVisit(): off until teardown / forgetLayer / the next setup
    std::string m_pauseReason;
    bool m_ringOk = true;
    std::string m_setupError;
    bool m_realUsesSlot2 = false;   // the real player 1's unique id is not 1 (activation.hpp)
    int m_realUid = 0;
    int m_firstCloneUid = 0;
    int m_poolWarmed = 0;
    int m_createdThisFrame = 0;
    int m_step = 0;
    double m_frame = 0.0;
    float m_stepDt = 0.25f;
    float m_tickDt = 0.f;
    float m_elapsed = 0.f;
    uint32_t m_nextInputId = 1;
    int m_nextJobId = 1;
    int m_pendingJob = 0;
    double m_simMs = 0.0;
    double m_simEma = 0.0;
    double m_simFrameMs = 0.0;   // clone sim time of the rendered frame in progress
    double m_simPeakMs = 0.0;    // worst rendered frame since the last 5 s summary
    double m_stepCostUs = 0.0;   // EMA cost of one clone step (budget.hpp)
    int m_stepBudget = solver::budget::kBudget.minStepsPerFrame;   // clone steps allowed this rendered frame
    int m_simFrames = 0;
    double m_lastPerfLog = 0.0;
    double m_lastSummaryLog = 0.0;
    int m_cloneSteps = 0;
    int m_cloneStepsThisFrame = 0;
    int m_realDeathStep = -1;
    double m_realDeathFrame = 0.0;
    int m_realDeathObj = -1;
    bool m_realDead = false;
    std::vector<double> m_wouldBeDeathFrames;
    std::vector<DeathMark> m_deathMarks;                // real / would-be deaths this attempt (frame, object id, x)
    Clone m_shadow;
    Clone m_shadow2;                    // v0.8.3: the P2 clone of the pair shadow (created at the first dual step)
    bool m_pairLogged = false;
    uint8_t m_simSelfBit = solver::activation::kSelf;
    PlayerObject* m_simPartner = nullptr;
    int m_shadowLogged = 0;
    int m_anticheatTouches = 0;
    Counters m_counters;   // per level
    Counters m_attempt;    // per attempt

    // sequence jobs (v0.6.1): at most one runs at a time, candidates wait in a short queue
    SeqSink m_seqSink;
    bool m_seqOn = true;
    std::deque<LocalDone> m_seqLocals;              // finished local windows of the newest inputs (this attempt)
    std::deque<SeqJob> m_seqQueue;                  // candidates waiting for an idle solver
    SeqJob m_seq;                                   // the running job (valid while m_seqActive)
    bool m_seqActive = false;
    bool m_seqClosed = false;                       // the level was completed: no further sequence work this attempt
    // v0.15.0: the level end. m_endPending: the end animation started at m_endStep (noteLevelEnd),
    // finalisation waits for the next step boundary; m_levelEnded: done, nothing is measured until the restart
    bool m_endPending = false;
    bool m_levelEnded = false;
    int m_endStep = 0;
    Job m_seqCtx;                                   // simStep context of a sequence clone (no single moved input)
    std::unordered_set<uint64_t> m_seqSeen;         // (first input id, size) already queued this attempt
    std::unordered_map<int64_t, int> m_seqPlaces;   // measurements per place on this level visit
    int m_seqNextId = 1;
    int m_seqJobsThisAttempt = 0;
    int m_seqTriplesThisAttempt = 0;
    int m_seqStepsThisFrame = 0;
    int m_localStepsLastFrame = 0;                  // clone steps of the local jobs in the previous rendered frame
    int m_seqCreatedThisFrame = 0;
    size_t m_seqRoundRobin = 0;
    SeqCounters m_seqCounters;                      // per level

    // v0.7.0 (docs/TIMING_SOLVER_V2.md): one timing_result per bound job, SA candidates, marks
    ResultSink m_resultSink;
    solver::ResultLedger<ResultEntry> m_ledger;     // bound jobs of this attempt until their result is emitted
    solver::cluster::ClusterTracker m_clusters;     // connectedNext links of this attempt's inputs
    // v0.15.0: the timing_results of this attempt's Ship controls until both ends reported (the
    // debug card, core/solver/control_card.hpp). By control index; at most kMaxControlSlots.
    struct ControlSlot {
        std::optional<telemetry::TimingResultPayload> press, release;
    };
    std::map<int, ControlSlot> m_controlSlots;
    static constexpr size_t kMaxControlSlots = 256;
    void cardNote(TimingResultOut& out);
    void cardsFlush();
    bool m_saOn = true;
    std::vector<SACandidate> m_saCands;             // inputs whose SA window waits for a delayed-replay job
    int64_t m_saOrder = 0;
    SACounters m_saCounters;                        // per level
    std::deque<SACandidate> m_saRecent;             // recent local results (negative-control donors)
    std::vector<std::pair<double, int>> m_portalMarks;          // (frame, object id) of the real player's portals this attempt
    std::vector<std::pair<double, bool>> m_shadowMarks;         // (frame, real death not reproduced) of shadow mismatches this attempt
    solver::budget::ReplayBreaker m_breaker;
    bool m_replayBroken = false;
    std::string m_firstShadowWhy;                   // the attempt's first shadow mismatch (replay breaker line)
    int m_tracedThisAttempt = 0;
    bool m_lastInputLogged = false;                 // the latest input() call appended to m_log
    solver::trace::View m_lastTrace;                // the last traced input (overlay), kept across attempts of the level
    // v0.7.1 (the Fable review)
    double m_subTickClockMaxMs = 0.0;               // D11: max |engine - tracker| sub-tick of the level's bound inputs (5 s summary)
    /// D6: a MISS job's finished local result, held until every job that could share its death resolved.
    struct PendingMiss {
        Job job;
        JobResult r;
        double deathFrame = solver::kNaN;
    };
    std::vector<PendingMiss> m_pendingMisses;
    std::vector<std::pair<double, float>> m_speedMarks;   // D7: (frame, new speed) of the real player's speed changes this attempt
    std::unordered_map<int64_t, int> m_saPlaces;          // D10: finished SA results per position bucket this level visit
    // v0.14.0 lockstep compensation (docs/SHIP_SOLVER.md §4.3)
    std::vector<CompJob> m_compJobs;
    CompCounters m_compCounters;                          // per level
};

}  // namespace gprl::clone
