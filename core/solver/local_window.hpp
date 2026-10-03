#pragma once
// LocalWindowSolver / HoldRangeSolver (ARCHITECTURE §6, SPEC §6, §8).
//
// A LOCAL window moves ONE input (a press or a release) while every other input stays exactly as
// performed. The result is stored in milliseconds; frames at 240 FPS are a display conversion
// (framesAt240 / formatWindow). The moved input never crosses its neighbouring inputs of the same
// player + button (a press cannot move past the previous release), and never before the oracle's
// history start: both become side limits for BoundarySearch and are reported as `blocked`.
//
// HoldRangeSolver: min/max hold duration with the press fixed (moves the release), and the mirror
// case with the release fixed (moves the press).
#include <string>
#include <vector>

#include "boundary_search.hpp"

namespace gprl::solver {

struct LocalWindowConfig {
    std::string version = "local-window/0.1.0";
    BoundarySearchConfig search;
    double horizonSeconds = 0.5;      // how long a shifted copy must survive (or resync)
    double neighbourMarginMs = 0.01;  // keep this much distance from an adjacent same-button input
};

struct WindowResult {
    std::string solverVersion;
    bool valid = true;
    std::string invalidReason;
    InputKind kind = InputKind::Press;
    double actualMs = 0.0;
    // Conservative pass edges (absolute ms). The true edge lies within [earliestFailMs, earliestMs]
    // and [latestMs, latestFailMs]; NaN fail edge = unbounded on that side.
    double earliestMs = 0.0;
    double latestMs = 0.0;
    double earliestFailMs = kNaN;
    double latestFailMs = kNaN;
    bool boundedEarly = false;
    bool boundedLate = false;
    bool blockedEarly = false;
    bool blockedLate = false;
    double resolutionMs = 0.0;   // widest bracket of the two sides
    bool nonMonotonic = false;
    int trials = 0;
    bool budgetExhausted = false;
    BoundaryResult early;
    BoundaryResult late;
    std::vector<std::string> debug;

    double widthMs() const { return latestMs - earliestMs; }
    double widthFrames240() const { return framesAt240(widthMs()); }
};

struct HoldRangeResult {
    std::string solverVersion;
    bool valid = true;
    std::string invalidReason;
    double pressMs = 0.0;
    double releaseMs = 0.0;
    double actualHoldMs = 0.0;
    double minHoldMs = 0.0;      // shortest hold that still passes (conservative)
    double maxHoldMs = 0.0;      // longest hold that still passes (conservative)
    bool boundedMin = false;
    bool boundedMax = false;
    bool movedRelease = true;    // true: press fixed, release moved; false: release fixed, press moved
    double resolutionMs = 0.0;
    int trials = 0;
    WindowResult window;         // the underlying local window of the moved input
    std::vector<std::string> debug;
};

class LocalWindowSolver {
public:
    LocalWindowSolver(IPhysicsOracle& oracle, LocalWindowConfig config = {});

    /// Local window of input `movingIndex`. The early limit is the oracle's history start for
    /// `base` and the neighbouring same-button inputs on both sides.
    WindowResult solve(SnapshotId base, InputSchedule const& schedule, size_t movingIndex);

    LocalWindowConfig const& config() const { return m_config; }

private:
    IPhysicsOracle& m_oracle;
    LocalWindowConfig m_config;
};

class HoldRangeSolver {
public:
    HoldRangeSolver(IPhysicsOracle& oracle, LocalWindowConfig config = {});

    /// Press at `pressIndex` fixed, release at `releaseIndex` moved.
    HoldRangeResult solveMovingRelease(SnapshotId base, InputSchedule const& schedule, size_t pressIndex, size_t releaseIndex);
    /// Release fixed, press moved.
    HoldRangeResult solveMovingPress(SnapshotId base, InputSchedule const& schedule, size_t pressIndex, size_t releaseIndex);

private:
    HoldRangeResult solveImpl(SnapshotId base, InputSchedule const& schedule, size_t pressIndex, size_t releaseIndex, bool moveRelease);

    LocalWindowSolver m_windows;
    LocalWindowConfig m_config;
};

/// Side limits derived from the schedule: distance to the previous / next input of the same
/// player + button (minus the margin). NaN when there is none.
double earlierNeighbourLimitMs(InputSchedule const& schedule, size_t index, double marginMs);
double laterNeighbourLimitMs(InputSchedule const& schedule, size_t index, double marginMs);

/// "1.67 ms = 0.40 frames @240" style display string (SPEC §6).
std::string formatWindow(double widthMs);

}  // namespace gprl::solver
