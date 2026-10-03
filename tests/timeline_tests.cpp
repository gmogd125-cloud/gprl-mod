// timeline host tests (docs/SOLVER_DESIGN.md §9.2): FPC Engine::input math as pure functions
// (frac, betweenSteps, snapAhead parking, half-tick frames, frames = 0 steps), the history walk for
// a target frame, ring wrap, grid offsets, ms <-> frames.
#include "test_util.hpp"

#include "../core/solver/timeline.hpp"

#include <map>

using namespace gprl;
using namespace gprl::solver::timeline;

namespace {

void testClosedStepFrames() {
    SECTION("frames of a closed step: 1, 0.5 for a half tick, 0 when physics did not run");
    CHECK(closedStepFrames(false, 0.25) == 1.0);
    CHECK(closedStepFrames(true, 0.125) == 0.5);
    CHECK(closedStepFrames(false, 0.0) == 0.0);
    CHECK(closedStepFrames(true, 0.0) == 0.0);
    CHECK(closedStepFrames(false, 1e-8) == 0.0);
}

void testLearnTickDt() {
    SECTION("tick delta bookkeeping");
    CHECK(learnTickDt(0.0, false, true, 0.25) == 0.25);      // a whole tick step teaches the tick
    CHECK(learnTickDt(0.0, true, true, 0.125) == 0.25);      // a half tick doubles when nothing is known
    CHECK(learnTickDt(0.25, true, true, 0.125) == 0.25);     // known: a half tick changes nothing
    CHECK(learnTickDt(0.25, false, true, 0.0) == 0.25);      // no physics: unchanged
    CHECK(learnTickDt(0.25, false, true, 0.7) == 0.25);      // absurd deltas ignored
}

void testPlaceInput() {
    SECTION("input placement: at the step start, inside the step (CBF), between steps (bots)");
    // step 300 starts at frame 299, whole tick, tick dt 0.25, nothing run yet
    auto p = placeInput(300, 299.0, 1.0, false, 0.25, 0.25, 0.0);
    CHECK(p.step == 300 && p.t == 299.0 && !p.betweenSteps && p.frac == 0.0);
    // CBF split: 0.1 of 0.25 run before the input -> frame 299.4
    auto q = placeInput(300, 299.0, 1.0, false, 0.25, 0.25, 0.1);
    CHECK(q.step == 300 && !q.betweenSteps);
    CHECK_NEAR(q.t, 299.4, 1e-9);
    // half tick step (0.5 frames, expected delta 0.125): 0.0625 run -> frame 299.25
    auto h = placeInput(300, 299.0, 0.5, true, 0.25, 0.125, 0.0625);
    CHECK_NEAR(h.t, 299.25, 1e-9);
    CHECK_NEAR(h.expect, 0.125, 1e-12);
    // the whole step already ran (a bot firing after the physics update): parked as the next step
    auto b = placeInput(300, 299.0, 1.0, false, 0.25, 0.25, 0.25);
    CHECK(b.betweenSteps && b.step == 301 && b.t == 300.0 && b.frac == 0.0);
    // unknown tick dt falls back to the last step delta
    auto u = placeInput(5, 4.0, 1.0, false, 0.0, 0.25, 0.125);
    CHECK_NEAR(u.t, 4.5, 1e-9);
    // no delta known at all: the step start
    auto z = placeInput(5, 4.0, 1.0, false, 0.0, 0.0, 0.1);
    CHECK(z.t == 4.0 && !z.betweenSteps);
}

void testGridOffsets() {
    SECTION("coarse grid offsets keep shifts on whole-tick boundaries");
    auto a = gridOffsets(299.0);
    CHECK(a.late == 0.0 && a.early == 0.0);
    auto b = gridOffsets(299.5);
    CHECK_NEAR(b.late, 0.5, 1e-12);
    CHECK_NEAR(b.early, 0.5, 1e-12);
    auto c = gridOffsets(10.25);
    CHECK_NEAR(c.late, 0.75, 1e-12);
    CHECK_NEAR(c.early, 0.25, 1e-12);
    auto d = gridOffsets(12.9999999);
    CHECK(d.late == 0.0 && d.early == 0.0);
}

void testHorizonAndUnits() {
    SECTION("horizon frame and ms <-> frames");
    CHECK_NEAR(horizonFrame(100.0, 0.5, 10), 230.0, 1e-9);
    CHECK_NEAR(framesToMs(1.0), 1000.0 / 240.0, 1e-12);
    CHECK_NEAR(msToFrames(1000.0 / 240.0), 1.0, 1e-12);
    CHECK_NEAR(framesToMs(msToFrames(12.345)), 12.345, 1e-12);
    CHECK(kTickUpdateDt == 0.25);
}

void testRingIndex() {
    SECTION("ring index wraps and never goes negative");
    CHECK(ringIndex(1, 1024) == 1);
    CHECK(ringIndex(1024, 1024) == 0);
    CHECK(ringIndex(1025, 1024) == 1);
    CHECK(ringIndex(-1, 1024) == 1023);
    CHECK(ringIndex(0, 8) == 0);
}

void testStepForFrame() {
    SECTION("history walk for a target frame (FPC trySpawn loop)");
    // steps 1..10 with frames 0, 1, 2, 2.5, 3, 4, ... (step 4 is a half tick after step 3)
    std::map<int, double> frames = {{1, 0.0}, {2, 1.0}, {3, 2.0}, {4, 2.5}, {5, 3.0}, {6, 4.0}, {7, 5.0}, {8, 6.0}, {9, 7.0}, {10, 8.0}};
    auto frameOf = [&](int k, double& f) {
        auto it = frames.find(k);
        if (it == frames.end()) return false;
        f = it->second;
        return true;
    };
    CHECK(stepForFrame(10, 8.0, frameOf) == 10);      // target at the step's own frame
    CHECK(stepForFrame(10, 7.5, frameOf) == 9);       // inside step 9
    CHECK(stepForFrame(10, 2.7, frameOf) == 4);       // the half tick step
    CHECK(stepForFrame(10, 2.5, frameOf) == 4);
    CHECK(stepForFrame(10, 2.4, frameOf) == 3);
    CHECK(stepForFrame(10, 0.0, frameOf) == 1);
    CHECK(stepForFrame(10, -1.0, frameOf) == -1);     // before the attempt started
    // a hole in the ring (overwritten) stops the walk
    frames.erase(6);
    CHECK(stepForFrame(10, 6.5, frameOf) == 8);
    CHECK(stepForFrame(10, 4.5, frameOf) == -1);
    CHECK(stepForFrame(11, 5.0, frameOf) == -1);      // starting step missing
}

void testApplyInStep() {
    SECTION("where the clone's shifted input is applied inside a step");
    // sub-tick: anywhere inside [f0, f0 + fr)
    auto a = applyInStep(299.4, 299.0, 1.0, true);
    CHECK(a.now);
    CHECK_NEAR(a.frac, 0.4, 1e-12);
    auto b = applyInStep(300.0, 299.0, 1.0, true);
    CHECK(!b.now);   // exactly the next step's start
    // whole steps only: nearest boundary
    auto c = applyInStep(299.4, 299.0, 1.0, false);
    CHECK(c.now && c.frac == 0.0);
    auto d = applyInStep(299.6, 299.0, 1.0, false);
    CHECK(!d.now);
    auto e = applyInStep(299.5, 299.0, 0.5, false);   // a half tick step: 299.5 is its end
    CHECK(!e.now);
    auto f = applyInStep(299.0, 299.0, 0.0, false);   // a step without physics never applies
    CHECK(!f.now);
}

}  // namespace

int main() {
    testClosedStepFrames();
    testLearnTickDt();
    testPlaceInput();
    testGridOffsets();
    testHorizonAndUnits();
    testRingIndex();
    testStepForFrame();
    testApplyInStep();
    return gprl::test::finish("timeline_tests");
}
