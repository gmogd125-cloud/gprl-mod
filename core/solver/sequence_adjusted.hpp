#pragma once
// SEQUENCE-ADJUSTED windows `gprl-clone-sa/1` (docs/TIMING_SOLVER_V2.md §2.5-§2.9, V2-D2, V2-D3;
// AUDIT §5B, §6, §7, §10).
//
// The LOCAL window moves one input and keeps every other input exactly as performed; on wave /
// ship a death further along the corridor then bounds every input before it (RC1). The
// sequence-adjusted window of input i, shift s:
//
//   - inputs before i: fixed as performed (causal: the player cannot change the past)
//   - input i at t_i + s
//   - the next inputs of i's connected cluster may FOLLOW i by the same s (adaptation family F):
//       pair   = i+1 moved too (press: the hold is kept; release: the gap to the next press)
//       chain2 = i+1..i+2,  chain3 = i+1..i+3 (continuous-control modes)
//     later inputs outside the member's followers stay fixed; a member whose last follower
//     would cross the next fixed input is skipped (the crossed input must follow too)
//   - SA(s) = pass       local(s) passed, or some member of F re-joined / survived its look-ahead
//             fail       local(s) died and, for the member reached last, the death happened before
//                        the next input that a longer member would move could act (laterFixed == 0
//                        and t_next + s >= the death frame): no longer chain can change a death that
//                        happened before its extra follower acts - decided with no simulation; or F
//                        is exhausted (maxChain, a cluster break) and its last member died inside its
//                        adapted span (laterFixed == 0)
//             undecided  otherwise (F exhausted after a death that a non-moved input could have
//                        influenced, the trial budget, a proof that failed)
//   - the window is the contiguous `pass` run around 0 per side; a side ends at the first `fail`
//     (midpoint edge, cause self by construction), at `undecided` (conservative edge = last pass,
//     never inside the local window) or at a limit (range / history / previous input / attempt
//     start; the NEXT input is no limit while it can follow - a late shift where no member of F is a
//     legal schedule, e.g. the next input lies across a cluster break, stops at `neighbour`)
//   - the walk starts OUTSIDE the local bracket: untested shifts between the local run and the
//     local first fail are the local planner's (never simulated here), and only brackets beyond
//     the local window are refined, so W_local ⊆ W_SA holds on the reported edges
//
// W_local ⊆ W_SA,F ⊆ W_true: an existence proof, a LOWER bound on the true sequence window; a
// `fail` edge means "no continuation in F survives its adapted span", not "none exists".
//
// PAIR window (V2-D3): the input and the next one shifted together, everything else fixed, the
// local success rule, over the whole range (also inside the local window) for presses whose
// release is effective - the hold-preserving shift of the pair. It shares its trials with the SA
// walk (both are member `pair` at shift s).
//
// PURE C++20 (no Geode includes); host-tested in tests/sequence_adjusted_tests.cpp on the
// kinematic oracle (tests/kinematic_oracle.hpp). The engine (src/solver/CloneEngine) runs the
// trials as delayed replays on the M4 machinery (V2-D5); runSAAgainstOracle is the reference
// driver on the pull oracle. Every loop here is bounded (§3.7): a walk has at most
// 2 x maxShiftTicks + refinePointsSubtick slots, escalation at most maxChain steps, a member at
// most maxTrialsPerInput trials.
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "boundary_search.hpp"
#include "local_window.hpp"
#include "pass_planner.hpp"
#include "timing_status.hpp"

namespace gprl::solver {

constexpr char const* kSASolverVersion = "gprl-clone-sa/4";   // v0.14.0: every sequence window of this build (the lockstep compensation planner for connected-control modes, core/solver/compensation.hpp, and the delayed replay for the rest); /3 = v0.11.0 settled look-ahead; /2 = v0.8.0 isolated simulator

/// Every threshold of the SA solver in ONE versioned object (DEV DEFAULTS; the ready line names
/// the version). `horizonSeconds` / `maxShiftTicks` are the engine's settings at job time.
struct SAConfig {
    char const* version = kSASolverVersion;
    int maxShiftTicks = 10;               // the SA walk's range per side (the local range)
    int maxChain = 3;                     // pair, chain2, chain3
    int maxTrialsPerInput = 40;           // SA + pair trials charged to one input at most
    // the full PAIR walk of presses whose release is effective (~|W_local| + 2 extra trials per
    // press). v0.7.1 (Fable review D9, W9; DEV DEFAULT): OFF - the SA walk already runs the pair
    // trials it needs; `pair` is emitted only when those cover a contiguous run from 0 on both sides
    bool pairWalkForPresses = false;
    int maxInputsPerJob = 4;              // members of one delayed-replay job (engine chunking)
    double maxJobSpanTicks = 96.0;        // first-to-last member distance of one job
    int convergeSteps = 16;               // steps a trial must match the recorded run to count as re-joined
    double neighbourMarginFrames = 0.0024;   // two inputs keep at least this distance (0.01 ms)
    int refinePointsSubtick = 3;          // CBF: points inside each bounded SA bracket (one round)
    double priorityMaxLocalWidthTicks = 12.0;   // wider local windows are not queued (barely matter for precision)
    bool requireNegativeControl = true;   // a known lockstep death must reproduce in the delayed replay
    int queueMax = 16;                    // candidates waiting for an idle solver
    int ringReserveSteps = 64;            // history steps kept between a job's base and the ring's tail
    double horizonSeconds = 0.5;          // the engine's look-ahead (local success rule)
    // v0.7.1 (Fable review D2, W2; DEV DEFAULT): a family member's look-ahead is measured from its
    // LAST moved input (max moved frame + horizonSeconds x 240 + maxShiftTicks), so every moved
    // input gets the evidence the local window gives its one input. Off = Opus's t + s + horizon
    // (a chain3 follower 96 ticks after the member had ~34 frames of evidence).
    bool horizonFromLastMoved = true;
    // v0.7.1 (Fable review D10, W10; DEV DEFAULT): a candidate whose position bucket (the M4
    // sequence::positionKey x bucket) already has this many finished SA results this level visit
    // ranks behind every unmeasured position (cluster::saQueueRank); never skipped
    int saMaxPerPosition = 2;
};
constexpr SAConfig kSA{};

/// Fable D2 ring arithmetic (pinned in tests/tuning_tests.cpp): the longest stretch of history, in
/// frames, one SA job can need from its base snapshot to its last trial's look-ahead: the base lies
/// maxShiftTicks + 1 before the first member, the last member at most maxJobSpanTicks after it,
/// its followers inside its look-ahead (the job's context ends there: CloneEngine::saStart), and a
/// chain's look-ahead runs one horizon past its last shifted follower.
constexpr double saWorstSpanFrames(SAConfig const& c) {
    double const horizon = c.horizonSeconds * 240.0 + static_cast<double>(c.maxShiftTicks);
    return static_cast<double>(c.maxShiftTicks + 1) + c.maxJobSpanTicks + horizon + static_cast<double>(c.maxShiftTicks) + horizon;
}

/// Fable review D3b (§3.1): what proved a passing SA shift. `local`: the LOCKSTEP local job's trial
/// passed (the shift's outcome is in the local planner's list: inside the local window, or a
/// passing island); `rejoined`: an SA trial reproduced the recorded run (samePhysics for
/// convergeSteps after every moved input was applied: wave pairs by construction, §1.3);
/// `survived`: an SA trial was only alive at its look-ahead (the horizon convention; the only proof
/// ship / ufo / swing chains can give). An SA trial of the member ALONE (k = 0, at a shift the
/// lockstep job left untested) is an SA trial too: `rejoined` / `survived` by its outcome, never
/// `local` - so a `local` side never widened (the evidence gate's proofConsistent). Ordered by
/// weakness.
/// v0.14.0 (docs/SHIP_SOLVER.md §4.1): `compensated` = a chain with ADAPTED followers (the same
/// shift or independent offsets) settled in a verified lockstep simulation - the proof a flying
/// mode gives when an exact re-join is impossible; `survived` = the member alone merely alive /
/// settled at the look-ahead. Ordered by weakness (Survived is the weakest).
enum class SAProof : uint8_t { Local = 0, Rejoined = 1, Compensated = 2, Survived = 3 };
constexpr char const* name(SAProof p) {
    switch (p) {
        case SAProof::Local: return "local";
        case SAProof::Rejoined: return "rejoined";
        case SAProof::Compensated: return "compensated";
        case SAProof::Survived: return "survived";
    }
    return "local";
}

/// Family member: how many following inputs are moved with the measured one.
/// v0.14.0 (docs/SHIP_SOLVER.md §4.1): Comp1..Comp3 = k followers moved by INDEPENDENT offsets
/// (the lockstep compensation planner, core/solver/compensation.hpp); Pair / Chain2 / Chain3 keep
/// their meaning (every follower by the member's shift).
enum class SAAdaptation : uint8_t { Local = 0, Pair = 1, Chain2 = 2, Chain3 = 3, Comp1 = 4, Comp2 = 5, Comp3 = 6 };
constexpr char const* name(SAAdaptation a) {
    switch (a) {
        case SAAdaptation::Local: return "local";
        case SAAdaptation::Pair: return "pair";
        case SAAdaptation::Chain2: return "chain2";
        case SAAdaptation::Chain3: return "chain3";
        case SAAdaptation::Comp1: return "comp1";
        case SAAdaptation::Comp2: return "comp2";
        case SAAdaptation::Comp3: return "comp3";
    }
    return "local";
}
/// The uniform adaptation of k moved followers (1..3), and the compensated one.
constexpr SAAdaptation uniformAdaptation(int k) { return k <= 1 ? SAAdaptation::Pair : k == 2 ? SAAdaptation::Chain2 : SAAdaptation::Chain3; }
constexpr SAAdaptation compensatedAdaptation(int k) { return k <= 1 ? SAAdaptation::Comp1 : k == 2 ? SAAdaptation::Comp2 : SAAdaptation::Comp3; }
constexpr bool isCompensated(SAAdaptation a) { return a == SAAdaptation::Comp1 || a == SAAdaptation::Comp2 || a == SAAdaptation::Comp3; }

/// One logged input of the job's span (the attempt's input log, time order).
struct SAContextInput {
    uint32_t id = 0;
    double frame = 0.0;         // recorded frame (fractional with CBF)
    bool down = true;
    bool breakBefore = false;   // a cluster break between the previous logged input and this one
                                // (gamemode / gravity / size / dual / teleport portal, a death,
                                // measured connectedNext == false): never moved as a follower
};
struct SAContext {
    std::vector<SAContextInput> inputs;   // ascending frames; every member is one of them
};

/// One measured input (a member of the job) with its finished LOCAL window.
struct SAInput {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = true;
    bool subtick = false;                 // Click Between Frames: sub-tick placement + SA refinement
    double earlyLimitFrames = kNaN;       // |shift| limit on the early side (history / previous input), NaN = range
    LimitKind earlyLimitKind = LimitKind::History;
    // v0.7.1 (MOD verifier): the LOCAL planner's late limit (the next input minus the margin), NaN =
    // range. Only the local run / bracket the SA walk starts from is read within it (the local
    // window's own view, PassPlanner::walkSide), never the SA slots: a late clone beyond the limit
    // that died before the next input was logged stays Died in the outcome list, and read as a local
    // bracket it made a `neighbour` SA side inherit a failMs the local window does not have (D1)
    double lateLimitFrames = kNaN;
    std::vector<ShiftOutcome> local;      // the local job's per-shift outcomes (applied shifts, laterFixed, deaths)
    bool pairWalk = false;                // measure the full PAIR window (a press whose release is effective)
};

/// A shift combination to run as a delayed replay (engine) or a pull-oracle trial (tests).
struct SAMoved {
    uint32_t id = 0;
    bool down = true;
    double frame = 0.0;         // where the shifted copy is applied
};
struct SATrial {
    int id = -1;
    int member = 0;
    double shiftFrames = 0.0;
    SAAdaptation adaptation = SAAdaptation::Pair;
    std::vector<SAMoved> moved;         // the member itself first, then its followers
    double lookAheadFrame = 0.0;        // pass when alive here with every moved input applied (or re-joined earlier)
    double attributeAfterFrame = 0.0;   // laterFixed counts non-moved inputs recorded after this frame
};

struct SAOutcome {
    enum class Kind : uint8_t { Pass, Died, Invalid };
    int id = -1;
    Kind kind = Kind::Invalid;
    bool rejoined = false;       // Pass: re-joined the recorded run (else survived the look-ahead)
    double deathFrame = kNaN;    // Died: end of the step the clone died in (absolute frame)
    int laterFixed = 0;          // Died: non-moved later inputs applied before the death
    int objectId = -1;
    double deathX = kNaN;
    bool extension = false;
    std::string reason;          // Invalid: why
};

/// One side of an SA / pair window, relative to the member's recorded frame.
struct SAEdge {
    double passFrames = 0.0;     // signed: last passing shift of the contiguous run
    double failFrames = kNaN;    // signed: first failing shift beyond it (NaN = not bounded)
    EdgeStop stop = EdgeStop::Range;
    EdgeCause cause = EdgeCause::None;
    int laterInputs = -1;
    double failAfterFrames = kNaN;   // death frame - the member's recorded frame
    int failObjectId = -1;
    double failDeathX = kNaN;
    bool refined = false;        // the bracket was refined below one tick (CBF)
    // v0.7.1 (Fable review D1, W1): a side that ended without an SA fail (undecided / a limit) at
    // the LOCAL window's last pass inherits the local bracket: passFrames / failFrames are the
    // local edge's exact values, `stop` stays what ended the SA walk (not `fail`, no cause), and the
    // reported edge is the local midpoint - so sequence ⊇ local holds on the emitted numbers too
    bool inherited = false;
    // v0.7.1 (Fable review D3b): the weakest proof among the passes that widened this side beyond
    // the local edge (`local` when it never widened), and the last pass contiguous from the local
    // edge whose proof is `local` or `rejoined` (signed; == passFrames unless proof is `survived`)
    SAProof proof = SAProof::Local;
    double provenPassFrames = kNaN;
    // v0.14.0 (docs/SHIP_SOLVER.md §4.1): the follower offsets (frames, relative to each follower's
    // recorded frame) of the pass that set this edge's last pass; empty when that pass was local
    std::vector<double> followerOffsetsFrames;

    bool bounded() const { return stop == EdgeStop::Fail && !std::isnan(failFrames); }
    /// A [pass, fail] bracket the reported edge is the midpoint of: an SA fail, or the inherited local one.
    bool hasBracket() const { return !std::isnan(failFrames) && (stop == EdgeStop::Fail || inherited); }
    /// The edge the window reports (§2.9, SD D8): midpoint of a bracket, else the pass.
    double edgeFrames() const { return hasBracket() ? 0.5 * (passFrames + failFrames) : passFrames; }
};

struct SAWindow {
    bool present = false;
    SAEdge early, late;
    int trials = 0;
    double resolutionFrames = 1.0;   // widest bounded bracket, one tick when none
};

struct SAResult {
    bool valid = false;
    SAWindow sequence;
    bool decided = false;                     // both sides ended at a fail or a limit
    bool sideDecided[2] = {false, false};     // early, late
    std::vector<SAAdaptation> adaptationUsed; // members that produced passes beyond the local window
    int undecidedShifts = 0;
    SAWindow pair;                            // present only when its walk completed
    int trials = 0;                           // SA + pair trials charged to this member (boundary simulations)
    status::Reason failure = status::Reason::SaUndecided;   // why a side is undecided
    bool isolated = false;                    // no later input inside the look-ahead: W_SA = W_local
    bool openRange = false;                   // Fable D3a: both local sides open to the search limit: W_SA = W_local, decided
    bool survivedOnly = false;                // Fable D3b: a side widened on survived-only passes (proof `survived`)
    std::vector<std::string> debug;
};

class SAPlanner {
public:
    SAPlanner() = default;
    SAPlanner(SAConfig const& cfg, std::vector<SAInput> members, SAContext ctx);

    int members() const { return static_cast<int>(m_members.size()); }
    SAInput const& member(int i) const { return m_members[static_cast<size_t>(i)].in; }
    /// Some decision needs a simulation (else every result is final at construction).
    bool needsTrials() const { return !m_queue.empty() || m_outstanding > 0; }
    bool memberNeedsTrials(int i) const;
    /// Fable D3a: member i's local window is open on both sides to the SA search limit (no SA work).
    bool memberOpenRange(int i) const { return i >= 0 && i < members() && m_members[static_cast<size_t>(i)].openRange; }

    /// Up to `max` trials to run next. Empty = nothing to hand out right now.
    std::vector<SATrial> nextBatch(int max);
    void ingest(SAOutcome const& outcome);
    /// No further trials (the job ended, expired, was dropped): every side still waiting for a
    /// simulation ends `undecided` with `why` (a sequence_dependent reason), never guessed.
    void finish(status::Reason why);
    bool done() const;
    bool finished() const { return m_finished; }
    bool hasPending() const { return !m_finished && !m_queue.empty(); }
    int outstanding() const { return m_outstanding; }
    int requested() const { return m_requested; }
    /// Fable D2: an upper bound of every trial's lookAheadFrame this planner can ever hand out (the
    /// job's look-ahead: its controls must prove the replay that far). Bounded by members x maxChain.
    double lookAheadBound() const;

    SAResult result(int member) const;
    /// "late +3 pair P +4 chain2 P +5 local D/0 -> fail | early -2 pair P -3 pair D/1 chain2 P ..." for the log.
    std::string describe(int member) const;

private:
    enum class St : uint8_t { Open, Pass, Fail, Undecided };
    struct Slot {
        double mag = 0.0;
        bool refine = false;
        St st = St::Open;
        SAAdaptation used = SAAdaptation::Local;
        SAProof proof = SAProof::Local;   // Pass: what proved it (D3b)
        bool haveDeath = false;
        double deathFrame = kNaN;
        int laterFixed = 0;
        int objectId = -1;
        double deathX = kNaN;
        bool extension = false;
        status::Reason why = status::Reason::SaUndecided;
    };
    struct Walk {
        std::vector<Slot> slots;
        size_t frontier = 0;
        bool ended = false;
        EdgeStop stop = EdgeStop::Range;
        size_t endIndex = 0;          // the slot that ended the walk (fail / undecided), == size at a limit
        bool refinePlanned = false;
        size_t refineFailIndex = 0;   // the whole-tick fail a refinement round brackets
    };
    struct Member {
        SAInput in;
        int ctxIndex = -1;
        std::array<double, 5> nextFrame{};      // frame of the 1st..4th input after the member (index 1..4), NaN = none
        std::array<bool, 5> followerOk{};       // reachable as a follower (no break up to it)
        std::array<uint32_t, 5> nextId{};
        std::array<bool, 5> nextDown{};
        Walk sa[2];                             // 0 = early (negative shifts), 1 = late
        Walk pair[2];
        std::vector<double> lattice[2];         // the tick lattice per side (D9: `pair` from the SA walk's own pair trials)
        bool pairWalk = false;
        int trials = 0;
        bool budgetHit = false;
        bool isolated = false;
        bool openRange = false;                 // Fable D3a: both local sides open to the SA limit
        double limit[2] = {0.0, 0.0};
        // the LOCAL window's contiguous pass run and the far end of its bracket per side (|frames|,
        // NaN = open): the SA walk starts OUTSIDE the local bracket (§2.9) so W_local ⊆ W_SA holds
        // on the reported edges, and it never refines a bracket the local planner owns
        double localRun[2] = {0.0, 0.0};
        double localFail[2] = {kNaN, kNaN};
    };
    using Key = std::tuple<int, int, int64_t>;   // member, adaptation, shift key
    struct Cache {
        bool requested = false;
        bool known = false;
        SAOutcome out;
    };
    enum class Need : uint8_t { Known, Waiting, Budget, Impossible };

    static int64_t shiftKey(double s) { return static_cast<int64_t>(std::llround(s * 1e6)); }
    bool crosses(Member const& m, int k, double s) const;
    /// Some member of F (local, or the member + 1..maxChain followers) is a legal schedule at s:
    /// its last moved input does not cross the next fixed one and no follower lies across a break.
    bool anyLegalMember(Member const& m, double s) const;
    ShiftOutcome const* localAt(Member const& m, double s) const;
    Need need(int mi, int k, double s, SAOutcome const** out);
    void advanceAll();
    void advanceWalk(int mi, int side);
    void advancePair(int mi, int side);
    St decide(int mi, int side, Slot& slot, double s);
    static void endWalk(Walk& w, EdgeStop stop, size_t at);
    SAEdge edgeOf(Member const& m, Walk const& w, int side, bool pairWalk) const;
    /// Fable D9: one side of the PAIR window rebuilt from the pair trials the SA walk ran (cache,
    /// read-only); `complete` false when a lattice point from 0 outward has no known pair outcome.
    Walk pairFromCache(int mi, int side, bool& complete, int& trials) const;

    SAConfig m_cfg{};
    std::vector<Member> m_members;
    SAContext m_ctx;
    std::map<Key, Cache> m_cache;
    std::map<int, Key> m_trialKey;
    std::deque<SATrial> m_queue;
    int m_nextId = 0;
    int m_outstanding = 0;
    int m_requested = 0;
    bool m_finished = false;
    status::Reason m_finishReason = status::Reason::SaUndecided;
};

/// Frames of the local window of a WindowResult side (for the W_local ⊆ W_SA edge rule).
double localEdgeFrames(BoundaryResult const& side);

/// Reference driver for host tests: builds the members' SAInputs from their local planners (run
/// with runAgainstOracle first), the context from `schedule`, and runs every SA trial against the
/// pull oracle (one trial per planned schedule) until done. `members` = schedule indices (the
/// measured inputs), `breaks` = schedule indices with a cluster break before them. Deaths are
/// attributed with the offline rule (laterFixedBefore). Returns the number of oracle trials.
struct SARunInput {
    SAConfig config;
    std::vector<size_t> members;
    std::vector<PassPlanner const*> locals;   // one per member, finished
    std::vector<bool> pairWalk;               // per member (empty = presses whose next input exists)
    std::vector<size_t> breaks;
    bool subtick = false;
};
int runSAAgainstOracle(IPhysicsOracle& oracle, SnapshotId base, InputSchedule const& schedule, SARunInput const& in, SAPlanner& out);

/// The engine's SA spawn loop (CloneEngine::runSequence, §3.7 T-PROG-1), pure so it is host-tested:
/// at most `parallel - running` iterations; each one either spawns exactly one planner trial (the
/// planner pops it) or breaks. `alloc()` returns a clone index or -1 (none right now: nothing is
/// taken from the planner); `spawn(idx, trial)` returns false when the clone could not be started,
/// then the trial is ingested as Invalid ("could not be spawned") and the loop stops. A clone index
/// obtained without a trial is left idle. Returns the number of trials spawned.
template <class Alloc, class Spawn>
int spawnSATrials(SAPlanner& planner, int running, int parallel, Alloc&& alloc, Spawn&& spawn) {
    int spawned = 0;
    for (int guard = 0; guard < parallel && running + spawned < parallel && planner.hasPending(); ++guard) {
        int idx = alloc();
        if (idx < 0) break;
        auto batch = planner.nextBatch(1);
        if (batch.empty()) break;
        if (!spawn(idx, batch.front())) {
            SAOutcome o;
            o.id = batch.front().id;
            o.kind = SAOutcome::Kind::Invalid;
            o.reason = "could not be spawned";
            planner.ingest(o);
            break;
        }
        ++spawned;
    }
    return spawned;
}

}  // namespace gprl::solver
