#pragma once
// WindowResult -> telemetry TimingWindowPayload (docs/SOLVER_DESIGN.md §3.5, D8).
//
//   edges        midpoint of each bounded side's bracket (FPC convention, NaN semantics): a frame
//                perfect reports 4.17 ms [-2.08, +2.08]; an unbounded side keeps the conservative
//                pass edge
//   tickMs       exactly 1000/240 (vocab.hpp kTickMs); the planner already converted frames
//   actualMs     the input event's t * 1000 plus its sub-tick part (0 without Click Between Frames;
//                timing_units actualMs, RC-minor 6), windows.ts tolerates < 1 tick + 1 ms
//   resolutionMs the widest bracket of the bounded sides, else one tick. v2 (V2-D8): NEVER clamped
//                to the width - a window narrower than its own bracket is reported as it is
//                (`widthBelowResolution`, the timing is low_confidence) and the server's
//                `window_below_resolution` gate decides the player path
//   hold fields  releases only: (earliestMs - pressMs, latestMs - pressMs) as Nullable values;
//                presses (and releases whose press is unknown) leave them Absent
//   solverVersion "gprl-clone/2", "gprl-clone/2-cbf" when a sub-tick pass refined the window
//                (v0.7.0: MISS windows and resolutions changed meaning, docs/TIMING_SOLVER_V2.md §3.5)
//   fingerprint  the caller's TimingFingerprint with windowMs = latestMs - earliestMs (bit-exact)
//
// checkWindowPayload mirrors api/src/processing/windows.ts so host tests prove every payload the
// mod emits passes the server's geometry / consistency gate. Pure C++20.
#include <string>
#include <vector>

#include "../fingerprint.hpp"
#include "../telemetry.hpp"
#include "local_window.hpp"

namespace gprl::solver {

constexpr char const* kSolverVersion = "gprl-clone/5";       // v0.14.0: lockstep compensation for connected-control modes (docs/SHIP_SOLVER.md); /4 = v0.11.0 settled look-ahead; /3 = v0.8.0 isolated simulator
constexpr char const* kSolverVersionCbf = "gprl-clone/5-cbf";
inline char const* solverVersionFor(bool refined) { return refined ? kSolverVersionCbf : kSolverVersion; }

struct WindowEventContext {
    int64_t inputSeq = 0;
    InputKind kind = InputKind::Press;
    double actualMs = 0.0;          // the input event's t * 1000 + sub-tick ms (timing_units actualMs)
    double pressMs = kNaN;          // release: its press's actualMs (NaN = unknown -> hold Absent)
    bool refined = false;           // at least one sub-tick pass refined the window
};

struct WindowEventResult {
    bool ok = false;
    std::string error;
    telemetry::TimingWindowPayload payload;
    double widthMs = 0.0;
    bool hit = false;               // actualMs inside [earliestMs, latestMs]
    bool widthBelowResolution = false;   // bounded window narrower than its resolution (low confidence)
    std::vector<std::string> debug;
};

/// Builds the payload. Fails (ok = false) instead of producing a non-finite or inverted window.
WindowEventResult buildWindowEvent(WindowResult const& w, WindowEventContext const& ctx, TimingFingerprint fingerprint);

/// C++ mirror of windows.ts evaluateWindowEvidence (without the allowlist lookup): the reasons
/// the server would reject the payload. `eventT` is the event's `t` (seconds).
struct WindowCheckParams {
    double minResolutionMs = 0.01;
    double maxResolutionMs = 4.2;
    double maxBoundedWindowMs = 2000.0;
    double actualVsLevelTimeToleranceMs = kTickMs + 1.0;
    double fingerprintWindowToleranceMs = 0.01;
};
struct WindowCheck {
    bool accepted = true;
    std::vector<std::string> reasons;
};
WindowCheck checkWindowPayload(telemetry::TimingWindowPayload const& p, double eventT, WindowCheckParams const& params = {});

}  // namespace gprl::solver
