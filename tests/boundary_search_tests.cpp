// BoundarySearch host tests with the SYNTHETIC 1-D oracle (tests/synthetic_oracle.hpp).
#include "test_util.hpp"
#include "synthetic_oracle.hpp"

#include "../core/solver/boundary_search.hpp"

using namespace gprl;
using namespace gprl::solver;
using gprl::test::SyntheticOracle;

namespace {

BoundarySearchConfig cfg(double resolutionMs = 0.05, int maxTrials = 40, double maxShiftTicks = 10.0) {
    BoundarySearchConfig c;
    c.resolutionMs = resolutionMs;
    c.maxTrials = maxTrials;
    c.maxShiftMs = maxShiftTicks * kTickMs;
    return c;
}

void testSimpleBounded() {
    SECTION("simple bounded window [a, b] around the actual input");
    // pass iff t in [100, 112.5]; actual at 105
    SyntheticOracle oracle({{100.0, 112.5}}, 0);
    BoundarySearch search(oracle, cfg());
    auto s = test::singleInput(105.0);
    auto early = search.search(0, s, 0, Side::Earlier, kNaN, 0.5);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(early.valid && late.valid);
    CHECK(early.bounded);
    CHECK(late.bounded);
    CHECK(!early.blocked && !late.blocked);
    // pass edge within resolution of the true edge, on the conservative side
    CHECK_NEAR(105.0 + early.passShiftMs, 100.0, 0.05);
    CHECK(105.0 + early.passShiftMs >= 100.0);
    CHECK(105.0 + early.failShiftMs < 100.0);
    CHECK_NEAR(105.0 + late.passShiftMs, 112.5, 0.05);
    CHECK(105.0 + late.passShiftMs <= 112.5);
    CHECK(early.bracketMs <= 0.05 + 1e-9);
    CHECK(late.bracketMs <= 0.05 + 1e-9);
    CHECK(!early.nonMonotonic && !late.nonMonotonic);
    CHECK(!early.budgetExhausted && !late.budgetExhausted);
    CHECK(early.trialCount == static_cast<int>(early.trials.size()));
    // deterministic: same trial sequence on a second run
    oracle.reset();
    auto early2 = search.search(0, s, 0, Side::Earlier, kNaN, 0.5);
    CHECK(early2.trials.size() == early.trials.size());
    bool same = true;
    for (size_t i = 0; i < early.trials.size() && i < early2.trials.size(); ++i) same = same && early.trials[i].shiftMs == early2.trials[i].shiftMs;
    CHECK_MSG(same, "trial sequence must be deterministic");
}

void testSubFrameWindow() {
    SECTION("sub-frame 1.67 ms window resolves to < 0.05 ms per side within budget");
    // window [200.0, 201.67] (0.4 frames); actual at 200.8
    double const a = 200.0, b = 201.67, actual = 200.8;
    SyntheticOracle oracle({{a, b}}, 0);
    BoundarySearch search(oracle, cfg(0.05, 40));
    auto s = test::singleInput(actual);
    auto early = search.search(0, s, 0, Side::Earlier, kNaN, 0.5);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(early.bounded && late.bounded);
    CHECK(early.bracketMs <= 0.05 + 1e-9);
    CHECK(late.bracketMs <= 0.05 + 1e-9);
    CHECK_NEAR(actual + early.passShiftMs, a, 0.05);
    CHECK_NEAR(actual + late.passShiftMs, b, 0.05);
    double width = (actual + late.passShiftMs) - (actual + early.passShiftMs);
    CHECK_NEAR(width, 1.67, 0.1);
    // budget: one coarse trial (first tick step fails) + island scan (2) + ceil(log2(4.1667/0.05)) = 7 bisections ~ 10 per side
    CHECK_MSG(early.trialCount <= 12, "early side used " + std::to_string(early.trialCount) + " trials");
    CHECK_MSG(late.trialCount <= 12, "late side used " + std::to_string(late.trialCount) + " trials");
    CHECK_MSG(oracle.trials() <= 24, "total trials " + std::to_string(oracle.trials()));
    CHECK(!early.budgetExhausted && !late.budgetExhausted);
}

void testUnboundedLate() {
    SECTION("unbounded late side: passes all the way to the max shift");
    SyntheticOracle oracle({{100.0, 10000.0}}, 0);
    BoundarySearch search(oracle, cfg(0.05, 40, 10.0));
    auto s = test::singleInput(105.0);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(late.valid);
    CHECK(!late.bounded);
    CHECK(std::isnan(late.failShiftMs));
    CHECK_NEAR(late.passShiftMs, 10.0 * kTickMs, 1e-9);
    CHECK(late.bracketMs == 0.0);
    CHECK(!late.budgetExhausted);
    // exactly 10 coarse trials, no bisection
    CHECK_MSG(late.trialCount == 10, "trials " + std::to_string(late.trialCount));
    // side limit not a multiple of the step: the limit itself is tried last
    auto late2 = search.search(0, s, 0, Side::Later, 2.5 * kTickMs, 0.5);
    CHECK(!late2.bounded);
    CHECK_NEAR(late2.passShiftMs, 2.5 * kTickMs, 1e-9);
    CHECK(late2.trials.back().phase == TrialPhase::Limit);
}

void testBlockedEarly() {
    SECTION("blocked early side: no history before the input");
    // history starts at 103 ms; actual at 105 -> only 2 ms of room (< one tick step)
    SyntheticOracle oracle({{0.0, 10000.0}}, 0, 103.0);
    BoundarySearch search(oracle, cfg());
    auto s = test::singleInput(105.0);
    auto early = search.search(0, s, 0, Side::Earlier, 105.0 - 103.0, 0.5);
    CHECK(early.valid);
    CHECK(early.blocked);
    CHECK(!early.bounded);
    CHECK_NEAR(early.passShiftMs, -2.0, 1e-9);
    // fully blocked: zero room
    auto early0 = search.search(0, s, 0, Side::Earlier, 0.0, 0.5);
    CHECK(early0.blocked);
    CHECK(early0.trialCount == 0);
    CHECK(early0.passShiftMs == 0.0);
    // blocked but the edge lies inside the room: bisect within it
    SyntheticOracle oracle2({{104.0, 10000.0}}, 0, 103.0);
    BoundarySearch search2(oracle2, cfg());
    auto e2 = search2.search(0, s, 0, Side::Earlier, 2.0, 0.5);
    CHECK(e2.blocked);
    CHECK(e2.bounded);
    CHECK_NEAR(105.0 + e2.passShiftMs, 104.0, 0.05);
}

void testNonMonotonicIsland() {
    SECTION("non-monotonic island beyond the first fail is recorded, window stays contiguous");
    // contiguous pass [100, 108]; island [112.6, 116.8]; actual at 105 -> 1 tick later (109.17) fails,
    // 2 ticks (113.33) passes (island), 3 ticks (117.5) fails
    SyntheticOracle oracle({{100.0, 108.0}, {112.6, 116.8}}, 0);
    BoundarySearch search(oracle, cfg());
    auto s = test::singleInput(105.0);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(late.valid);
    CHECK(late.bounded);
    CHECK(late.nonMonotonic);
    CHECK(late.islands.size() == 1);
    if (!late.islands.empty()) {
        CHECK_NEAR(105.0 + late.islands[0].fromShiftMs, 113.3333, 0.01);
        CHECK_NEAR(105.0 + late.islands[0].toShiftMs, 113.3333, 0.01);
    }
    // the reported window edge is the contiguous one (108), not the island
    CHECK_NEAR(105.0 + late.passShiftMs, 108.0, 0.05);
    CHECK(105.0 + late.passShiftMs <= 108.0);
}

void testBudget() {
    SECTION("trial budget: bisection stops and reports budgetExhausted");
    SyntheticOracle oracle({{100.0, 112.5}}, 0);
    BoundarySearchConfig c = cfg(0.0001, 6);   // impossible resolution, tiny budget
    c.islandScanSteps = 0;
    BoundarySearch search(oracle, c);
    auto s = test::singleInput(105.0);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(late.bounded);
    CHECK(late.budgetExhausted);
    CHECK(late.trialCount == 6);
    CHECK(late.bracketMs > 0.0001);
    // still conservative: pass edge inside the true window
    CHECK(105.0 + late.passShiftMs <= 112.5);
    CHECK(105.0 + late.failShiftMs > 112.5);
}

void testInvalid() {
    SECTION("oracle Invalid aborts the side with valid = false");
    SyntheticOracle oracle({{100.0, 112.5}}, 0);
    oracle.setInvalidAt(105.0 + kTickMs);
    BoundarySearch search(oracle, cfg());
    auto s = test::singleInput(105.0);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(!late.valid);
    CHECK(!late.invalidReason.empty());
    CHECK(late.trialCount == 1);
}

void testResyncCountsAsPass() {
    SECTION("Resynced outcomes count as passes");
    SyntheticOracle oracle({{100.0, 112.5}}, 0);
    oracle.setResync(true);
    BoundarySearch search(oracle, cfg());
    auto s = test::singleInput(105.0);
    auto late = search.search(0, s, 0, Side::Later, kNaN, 0.5);
    CHECK(late.bounded);
    CHECK_NEAR(105.0 + late.passShiftMs, 112.5, 0.05);
    bool sawResync = false;
    for (auto const& t : late.trials) if (t.pass && t.outcome.kind == OutcomeKind::Resynced) sawResync = true;
    CHECK(sawResync);
}

}  // namespace

int main() {
    testSimpleBounded();
    testSubFrameWindow();
    testUnboundedLate();
    testBlockedEarly();
    testNonMonotonicIsland();
    testBudget();
    testInvalid();
    testResyncCountsAsPass();
    return gprl::test::finish("boundary_search_tests");
}
