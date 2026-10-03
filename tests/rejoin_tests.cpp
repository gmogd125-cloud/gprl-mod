// core/solver/rejoin.hpp host tests (docs/SHIP_SOLVER.md §11.2): the re-join tracker a
// compensated trial is judged with.
//
//   exact       16 samePhysics steps -> Exact at once, error 0, the streak's first frame
//   approx      8 steps inside the tolerance -> Approx; a pass only `settleFrames` after the streak began
//   a broken streak starts over; a sample with another discrete state never counts
//   parallel    the same velocity on a nearby height, held at the look-ahead -> Parallel (decided there)
//   none        a drifting trial (another velocity) or one too far off is `ended` at the look-ahead
//   an exact streak upgrades an approximate re-join
#include "test_util.hpp"

#include "../core/solver/rejoin.hpp"

using namespace gprl::solver::rejoin;

namespace {

Sample at(double frame, double dy, double dvy, bool discrete = true, bool exact = false) { return {frame, dy, dvy, discrete, exact}; }

void testExact() {
    SECTION("exact: exactSteps consecutive samePhysics steps -> Exact, error 0, a pass at once");
    Tracker t;
    RejoinConfig cfg;
    for (int i = 0; i < cfg.exactSteps - 1; ++i) {
        t.step(at(100.0 + i, 0.0, 0.0, true, true), cfg);
        CHECK(t.kind() != Kind::Exact);
    }
    t.step(at(100.0 + cfg.exactSteps - 1, 0.0, 0.0, true, true), cfg);
    CHECK(t.kind() == Kind::Exact && t.passed(cfg));
    CHECK(t.frame() == 100.0 && t.errY() == 0.0 && t.errVy() == 0.0);
    CHECK(rejoined(t.kind()) && std::string(name(t.kind())) == "exact");
}

void testApprox() {
    SECTION("approx: `steps` steps inside tolY / tolVy with the same discrete state -> Approx; passed only settleFrames after the streak began");
    Tracker t;
    RejoinConfig cfg;
    for (int i = 0; i < cfg.steps; ++i) t.step(at(200.0 + i, 0.5 + 0.1 * i, 0.01), cfg);
    CHECK(t.kind() == Kind::Approx);
    CHECK(!t.passed(cfg));
    CHECK(t.frame() == 200.0);
    CHECK_NEAR(t.errY(), 0.5 + 0.1 * (cfg.steps - 1), 1e-12);
    CHECK_NEAR(t.errVy(), 0.01, 1e-12);
    // alive, but the deviation may grow again: it stays the re-join that was proven
    for (int i = cfg.steps; i < static_cast<int>(cfg.settleFrames); ++i) {
        t.step(at(200.0 + i, 5.0, 0.2), cfg);
        CHECK(!t.passed(cfg));
    }
    t.step(at(200.0 + cfg.settleFrames, 5.0, 0.2), cfg);
    CHECK(t.passed(cfg) && t.kind() == Kind::Approx);
    CHECK_NEAR(t.errY(), 0.5 + 0.1 * (cfg.steps - 1), 1e-12);   // the streak's own error, not what came after
}

void testBrokenStreak() {
    SECTION("a streak broken by one step outside the tolerance (or with another discrete state) starts over");
    RejoinConfig cfg;
    {
        Tracker t;
        for (int i = 0; i < cfg.steps - 1; ++i) t.step(at(300.0 + i, 1.0, 0.0), cfg);
        t.step(at(300.0 + cfg.steps - 1, cfg.tolY + 0.5, 0.0), cfg);   // one step too far
        CHECK(t.kind() == Kind::None);
        for (int i = 0; i < cfg.steps; ++i) t.step(at(320.0 + i, 1.0, 0.0), cfg);
        CHECK(t.kind() == Kind::Approx && t.frame() == 320.0);
    }
    {
        Tracker t;
        for (int i = 0; i < cfg.steps * 3; ++i) t.step(at(400.0 + i, 0.0, 0.0, false), cfg);   // e.g. holding while the recorded run is not
        CHECK(t.kind() == Kind::None);
    }
    {
        Tracker t;
        for (int i = 0; i < cfg.steps * 3; ++i) t.step(at(400.0 + i, 0.0, cfg.tolVy * 1.5), cfg);   // another velocity: drifting
        CHECK(t.kind() == Kind::None);
    }
}

void testParallel() {
    SECTION("parallel: the same velocity on a height within tolYWide, held at the look-ahead -> Parallel there (never before)");
    RejoinConfig cfg;
    Tracker t;
    double const dy = cfg.tolY + 2.0;   // outside the tight tolerance, inside the wide one
    CHECK(dy < cfg.tolYWide);
    int const n = static_cast<int>(cfg.parallelFrames);
    for (int i = 0; i < n - 1; ++i) {
        t.step(at(500.0 + i, dy, 0.0), cfg);
        CHECK(t.kind() == Kind::None);
        CHECK(!t.passed(cfg) && !t.ended(cfg));
    }
    t.step(at(500.0 + n - 1, dy, 0.0), cfg);
    CHECK(t.kind() == Kind::Parallel && t.passed(cfg) && !t.ended(cfg));
    CHECK_NEAR(t.errY(), dy, 1e-12);
    CHECK(std::string(name(t.kind())) == "parallel");
}

void testNone() {
    SECTION("none: a drifting trial, or one beyond tolYWide, is `ended` at the look-ahead (SURVIVES_NO_REJOIN), never passed");
    RejoinConfig cfg;
    int const n = static_cast<int>(cfg.parallelFrames);
    {
        Tracker t;
        for (int i = 0; i < n; ++i) t.step(at(600.0 + i, 0.5 + 0.05 * i, 0.2), cfg);   // one tick of thrust off: drifting
        CHECK(t.kind() == Kind::None && t.ended(cfg) && !t.passed(cfg));
        CHECK(!rejoined(t.kind()));
    }
    {
        Tracker t;
        for (int i = 0; i < n; ++i) t.step(at(600.0 + i, cfg.tolYWide + 1.0, 0.0), cfg);   // parallel but too far off
        CHECK(t.kind() == Kind::None && t.ended(cfg));
    }
    {
        // parallel for a while, then it leaves the wide tolerance shortly before the look-ahead
        Tracker t;
        for (int i = 0; i < n - 3; ++i) t.step(at(600.0 + i, 4.0, 0.0), cfg);
        for (int i = n - 3; i < n; ++i) t.step(at(600.0 + i, cfg.tolYWide + 2.0, 0.0), cfg);
        CHECK(t.kind() == Kind::None && t.ended(cfg));
    }
}

void testUpgrade() {
    SECTION("an exact streak after an approximate re-join upgrades it to Exact");
    RejoinConfig cfg;
    Tracker t;
    for (int i = 0; i < cfg.steps; ++i) t.step(at(700.0 + i, 0.4, 0.0), cfg);
    CHECK(t.kind() == Kind::Approx);
    for (int i = 0; i < cfg.exactSteps; ++i) t.step(at(710.0 + i, 0.0, 0.0, true, true), cfg);
    CHECK(t.kind() == Kind::Exact && t.passed(cfg) && t.errY() == 0.0);
}

void testConfig() {
    SECTION("defaults: tolVy under half of one tick of ship thrust (>= 0.17), the look-ahead = the settle length");
    CHECK(kRejoin.tolVy < 0.17 / 2.0 + 1e-12);
    CHECK(kRejoin.tolY <= kRejoin.tolYWide);
    CHECK(kRejoin.parallelFrames == 180.0);
    CHECK(kRejoin.maxFramesAfterLastMoved() >= kRejoin.parallelFrames);
}

}  // namespace

int main() {
    testExact();
    testApprox();
    testBrokenStreak();
    testParallel();
    testNone();
    testUpgrade();
    testConfig();
    return gprl::test::finish("rejoin_tests");
}
