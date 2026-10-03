// LocalWindowSolver / HoldRangeSolver host tests with the SYNTHETIC oracle.
#include "test_util.hpp"
#include "synthetic_oracle.hpp"

#include "../core/solver/local_window.hpp"
#include "../core/solver/sequence.hpp"

using namespace gprl;
using namespace gprl::solver;
using gprl::test::SyntheticOracle;

namespace {

LocalWindowConfig cfg() {
    LocalWindowConfig c;
    c.search.resolutionMs = 0.05;
    c.search.maxTrials = 40;
    return c;
}

void testPressWindow() {
    SECTION("press window with other inputs fixed");
    // schedule: release at 50, press at 105 (moving), release at 140. Window of the press: [100, 112.5]
    InputSchedule s;
    s.inputs.push_back({50.0, 1, Button::Jump, false});
    s.inputs.push_back({105.0, 1, Button::Jump, true});
    s.inputs.push_back({140.0, 1, Button::Jump, false});
    SyntheticOracle oracle({{100.0, 112.5}}, 1, 0.0);
    LocalWindowSolver solver(oracle, cfg());
    auto w = solver.solve(0, s, 1);
    CHECK(w.valid);
    CHECK(w.kind == InputKind::Press);
    CHECK_NEAR(w.actualMs, 105.0, 1e-9);
    CHECK_NEAR(w.earliestMs, 100.0, 0.05);
    CHECK_NEAR(w.latestMs, 112.5, 0.05);
    CHECK(w.earliestMs >= 100.0 && w.latestMs <= 112.5);
    CHECK(w.boundedEarly && w.boundedLate);
    CHECK(!w.blockedEarly && !w.blockedLate);
    CHECK(w.resolutionMs <= 0.05 + 1e-9);
    CHECK_NEAR(w.widthMs(), 12.5, 0.1);
    CHECK_NEAR(w.widthFrames240(), 3.0, 0.03);
    CHECK(w.trials == w.early.trialCount + w.late.trialCount);
    CHECK(!w.solverVersion.empty());
    // the other inputs were never moved
    for (auto const& t : w.late.trials) (void)t;
    CHECK(s.inputs[0].tMs == 50.0 && s.inputs[2].tMs == 140.0);
}

void testReleaseWindowNeighbourLimits() {
    SECTION("release window is limited by its own press (cannot cross it)");
    // press at 100, release at 104 (moving). Oracle passes [90, 200]; the earlier limit is the press.
    InputSchedule s;
    s.inputs.push_back({100.0, 1, Button::Jump, true});
    s.inputs.push_back({104.0, 1, Button::Jump, false});
    SyntheticOracle oracle({{90.0, 200.0}}, 1, 0.0);
    LocalWindowSolver solver(oracle, cfg());
    auto w = solver.solve(0, s, 1);
    CHECK(w.valid);
    CHECK(w.kind == InputKind::Release);
    CHECK(w.blockedEarly);          // room (4 ms - margin) < one tick
    CHECK(!w.boundedEarly);         // never failed within that room
    CHECK_NEAR(w.earliestMs, 100.0 + cfg().neighbourMarginMs, 1e-6);
    CHECK(w.earliestMs > 100.0);
    CHECK(!w.boundedLate);          // unbounded up to max shift
    CHECK_NEAR(w.latestMs, 104.0 + cfg().search.maxShiftMs, 1e-9);
    // a second player's inputs are a different channel: no limit
    InputSchedule s2 = s;
    s2.inputs.push_back({103.0, 2, Button::Jump, true});
    CHECK(std::isnan(laterNeighbourLimitMs(s2, 1, 0.01)));
    CHECK_NEAR(earlierNeighbourLimitMs(s2, 1, 0.01), 3.99, 1e-9);
}

void testHistoryLimit() {
    SECTION("history start limits the early side and is reported as blocked");
    InputSchedule s = test::singleInput(105.0);
    SyntheticOracle oracle({{0.0, 1000.0}}, 0, 104.0);   // history starts 1 ms before the input
    LocalWindowSolver solver(oracle, cfg());
    auto w = solver.solve(0, s, 0);
    CHECK(w.valid);
    CHECK(w.blockedEarly);
    CHECK_NEAR(w.earliestMs, 104.0, 1e-9);
    // no trial went before the history start (the oracle would have returned Invalid)
    for (double t : oracle.trialTimes()) CHECK(t >= 104.0);
}

void testHoldRange() {
    SECTION("hold range with the press fixed");
    // press at 100, release at 130 (moving). Release passes in [115, 150] -> hold [15, 50]
    InputSchedule s;
    s.inputs.push_back({100.0, 1, Button::Jump, true});
    s.inputs.push_back({130.0, 1, Button::Jump, false});
    SyntheticOracle oracle({{115.0, 150.0}}, 1, 0.0);
    LocalWindowConfig c = cfg();
    c.search.maxShiftMs = 30.0;
    HoldRangeSolver solver(oracle, c);
    auto h = solver.solveMovingRelease(0, s, 0, 1);
    CHECK(h.valid);
    CHECK(h.movedRelease);
    CHECK_NEAR(h.actualHoldMs, 30.0, 1e-9);
    CHECK_NEAR(h.minHoldMs, 15.0, 0.05);
    CHECK_NEAR(h.maxHoldMs, 50.0, 0.05);
    CHECK(h.minHoldMs >= 15.0 && h.maxHoldMs <= 50.0);
    CHECK(h.boundedMin && h.boundedMax);
    CHECK(h.trials > 0);
    // mirror: release fixed, press moved. Press passes in [80, 110] -> hold = 130 - press in [20, 50]
    SyntheticOracle oracle2({{80.0, 110.0}}, 0, 0.0);
    HoldRangeSolver solver2(oracle2, c);
    auto h2 = solver2.solveMovingPress(0, s, 0, 1);
    CHECK(h2.valid);
    CHECK(!h2.movedRelease);
    CHECK_NEAR(h2.minHoldMs, 20.0, 0.05);
    CHECK_NEAR(h2.maxHoldMs, 50.0, 0.05);
    // bad pairing rejected
    auto bad = solver.solveMovingRelease(0, s, 1, 0);
    CHECK(!bad.valid);
}

void testDisplayHelpers() {
    SECTION("frames at 240 are a display conversion of ms");
    CHECK_NEAR(framesAt240(1000.0 / 240.0), 1.0, 1e-12);
    CHECK_NEAR(framesAt240(1.67), 0.4008, 1e-3);
    auto str = formatWindow(1.67);
    CHECK_MSG(str == "1.67 ms = 0.40 frames @240", str);
}

void testSequenceSolverIsReal() {
    SECTION("SequenceSolver is no longer a stub (v0.6.1): it runs the oracle; the joint cases live in sequence_tests");
    // This 1-D oracle only looks at input 0, so input 1 can sit anywhere: the pair is independent
    // (100 ms apart, so no shift combination within +-10 ticks swaps their order).
    InputSchedule s;
    s.inputs.push_back({100.0, 1, Button::Jump, true});
    s.inputs.push_back({200.0, 1, Button::Jump, false});
    SyntheticOracle oracle({{90.0, 110.0}}, 0, 0.0);
    SequenceSolverConfig sc;
    sc.local = cfg();
    SequenceSolver seq(oracle, sc);
    auto r = seq.solve(0, s, {0, 1});
    CHECK_MSG(r.result.valid, r.result.invalidReason);
    CHECK(r.result.solverVersion == std::string(kSequenceSolverVersion));
    CHECK(oracle.trials() > 0);
    CHECK(r.trials > 0 && r.result.samples >= 1);
    CHECK(r.inputIndices.size() == 2 && r.localWindows.size() == 2);
    CHECK_NEAR(r.result.jointFeasibleShare, 1.0, 1e-12);
    // a single input is not a sequence
    auto one = seq.solve(0, s, {0});
    CHECK(!one.result.valid);
}

}  // namespace

int main() {
    testPressWindow();
    testReleaseWindowNeighbourLimits();
    testHistoryLimit();
    testHoldRange();
    testDisplayHelpers();
    testSequenceSolverIsReal();
    return gprl::test::finish("local_window_tests");
}
