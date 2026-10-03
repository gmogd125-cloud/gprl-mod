#pragma once
// Timeline bookkeeping of the lockstep clone engine as PURE functions (docs/SOLVER_DESIGN.md §3.1,
// FPC Engine::stepBegin / Engine::input / trySpawn math), so the frame arithmetic the windows
// depend on is host-tested (tests/timeline_tests.cpp) without the game.
//
// Time axis: a "frame" is one 240 Hz physics tick. GD runs a tick as two half ticks when a queued
// input lands inside it (m_isBetweenSteps), so every GJBaseGameLayer::processCommands call is one
// STEP of 1 or 0.5 frames (0 when physics did not run, e.g. the death pause). Steps index the
// history ring; frames are the unit of shifts, windows and the look-ahead. PlayerObject::update(dt)
// takes dt in 1/60 s units (0.25 per tick, GD_PHYSICS_NOTES.md).
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../vocab.hpp"
#include "budget.hpp"

namespace gprl::solver::timeline {

// budget/3 (v0.7.0): 2048 steps, 2 steps per tick worst case -> 4.3 s of history (8.5 s at one
// step per tick). v0.6.x: 1024. The one definition lives in budget.hpp.
constexpr int kHistorySteps = budget::kHistorySteps;
constexpr double kTickUpdateDt = 0.25;     // PlayerObject::update units of one 240 TPS tick

/// Ring slot of a step (steps start at 1; negative / zero handled).
constexpr int ringIndex(int step, int size = kHistorySteps) { return ((step % size) + size) % size; }

/// Frames of a step that just ended: 0 when physics did not run (elapsed ~ 0), else 0.5 for a GD
/// half tick and 1 otherwise (FPC stepBegin "prev.frames").
inline double closedStepFrames(bool halfTick, double elapsed) {
    if (elapsed <= 1e-7) return 0.0;
    return halfTick ? 0.5 : 1.0;
}

/// Tick delta bookkeeping of stepBegin: the delta of a full tick learned from the step that just
/// ran (a whole-tick step's elapsed, or twice a half tick's when nothing is known yet).
inline double learnTickDt(double tickDt, bool prevHalfTick, bool prevKnown, double elapsed) {
    if (!(elapsed > 1e-6 && elapsed < 0.5)) return tickDt;
    if (prevKnown && !prevHalfTick) return elapsed;
    if (tickDt <= 0.0) return prevHalfTick ? elapsed * 2.0 : elapsed;
    return tickDt;
}

/// Where a real input lands on the frame axis (FPC Engine::input). `elapsed` = player-update
/// deltas already run in the current step when pushButton is called.
struct InputPlacement {
    double t = 0.0;            // frames, fractional inside the step (sub-tick form)
    int step = 0;              // step the input belongs to (the next one when betweenSteps)
    double frac = 0.0;         // fraction of the step already simulated
    bool betweenSteps = false; // the step's physics already ran: the input applies before the next step
    double expect = 0.0;       // expected delta of the step
};

inline InputPlacement placeInput(int step, double stepFrame, double stepFrames, bool halfTick, double tickDt, double stepDt, double elapsed) {
    InputPlacement p;
    p.expect = tickDt > 0.0 ? (halfTick ? tickDt * 0.5 : tickDt) : stepDt;
    p.frac = p.expect > 0.0 ? std::clamp(elapsed / p.expect, 0.0, 1.0) : 0.0;
    p.betweenSteps = p.expect > 0.0 && elapsed >= p.expect * 0.999;
    if (p.betweenSteps) {
        p.step = step + 1;
        p.t = stepFrame + stepFrames;
        p.frac = 0.0;
    }
    else {
        p.step = step;
        p.t = stepFrame + p.frac * stepFrames;
    }
    return p;
}

/// Coarse-grid alignment of the shifts (PlannerConfig late/earlyOffsetFrames): 0 when the input
/// sits on a whole tick, else the distance to the next (late) / previous (early) whole tick.
struct GridOffsets {
    double late = 0.0;
    double early = 0.0;
};
inline GridOffsets gridOffsets(double t) {
    double frac = t - std::floor(t);
    if (frac < 1e-6 || frac > 1.0 - 1e-6) return {0.0, 0.0};
    return {1.0 - frac, frac};
}

/// Horizon frame of a job (FPC: t + horizon seconds * 240 + max shift).
inline double horizonFrame(double t, double horizonSeconds, int maxShiftTicks) {
    return t + horizonSeconds * kTicksPerSecond + static_cast<double>(maxShiftTicks);
}

inline double framesToMs(double frames) { return frames * kTickMs; }
inline double msToFrames(double ms) { return ms / kTickMs; }

/// The step whose snapshot a shifted copy starts from (FPC trySpawn walk): from `fromStep` walk
/// back while the step's frame is above `targetFrame`. `frameOf(k, frame)` returns false when
/// step k is not in the ring. Returns -1 when the history does not reach back that far.
template <class FrameOf>
int stepForFrame(int fromStep, double targetFrame, FrameOf&& frameOf) {
    int k0 = fromStep;
    while (true) {
        double f = 0.0;
        if (!frameOf(k0, f)) return -1;
        if (f <= targetFrame + 1e-9) return k0;
        double fp = 0.0;
        if (k0 - 1 < 1 || !frameOf(k0 - 1, fp)) return -1;
        --k0;
    }
}

/// Where the clone's own (shifted) input is applied in step [f0, f0 + fr) (FPC simStep): with
/// sub-tick placement anywhere inside the step, otherwise at the nearest step boundary.
struct InputApply {
    bool now = false;      // applied in this step
    double frac = 0.0;     // fraction of the step (0 = at the start)
};
inline InputApply applyInStep(double inputFrame, double f0, double fr, bool subtick) {
    InputApply a;
    if (fr <= 0.0) return a;
    double pos = (inputFrame - f0) / fr;
    if (subtick) {
        a.now = pos < 1.0 - 1e-6;
        a.frac = std::clamp(pos, 0.0, 1.0);
    }
    else a.now = pos < 0.5 - 1e-6;
    return a;
}

}  // namespace gprl::solver::timeline
