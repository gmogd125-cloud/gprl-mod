#pragma once
// Timing windows in the isolated simulator (docs/BACKGROUND_ANALYZER_DESIGN.md §4.4, AN-D6),
// measured exactly like the live clone solver measures them (core/solver/pass_planner.hpp
// semantics, core/solver/settle.hpp reused verbatim):
//
//   control    the inputs replayed as given from `start`; an input is measurable only when the
//              control stays alive and SETTLES after it (or completes the level); otherwise the
//              window is skipped and counted (WindowsProgress::skippedControl)
//   shifts     k = +-1..maxShift whole ticks (pass 0), the input moved by k with every other
//              input replayed as performed; the range stops before a neighbouring input of the
//              same button (a sequence-dependent edge: that side has no fail -> bounded = false)
//   pass       the shifted run settles alive (settle.hpp) OR re-joins the control trajectory
//              (equal Engine::stateHash at the same tick for `rejoinTicks` consecutive ticks)
//   fail       the shifted run dies
//   not tested the run is still unsettled at the look-ahead's end or enters an unsupported span:
//              a gap that widens the bracket, never an edge (walkSide semantics)
//   edges      lastPass = the largest |k| passing contiguously from 0, firstFail = the first fail
//              beyond it; a bounded side reports the MIDPOINT of [lastPass, firstFail] in ms, an
//              unbounded side its conservative pass edge (-lastPass * 1000/240 ... )
//   refine     with `subTick` (the recording says Click Between Frames) each bounded bracket is
//              refined `subTickPasses` times with `pointsPerPass` sub-tick placements (7 -> 1/8
//              tick, twice -> 1/64) through Engine::stepPart; resolutionMs = the widest bounded
//              bracket, else one tick
//   resolution (gprl-sim/2, review M9) a window is never emitted bounded on both sides with
//              windowMs < resolutionMs: that happens when one side's bracket stayed coarse (its
//              refinement trials were not tested) while the other was refined. The coarser side is
//              then reported UNBOUNDED at its conservative pass edge (lastPass), the resolution
//              recomputed from the side still bounded; WindowsProgress::demoted counts them. (The
//              server's simWindowUsable also refuses any window narrower than its resolution.)
//
// Pure C++20, host-tested in tests/sim_windows_tests.cpp. Resumable: the control run and its
// snapshots live in WindowsProgress::state (freed once done, review M4); `measureWindows`
// continues at `progress.next`. mayContinue() is polled before every shifted trial (a few
// thousand engine ticks at most); a window interrupted mid-way resumes at the same trial on the
// next call (the walk of each side is kept in the state), so the result equals an uninterrupted run.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../geometry_hash.hpp"
#include "result.hpp"
#include "world.hpp"

namespace gprl::sim {

struct WindowsConfig {
    int maxShift = 10;                 // ticks each side, like the live solver
    bool subTick = false;              // sub-tick refinement (only when the recording reports CBF)
    int subTickPasses = 1;             // 1 = 1/8 tick, 2 = 1/64 tick
    int pointsPerPass = 7;
    int rejoinTicks = 16;              // consecutive equal state hashes = re-joined the control
    double lookaheadSeconds = 8.0;     // settle::kSettle.maxSeconds: unsettled beyond this = not tested
    int tickOffset = 0;                // level tick of this input list's tick 0 (segment / attempt start)
    WindowSource source = WindowSource::Reference;
    int maxWindows = 4000;             // stop measuring when `out` holds this many
    double neighbourMarginFrames = 0.0024;   // LocalWindowConfig::neighbourMarginMs in frames
    bool keepState = false;            // keep WindowsProgress::state after done (centreInputs reuses the control)
};

struct WindowsState;   // opaque (windows.cpp): the control run, its snapshots, measurability

struct WindowsProgress {
    int next = 0;                      // next input index to measure
    int total = 0;                     // inputs in the list
    int measured = 0;                  // windows emitted
    int skippedControl = 0;            // control died / ended before settling after the input
    int skippedNeighbour = 0;          // no shift possible on either side
    int skippedCap = 0;                // maxWindows reached
    int notTested = 0;                 // trials that ended unsettled / in a span
    uint64_t ticks = 0;                // engine ticks spent
    int trials = 0;                    // shifted simulations run
    bool controlReady = false;
    bool done = false;
    std::vector<std::string> debug;
    std::shared_ptr<WindowsState> state;
    int demoted = 0;                   // both-bounded windows narrower than their resolution, one side made unbounded (M9)
};

/// Measures the windows of `inputs` (steps relative to `start`). Returns `progress.done`. Stops
/// early (and resumes on the next call) when `mayContinue()` says no (polled before every
/// trial and window) or when this call spent `tickBudget` engine ticks (0 = unlimited).
bool measureWindows(World const& world, std::vector<RecordedInput> const& inputs, StartState const& start, WindowsConfig const& cfg,
                    std::function<bool()> const& mayContinue, uint64_t tickBudget, std::vector<SimWindow>& out, WindowsProgress& progress);

/// The geometry hash at `playerX` over the world's gameplay objects (core/geometry_hash rules).
gprl::GeometryHash worldGeometryHash(World const& world, double playerX);

/// The M9 rule on one window, in ticks: each side's last passing shift, first failing shift (NaN =
/// unbounded) and bracket. Returns the side to report unbounded so that a window bounded on both
/// sides is never narrower than its resolution (the wider bracket): -1 early, +1 late, 0 none.
int coarseSideToDemote(double earlyLastPass, double earlyFirstFail, double earlyBracket, double lateLastPass, double lateFirstFail, double lateBracket);

struct ReferenceSegment;

/// Re-centres the inputs of a reference segment inside their own local windows. The beam search
/// keeps the LATEST clearing input (stable order), so a reference input sits at the late edge of
/// its window and a +-maxShift measurement reports it one-sided. Here every input's window is
/// measured once at +-probeShift ticks (other inputs as they are); an input bounded on both
/// sides is moved to the midpoint of its pass range (whole ticks, in order, never crossing a
/// neighbour of the same button). Rounds repeat until nothing moves (at most `maxRounds`).
///
/// Resumable (gprl-sim/2, review H7): per segment the state is the round, its phase (measure /
/// apply / replay), the input being applied and a live engine that replays the CURRENT inputs
/// forward only. A move is validated from the live engine's snapshot just before it: the moved
/// run must re-join the round's control trajectory (equal Engine::stateHash for `rejoinTicks`
/// consecutive ticks after the input), complete the level or reach the segment's end alive - a
/// death, an unsupported span or no re-join within the look-ahead rejects it. The round ends with
/// ONE replay of the segment (chunked) that must still reach the segment's end alive (completed,
/// or `seg.ticks` ticks at x >= x1); if it does not (a hash re-join was only approximate) the
/// whole round is reverted and centring stops. mayContinue() is polled before every trial, every
/// applied input and every 4096 replay ticks. /1 replayed the whole segment once per move and
/// never yielded (seconds on a long level).
struct CentreState;   // opaque (windows.cpp)
struct CentreProgress {
    int round = 0;
    int moved = 0;                     // inputs moved (validated rounds only)
    int rejected = 0;                  // moves refused (crossing a neighbour, or the validation)
    int revertedRounds = 0;            // rounds undone by the final replay
    uint64_t ticks = 0;                // engine ticks spent
    bool done = false;
    std::vector<std::string> debug;
    std::shared_ptr<CentreState> state;
};
bool centreInputsResumable(World const& world, ReferenceSegment& seg, int probeShift, int maxRounds, std::function<bool()> const& mayContinue,
                           CentreProgress& progress);

/// centreInputsResumable run to the end without polling. Returns the number of inputs moved;
/// `ticks` accumulates the engine ticks spent.
int centreInputs(World const& world, ReferenceSegment& seg, int probeShift, int maxRounds, uint64_t& ticks, std::vector<std::string>* debug);

}  // namespace gprl::sim
