#pragma once
// Trajectory RE-JOIN of a compensated trial (docs/SHIP_SOLVER.md §11 "controls/1"; owner prompt
// "SHIP COUNTING / COMPENSATED TIMING WINDOW REWRITE" §8, 2026-10-03).
//
// Until v0.14.x a compensated trial passed when it re-joined the recorded run EXACTLY (16 steps
// of samePhysics) or was merely alive `flySeconds` after its last moved input ("settled"). A ship
// that is alive 0.75 s later with another vertical velocity has not come back to the run the
// player performed: it is drifting away from it, and every later input meets another state.
// Surviving the immediate obstacle is not the same as the timing shift being playable.
//
// From `gprl-clone-sa/6` a pass BEYOND the local window must re-join the recorded trajectory.
// After every moved input of a trial was applied, each step is compared with the recorded run:
//
//   exact      samePhysics with the recorded run for `exactSteps` consecutive steps (as before);
//              a pass at once
//   approx     for `steps` consecutive steps the trial's discrete state equals the recorded one
//              (gamemode, gravity, size, speed, held buttons, ground / slope / dash flags) and
//              |dy| <= tolY, |dvy| <= tolVy; then it must still be alive `settleFrames` after the
//              streak began, every later input replayed as performed. The pass is declared at
//              that frame (the trial ends there: "re-join early termination", prompt §16)
//   parallel   the same velocity (|dvy| <= tolVy) on a height within tolYWide of the recorded
//              one, the same discrete state, held for the last `steps` steps when the trial is
//              still alive `parallelFrames` after its last moved input: the ship flies the
//              recorded path at a small constant offset, drifts nowhere, and every later input
//              worked for the whole look-ahead. The weakest re-join (its height error is kept
//              with the result)
//   level_end  alive with every moved input applied when the real run reached the end of the
//              level: nothing follows that could differ (the engine's level-end rule)
//   none       alive at `parallelFrames` after its last moved input with none of the above (a
//              different velocity, or too far off): SURVIVES_NO_REJOIN. Never a pass of the
//              compensated window; the search goes on, and a side that finds no re-joining
//              schedule ends undecided (`sa_survives_no_rejoin`)
//
// The tolerances are DEV DEFAULTS in ONE versioned object. One tick more or less of ship hold
// changes m_yVelocity by 0.17-0.24 (GD 2.2 PlayerObject::updateJump: gravity 0.958199 x dt 0.25 x
// 0.4-0.5 x (1.0 holding + 0.8-1.2 released)); tolVy is under half of the smallest such step, so a
// trial inside the tolerance could not be brought closer by moving a follower one tick: it is as
// close as the input timing resolution allows, and its height drifts by at most tolVy x 0.25 =
// 0.02 units per tick afterwards. tolY is a tenth of a block, tolYWide three tenths.
//
// PURE C++20 (no Geode includes); the engine (src/solver/CloneEngine compAfterStep) and the host
// tests' oracle adapter feed the same Tracker. Host-tested in tests/rejoin_tests.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gprl::solver::rejoin {

enum class Kind : uint8_t { None = 0, Approx = 1, Exact = 2, LevelEnd = 3, Parallel = 4 };
constexpr char const* name(Kind k) {
    switch (k) {
        case Kind::None: return "none";
        case Kind::Approx: return "approx";
        case Kind::Exact: return "exact";
        case Kind::LevelEnd: return "level_end";
        case Kind::Parallel: return "parallel";
    }
    return "none";
}
/// A pass that counts for the compensated window.
constexpr bool rejoined(Kind k) { return k != Kind::None; }

struct RejoinConfig {
    double tolY = 3.0;              // units (a block is 30): |y_trial - y_recorded| at most this
    double tolVy = 0.08;            // PlayerObject::m_yVelocity units: under half of one tick of ship thrust change (>= 0.17)
    int steps = 8;                  // consecutive steps inside the tolerance with the same discrete state
    double settleFrames = 60.0;     // approx: alive this many frames after the streak began (0.25 s)
    int exactSteps = 16;            // consecutive samePhysics steps = an exact re-join (CONVERGE_STEPS)
    double tolYWide = 9.0;          // parallel: the constant height offset at most this (0.3 block)
    double parallelFrames = 180.0;  // parallel / none: decided this long after the last moved input (the settle length, 0.75 s)

    /// The longest a trial runs after its last moved input was applied (the planner's look-ahead).
    constexpr double maxFramesAfterLastMoved() const { return parallelFrames + 2.0; }
};
inline constexpr RejoinConfig kRejoin{};

/// One step of a trial AFTER every moved input was applied, compared with the recorded run at
/// the same step.
struct Sample {
    double frame = 0.0;           // frame reached after the step
    double dy = 0.0;              // y_trial - y_recorded
    double dvy = 0.0;             // vy_trial - vy_recorded
    bool discreteEqual = false;   // gamemode, gravity, size, speed, held buttons, ground / slope / dash flags equal
    bool exact = false;           // samePhysics (position, velocity and every flag within the exact tolerances)
};

/// Feeds on the steps of one (alive) trial in order; `passed` says when the trial may end as a
/// pass, `ended` when it ends without a re-join.
class Tracker {
public:
    void reset() { *this = Tracker{}; }

    void step(Sample const& s, RejoinConfig const& cfg = kRejoin) {
        if (m_firstFrame > 1e299) m_firstFrame = s.frame;
        m_lastFrame = s.frame;
        // exact streak (wins over everything: the trial IS the recorded run from here on)
        if (s.exact) {
            if (m_exactStreak == 0) m_exactStart = s.frame;
            if (++m_exactStreak >= std::max(1, cfg.exactSteps) && m_kind != Kind::Exact) {
                m_kind = Kind::Exact;
                m_frame = m_exactStart;
                m_errY = 0.0;
                m_errVy = 0.0;
            }
        }
        else m_exactStreak = 0;
        if (m_kind == Kind::Exact || m_kind == Kind::Approx) return;
        bool const sameVy = s.discreteEqual && std::fabs(s.dvy) <= cfg.tolVy;
        // approximate streak: the first completed one is kept (its start is the re-join frame)
        if (sameVy && std::fabs(s.dy) <= cfg.tolY) {
            if (m_streak == 0) {
                m_streakStart = s.frame;
                m_streakErrY = 0.0;
                m_streakErrVy = 0.0;
            }
            m_streakErrY = std::max(m_streakErrY, std::fabs(s.dy));
            m_streakErrVy = std::max(m_streakErrVy, std::fabs(s.dvy));
            if (++m_streak >= std::max(1, cfg.steps)) {
                m_kind = Kind::Approx;
                m_frame = m_streakStart;
                m_errY = m_streakErrY;
                m_errVy = m_streakErrVy;
                return;
            }
        }
        else m_streak = 0;
        // parallel streak: the same velocity on a nearby height; it only counts at the look-ahead
        if (sameVy && std::fabs(s.dy) <= cfg.tolYWide) {
            if (m_wide == 0) {
                m_wideStart = s.frame;
                m_wideErrY = 0.0;
                m_wideErrVy = 0.0;
            }
            m_wideErrY = std::max(m_wideErrY, std::fabs(s.dy));
            m_wideErrVy = std::max(m_wideErrVy, std::fabs(s.dvy));
            ++m_wide;
        }
        else m_wide = 0;
        if (atLookAhead(cfg) && m_wide >= std::max(1, cfg.steps)) {
            m_kind = Kind::Parallel;
            m_frame = m_wideStart;
            m_errY = m_wideErrY;
            m_errVy = m_wideErrVy;
        }
    }

    Kind kind() const { return m_kind; }
    /// First frame of the streak that proved the re-join (NaN while none).
    double frame() const { return m_frame; }
    /// Largest |dy| / |dvy| over that streak (0 for an exact re-join).
    double errY() const { return m_errY; }
    double errVy() const { return m_errVy; }

    /// The trial may end as a pass now: an exact or parallel re-join at once, an approximate one
    /// once it has been alive `settleFrames` after the streak began.
    bool passed(RejoinConfig const& cfg = kRejoin) const {
        if (m_kind == Kind::Exact || m_kind == Kind::Parallel) return true;
        if (m_kind == Kind::Approx) return m_lastFrame >= m_frame + cfg.settleFrames - 1e-9;
        return false;
    }
    /// The trial ends as SURVIVES_NO_REJOIN now: its look-ahead passed without a re-join and no
    /// approximate re-join is waiting for its settle time.
    bool ended(RejoinConfig const& cfg = kRejoin) const { return m_kind == Kind::None && atLookAhead(cfg); }

private:
    bool atLookAhead(RejoinConfig const& cfg) const {
        return m_firstFrame < 1e299 && m_lastFrame >= m_firstFrame - 1.0 + cfg.parallelFrames - 1e-9;
    }

    Kind m_kind = Kind::None;
    double m_frame = std::numeric_limits<double>::quiet_NaN();
    double m_errY = std::numeric_limits<double>::quiet_NaN();
    double m_errVy = std::numeric_limits<double>::quiet_NaN();
    int m_exactStreak = 0;
    double m_exactStart = 0.0;
    int m_streak = 0;
    double m_streakStart = 0.0;
    double m_streakErrY = 0.0;
    double m_streakErrVy = 0.0;
    int m_wide = 0;
    double m_wideStart = 0.0;
    double m_wideErrY = 0.0;
    double m_wideErrVy = 0.0;
    double m_firstFrame = 1e300;
    double m_lastFrame = -1e300;
};

}  // namespace gprl::solver::rejoin
