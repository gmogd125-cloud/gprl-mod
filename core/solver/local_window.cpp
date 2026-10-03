#include "local_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gprl::solver {

namespace {

bool sameChannel(ScheduledInput const& a, ScheduledInput const& b) {
    return a.player == b.player && a.button == b.button;
}

double minNonNan(double a, double b) {
    if (std::isnan(a)) return b;
    if (std::isnan(b)) return a;
    return std::min(a, b);
}

}  // namespace

double earlierNeighbourLimitMs(InputSchedule const& schedule, size_t index, double marginMs) {
    if (index >= schedule.inputs.size()) return kNaN;
    auto const& me = schedule.inputs[index];
    double best = kNaN;
    for (size_t i = 0; i < schedule.inputs.size(); ++i) {
        if (i == index) continue;
        auto const& o = schedule.inputs[i];
        if (!sameChannel(me, o) || o.tMs > me.tMs) continue;
        double lim = me.tMs - o.tMs - marginMs;
        best = std::isnan(best) ? lim : std::min(best, lim);
    }
    return std::isnan(best) ? kNaN : std::max(0.0, best);
}

double laterNeighbourLimitMs(InputSchedule const& schedule, size_t index, double marginMs) {
    if (index >= schedule.inputs.size()) return kNaN;
    auto const& me = schedule.inputs[index];
    double best = kNaN;
    for (size_t i = 0; i < schedule.inputs.size(); ++i) {
        if (i == index) continue;
        auto const& o = schedule.inputs[i];
        if (!sameChannel(me, o) || o.tMs < me.tMs) continue;
        double lim = o.tMs - me.tMs - marginMs;
        best = std::isnan(best) ? lim : std::min(best, lim);
    }
    return std::isnan(best) ? kNaN : std::max(0.0, best);
}

std::string formatWindow(double widthMs) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f ms = %.2f frames @240", widthMs, framesAt240(widthMs));
    return buf;
}

LocalWindowSolver::LocalWindowSolver(IPhysicsOracle& oracle, LocalWindowConfig config)
    : m_oracle(oracle), m_config(std::move(config)) {}

WindowResult LocalWindowSolver::solve(SnapshotId base, InputSchedule const& schedule, size_t movingIndex) {
    WindowResult w;
    w.solverVersion = m_config.version + "+" + m_config.search.version;
    if (movingIndex >= schedule.inputs.size()) {
        w.valid = false;
        w.invalidReason = "movingIndex out of range";
        return w;
    }
    auto const& in = schedule.inputs[movingIndex];
    w.kind = in.down ? InputKind::Press : InputKind::Release;
    w.actualMs = in.tMs;

    // Early side: history start and previous same-button input. Late side: next same-button input.
    double histStart = m_oracle.historyStartMs(base);
    double histLimit = std::isnan(histStart) ? 0.0 : std::max(0.0, in.tMs - histStart);
    double earlyLimit = minNonNan(histLimit, earlierNeighbourLimitMs(schedule, movingIndex, m_config.neighbourMarginMs));
    double lateLimit = laterNeighbourLimitMs(schedule, movingIndex, m_config.neighbourMarginMs);
    {
        char buf[160];
        std::snprintf(buf, sizeof buf, "actual %.4f ms, early limit %.4f ms (history %.4f), late limit %s", in.tMs, earlyLimit,
                      histLimit, std::isnan(lateLimit) ? "config" : "neighbour");
        w.debug.emplace_back(buf);
    }

    BoundarySearch search(m_oracle, m_config.search);
    w.early = search.search(base, schedule, movingIndex, Side::Earlier, earlyLimit, m_config.horizonSeconds);
    w.late = search.search(base, schedule, movingIndex, Side::Later, lateLimit, m_config.horizonSeconds);
    w.trials = w.early.trialCount + w.late.trialCount;
    if (!w.early.valid || !w.late.valid) {
        w.valid = false;
        w.invalidReason = !w.early.valid ? w.early.invalidReason : w.late.invalidReason;
        return w;
    }
    w.earliestMs = in.tMs + w.early.passShiftMs;
    w.latestMs = in.tMs + w.late.passShiftMs;
    w.earliestFailMs = std::isnan(w.early.failShiftMs) ? kNaN : in.tMs + w.early.failShiftMs;
    w.latestFailMs = std::isnan(w.late.failShiftMs) ? kNaN : in.tMs + w.late.failShiftMs;
    w.boundedEarly = w.early.bounded;
    w.boundedLate = w.late.bounded;
    w.blockedEarly = w.early.blocked;
    w.blockedLate = w.late.blocked;
    w.resolutionMs = std::max(w.early.bracketMs, w.late.bracketMs);
    w.nonMonotonic = w.early.nonMonotonic || w.late.nonMonotonic;
    w.budgetExhausted = w.early.budgetExhausted || w.late.budgetExhausted;
    for (auto const& d : w.early.debug) w.debug.push_back("early: " + d);
    for (auto const& d : w.late.debug) w.debug.push_back("late: " + d);
    w.debug.push_back("window " + formatWindow(w.widthMs()));
    return w;
}

HoldRangeSolver::HoldRangeSolver(IPhysicsOracle& oracle, LocalWindowConfig config)
    : m_windows(oracle, config), m_config(std::move(config)) {}

HoldRangeResult HoldRangeSolver::solveMovingRelease(SnapshotId base, InputSchedule const& schedule, size_t pressIndex, size_t releaseIndex) {
    return solveImpl(base, schedule, pressIndex, releaseIndex, true);
}

HoldRangeResult HoldRangeSolver::solveMovingPress(SnapshotId base, InputSchedule const& schedule, size_t pressIndex, size_t releaseIndex) {
    return solveImpl(base, schedule, pressIndex, releaseIndex, false);
}

HoldRangeResult HoldRangeSolver::solveImpl(SnapshotId base, InputSchedule const& schedule, size_t pressIndex, size_t releaseIndex, bool moveRelease) {
    HoldRangeResult h;
    h.solverVersion = "hold-range/0.1.0+" + m_config.version;
    h.movedRelease = moveRelease;
    if (pressIndex >= schedule.inputs.size() || releaseIndex >= schedule.inputs.size()) {
        h.valid = false;
        h.invalidReason = "index out of range";
        return h;
    }
    auto const& press = schedule.inputs[pressIndex];
    auto const& release = schedule.inputs[releaseIndex];
    if (!press.down || release.down || !sameChannel(press, release) || release.tMs < press.tMs) {
        h.valid = false;
        h.invalidReason = "pressIndex/releaseIndex are not a press followed by its release";
        return h;
    }
    h.pressMs = press.tMs;
    h.releaseMs = release.tMs;
    h.actualHoldMs = release.tMs - press.tMs;
    h.window = m_windows.solve(base, schedule, moveRelease ? releaseIndex : pressIndex);
    h.trials = h.window.trials;
    if (!h.window.valid) {
        h.valid = false;
        h.invalidReason = h.window.invalidReason;
        return h;
    }
    if (moveRelease) {
        // hold = release - press; earlier release = shorter hold
        h.minHoldMs = h.window.earliestMs - press.tMs;
        h.maxHoldMs = h.window.latestMs - press.tMs;
        h.boundedMin = h.window.boundedEarly;
        h.boundedMax = h.window.boundedLate;
    }
    else {
        // hold = release - press; later press = shorter hold
        h.minHoldMs = release.tMs - h.window.latestMs;
        h.maxHoldMs = release.tMs - h.window.earliestMs;
        h.boundedMin = h.window.boundedLate;
        h.boundedMax = h.window.boundedEarly;
    }
    h.resolutionMs = h.window.resolutionMs;
    char buf[160];
    std::snprintf(buf, sizeof buf, "hold %.4f ms actual, valid [%.4f, %.4f] ms (%s moved)", h.actualHoldMs, h.minHoldMs, h.maxHoldMs,
                  moveRelease ? "release" : "press");
    h.debug.emplace_back(buf);
    return h;
}

}  // namespace gprl::solver
