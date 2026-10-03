// timing_units host tests (docs/TIMING_SOLVER_V2.md §2.1, §5; AUDIT §8, §9, §13, §17):
// seconds <-> ms, seconds <-> 240 FPS frames, the three display fields from ONE canonical value,
// no truncation / rounding before the display, the level-time convention and the D50 mapping.
// The TypeScript counterpart (shared/src/level-analysis/canonical-window.ts canonicalWindowView)
// is the BACKEND builder's and has its own tests.
#include "test_util.hpp"

#include "../core/solver/timing_units.hpp"
#include "../core/solver/timeline.hpp"

#include <cmath>
#include <string>

using namespace gprl;
using namespace gprl::solver;

namespace {

void testConversions() {
    SECTION("seconds <-> ms <-> 240 FPS frames: round trips, no truncation, one canonical value");
    CHECK_NEAR(units::framesToMs(1.0), 1000.0 / 240.0, 1e-15);
    CHECK_NEAR(units::framesToMs(4.0), 16.666666666666668, 1e-12);
    CHECK_NEAR(units::msToFrames240(16.666666666666668), 4.0, 1e-12);
    CHECK_NEAR(units::secondsToFrames240(0.0166666666666667), 4.0, 1e-9);
    for (double ms : {0.065104166666666671, 0.5208333333333334, 4.166666666666667, 16.666666666666668, 26.27, 1234.5678}) {
        auto w = units::canonicalWindow(ms);
        CHECK(w.windowMs == ms);                                   // the canonical value itself, untouched
        CHECK_NEAR(w.windowSeconds * 1000.0, ms, 1e-12);
        CHECK_NEAR(w.equivalentFrames240, ms * 240.0 / 1000.0, 1e-12);
        CHECK_NEAR(units::framesToMs(w.equivalentFrames240), ms, 1e-9);   // round trip
        CHECK_NEAR(units::secondsToFrames240(w.windowSeconds), w.equivalentFrames240, 1e-9);
    }
    // sub-frame windows are legal and keep full precision (nothing is capped at a frame)
    auto tiny = units::canonicalWindow(0.065104166666666671);
    CHECK(tiny.equivalentFrames240 > 0.0156 && tiny.equivalentFrames240 < 0.0157);
    // AUDIT §3 example: 26.27 ms -> 6.30 frames at 240 FPS
    CHECK(units::framesText(26.27) == "6.30 f");
    CHECK(units::windowText(26.27) == "26.27 ms (6.30 f)");
    CHECK(units::windowText(0.5208333333333334) == "0.521 ms (0.13 f)");
    // the timeline's thin wrappers agree with the one definition
    CHECK(timeline::framesToMs(3.5) == units::framesToMs(3.5));
    CHECK(timeline::msToFrames(12.5) == 12.5 / kTickMs);
}

void testPositions() {
    SECTION("canonical input time: actualMs = t x 1000 + sub-tick ms; level time = (levelTick - 1) / 240 + sub-tick");
    CHECK(units::actualMs(14.5833, 0.0) == 14.5833 * 1000.0);
    CHECK_NEAR(units::actualMs(1.5, units::subTickMs(0.25)), 1500.0 + 0.25 * kTickMs, 1e-12);
    CHECK(units::subTickMs(0.0) == 0.0);
    // the v0.6.2 convention: input events satisfy tick = t x 240 + 1 (Deadlocked #670: t=14.5833 tick=3501)
    CHECK_NEAR(units::levelTimeSeconds(3501, 0.0), 14.583333333333334, 1e-12);
    CHECK_NEAR(units::levelTimeSeconds(3501, 2.0), 14.585333333333334, 1e-12);
    CHECK_NEAR(units::levelTimeSeconds(1, 0.0), 0.0, 1e-15);
}

void testD50() {
    SECTION("D50 = 1.34898 / W_seconds; at 240 FPS ~ 323.76 / frames (AUDIT §13 table +-0.01)");
    struct Row { double frames, d50; };
    for (Row r : {Row{1, 323.76}, {2, 161.88}, {3, 107.92}, {4, 80.94}, {5, 64.75}, {6, 53.96}, {8, 40.47}, {10, 32.38}}) {
        CHECK_NEAR(units::d50(units::framesToMs(r.frames)), r.d50, 0.01);
    }
    CHECK_NEAR(units::d50(16.667), 80.94, 0.01);
    CHECK(std::isnan(units::d50(0.0)) && std::isnan(units::d50(-1.0)));
}

}  // namespace

int main() {
    testConversions();
    testPositions();
    testD50();
    return gprl::test::finish("timing_units_tests");
}
