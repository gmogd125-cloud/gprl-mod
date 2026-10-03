#pragma once
// The ONE place frames / milliseconds / seconds convert (docs/TIMING_SOLVER_V2.md §2.1, V2-D10;
// AUDIT §8, §9). TypeScript counterpart: shared/src/level-analysis/canonical-window.ts
// `canonicalWindowView` ({ms, seconds, frames240}) on top of shared/src/telemetry/units.ts.
//
//   frame   = one 240 Hz physics tick. Shifts, windows and look-aheads are frames inside the
//             engine and milliseconds (double) on the wire: ms = frames x 1000/240, never rounded
//             before the final display.
//   window  = ONE canonical double, windowMs = latestMs - earliestMs. Every display value
//             (seconds, 240 FPS frames, the "4.00 f" text) is generated from it by the functions
//             below; nothing stores seconds or frames next to milliseconds.
//   240 FPS frames are a DISPLAY conversion only: nothing in the solver is capped at one frame.
//
// PURE C++20 (no Geode includes); host-tested in tests/timing_units_tests.cpp.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../vocab.hpp"

namespace gprl::solver::units {

/// The three display fields of one window, all generated from the same canonical milliseconds.
struct CanonicalWindow {
    double windowMs = 0.0;
    double windowSeconds = 0.0;
    double equivalentFrames240 = 0.0;
};

/// canonicalWindow(ms) = { windowMs = ms, windowSeconds = ms / 1000, equivalentFrames240 = ms x 240 / 1000 }.
inline CanonicalWindow canonicalWindow(double ms) {
    CanonicalWindow w;
    w.windowMs = ms;
    w.windowSeconds = ms / 1000.0;
    w.equivalentFrames240 = ms * kTicksPerSecond / 1000.0;
    return w;
}

/// Engine frames (240 Hz ticks, fractional with Click Between Frames) -> milliseconds.
inline double framesToMs(double frames) { return frames * kTickMs; }
/// Milliseconds -> 240 FPS frames (display).
inline double msToFrames240(double ms) { return ms * kTicksPerSecond / 1000.0; }
/// Seconds -> 240 FPS frames (display): frames = seconds x 240 (AUDIT §8).
inline double secondsToFrames240(double seconds) { return seconds * kTicksPerSecond; }
/// A fraction of one tick (the input event's tSubTick, [0,1)) -> milliseconds.
inline double subTickMs(double tickFraction) { return tickFraction * kTickMs; }

/// Canonical input time on the attempt axis: the input event's t x 1000 plus its sub-tick part
/// (0 without Click Between Frames). The same axis for timing_window and timing_result (RC-minor 6).
inline double actualMs(double eventT, double subTickMsValue) { return eventT * 1000.0 + subTickMsValue; }
/// Fable review D11 (W8): the ENGINE's own sub-tick of an input at `frame` (its frame since the
/// attempt start, CloneEngine::input): frac(frame) x 1000/240 in [0, 1000/240). 0 for a whole-tick
/// input (every owner session so far), 2.083 ms for a half tick. Every shift of the input's windows
/// is measured from that frame, so this is the sub-tick part of the windows' actualMs.
inline double engineSubTickMs(double frame) {
    if (!std::isfinite(frame)) return 0.0;
    double f = frame - std::floor(frame);
    if (f < 1e-9 || f > 1.0 - 1e-9) return 0.0;
    return f * kTickMs;
}

/// v0.7.1: the float noise of a window width. A timing_window's edges are actualMs + midpoint, so
/// at a level time of seconds `latest - earliest` misses a one-tick bracket by an ulp
/// (4.166666666666629 ms against 4.166666666666667 ms in the owner's 19.47.37 log: 18 of 21
/// `window_below_resolution` gate refusals there): such a window is NOT narrower than its
/// resolution. 1e-6 ms is far below any real lattice step (1/64 tick = 0.065 ms).
constexpr double kWidthNoiseMs = 1e-6;
inline bool widthBelowResolution(double widthMs, double resolutionMs) { return widthMs < resolutionMs - kWidthNoiseMs; }

/// level_time_seconds from a LEVEL tick (the attempt tick plus the StartPos tick, server side)
/// and the sub-tick part: (levelTick - 1) / 240 + subTickMs / 1000 (§2.2: every Deadlocked input
/// event of the v0.6.2 log satisfies tick = t x 240 + 1).
inline double levelTimeSeconds(int64_t levelTick, double subTickMsValue) {
    return (static_cast<double>(levelTick) - 1.0) / kTicksPerSecond + subTickMsValue / 1000.0;
}

/// Individual timing D50 (AUDIT §13): D50 = 1.34898 / W_seconds; at 240 FPS about 323.76 / frames.
/// NaN for a non-positive width (no timing can be "infinitely precise").
inline double d50(double windowMs) {
    if (!(windowMs > 0.0) || !std::isfinite(windowMs)) return std::nan("");
    return 1.34898 / (windowMs / 1000.0);
}

/// "16.67 ms (4.00 f)" - the text form of a canonical window (two decimals each; three for ms
/// below one millisecond so CBF-scale windows stay readable).
inline std::string windowText(double ms) {
    if (!std::isfinite(ms)) return "-";
    auto w = canonicalWindow(ms);
    char buf[64];
    if (std::fabs(ms) < 1.0) std::snprintf(buf, sizeof buf, "%.3f ms (%.2f f)", w.windowMs, w.equivalentFrames240);
    else std::snprintf(buf, sizeof buf, "%.2f ms (%.2f f)", w.windowMs, w.equivalentFrames240);
    return buf;
}

/// "4.00 f" - 240 FPS frames of a canonical window, two decimals.
inline std::string framesText(double ms) {
    if (!std::isfinite(ms)) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f f", canonicalWindow(ms).equivalentFrames240);
    return buf;
}

}  // namespace gprl::solver::units
