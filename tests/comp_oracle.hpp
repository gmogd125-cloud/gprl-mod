#pragma once
// One compensated trial on the kinematic DEV FIXTURE oracle, judged the way the engine judges a
// lockstep trial (src/solver/CloneEngine compAfterStep, docs/SHIP_SOLVER.md §11.2): the re-join
// tracker (core/solver/rejoin.hpp) runs over the steps after the last moved input was applied;
//
//   exact re-join                                               -> Pass, rejoin exact
//   inside the tolerance for `steps` steps, then alive
//   `settleFrames` after the streak began                       -> Pass, rejoin approx (the trial ENDS
//                                                                   there: a later death is never seen)
//   the same velocity on a nearby height at the look-ahead      -> Pass, rejoin parallel
//   alive at the look-ahead with none of them                   -> Pass, rejoin none (SURVIVES_NO_REJOIN)
//   died before any of that                                     -> Died (laterFixed by the offline rule)
//
// Shared by tests/compensation_tests.cpp, tests/ship_pack_tests.cpp and tests/ship_cases_tests.cpp.
#include <cmath>
#include <vector>

#include "kinematic_oracle.hpp"

#include "../core/solver/compensation.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/rejoin.hpp"

namespace gprl::test {

/// The compensation config of the DEV FIXTURE: its ship gains 0.08 (thrust) + 0.03..0.06 (gravity)
/// of vertical velocity per tick of hold (units per tick, not GD's m_yVelocity), so one tick of
/// input changes vy by 0.11-0.14; the re-join tolerance is under half of that, like the engine's
/// default is under half of GD's step (core/solver/rejoin.hpp).
inline solver::comp::CompConfig fixtureComp() {
    solver::comp::CompConfig c = solver::comp::kComp;
    c.rejoin.tolVy = 0.04;
    c.rejoin.tolY = 2.0;
    c.rejoin.tolYWide = 5.0;
    return c;
}

inline void compOracleTrial(kin::KinematicOracle& o, solver::InputSchedule const& ref, solver::InputSchedule const& sched, solver::comp::CompTrial const& t,
                            solver::comp::CompOutcome& out, solver::comp::CompConfig const& cfg = fixtureComp(), bool subtick = false) {
    using namespace gprl::solver;
    using namespace gprl::solver::comp;
    o.setReference(ref);
    double first = t.moved.front().frame;
    for (auto const& mv : t.moved) first = std::min(first, mv.frame);
    double const earliest = std::min(first, t.attributeAfterFrame);
    std::vector<kin::KinematicOracle::Dev> dev;
    Outcome r = o.trialTrace(sched, (t.lookAheadFrame - earliest) / 240.0, t.devFromFrame * kTickMs, dev);
    // the first step whose state already has every moved input applied
    double const appliedTick = std::floor(t.lastMovedFrame + (subtick ? 0.0 : 0.5) + 1e-9);
    rejoin::Tracker tracker;
    bool passed = false, ended = false;
    for (auto const& d : dev) {
        out.dev.push_back({d.frame, d.dy, d.dvy});
        if (d.dead) break;
        if (d.frame < appliedTick + 1.0 - 1e-9) continue;
        tracker.step({d.frame, d.dy, d.dvy, d.discrete, d.exact}, cfg.rejoin);
        // the engine ends the trial at a re-join (or at its look-ahead): nothing after that step is simulated
        if (cfg.requireRejoin && tracker.passed(cfg.rejoin)) { passed = true; break; }
        if (cfg.requireRejoin && tracker.ended(cfg.rejoin)) { ended = true; break; }
    }
    if (passed) {
        out.kind = CompOutcome::Kind::Pass;
        out.rejoin = tracker.kind();
        out.rejoinFrame = tracker.frame();
        out.rejoinErrY = tracker.errY();
        out.rejoinErrVy = tracker.errVy();
        return;
    }
    if (ended) {
        // alive at the look-ahead, not re-joined: SURVIVES_NO_REJOIN
        out.kind = CompOutcome::Kind::Pass;
        out.rejoin = rejoin::Kind::None;
        return;
    }
    switch (r.kind) {
        case OutcomeKind::Survived:
            out.kind = CompOutcome::Kind::Pass;
            out.rejoin = rejoin::Kind::None;
            break;
        case OutcomeKind::Resynced:
            out.kind = CompOutcome::Kind::Pass;
            out.rejoin = rejoin::Kind::Exact;
            out.rejoinFrame = r.tMs / kTickMs;
            out.rejoinErrY = 0.0;
            out.rejoinErrVy = 0.0;
            break;
        case OutcomeKind::Died: {
            out.kind = CompOutcome::Kind::Died;
            out.deathFrame = r.tMs / kTickMs;
            out.objectId = r.objectId;
            std::vector<size_t> moved;
            for (size_t k = 1; k < t.moved.size(); ++k) moved.push_back(static_cast<size_t>(t.moved[k].id) - 1);
            out.laterFixed = laterFixedBefore(ref, static_cast<size_t>(t.moved.front().id) - 1, moved, r.tMs);
            break;
        }
        case OutcomeKind::Invalid:
            out.kind = CompOutcome::Kind::Invalid;
            out.reason = r.reason;
            break;
    }
}

}  // namespace gprl::test
