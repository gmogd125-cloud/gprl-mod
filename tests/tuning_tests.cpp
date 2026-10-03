// Solver tuning host tests (docs/SOLVER_DESIGN.md §12): the budget config object and its
// arithmetic (adaptive clone-step budget from the measured cost, clone creation rationing,
// catch-up, coverage) and the log diagnostics (mismatch kinds, drop buckets).
#include "test_util.hpp"

#include "../core/solver/budget.hpp"
#include "../core/solver/diagnostics.hpp"
#include "../core/solver/sequence.hpp"
#include "../core/solver/sequence_adjusted.hpp"
#include "../core/solver/timeline.hpp"

#include <cmath>
#include <string>

using namespace gprl;
using namespace gprl::solver::budget;
using namespace gprl::solver::diagnostics;

namespace {

void testConfig() {
    SECTION("one config object with a version; the values the engine runs with");
    CHECK(std::string(kBudget.version) == "gprl-clone-budget/3");
    // budget/3 (v0.7.0): the history ring is 2048 steps, one definition (timeline + engine read it)
    CHECK(kBudget.historySteps == 2048 && kHistorySteps == 2048);
    CHECK(gprl::solver::timeline::kHistorySteps == kHistorySteps);
    CHECK(kBudget.maxJobs == 24);
    CHECK(kBudget.maxClones == 320);
    CHECK(kBudget.poolWarm == 48 && kBudget.poolWarm <= kBudget.maxClones);
    CHECK(kBudget.cloneCreatesPerFrame >= 1);
    CHECK(kBudget.minStepsPerFrame >= 21 * 14);           // a coarse pass advances >= 14 steps per frame
    CHECK(kBudget.maxStepsPerFrame >= kBudget.minStepsPerFrame);
    CHECK(kBudget.throttleOnMs > kBudget.throttleOffMs);
    CHECK(kBudget.targetSimMsPerFrame < kBudget.throttleOnMs);   // the budget acts before the guard
    CHECK(kBudget.spawnDeadlineSteps > 0 && kBudget.spawnDeadlineSteps < 1024);
    CHECK_NEAR(kBudget.slowFrameDt, 1.0 / 30.0, 1e-12);
}

void testStepCost() {
    SECTION("cost per clone step: EMA per frame, frames without steps leave it alone");
    double ema = 0.0;
    ema = updateStepCostUs(ema, 0.3, 100);      // first sample: 3 us
    CHECK_NEAR(ema, 3.0, 1e-9);
    ema = updateStepCostUs(ema, 0.0, 0);        // no steps: unchanged
    CHECK_NEAR(ema, 3.0, 1e-9);
    ema = updateStepCostUs(ema, 1.3, 100);      // 13 us sample moves 10 % of the way
    CHECK_NEAR(ema, 4.0, 1e-9);
    CHECK_NEAR(updateStepCostUs(5.0, -1.0, 10), 5.0, 1e-12);   // garbage ignored
}

void testStepBudget() {
    SECTION("clone steps per frame = target ms / cost, clamped, sane for garbage");
    CHECK(stepBudgetForFrame(2.0) == 1000);                          // 2 ms / 2 us
    CHECK(stepBudgetForFrame(3.0) == 666);
    CHECK(stepBudgetForFrame(0.1) == kBudget.maxStepsPerFrame);      // absurdly cheap: the cap
    CHECK(stepBudgetForFrame(50.0) == kBudget.minStepsPerFrame);     // very slow machine: the floor
    CHECK(stepBudgetForFrame(0.0) == stepBudgetForFrame(kBudget.defaultStepCostUs));
    CHECK(stepBudgetForFrame(std::nan("")) == stepBudgetForFrame(kBudget.defaultStepCostUs));
    CHECK(stepBudgetForFrame(-1.0) == stepBudgetForFrame(kBudget.defaultStepCostUs));
    // the in-game cost (2.5-3.1 us) gives 645-800 steps: more than a coarse pass needs per frame
    CHECK(stepBudgetForFrame(2.5) >= 21 * 30);
    // the old fixed cap 4000 at 3 us was 12 ms: the budget never allows that
    CHECK(stepBudgetForFrame(3.0) * 3.0 / 1000.0 <= kBudget.targetSimMsPerFrame + 1e-9);
}

void testCloneCreation() {
    SECTION("clone creation: the control is never rationed, others at most N per frame, never past the pool max");
    CHECK(mayCreateClone(0, 0, false));
    CHECK(mayCreateClone(100, kBudget.cloneCreatesPerFrame - 1, false));
    CHECK(!mayCreateClone(100, kBudget.cloneCreatesPerFrame, false));
    CHECK(mayCreateClone(100, kBudget.cloneCreatesPerFrame, true));
    CHECK(!mayCreateClone(kBudget.maxClones, 0, true));
    CHECK(!mayCreateClone(kBudget.maxClones + 5, 0, false));
}

void testCatchUp() {
    SECTION("catch-up: a lagging clone advances a few steps per real step, others are unlimited");
    CHECK(catchUpBudget(0) == 0x7fffffff);
    CHECK(catchUpBudget(kBudget.catchUpLagSteps) == 0x7fffffff);
    CHECK(catchUpBudget(kBudget.catchUpLagSteps + 1) == kBudget.catchUpStepsPerStep);
    CHECK(catchUpBudget(500) == kBudget.catchUpStepsPerStep);
}

void testCoverage() {
    SECTION("coverage leaves out what could never have a window");
    CoverageInput c;
    CHECK_NEAR(coveragePercent(c), 0.0, 1e-12);
    c.inputs = 100;
    c.emitted = 80;
    CHECK_NEAR(coveragePercent(c), 80.0, 1e-9);
    c.skippedDead = 10;      // mashed in the death pause
    c.notWindowable = 10;    // deaths unrelated to the input / cut by a restart
    CHECK_NEAR(coveragePercent(c), 100.0, 1e-9);
    c.emitted = 60;
    CHECK_NEAR(coveragePercent(c), 75.0, 1e-9);
    // the 2026-09-30 busy level (452 inputs, 232 emitted, 58 not windowable, 0 dead skips) = 58.9 %
    CoverageInput busy{452, 232, 0, 58};
    CHECK_NEAR(coveragePercent(busy), 100.0 * 232.0 / 394.0, 1e-9);
    CoverageInput odd{5, 9, 0, 0};   // never above 100
    CHECK_NEAR(coveragePercent(odd), 100.0, 1e-9);
    CoverageInput allOut{5, 1, 3, 2};
    CHECK_NEAR(coveragePercent(allOut), 100.0, 1e-9);
}

void testMismatchKind() {
    SECTION("mismatch kinds from the control-vs-real line");
    CHECK(mismatchKind("step 339: touching rings 1 vs 0 | real (440.1,180.4) ... | control ...") == "rings");
    CHECK(mismatchKind("step 3392: speed | real (4403.8,315.9) vy -10.6 C air rings 0 ... spd 1.30 | control ... spd 0.90") == "speed");
    CHECK(mismatchKind("step 23655: position off by (0.0000,0.0031) | real ...") == "position");
    CHECK(mismatchKind("step 12: y velocity 3.12000 vs 3.11999 | ...") == "yvel");
    CHECK(mismatchKind("step 340: the control died on #8 at x=100 (frame 339.0) but the real player did not") == "control_died");
    CHECK(mismatchKind("step 340: the real player died on #8 at frame 339.0 but the control survived (...)") == "real_died");
    CHECK(mismatchKind("the control clone became invalid (teleport portal)") == "control_invalid");
    CHECK(mismatchKind("step 5: ground flag") == "ground");
    CHECK(mismatchKind("step 5: gravity") == "gravity");
    CHECK(mismatchKind("step 5: game mode") == "mode");
    CHECK(mismatchKind("step 5: dash") == "dash");
    CHECK(mismatchKind("step 5: slope flag") == "slope");
    CHECK(mismatchKind("step 5: size") == "size");
    CHECK(mismatchKind("step 5: held buttons") == "buttons");
    CHECK(mismatchKind("step 5: last position") == "lastpos");
    CHECK(mismatchKind("step 5: something new") == "other");
    CHECK(mismatchKind("") == "other");
}

void testDropBuckets() {
    SECTION("drop reasons -> summary buckets; not-windowable ones leave the coverage denominator");
    CHECK(dropBucket("mismatch") == DropBucket::Mismatch);
    CHECK(dropBucket("invalid") == DropBucket::Invalid);
    CHECK(dropBucket("no_pass") == DropBucket::NoPass);
    CHECK(dropBucket("pool") == DropBucket::Pool);
    CHECK(dropBucket("budget") == DropBucket::Budget);
    CHECK(dropBucket("history lost") == DropBucket::History);
    CHECK(dropBucket("blocked") == DropBucket::Blocked);
    CHECK(dropBucket("no_shift_tested") == DropBucket::Blocked);
    CHECK(dropBucket("unbound") == DropBucket::Unbound);
    CHECK(dropBucket("reset") == DropBucket::Reset);
    CHECK(dropBucket("teardown") == DropBucket::Reset);
    CHECK(dropBucket("level_end") == DropBucket::LevelEnd);
    CHECK(dropBucket("input event dropped by the ring") == DropBucket::Other);
    CHECK(notWindowable(DropBucket::NoPass) && notWindowable(DropBucket::Reset) && notWindowable(DropBucket::LevelEnd));
    CHECK(!notWindowable(DropBucket::Mismatch) && !notWindowable(DropBucket::Pool) && !notWindowable(DropBucket::Invalid));
}

void testSABudget() {
    SECTION("budget/3 SA budget: re-tuned M4 fields; the local jobs keep >= 65 % of the frame's steps");
    CHECK(kSABudget.frameShare == 0.35 && kSABudget.targetMsPerFrame == 0.75 && kSABudget.idleLoadShare == 0.6);
    CHECK(kSABudget.parallelClones == 6 && kSABudget.stepsPerClonePerStep == 16 && kSABudget.poolReserve == 24 && kSABudget.cloneCreatesPerFrame == 1);
    gprl::solver::SequenceConfig sc;
    sc.frameShare = kSABudget.frameShare;
    sc.targetMsPerFrame = kSABudget.targetMsPerFrame;
    sc.idleLoadShare = kSABudget.idleLoadShare;
    // the unchanged M4 arithmetic with the SA shares: never more than 35 % of the budget
    for (int budget : {300, 350, 1000, 6000}) {
        int allowance = gprl::solver::sequence::stepAllowance(budget, 0, 0, 0, 3.0, sc);
        CHECK(allowance <= static_cast<int>(0.35 * budget) + 1);
        CHECK(budget - allowance >= static_cast<int>(0.65 * budget) - 1);
    }
    // the load guard leaves the throttle for the local jobs
    CHECK(gprl::solver::sequence::loadAllows(3.0, kBudget.throttleOnMs, sc));
    CHECK(!gprl::solver::sequence::loadAllows(3.3, kBudget.throttleOnMs, sc));
    CHECK(std::string(gprl::solver::kSA.version) == "gprl-clone-sa/6");
    CHECK(gprl::solver::kSA.maxChain == 3 && gprl::solver::kSA.maxTrialsPerInput == 40 && gprl::solver::kSA.maxInputsPerJob == 4);
    CHECK(gprl::solver::kSA.maxJobSpanTicks == 96.0 && gprl::solver::kSA.queueMax == 16 && gprl::solver::kSA.ringReserveSteps == 64);
    CHECK(gprl::solver::kSA.priorityMaxLocalWidthTicks == 12.0 && gprl::solver::kSA.refinePointsSubtick == 3);
    // Fable review D2 (look-ahead from the last moved follower): the longest history one SA job
    // can need - base (maxShift + 1 before the first member) + span 96 + the last member's
    // followers inside its look-ahead (130) + their shift (10) + one horizon after the last one
    // (130) = 377 frames - still fits the ring with its reserve even when every step is a half
    // tick (2 steps per frame): 2 x 377 + 64 = 818 < 2048
    CHECK(gprl::solver::kSA.horizonFromLastMoved && !gprl::solver::kSA.pairWalkForPresses);
    double const span = gprl::solver::saWorstSpanFrames(gprl::solver::kSA);
    CHECK_NEAR(span, 11.0 + 96.0 + 130.0 + 10.0 + 130.0, 1e-9);
    CHECK(2.0 * span + gprl::solver::kSA.ringReserveSteps < static_cast<double>(kHistorySteps));
    // ... and Opus's span (a follower + 96 ticks) plus the D2 extension is what the ring must hold
    CHECK(span - 96.0 < static_cast<double>(kHistorySteps - gprl::solver::kSA.ringReserveSteps));
}

void testReplayBreaker() {
    SECTION("replay breaker (V2-D13): > 20 mismatches in the first 480 steps, or > 25 % of a full 2400-step window; trips once");
    ReplayBreaker early;
    int tripAt = -1;
    for (int k = 1; k <= 480; ++k) {
        if (early.step(k % 10 == 0) && tripAt < 0) tripAt = k;   // one mismatch every 10 steps: the 21st at step 210
    }
    CHECK(tripAt == 210);
    CHECK(early.tripped() && early.trippedEarly());
    CHECK(!early.step(true));   // never trips twice
    early.reset();
    CHECK(!early.tripped() && !early.trippedEarly() && early.steps() == 0 && early.mismatches() == 0);
    ReplayBreaker quiet;
    for (int k = 1; k <= 5000; ++k) CHECK(!quiet.step(k % 30 == 0));   // 3.3 %: 16 in the first 480 steps, never trips
    CHECK(!quiet.tripped() && quiet.mismatches() == 166);
    ReplayBreaker late;
    int lateTrip = -1;
    for (int k = 1; k <= 6000; ++k) {
        bool mm = k > 3000 && k % 3 == 0;   // clean start, then 33 %
        if (late.step(mm) && lateTrip < 0) lateTrip = k;
    }
    CHECK(lateTrip > 3000 && late.tripped() && !late.trippedEarly());
    CHECK(late.windowMismatches() <= 2400);
    late.reset();
    CHECK(!late.tripped() && late.steps() == 0);
    // the Eon Startpos 240hz session: 466761 mismatches in 576124 steps (81 %): trips at once
    ReplayBreaker eon;
    int eonTrip = -1;
    for (int k = 1; k <= 480 && eonTrip < 0; ++k) {
        if (eon.step(k % 5 != 0)) eonTrip = k;
    }
    CHECK(eonTrip > 0 && eonTrip <= 30);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    testConfig();
    testStepCost();
    testStepBudget();
    testCloneCreation();
    testCatchUp();
    testCoverage();
    testMismatchKind();
    testDropBuckets();
    testSABudget();
    testReplayBreaker();
    return gprl::test::finish("tuning_tests");
}
