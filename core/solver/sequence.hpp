#pragma once
// Sequence windows (MASTER §5 "sequence difficulty", SPEC §8; docs/SOLVER_DESIGN.md §13).
//
// A LOCAL window moves one input while every other input stays exactly as performed. That says
// nothing about what happens when two neighbouring inputs are BOTH off: a 3-tick press followed by
// a 3-tick release does not mean every (press, release) combination inside the 3 x 3 box passes -
// the feasible region in (s1, s2) shift space can be a diagonal band (the hold length matters, not
// the two times). The SEQUENCE solver measures that region for a pair / triple of neighbouring
// inputs and reports
//
//     jointFeasibleShare = measure(feasible region inside the box of local windows)
//                          / product of the local window widths          ("independence ratio")
//
// 1 = the inputs are independent (every combination of individually valid timings works),
// smaller = the sequence is tighter than its local windows suggest.
//
// This file is the PURE part (C++20, no Geode / cocos includes; host-tested in
// tests/sequence_tests.cpp on a synthetic oracle with known joint feasibility):
//   - SequenceConfig     every threshold of the sequence solver in ONE object with a version
//                        (`gprl-clone-seq/1`, the `solverVersion` of `sequence_window` events)
//   - SequenceAxis       one input's local window as a lattice of ATOMS (shift positions with the
//                        measure each stands for): whole passing ticks without sub-tick placement,
//                        an even subdivision of the window with Click Between Frames
//   - SequencePlanner    grid-then-refine sampling of the shift lattice as BATCHES (the lockstep
//                        clone engine cannot answer a trial synchronously, like PassPlanner):
//                        1. a coarse node grid per axis that always contains the first, the last
//                           and the actual (shift 0) atom,
//                        2. boxes between neighbouring nodes; a box whose corners disagree is cut
//                           in two at its middle atom, along the axis its corners disagree on most,
//                           the largest box first, until it is one atom wide or the sample cap is hit,
//                        3. the share is the weighted mean over ALL atoms: sampled atoms count as
//                           measured; an atom inside a box whose corners disagree takes the side of
//                           0.5 the corners' multilinear interpolation falls on at its position.
//                        Points that need no simulation are never simulated: a combination with at
//                        most one shifted input is a local trial (known to pass), and a combination
//                        that would swap the order of two inputs is infeasible by definition.
//   - SequenceSolver     the reference implementation on the pull oracle (IPhysicsOracle): local
//                        windows with LocalWindowSolver, then the planner. The runtime path is
//                        src/solver/CloneEngine (sequence jobs), which drives the same planner.
//   - buildSequenceEvent the telemetry payload (`sequence_window`) + the C++ mirror of the
//                        server's evidence gate (api/src/processing/sequence.ts).
//
// What it does NOT claim: a passing region that lies entirely inside a box whose corners all fail
// (or a hole inside an all-pass box) is not seen. `resolutionMs` is how precisely the BOUNDARY of
// the feasible region is located (the widest edge of a box whose corners disagree, of the coarse
// grid when none does); the blind spot for isolated features is the coarse grid spacing.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../telemetry.hpp"
#include "local_window.hpp"

namespace gprl::solver {

constexpr char const* kSequenceSolverVersion = "gprl-clone-seq/2";   // /2: the isolated engine (v0.8.x, docs/LIVE_ISOLATION_DESIGN.md §5); /1 withdrawn server-side
/// Inputs per sequence_window event (schema.ts SEQUENCE_WINDOW_LIMITS).
constexpr int kSequenceMinInputs = 2;
constexpr int kSequenceMaxInputs = 3;

/// Every threshold of the sequence solver (one object, versioned). DEV DEFAULTS until in-game logs
/// say otherwise; the Geode log names the version in the solver's ready line.
struct SequenceConfig {
    char const* version = kSequenceSolverVersion;
    // ---- grouping: which inputs form a sequence (neighbours in the input log, same attempt) ----
    int maxGroupSize = 3;               // 2 = pairs only, 3 = pairs and triples
    double maxGapTicks = 24.0;          // two inputs further apart than this (100 ms) are not one sequence
    double maxTripleSpanTicks = 36.0;   // first-to-last distance of a triple (150 ms)
    // ---- lattice ----
    int nodesPerAxisPair = 5;           // coarse grid: nodes per axis (first / actual / last always among them)
    int nodesPerAxisTriple = 3;
    int maxAtomsPerAxisPair = 21;       // sub-tick axes: atoms per axis at most (whole-tick axes keep every passing tick)
    int maxAtomsPerAxisTriple = 9;
    double neighbourMarginFrames = 0.0024;   // two inputs keep at least this distance (LocalWindowConfig 0.01 ms)
    // ---- sampling ----
    int maxSamplesPair = 48;            // simulated trials per job at most (grid + refinement; controls not counted)
    int maxSamplesTriple = 64;
    int maxRefineRounds = 12;           // a round = one cut of every open box (each waits for its new corners)
    int minSamples = 1;                 // fewer valid simulated trials = nothing measured (not emitted)
    double maxInvalidShare = 0.25;      // more invalid trials than this share = the job is dropped
    // ---- budget: "only when the solver is idle, lowest priority" (engine) ----
    double frameShare = 0.35;           // sequence clone steps per rendered frame <= this share of the step budget
    double targetMsPerFrame = 0.5;      // ... and <= this much clone sim per rendered frame at the measured step cost
    double idleLoadShare = 0.6;         // and only while the local jobs' expected steps + the sequence's stay under this share
    int stepsPerClonePerStep = 16;      // a lagging sequence clone advances at most this many steps per real step
    int parallelClones = 4;             // sequence clones running at once
    int poolReserve = 24;               // idle clones always left for the local jobs (a whole coarse pass + control)
    int cloneCreatesPerFrame = 1;       // PlayerObject::create for sequences per rendered frame
    int convergeSteps = 16;             // steps a trial must match the real run to count as re-joined (CONVERGE_STEPS)
    int ringReserveSteps = 64;          // history steps kept between a job's first snapshot and the ring's tail
    int queueMax = 8;                   // candidates waiting for an idle solver
    // ---- caps ----
    int maxJobsPerAttempt = 12;
    int maxTriplesPerAttempt = 4;
    int maxPerPosition = 2;             // measurements of the same place (first input's x bucket + size) per level visit
    double positionBucketUnits = 30.0;  // one block
    bool requireNegativeControl = true; // a known local death must reproduce in the delayed replay, else no job
};

constexpr SequenceConfig kSequence{};

/// What the sequence solver needs from one input's finished LOCAL window.
struct LocalWindowInfo {
    bool valid = false;
    std::string invalidReason;
    bool hit = false;                    // the actual input lies inside its window
    double earlyMs = 0.0;                // emitted edges relative to the actual input (midpoint of a bounded
    double lateMs = 0.0;                 //   side's bracket, else the last passing shift): <= 0 and >= 0 for a hit
    double widthMs = 0.0;                // lateMs - earlyMs: the width the timing_window event carries
    double resolutionMs = 0.0;           // widest bracket of the bounded sides, one tick when none
    std::vector<double> passShiftsMs;    // applied shifts that passed inside the window (without 0), ascending
    bool hasFail = false;                // a death next to the window: the negative control
    double failShiftMs = kNaN;
    int failObjectId = -1;
};

/// Edges exactly as core/solver/window_event builds them (D8), plus the passing shifts and the
/// nearest known death. Invalid for a window result that is not usable.
LocalWindowInfo localWindowInfo(WindowResult const& w);

/// One input of the group as a lattice of atoms along its shift axis.
struct SequenceAxis {
    std::vector<double> positions;   // signed shift of each atom from the actual input (frames), ascending
    std::vector<double> weights;     // measure (frames) each atom stands for; the sum is the local window width
    int origin = 0;                  // index of the atom at shift 0 (the input as performed)
    double earlyFrames = 0.0;
    double lateFrames = 0.0;
    double localWidthMs = 0.0;       // the width the input's timing_window event carries
    bool continuous = false;         // sub-tick placement (atoms subdivide the window) vs whole passing ticks
    bool valid = false;
    std::string invalidReason;

    int size() const { return static_cast<int>(positions.size()); }
    double widthFrames() const { return lateFrames - earlyFrames; }
};

/// `continuous` false: atoms = 0 and every passing applied shift (whole ticks); true: 0 plus an
/// even subdivision of each side, at most `maxAtoms` atoms, never finer than the local resolution.
/// Weights are the Voronoi cells of the atoms clipped to the window, so they add up to its width.
SequenceAxis makeAxis(LocalWindowInfo const& info, bool continuous, int maxAtoms);

/// Coarse node indices of an axis with `atoms` atoms: first, origin, last, then the middle of the
/// widest remaining gap until `nodes` are chosen (ascending, no duplicates).
std::vector<int> sequenceNodes(int atoms, int origin, int nodes);

using SequencePoint = std::array<int, kSequenceMaxInputs>;   // atom index per axis (unused axes 0)

/// One shift combination to simulate.
struct SequenceTrial {
    int id = -1;                                            // lattice index (what ingest() takes back)
    SequencePoint atom{};
    std::array<double, kSequenceMaxInputs> shiftFrames{};   // signed shift of each input from its actual time
};

struct SequenceOutcome {
    int id = -1;
    bool pass = false;
    bool invalid = false;   // the trial could not be run (teleport / dual portal, history lost, no clone)
};

struct SequenceResult {
    std::string solverVersion;
    bool valid = false;
    std::string invalidReason;
    bool trivial = false;            // no combination needed a simulation (nothing was measured)
    int inputs = 0;
    std::vector<double> localWidthsMs;
    double jointFeasibleShare = 0.0; // [0,1]
    double productMeasure = 0.0;     // ms^n: the product of the local widths
    double jointMeasure = 0.0;       // ms^n: share * product
    double resolutionMs = 0.0;       // boundary precision: widest edge of a box whose corners disagree (of any box when none does)
    int samples = 0;                 // simulated trials with a pass / fail outcome
    int passed = 0;
    int failed = 0;
    int invalidTrials = 0;
    int knownLocal = 0;              // lattice points known from the local windows (never simulated)
    int crossed = 0;                 // lattice points rejected because two inputs would swap order
    int latticePoints = 0;
    int boxes = 0;
    int mixedBoxes = 0;
    int refineRounds = 0;
    bool budgetExhausted = false;    // the sample cap stopped the refinement
    bool complete = false;           // every box with disagreeing corners is one atom wide
    std::vector<std::string> debug;
};

class SequencePlanner {
public:
    SequencePlanner() = default;
    /// `timesFrames`: the actual frame of each input, ascending (the order constraint).
    SequencePlanner(SequenceConfig const& cfg, std::vector<SequenceAxis> axes, std::vector<double> timesFrames);

    bool valid() const { return m_valid; }
    std::string const& invalidReason() const { return m_invalidReason; }
    /// Nothing to simulate at all (an axis with a single atom, or every off-axis point crossed).
    bool trivial() const { return m_valid && m_requested == 0; }

    /// Up to `maxTrials` combinations to simulate next. Empty = nothing to hand out right now:
    /// either outcomes are outstanding, or the planner is done().
    std::vector<SequenceTrial> nextBatch(int maxTrials);
    void ingest(SequenceOutcome const& outcome);
    /// No further trials (time / history ran out): result() reports what is known.
    void finish();
    bool done() const;
    /// A trial could be handed out right now (the engine reserves a clone only then).
    bool hasPending() const { return m_valid && !m_finished && (m_queueHead < m_queue.size() || (m_outstanding == 0 && !m_done)); }
    int outstanding() const { return m_outstanding; }
    int requested() const { return m_requested; }

    SequenceResult result() const;
    std::vector<SequenceAxis> const& axes() const { return m_axes; }
    int inputs() const { return m_n; }
    /// The shift of a lattice point (frames per input).
    std::array<double, kSequenceMaxInputs> shiftsOf(int id) const;
    /// "sim 22 (pass 14, fail 8), local 9, crossed 3 of 81 points; 12 boxes (4 mixed), 2 rounds".
    std::string describe() const;
    /// Pairs only: one text row per atom of the second input (latest first), one character per
    /// atom of the first: '#' pass, '.' fail, 'x' order crossed, 'o' known from the local windows,
    /// '!' invalid, ' ' not sampled. Empty for triples.
    std::vector<std::string> map() const;

private:
    enum class Kind : uint8_t { Unknown, Pass, Fail, Crossed, Invalid, KnownPass };
    struct Box {
        SequencePoint lo{};
        SequencePoint hi{};
        int level = 0;
        // The box this one was cut from (split()). A refinement round only starts once every
        // corner of every leaf is decided, so when a job is cut short (finish()) the only leaves
        // with an undecided corner are the children of the LAST round, and their parent's corners
        // are all decided: result() evaluates such a child on its parent (the cut is undone).
        bool child = false;
        SequencePoint parentLo{};
        SequencePoint parentHi{};
    };

    int flat(SequencePoint const& p) const;
    SequencePoint unflat(int id) const;
    bool crossedPoint(SequencePoint const& p) const;
    bool onAxis(SequencePoint const& p) const;
    static bool passKind(Kind k) { return k == Kind::Pass || k == Kind::KnownPass; }
    static bool failKind(Kind k) { return k == Kind::Fail || k == Kind::Crossed; }
    void corners(Box const& b, std::vector<int>& out) const;
    bool mixed(Box const& b) const;
    bool splittable(Box const& b) const;
    /// The axis to cut a box along: the splittable axis its corners disagree on most (ties: the
    /// longer one in atoms, then the first).
    int splitAxis(Box const& b) const;
    void split(Box const& b, int axis, Box& low, Box& high) const;
    double boxMeasure(Box const& b) const;
    void request(int id);
    void planRefinement();
    int sampleCap() const;

    SequenceConfig m_cfg{};
    std::vector<SequenceAxis> m_axes;
    std::vector<double> m_times;
    int m_n = 0;
    std::array<int, kSequenceMaxInputs> m_dim{1, 1, 1};
    int m_total = 0;
    std::vector<Kind> m_kind;
    std::vector<uint8_t> m_asked;        // 1 = handed to the queue (a simulated point)
    std::vector<int> m_queue;            // lattice points still to hand out, in order
    size_t m_queueHead = 0;
    std::vector<Box> m_leaves;
    int m_requested = 0;
    int m_outstanding = 0;
    int m_rounds = 0;
    bool m_budget = false;
    bool m_finished = false;
    bool m_done = false;
    bool m_valid = false;
    std::string m_invalidReason;
};

// ---- engine-side arithmetic as pure functions (host-tested) ----

namespace sequence {

/// The group of `size` neighbouring inputs ending at input `last` of `frames` (ascending input
/// frames of one attempt): indices `last - size + 1 .. last` when every neighbouring gap is at
/// most maxGapTicks (and a triple spans at most maxTripleSpanTicks); empty otherwise.
std::vector<int> groupEndingAt(std::vector<double> const& frames, int last, int size, SequenceConfig const& cfg = kSequence);

/// Clone steps the sequence job may run right now ("only when the solver is idle"):
///   own   its share of the rendered frame's step budget, never more than targetMsPerFrame of
///         clone sim at the measured cost per step (`stepCostUs`, <= 0 = not measured yet);
///   idle  what stays under the idle share once the LOCAL jobs have what they are expected to
///         need this frame: the larger of what they used so far and what they used in the
///         previous frame (a frame has several physics steps; the sequence job runs after the
///         local jobs of each step and must not eat what they need in the frame's later steps).
inline int stepAllowance(int frameBudget, int localUsedThisFrame, int localUsedLastFrame, int sequenceUsedThisFrame, double stepCostUs,
                         SequenceConfig const& cfg = kSequence) {
    double cap = cfg.frameShare * static_cast<double>(frameBudget);
    if (stepCostUs > 0.0) {
        double byTime = cfg.targetMsPerFrame * 1000.0 / stepCostUs;
        if (byTime < cap) cap = byTime;
    }
    int local = localUsedThisFrame > localUsedLastFrame ? localUsedThisFrame : localUsedLastFrame;
    int own = static_cast<int>(cap) - sequenceUsedThisFrame;
    int idle = static_cast<int>(cfg.idleLoadShare * static_cast<double>(frameBudget)) - local - sequenceUsedThisFrame;
    int v = own < idle ? own : idle;
    return v > 0 ? v : 0;
}

/// Whether the measured load leaves room for sequence work at all. `simEmaMs` is the engine's 1 s
/// average of clone sim per rendered frame, the number that pauses NEW local measurements at
/// `throttleOnMs` (BudgetConfig) - and it contains the sequence job's own time. Sequence work is
/// only allowed while that average plus the sequence job's whole per-frame time
/// (targetMsPerFrame) is still under the throttle: a second in which a sequence job ran can then
/// only end above the throttle when the local jobs' own load of that second was above it. (With
/// the step budget at its minimum - a machine several times slower than the measured one - 60 %
/// of the budget is more than the throttle allows; stepAllowance alone would not see that.)
inline bool loadAllows(double simEmaMs, double throttleOnMs, SequenceConfig const& cfg = kSequence) {
    return simEmaMs + cfg.targetMsPerFrame < throttleOnMs;
}

/// Whether a sequence trial may take a clone: an idle one only while more than the reserve stays
/// idle for the local jobs; a new one only under the pool cap minus the reserve and the per-frame
/// rations (its own, and never on top of a frame in which the local jobs used theirs up).
enum class CloneGrant : uint8_t { None, Idle, Create };
inline CloneGrant cloneGrant(int idleClones, int poolSize, int maxClones, int createdThisFrame, int sequenceCreatedThisFrame,
                             int localCreatesPerFrame, SequenceConfig const& cfg = kSequence) {
    if (idleClones > cfg.poolReserve) return CloneGrant::Idle;
    if (poolSize + cfg.poolReserve >= maxClones) return CloneGrant::None;
    if (sequenceCreatedThisFrame >= cfg.cloneCreatesPerFrame || createdThisFrame >= localCreatesPerFrame) return CloneGrant::None;
    return CloneGrant::Create;
}

/// Per-level key of "the same place": the first input's x bucket and the group size.
inline int64_t positionKey(double x, int groupSize, SequenceConfig const& cfg = kSequence) {
    double bucket = cfg.positionBucketUnits > 0.0 ? x / cfg.positionBucketUnits : x;
    int64_t b = static_cast<int64_t>(bucket >= 0.0 ? bucket : bucket - 1.0);
    return b * 8 + static_cast<int64_t>(groupSize);
}

}  // namespace sequence

// ---- reference solver on the pull oracle ----

struct SequenceSolverConfig {
    SequenceConfig sequence;
    LocalWindowConfig local;
    bool continuous = true;   // BoundarySearch bisects below a tick: the axes subdivide the windows
};

struct SequenceSolveResult {
    SequenceResult result;
    std::vector<WindowResult> localWindows;
    std::vector<size_t> inputIndices;
    int trials = 0;           // joint trials asked of the oracle (control included)
};

class SequenceSolver {
public:
    SequenceSolver(IPhysicsOracle& oracle, SequenceSolverConfig config = {}) : m_oracle(oracle), m_config(std::move(config)) {}

    /// Joint window of the inputs `group` (indices into the schedule, ascending in time). Local
    /// windows first (every other input fixed), then the lattice. Invalid when a member has no
    /// usable local window, is a miss, or the unshifted schedule does not pass.
    SequenceSolveResult solve(SnapshotId base, InputSchedule const& schedule, std::vector<size_t> const& group);

    SequenceSolverConfig const& config() const { return m_config; }

private:
    IPhysicsOracle& m_oracle;
    SequenceSolverConfig m_config;
};

// ---- telemetry ----

struct SequenceEventResult {
    bool ok = false;
    std::string error;
    telemetry::SequenceWindowPayload payload;
};

/// `inputSeqs`: the telemetry seq of each input event, in time order. Fails instead of producing
/// a payload the validator would reject.
SequenceEventResult buildSequenceEvent(SequenceResult const& r, std::vector<int64_t> const& inputSeqs);

/// C++ mirror of api/src/processing/sequence.ts evaluateSequenceEvidence (without the allowlist
/// and the cross-checks against stored rows): the reasons the server would not use the payload.
struct SequenceCheckParams {
    double maxLocalWidthMs = 2000.0;
    int maxSamples = 4096;
    double maxResolutionMs = 100.0;
};
struct SequenceCheck {
    bool accepted = true;
    std::vector<std::string> reasons;
};
SequenceCheck checkSequencePayload(telemetry::SequenceWindowPayload const& p, SequenceCheckParams const& params = {});

}  // namespace gprl::solver
