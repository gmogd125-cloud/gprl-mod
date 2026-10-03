#pragma once
// PassPlanner (docs/SOLVER_DESIGN.md §3.4): BoundarySearch semantics as BATCHED passes, for an
// oracle that cannot answer a trial synchronously. The lockstep clone engine (src/solver) steps
// every shifted copy of an input in parallel with the real game, so instead of "trial, look at the
// answer, decide the next trial" the search is planned pass by pass:
//
//   pass 0 (coarse)   = BoundarySearch phases Coarse / IslandScan / Limit in parallel: shifts of
//                       +-k coarse steps on each side up to the side limit (history + previous
//                       neighbour on the early side, the next neighbour on the late side when it is
//                       known), plus the exact limit point when the limit is not on the grid
//   pass 1..n (refine)= only with sub-tick input placement (Click Between Frames): `pointsPerPass`
//                       shifts spread inside each side's [last pass, first fail] bracket, until the
//                       bracket is at most `resolutionFrames` or `maxRefinePasses` passes ran
//
// Outcomes are ingested per pass (`ShiftOutcome`: the shift asked for, the shift the engine
// actually applied - the step grid without CBF -, and what happened). The side walk is FPC's
// AnalysisCore `walkSide` over the latest outcome per applied shift: `lastPass` = largest shift
// passing contiguously from 0, `firstFail` = the first fail beyond it, islands recorded beyond
// the first fail, `NotTested` gaps simply widen the bracket (honest), `Invalid` invalidates.
//
// Misses (SOLVER_DESIGN D6): when the control clone died together with the real player the actual
// input failed; the window is the contiguous pass run nearest to 0 among the tested shifts (it can
// lie entirely on one side of 0) bracketed by its adjacent fails, so `result()` yields edges with
// the actual time outside [earliestMs, latestMs].
//
// `result()` produces the same `WindowResult` shape as LocalWindowSolver (conservative pass edges,
// fail edges, brackets, flags, every trial) so window_event.hpp builds the payload from either.
// Pure C++20: no Geode includes; host-tested in tests/pass_planner_tests.cpp, including the
// equivalence with BoundarySearch/LocalWindowSolver over the synthetic oracle.
#include <cstdint>
#include <string>
#include <vector>

#include "boundary_search.hpp"
#include "local_window.hpp"

namespace gprl::solver {

/// `WindowResult::invalidReason` of a window without a single tested shift (RC-minor 2): the
/// status is `unresolved` (`no_shift_tested`), never a 0 ms window.
constexpr char const* kNoShiftTested = "no_shift_tested";

struct PlannerConfig {
    int maxShiftTicks = 10;               // never move an input further than this (frames = ticks)
    int coarseStepTicks = 1;              // coarse grid step
    int islandScanSteps = 2;              // engine pruning keeps this many coarse steps beyond the first fail
    bool subtick = false;                 // sub-tick placement available (CBF active): refinement passes run
    int pointsPerPass = 7;                // refinement points per bracket per pass (7 -> width / 8)
    double resolutionFrames = 0.125;      // stop refining a bracket at this width (1/8 tick)
    int maxRefinePasses = 1;              // 1/8 tick = 1 pass, 1/64 tick = 2 passes
    double neighbourMarginFrames = 0.0024;   // LocalWindowConfig::neighbourMarginMs (0.01 ms) in frames
    // Step-grid alignment of the coarse shifts (engine-computed from the input's fractional frame,
    // 0 when the input sits on a whole tick, else the distance to the next / previous whole tick).
    // Without CBF an input can only be applied at step boundaries; aligning the grid keeps every
    // applied shift on the grid so brackets stay exactly one tick wide.
    double lateOffsetFrames = 0.0;
    double earlyOffsetFrames = 0.0;
    // v0.6.x near-shift pruning (engine `advance` + runAgainstOracle): once a smaller shift died,
    // clones more than islandScanSteps coarse steps beyond it were cancelled while still alive.
    // In a MISS the passing run lies away from 0, so its neighbours were cancelled (RC3.1). OFF in
    // v2 (V2-D8); kept behind this flag for comparison runs only. When set it wins over
    // pruneBeyondHitFail.
    bool pruneNearShifts = false;
    // v0.7.1 (Fable review D5, W5; DEV DEFAULT): the island-scan pruning comes back for HIT sides
    // only. A side is pruned beyond `firstFail + islandScanSteps` coarse steps only when the tested
    // shift nearest to 0 on that side PASSED (hitPruneLimit): the side's passing run then starts at
    // that shift and ends before its first fail, so nothing beyond the island scan can belong to
    // it - hit or miss. A side whose nearest shift died (a miss, or a one-sided hit) or is still
    // running is never pruned, so the RC3.1 miss defect cannot come back.
    bool pruneBeyondHitFail = true;
};

/// One tested shift of a side as the pruning rule sees it (|shift| and what is known so far).
enum class ProbeState : uint8_t { Running, Passed, Died, Other };
struct PruneProbe {
    double mag = 0.0;
    ProbeState state = ProbeState::Running;
};
/// Fable review D5: the |shift| beyond which the side's shifts are pointless, or NaN (never
/// prune). Only when the probe nearest to 0 (NotTested shifts are not probes) PASSED and a shift
/// of the side died: smallest died |shift| + `scanFrames`. The nearest probe still running, dead or
/// invalid -> NaN. Pure; the engine (CloneEngine advance / prunedByDeath) and runAgainstOracle use it.
double hitPruneLimit(std::vector<PruneProbe> const& side, double scanFrames);

enum class ShiftKind : uint8_t { Survived, Resynced, Died, Invalid, NotTested };
constexpr char const* name(ShiftKind k) {
    switch (k) {
        case ShiftKind::Survived: return "survived";
        case ShiftKind::Resynced: return "resynced";
        case ShiftKind::Died: return "died";
        case ShiftKind::Invalid: return "invalid";
        case ShiftKind::NotTested: return "not_tested";
    }
    return "not_tested";
}
constexpr bool passed(ShiftKind k) { return k == ShiftKind::Survived || k == ShiftKind::Resynced; }

struct ShiftOutcome {
    double nominalFrames = 0.0;    // signed shift the pass asked for
    double appliedFrames = 0.0;    // signed shift the engine actually applied (equal without rounding)
    ShiftKind kind = ShiftKind::NotTested;
    // Died: frames from the UNSHIFTED input frame to the end of the step the clone died in (the
    // log's `@N`, docs/TIMING_SOLVER_V2.md §1 RC1). Resynced: frames to the end of the streak.
    double deathAfterFrames = 0.0;
    int objectId = -1;             // Died: killer object id (-1 = boundary / unknown)
    std::string reason;            // Invalid / NotTested: why
    int pass = 0;                  // filled by ingest()
    bool extension = false;        // finished in the death pause (synthetic ticks)
    // v2 attribution (§2.3). Died: logged later inputs that were NOT moved in this trial (later
    // than the measured input's recorded frame) and that the engine applied before the death;
    // 0 = the death is this input's own (self), >= 1 = downstream. Counted by the engine's
    // simStep, never re-derived from frame arithmetic afterwards (offline rule: an input logged at
    // frame F was applied before a death reported at D iff F < D).
    int laterFixed = 0;
    double rejoinAfterFrames = kNaN;  // Resynced: first frame of the converged streak - the unshifted input frame
    double deathX = kNaN;             // Died: the clone's x at the death
};

/// What is known about one side of the window. Edges are SIGNED frames relative to the actual
/// input (early side negative for a hit); for a miss both edges may share a sign.
struct SideState {
    double limit = 0.0;            // |shift| limit that applies (frames)
    bool limitKnown = false;       // late side: false until the next same-channel input is logged
    bool blocked = false;          // limit < one coarse step
    double lastPass = 0.0;         // hit walk: |shift| passing contiguously from 0
    double firstFail = kNaN;       // hit walk: first |shift| failing beyond lastPass
    double passEdge = 0.0;         // signed conservative edge
    double failEdge = kNaN;        // signed first fail beyond the edge (NaN = unbounded)
    double coarseMax = 0.0;        // largest coarse magnitude planned on this side
    bool exhausted = false;        // every coarse point passed: unbounded up to the limit
    bool invalid = false;
    std::string invalidReason;
    bool nonMonotonic = false;
    std::vector<Island> islands;   // signed frames
    bool budgetCut = false;        // a NotTested point lies inside the bracket (pool / cancel)
    int refinePasses = 0;
    bool limitPointPlanned = false;
    // v2 (§2.4): why the side ends and what caused the bounding death (`fail` stops only)
    EdgeStop stop = EdgeStop::Range;
    EdgeCause cause = EdgeCause::None;
    int laterInputs = -1;
    double failAfterFrames = kNaN;   // death frame - the unshifted input frame
    int failObjectId = -1;
    double failDeathX = kNaN;
    bool failExtension = false;

    bool bounded() const { return !std::isnan(failEdge); }
    double widthFrames() const { return bounded() ? std::fabs(failEdge - passEdge) : 0.0; }
};

class PassPlanner {
public:
    PassPlanner() = default;
    /// `earlyLimitFrames`: distance to the oldest usable snapshot / the previous same-channel input
    /// (already minus the margin); NaN = only the config maximum.
    PassPlanner(PlannerConfig cfg, double earlyLimitFrames);

    /// The next same-channel input was logged `frames` after the actual input (already minus the
    /// margin). Shifts beyond it are never tested (the engine marks them Limit / NotTested).
    void setLateLimit(double frames);
    /// The early side could not be spawned that far back (history ring): reported as blocked.
    void markEarlyBlocked();
    /// What the early limit given to the constructor is (the edge stop when the side runs out of
    /// room before a fail): the previous input, the history ring or the attempt start. Default:
    /// History. The late limit set by setLateLimit is always the next input (Neighbour).
    void setEarlyLimitKind(LimitKind k);
    /// Misses: the control clone's own death (it died with the real player). Its `laterFixed`
    /// attributes a miss edge the control itself bounds. Optional (unknown = self).
    void setControlDeath(ShiftOutcome const& control);

    /// Shifts (signed frames) to spawn for the next pass, closest first, later / earlier
    /// alternating. Empty = nothing more to test (done). The control (shift 0) is not listed: the
    /// engine always spawns one per pass.
    std::vector<double> nextPass();
    /// Outcomes of the pass that was planned last. `controlOk` = the control clone survived the
    /// horizon and matched the real player; `controlDiedWithReal` = it died together with the real
    /// player (miss, D6). Neither -> the pass is unusable and the planner finishes invalid.
    void ingest(std::vector<ShiftOutcome> const& outcomes, bool controlOk, bool controlDiedWithReal, bool spawnComplete = true);

    /// No further passes (level ended / reset): result() reports what was ingested so far.
    void finish() { m_finished = true; }

    bool started() const { return m_passesPlanned > 0; }
    bool ingested() const { return m_passesIngested > 0; }
    bool done() const;
    bool refined() const { return m_refined; }
    bool miss() const { return m_miss; }
    bool controlFailed() const { return m_controlFailed; }
    int passesPlanned() const { return m_passesPlanned; }
    int passesIngested() const { return m_passesIngested; }
    SideState const& early() const { return m_early; }
    SideState const& late() const { return m_late; }
    std::vector<ShiftOutcome> const& outcomes() const { return m_all; }
    PlannerConfig const& config() const { return m_cfg; }

    /// WindowResult in the local_window.hpp shape, frames -> ms at 1000/240 (vocab.hpp kTickMs).
    /// v2: each side carries its EdgeAttribution (stop, cause, later inputs); a window with no
    /// tested shift on either side is invalid `no_shift_tested` (RC-minor 2), never a 0 ms window.
    WindowResult result(double actualMs) const;
    /// True when at least one shift within a side's limit has a pass / fail / invalid outcome.
    bool anyShiftTested() const;
    /// "-2D@18.0/0Ls -1A +0A +1R +3D@66.0/3Ld#1717 (2 not tested)" for the log: `@N` frames after
    /// the UNSHIFTED input, `/kL` = k later fixed inputs applied before the death, `s` / `d` = self /
    /// downstream, `e` = finished in the death pause.
    std::string describe() const;

private:
    std::vector<double> coarseMagnitudes(SideState const& side, double offset) const;
    void attribute(SideState& side, ShiftOutcome const* fail) const;
    EdgeStop openStop(SideState const& side, bool late, bool untestedBeyond) const;
    std::vector<double> planRefinement(bool commit);
    void recompute();
    void walkSide(SideState& side, bool late);
    void findMissRun();

    PlannerConfig m_cfg;
    SideState m_early, m_late;
    std::vector<ShiftOutcome> m_all;
    int m_passesPlanned = 0;
    int m_passesIngested = 0;
    bool m_refined = false;
    bool m_miss = false;
    bool m_controlFailed = false;
    bool m_controlSeen = false;
    bool m_spawnIncomplete = false;
    bool m_noPass = false;          // miss without any passing shift
    bool m_lastPassSubdivides = false;   // the pass planned last placed points inside a bracket
    bool m_finished = false;
    LimitKind m_earlyLimitKind = LimitKind::History;
    bool m_earlyHistoryMissing = false;   // markEarlyBlocked: a shift could not be spawned (ring)
    ShiftOutcome m_controlDeath;
    bool m_haveControlDeath = false;
};

/// Reference loop for host tests: drives `planner` against a pull oracle (one trial per planned
/// shift, the control at the actual time) until done. Returns the number of trials. Deaths are
/// attributed with the offline rule (§2.3): `laterFixed` = inputs of the schedule after the
/// moving one (by recorded time) whose time lies before the death. The horizon of every trial is
/// measured from the UNSHIFTED input like the engine's fixed look-ahead (SD §3.3). Pass 0 is
/// pruned like the engine: PlannerConfig::pruneBeyondHitFail (hitPruneLimit, the default) or the
/// v0.6.x rule with pruneNearShifts (comparison runs); the pull oracle knows every outcome at once,
/// so every shift beyond the limit is reported NotTested ("cancelled").
int runAgainstOracle(IPhysicsOracle& oracle, PassPlanner& planner, SnapshotId base, InputSchedule const& schedule,
                     size_t movingIndex, double horizonSeconds);

/// Offline attribution rule (§2.3): the number of inputs of `schedule` (other than `movingIndex`
/// and the indices in `moved`) recorded after the moving input whose time lies strictly before
/// `deathMs`. With whole ticks: an input logged at frame F was applied before a death reported at
/// deathFrame D iff F < D.
int laterFixedBefore(InputSchedule const& reference, size_t movingIndex, std::vector<size_t> const& moved, double deathMs);

}  // namespace gprl::solver
