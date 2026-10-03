// core/solver/ship_control.hpp host tests (docs/SHIP_SOLVER.md §11.3; owner prompt 2026-10-03 §2):
// a Ship CONTROL is one press + its matching release. Pairing of an attempt's input log:
//
//   press, release, press, release      two controls, each with its hold duration
//   a release first                     the button was held when the attempt started: a control without a press
//   press, press, release               a release the log never saw: the first control stays open, the second is complete
//   a trailing press                    a control without a release (not complete, no hold yet)
//   a cluster break inside a hold       still ONE control, `breakInside` (its phase is not measured across the break)
//   controlOf                           both ends of a control map to it; out of range -> index 0
#include "test_util.hpp"

#include "../core/solver/ship_control.hpp"

using namespace gprl::solver::control;

namespace {

std::vector<LoggedInput> logOf(std::vector<std::pair<double, bool>> const& in) {
    std::vector<LoggedInput> log;
    uint32_t id = 1;
    for (auto const& [frame, down] : in) log.push_back({id++, frame, down, false});
    return log;
}

void testPairs() {
    SECTION("press + release = one control: index, both ends, the hold duration");
    auto log = logOf({{581, true}, {591, false}, {628, true}, {641, false}, {660, true}, {676, false}});
    auto cs = pairControls(log);
    CHECK(cs.size() == 3);
    if (cs.size() == 3) {
        double const holds[] = {10.0, 13.0, 16.0};
        for (int i = 0; i < 3; ++i) {
            auto const& c = cs[static_cast<size_t>(i)];
            CHECK(c.index == i + 1 && c.press == 2 * i && c.release == 2 * i + 1);
            CHECK(c.complete() && !c.breakInside);
            CHECK(c.holdFrames(log) == holds[i]);
        }
    }
    CHECK(pairControls({}).empty());
}

void testHeldAtStart() {
    SECTION("a release first (held before the attempt, e.g. a StartPos): a control without a press, then normal controls");
    auto log = logOf({{12, false}, {30, true}, {41, false}});
    auto cs = pairControls(log);
    CHECK(cs.size() == 2);
    if (cs.size() == 2) {
        CHECK(cs[0].index == 1 && cs[0].press == -1 && cs[0].release == 0 && !cs[0].complete());
        CHECK(std::isnan(cs[0].holdFrames(log)));
        CHECK(cs[1].index == 2 && cs[1].press == 1 && cs[1].release == 2 && cs[1].complete());
        CHECK(cs[1].holdFrames(log) == 11.0);
    }
}

void testMissingRelease() {
    SECTION("press, press, release: the first control stays open (no release), the second is complete; a trailing press is an open control");
    auto log = logOf({{10, true}, {20, true}, {26, false}, {40, true}});
    auto cs = pairControls(log);
    CHECK(cs.size() == 3);
    if (cs.size() == 3) {
        CHECK(cs[0].press == 0 && cs[0].release == -1 && !cs[0].complete());
        CHECK(cs[1].press == 1 && cs[1].release == 2 && cs[1].complete() && cs[1].holdFrames(log) == 6.0);
        CHECK(cs[2].press == 3 && cs[2].release == -1 && !cs[2].complete());
        CHECK(std::isnan(cs[2].holdFrames(log)));
    }
}

void testBreakInside() {
    SECTION("a cluster break between a press and its release keeps ONE control (breakInside); a break between two controls marks neither");
    auto log = logOf({{10, true}, {20, false}, {40, true}, {52, false}});
    log[1].breakBefore = true;   // a portal in the middle of the first hold
    log[2].breakBefore = true;   // and one between the two controls
    auto cs = pairControls(log);
    CHECK(cs.size() == 2);
    if (cs.size() == 2) {
        CHECK(cs[0].complete() && cs[0].breakInside);
        CHECK(cs[1].complete() && !cs[1].breakInside);
    }
}

void testControlOf() {
    SECTION("controlOf: both ends map to their control; a position outside the log -> index 0");
    auto log = logOf({{10, true}, {20, false}, {40, true}, {52, false}});
    CHECK(controlOf(log, 0).index == 1 && controlOf(log, 1).index == 1);
    CHECK(controlOf(log, 2).index == 2 && controlOf(log, 3).index == 2);
    CHECK(controlOf(log, 3).press == 2 && controlOf(log, 3).release == 3);
    CHECK(controlOf(log, 4).index == 0 && controlOf(log, -1).index == 0);
}

}  // namespace

int main() {
    testPairs();
    testHeldAtStart();
    testMissingRelease();
    testBreakInside();
    testControlOf();
    return gprl::test::finish("ship_control_tests");
}
