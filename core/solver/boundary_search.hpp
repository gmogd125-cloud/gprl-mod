#pragma once
// BoundarySearch (ARCHITECTURE §6, SPEC §6-§7): finds how far ONE input can be moved to one side
// (earlier or later) before the run fails, without brute force:
//
//   1. coarse scan outward from the actual input in tick steps (configurable) until the first fail
//      -> bracket [last pass, first fail]
//   2. adaptive bisection of that bracket until its width <= resolutionMs (sub-frame: nothing is
//      capped at a frame, 1.67 ms windows are legal)
//   3. bounded by a trial budget, a maximum shift, and the side limit the caller passes (history
//      end on the early side, neighbouring inputs on either side)
//
// Special cases, all reported rather than hidden:
//   - unbounded side: every trial up to the limit passes -> bounded = false, passShift = limit
//   - blocked side: the limit is smaller than one coarse step (no history / an adjacent input);
//     the search still explores up to the limit and marks blocked = true
//   - non-monotonic islands: a pass found beyond the first fail. The window reported is the
//     contiguous one from the actual input; islands are recorded (coarse resolution) for telemetry.
//
// Deterministic: the same oracle and config give the same trial sequence. Every trial is returned.
#include <string>
#include <vector>

#include "oracle.hpp"

namespace gprl::solver {

struct BoundarySearchConfig {
    std::string version = "boundary-search/0.1.0";
    double tickMs = kTickMs;          // coarse step unit
    int coarseStepTicks = 1;          // coarse step in ticks
    double resolutionMs = 0.05;       // stop bisecting when the bracket is this narrow
    double maxShiftMs = 10.0 * kTickMs;  // never move an input further than this from the actual
    int maxTrials = 40;               // trial budget per side
    int islandScanSteps = 2;          // coarse steps to keep scanning past the first fail (islands)
};

enum class Side : uint8_t { Earlier, Later };
constexpr char const* name(Side s) { return s == Side::Earlier ? "earlier" : "later"; }

enum class TrialPhase : uint8_t { Coarse, IslandScan, Bisect, Limit };
constexpr char const* name(TrialPhase p) {
    switch (p) {
        case TrialPhase::Coarse: return "coarse";
        case TrialPhase::IslandScan: return "island";
        case TrialPhase::Bisect: return "bisect";
        case TrialPhase::Limit: return "limit";
    }
    return "coarse";
}

struct Trial {
    int index = 0;
    double shiftMs = 0.0;   // signed shift from the actual input time
    double tMs = 0.0;       // absolute time the input was tried at
    Outcome outcome;
    bool pass = false;
    TrialPhase phase = TrialPhase::Coarse;
};

struct Island {
    double fromShiftMs = 0.0;   // signed, coarse resolution
    double toShiftMs = 0.0;
};

// ---- v2 edge attribution (docs/TIMING_SOLVER_V2.md §2.3, §2.4; telemetry TimingEdgeV2) ----

/// Why a side of a window ends (schema.ts TIMING_EDGE_STOPS, same order).
enum class EdgeStop : uint8_t { Fail, Range, Neighbour, History, AttemptStart, Untested, Undecided };
constexpr char const* name(EdgeStop s) {
    switch (s) {
        case EdgeStop::Fail: return "fail";
        case EdgeStop::Range: return "range";
        case EdgeStop::Neighbour: return "neighbour";
        case EdgeStop::History: return "history";
        case EdgeStop::AttemptStart: return "attempt_start";
        case EdgeStop::Untested: return "untested";
        case EdgeStop::Undecided: return "undecided";
    }
    return "range";
}

/// What caused the death that bounds a `fail` edge (schema.ts TIMING_EDGE_CAUSES): `self` = no
/// later input that stayed fixed had been applied before the death, `downstream` = one or more
/// had (the bound is a property of the fixed continuation, not of this input), `extension` = the
/// trial ran synthetic death-pause ticks in a frozen world before dying (SOLVER_DESIGN D6).
enum class EdgeCause : uint8_t { None, Self, Downstream, Extension };
constexpr char const* name(EdgeCause c) {
    switch (c) {
        case EdgeCause::Self: return "self";
        case EdgeCause::Downstream: return "downstream";
        case EdgeCause::Extension: return "extension";
        case EdgeCause::None: return "none";
    }
    return "none";
}

/// The side limits a planner knows about (the stop of a side that ran out of room).
enum class LimitKind : uint8_t { Range, Neighbour, History, AttemptStart };
constexpr EdgeStop stopOf(LimitKind k) {
    switch (k) {
        case LimitKind::Range: return EdgeStop::Range;
        case LimitKind::Neighbour: return EdgeStop::Neighbour;
        case LimitKind::History: return EdgeStop::History;
        case LimitKind::AttemptStart: return EdgeStop::AttemptStart;
    }
    return EdgeStop::Range;
}

struct EdgeAttribution {
    bool attributed = false;           // filled by PassPlanner / SAPlanner (BoundarySearch leaves it)
    EdgeStop stop = EdgeStop::Range;
    EdgeCause cause = EdgeCause::None; // stop == Fail only
    int laterInputs = -1;              // later FIXED inputs applied before that death (-1 = unknown)
    double failAfterMs = kNaN;         // death time - the unshifted input time (ms)
    int failObjectId = -1;             // killer object id, -1 = no object / level boundary
    double failDeathX = kNaN;          // the clone's x at the death
    bool extension = false;            // the failing trial finished in the death pause
};

struct BoundaryResult {
    std::string version;
    Side side = Side::Earlier;
    bool valid = true;              // false when the oracle answered Invalid (result unusable)
    std::string invalidReason;
    double limitMs = 0.0;           // |shift| limit that applied on this side
    bool blocked = false;           // limit < one coarse step (history / neighbour), see header
    bool bounded = false;           // a fail was found within the limit
    double passShiftMs = 0.0;       // signed: last known contiguous pass (the conservative edge)
    double failShiftMs = kNaN;      // signed: first known fail after bisection (NaN if unbounded)
    double bracketMs = 0.0;         // |failShift - passShift| (0 when unbounded)
    bool nonMonotonic = false;
    std::vector<Island> islands;
    bool budgetExhausted = false;
    int trialCount = 0;
    std::vector<Trial> trials;
    std::vector<std::string> debug;
    EdgeAttribution edge;           // v2: why the side ends, and what caused its bounding death
};

class BoundarySearch {
public:
    BoundarySearch(IPhysicsOracle& oracle, BoundarySearchConfig config = {});

    /// Search one side for input `movingIndex` of `schedule`. `sideLimitMs` is the largest |shift|
    /// the caller allows on this side (NaN = only the config maximum). `horizonSeconds` is passed to
    /// the oracle.
    BoundaryResult search(SnapshotId base, InputSchedule const& schedule, size_t movingIndex, Side side,
                          double sideLimitMs, double horizonSeconds);

    BoundarySearchConfig const& config() const { return m_config; }

private:
    Trial runTrial(BoundaryResult& r, SnapshotId base, InputSchedule const& schedule, size_t movingIndex,
                   double actualMs, double shiftMs, double horizonSeconds, TrialPhase phase);

    IPhysicsOracle& m_oracle;
    BoundarySearchConfig m_config;
};

}  // namespace gprl::solver
