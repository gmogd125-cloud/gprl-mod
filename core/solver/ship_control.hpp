#pragma once
// SHIP CONTROLS (docs/SHIP_SOLVER.md §11.3; owner prompt "SHIP COUNTING / COMPENSATED TIMING
// WINDOW REWRITE" §2-§4, 2026-10-03): a ship is flown with HOLDS. One press and its matching
// release are ONE control unit with
//
//   a hold start      the press
//   a hold end        the release
//   a hold duration   release - press
//
// and a control can be mistimed in three ways that are not independent precision events:
//
//   PHASE      the whole hold earlier / later, its duration kept      (measured by the phase
//              search: the compensation planner with the release as the press's RIGID partner)
//   DURATION   the hold shorter / longer, its start kept              (= the release's own window:
//              the `hold` range of its result)
//   START      the press earlier / later with the release where it was (= the press's own window)
//
// This header only PAIRS the logged inputs of one attempt into controls (pure, host-tested in
// tests/ship_control_tests.cpp); the windows come from core/solver/compensation.hpp.
//
// Pairing rules, one button of one player, time order:
//   - a press opens a control; the next release closes it
//   - a release with no open control (the button was held when the attempt started, e.g. a
//     StartPos) is a control of its own without a press
//   - a press while a control is still open (a release the log never saw) closes nothing: the
//     open control stays without a release and a new one starts
//   - a cluster break BETWEEN a press and its release (a portal in the middle of the hold) does
//     not split the control - it is still one hold - but is recorded (`breakInside`): its phase
//     is not measured across the break
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace gprl::solver::control {

/// One logged input of the attempt (the engine's input log, time order).
struct LoggedInput {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = true;
    bool breakBefore = false;   // a cluster break between the previous logged input and this one
};

struct Control {
    int index = 1;              // 1-based control number in the attempt
    int press = -1;             // position in the log (-1 = none: held before the attempt)
    int release = -1;           // position in the log (-1 = not released (yet))
    bool breakInside = false;   // a cluster break between the press and the release
    /// The recorded hold in frames (NaN while an end is unknown).
    double holdFrames(std::vector<LoggedInput> const& log) const {
        if (press < 0 || release < 0) return std::numeric_limits<double>::quiet_NaN();
        return log[static_cast<size_t>(release)].frame - log[static_cast<size_t>(press)].frame;
    }
    bool complete() const { return press >= 0 && release >= 0; }
};

/// The controls of a log, in order. Bounded by the log size.
inline std::vector<Control> pairControls(std::vector<LoggedInput> const& log) {
    std::vector<Control> out;
    int open = -1;   // index into `out` of the control waiting for its release
    for (size_t i = 0; i < log.size(); ++i) {
        auto const& in = log[i];
        if (in.down) {
            Control c;
            c.index = static_cast<int>(out.size()) + 1;
            c.press = static_cast<int>(i);
            out.push_back(c);
            open = static_cast<int>(out.size()) - 1;
            continue;
        }
        if (open >= 0) {
            Control& c = out[static_cast<size_t>(open)];
            c.release = static_cast<int>(i);
            for (int j = c.press + 1; j <= c.release; ++j) {
                if (log[static_cast<size_t>(j)].breakBefore) c.breakInside = true;
            }
            open = -1;
            continue;
        }
        Control c;   // a release without its press
        c.index = static_cast<int>(out.size()) + 1;
        c.release = static_cast<int>(i);
        out.push_back(c);
    }
    return out;
}

/// The control the log's input at `position` belongs to (index 0 = none: position out of range).
inline Control controlOf(std::vector<LoggedInput> const& log, int position) {
    for (auto const& c : pairControls(log)) {
        if (c.press == position || c.release == position) return c;
    }
    Control none;
    none.index = 0;
    return none;
}

}  // namespace gprl::solver::control
