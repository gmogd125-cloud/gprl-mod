// v0.11.0 settled look-ahead rules (core/solver/settle.hpp): host-tested, no game.
#include "test_util.hpp"

#include "../core/solver/settle.hpp"

using namespace gprl::solver::settle;

namespace {

StepFacts ground(double frame, double moved, bool onGround, bool dashing = false, bool ring = false) {
    StepFacts f;
    f.flyingMode = false;
    f.onGround = onGround;
    f.dashing = dashing;
    f.touchingRing = ring;
    f.frameDone = frame;
    f.lastMovedFrame = moved;
    return f;
}

StepFacts flying(double frame, double moved) {
    StepFacts f;
    f.flyingMode = true;
    f.frameDone = frame;
    f.lastMovedFrame = moved;
    return f;
}

void testGroundModes() {
    SECTION("a ground-mode copy is settled after groundTicks consecutive ticks on the ground after its input");
    SettleState s;
    double moved = 1000.0;
    // in the air for 30 ticks (an orb jump), then on the ground
    for (int i = 1; i <= 30; ++i) CHECK(!update(s, ground(moved + i, moved, false)));
    for (int i = 31; i < 31 + kSettle.groundTicks - 1; ++i) CHECK(!update(s, ground(moved + i, moved, true)));
    CHECK(update(s, ground(moved + 30 + kSettle.groundTicks, moved, true)));
    CHECK(s.settled && s.groundStreak == kSettle.groundTicks);

    SECTION("leaving the ground, dashing or touching an orb resets the streak");
    SettleState r;
    for (int i = 1; i < kSettle.groundTicks; ++i) update(r, ground(moved + i, moved, true));
    CHECK(!r.settled && r.groundStreak == kSettle.groundTicks - 1);
    CHECK(!update(r, ground(moved + kSettle.groundTicks, moved, true, true)));   // dash
    CHECK(r.groundStreak == 0);
    for (int i = 1; i < kSettle.groundTicks; ++i) update(r, ground(moved + 100 + i, moved, true));
    CHECK(!update(r, ground(moved + 100 + kSettle.groundTicks, moved, true, false, true)));   // orb
    CHECK(r.groundStreak == 0);
    CHECK(!update(r, ground(moved + 200, moved, false)));
    CHECK(r.groundStreak == 0);

    SECTION("ground ticks BEFORE the input never count: the streak must lie after the last moved input");
    SettleState b;
    // on the ground long before the input (the copy starts from a snapshot before the input)
    for (int i = 0; i < 100; ++i) update(b, ground(moved - 100 + i, moved, true));
    CHECK(b.groundStreak == 100 && !b.settled);
    // still on the ground right after the input: settled only once groundTicks have passed since it
    for (int i = 1; i < kSettle.groundTicks; ++i) CHECK(!update(b, ground(moved + i, moved, true)));
    CHECK(update(b, ground(moved + kSettle.groundTicks, moved, true)));
}

void testFlyingModes() {
    SECTION("a flying-mode copy cannot touch the ground: settled when alive flySeconds after the input");
    SettleState s;
    double moved = 500.0;
    double need = kSettle.flySeconds * 240.0;
    CHECK(!update(s, flying(moved + need - 1.0, moved)));
    CHECK(update(s, flying(moved + need, moved)));
    // a mode change mid-trial: the ground streak starts from zero in the new mode
    SettleState m;
    update(m, flying(moved + 10.0, moved));
    CHECK(m.groundStreak == 0 && !m.settled);
    for (int i = 1; i <= kSettle.groundTicks; ++i) update(m, ground(moved + 10.0 + i, moved, true));
    CHECK(m.settled);
}

void testConfig() {
    SECTION("off = never settled; the look-ahead falls back to the horizon setting");
    SettleConfig off;
    off.enabled = false;
    SettleState s;
    for (int i = 1; i <= 100; ++i) CHECK(!update(s, ground(1000.0 + i, 1000.0, true), off));
    CHECK(lookAheadSeconds(off, 0.5) == 0.5);
    CHECK(lookAheadSeconds(kSettle, 0.5) == 8.0);
    SettleConfig wide;
    wide.maxSeconds = 30.0;
    CHECK(lookAheadSeconds(wide, 0.5) == 10.0);   // clamped
    SettleConfig narrow;
    narrow.maxSeconds = 0.2;
    CHECK(lookAheadSeconds(narrow, 0.5) == 1.0);
    // the defaults the mod ships
    CHECK(kSettle.enabled && kSettle.maxSeconds == 8.0 && kSettle.groundTicks == 24 && kSettle.flySeconds == 0.75);
}

}  // namespace

int main() {
    testGroundModes();
    testFlyingModes();
    testConfig();
    return gprl::test::finish("settle_tests");
}
