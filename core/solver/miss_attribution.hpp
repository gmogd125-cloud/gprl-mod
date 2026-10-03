#pragma once
// Miss attribution (docs/TIMING_SOLVER_V2_FABLE.md §3.3, Fable review D6 / W6).
//
// One real death used to become up to five MISS windows: every open job whose lockstep control died
// together with the real player was "the input that failed" (41 misses for 10 deaths in the owner's
// 13.28.40 log; the death at frame 4025 turned jobs 595-599 into five). By the causal definition of
// the sequence-adjusted window (the past is fixed, LATER inputs may adapt) an earlier input did NOT
// fail when a later input could still have saved the run. The engine knows this in the death pause
// without a single extra simulation:
//
//   M = the jobs whose control died with the real player AND whose planner has a passing run
//   M empty          -> nothing is the player's miss (every such job stays `unresolved`
//                       no_pass_death_unrelated, as before)
//   else             -> the job in M with the LATEST input frame is the miss (its timing_window and
//                       timing_result as before, `low_confidence (miss)`); every other job in M is
//                       `sequence_dependent (miss_downstream)`: a timing_result only, `miss` false,
//                       NO timing_window - neither a hit measurement (its run died) nor the player's
//                       miss
//
// A job WITHOUT a passing run is never a candidate: it neither wins nor blocks an earlier job.
// Ties on the input frame (two inputs in one step) go to the later job id (the later input).
//
// PURE C++20; host-tested in tests/miss_attribution_tests.cpp and on the real log extract in
// tests/attribution_tests.cpp (T-ATT-3). The engine (CloneEngine settleMisses) applies it per
// death once every job that could still join that death has resolved (or at the restart / level
// end / teardown with what resolved).
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace gprl::solver {

/// When a lockstep control's death counts as "died with the real player" (DEV DEFAULTS).
struct MissMatchConfig {
    double frameTolerance = 1.0;        // the same step +-1 (SD D6)
    double noObjectXTolerance = 2.0;    // Fable D6 / §3.3: when both ids are -1 (no object) the
                                        // positions must agree too (level units)
};
constexpr MissMatchConfig kMissMatch{};

/// The control died together with a real / would-be death of player 1: same step +-1, same killer
/// object, and - when neither death names an object (-1) - the same place within
/// noObjectXTolerance (object -1 alone matched any boundary death in the same step, RC3.4).
inline bool diedWithReal(double controlFrame, int controlObj, double controlX, double realFrame, int realObj, double realX,
                         MissMatchConfig const& cfg = kMissMatch) {
    if (!(std::fabs(controlFrame - realFrame) <= cfg.frameTolerance + 1e-6)) return false;
    if (controlObj != realObj) return false;
    if (controlObj == -1 && !(std::fabs(controlX - realX) <= cfg.noObjectXTolerance)) return false;
    return true;
}

struct MissCandidate {
    int jobId = 0;
    double inputFrame = 0.0;   // the input's recorded frame
    bool hasPassRun = false;   // the miss window has a passing run (the planner's result is valid)
};

/// The job attributed the death, or none (no candidate has a passing run). Bounded by the list.
inline std::optional<int> attributeMiss(std::vector<MissCandidate> const& candidates) {
    std::optional<int> best;
    double bestFrame = 0.0;
    for (auto const& c : candidates) {
        if (!c.hasPassRun) continue;
        if (!best || c.inputFrame > bestFrame + 1e-9 || (c.inputFrame > bestFrame - 1e-9 && c.jobId > *best)) {
            best = c.jobId;
            bestFrame = c.inputFrame;
        }
    }
    return best;
}

}  // namespace gprl::solver
