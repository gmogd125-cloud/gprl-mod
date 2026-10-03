#pragma once
// v0.11.0 (owner decision 2026-10-01: "make it so it checks further, kinda predicting what the
// player is going to do - if there is an orb then a portal, calculate the orb click but make sure
// the player can go fully through and touch the ground before counting the frame perfect;
// results don't have to be fast, the goal is accuracy").
//
// The settled look-ahead. Before v0.11.0 a shifted copy PASSED when it re-joined the recorded run
// or was simply alive 0.5 s after the input ("survived", the horizon convention). A copy alive in
// the air at 0.5 s may still die on the next hazard; "survived" was the weakest proof (D3b). Now a
// copy that has not re-joined keeps running - every later input replayed as performed - until it
// is SETTLED: in a ground mode (cube, ball, robot, spider) on the ground, not dashing, not on an
// orb, for `groundTicks` consecutive ticks after the last moved input; in a flying mode (ship,
// ufo, wave, swing) alive `flySeconds` after it. A copy still unsettled at the look-ahead's end
// (`maxSeconds`, 8 s by default) is NOT a pass and NOT a death: it is reported `not_tested`
// (reason `unsettled`), the same as a shift the engine could not run, so the window's edge is
// never widened by it. Pure rules, host-tested (tests/settle_tests.cpp); the engine
// (src/solver/CloneEngine.cpp simStep / advance / runSequence) feeds the facts.
//
// v0.14.6 (`gprl-clone/6`): `flySeconds` 2.0 -> 0.75. Measured on the owner's 2,218 Ship inputs of
// 2026-10-03 (mod v0.14.0-v0.14.3): a flying copy needs `flySeconds` of recorded run AFTER its
// input, so every input made in the last 2 s before a death, a noclip would-be death or a restart
// ended without a window (`no_pass_death_unrelated` 46 %, `cut_by_restart` 11 %,
// `sa_death_in_span` 8 %) and 33 inputs were left as rating evidence: the Ship calibration could
// not move. In a hard flying section most inputs are that close to the next death. 0.75 s is
// still longer than a ship needs to cross a corridor (a mistimed input shows within ~0.4 s), and
// a compensated trial has to be alive 0.75 s after its LAST moved follower.
#include <algorithm>
#include <cstdint>

#include "../vocab.hpp"

namespace gprl::solver::settle {

struct SettleConfig {
    bool enabled = true;
    double maxSeconds = 8.0;    // look-ahead end: a copy still unsettled here is `unsettled` (not tested)
    int groundTicks = 24;       // 0.1 s on the ground in a ground mode = settled
    double flySeconds = 0.75;   // flying modes cannot touch the ground: alive this long after the input = settled (2.0 until v0.14.5)
};
inline constexpr SettleConfig kSettle{};

/// What the engine reads off the clone after a step.
struct StepFacts {
    bool flyingMode = false;    // ship / ufo / wave / swing (modeChar S U W G)
    bool onGround = false;      // m_isOnGround
    bool dashing = false;       // m_isDashing
    bool touchingRing = false;  // m_touchingRings non-empty (an orb is being used: not at rest)
    double frameDone = 0.0;     // frame reached after the step
    double lastMovedFrame = 0.0;   // the trial's last moved input (the measured input for a local job)
};

struct SettleState {
    int groundStreak = 0;
    bool settled = false;
};

/// Updates the streak and the settled flag after a step. Returns `settled`.
inline bool update(SettleState& s, StepFacts const& f, SettleConfig const& cfg = kSettle) {
    if (!cfg.enabled) {
        s.settled = false;
        return false;
    }
    if (!f.flyingMode && f.onGround && !f.dashing && !f.touchingRing) ++s.groundStreak;
    else s.groundStreak = 0;
    double const since = f.frameDone - f.lastMovedFrame;
    if (f.flyingMode) s.settled = since >= cfg.flySeconds * kTicksPerSecond - 1e-9;
    else s.settled = s.groundStreak >= cfg.groundTicks && since >= static_cast<double>(cfg.groundTicks) - 1e-9;
    return s.settled;
}

/// The look-ahead in seconds the engine plans with: the settle maximum when the settled
/// look-ahead is on, else the plain horizon setting.
inline double lookAheadSeconds(SettleConfig const& cfg, double horizonSeconds) {
    return cfg.enabled ? std::clamp(cfg.maxSeconds, 1.0, 10.0) : horizonSeconds;
}

}  // namespace gprl::solver::settle
