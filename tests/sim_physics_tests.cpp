// Physics unit tests for the isolated simulator (core/sim/physics.*, collision.*): the constants
// table and every per-mode rule exercised through the pure functions, without the engine. The
// expected figures are derived from the constants inside the test (the cube jump height and
// airtime from yStart / gravity / dt / 0.9) so a constant change shows up here first.
#include "test_util.hpp"

#include "../core/sim/collision.hpp"
#include "../core/sim/physics.hpp"

#include <cmath>
#include <cstdio>

using namespace gprl::sim;

namespace {

PlayerState freshCube(bool mini = false) {
    PlayerState p;
    p.x = 0.0;
    p.y = 105.0;
    p.mode = Gamemode::Cube;
    p.mini = mini;
    applySpeed(p, Speed::Normal);
    p.onGround = true;
    return p;
}

/// A flat floor at the player's bottom: lands the player like checkCollisions does for the cube.
void floorSnap(PlayerState& p, double floorCentre, double t) {
    if (p.y < floorCentre && !p.boosted) {
        p.y = floorCentre;
        hitGround(p, -1, t);
    }
}

void testConstantsTable() {
    SECTION("speed table (updateTimeMod) and the units-per-second figures");
    CHECK_NEAR(speedParams(Speed::Half).playerSpeed, 0.7, 1e-12);
    CHECK_NEAR(speedParams(Speed::Normal).playerSpeed, 0.9, 1e-12);
    CHECK_NEAR(speedParams(Speed::Double).playerSpeed, 1.1, 1e-12);
    CHECK_NEAR(speedParams(Speed::Triple).playerSpeed, 1.3, 1e-12);
    CHECK_NEAR(speedParams(Speed::Quadruple).playerSpeed, 1.6, 1e-12);
    CHECK_NEAR(speedParams(Speed::Normal).speedMultiplier, 5.77000189, 1e-12);
    CHECK_NEAR(speedParams(Speed::Normal).yStart, 11.1800318, 1e-12);
    CHECK_NEAR(speedParams(Speed::Normal).gravity, 0.958199024, 1e-12);
    CHECK_NEAR(speedParams(Speed::Half).gravity, 0.940199, 1e-12);
    CHECK_NEAR(speedParams(Speed::Triple).yStart, 11.230032, 1e-12);
    CHECK_NEAR(speedParams(Speed::Quadruple).speedMultiplier, 6.000002, 1e-12);
    CHECK_NEAR(unitsPerSecond(Speed::Half), 251.16, 0.01);
    CHECK_NEAR(unitsPerSecond(Speed::Normal), 311.58, 0.01);
    CHECK_NEAR(unitsPerSecond(Speed::Double), 387.42, 0.01);
    CHECK_NEAR(unitsPerSecond(Speed::Triple), 468.0, 0.01);
    CHECK_NEAR(unitsPerSecond(Speed::Quadruple), 576.0, 0.01);

    SECTION("the step and gravity constants confirmed in 2.2081");
    CHECK(kConstants.dt == 0.25);
    CHECK(kConstants.verticalSlow == 0.9);
    CHECK(kConstants.flyerGravity == 0.9582);
    CHECK(kConstants.fallClamp == 15.0);
    CHECK(kConstants.flipGravityVelocityFactor == 0.5);
    CHECK(kConstants.innerHitboxScale == 0.3);
    CHECK(kConstants.snapThreshold == 10.0 && kConstants.snapThresholdFlyer == 6.0);
    CHECK(kConstants.floorY == 90.0);

    SECTION("size factors and hitboxes");
    CHECK(sizeFactor(Gamemode::Cube, false) == 1.0);
    CHECK(sizeFactor(Gamemode::Cube, true) == 0.8);
    CHECK(sizeFactor(Gamemode::Ship, true) == 0.85);
    CHECK(sizeFactor(Gamemode::Wave, true) == 0.85);
    CHECK(playerHalfSize(Gamemode::Cube, false) == 15.0);
    CHECK_NEAR(playerHalfSize(Gamemode::Cube, true), 9.0, 1e-12);
    CHECK_NEAR(playerHalfSize(Gamemode::Spider, false), 13.5, 1e-12);
    CHECK_NEAR(playerHalfSize(Gamemode::Wave, false), 5.0, 1e-12);
    CHECK_NEAR(playerHalfSize(Gamemode::Wave, true), 3.0, 1e-12);
    CHECK_NEAR(playerInnerHalfSize(Gamemode::Cube, false), 4.5, 1e-12);
    CHECK_NEAR(floorCentreY(Gamemode::Cube, false, 90.0), 105.0, 1e-12);
    CHECK_NEAR(floorCentreY(Gamemode::Cube, true, 90.0), 99.0, 1e-12);
    CHECK_NEAR(floorCentreY(Gamemode::Spider, false, 90.0), 103.5, 1e-12);
    CHECK_NEAR(floorCentreY(Gamemode::Wave, false, 90.0), 100.0, 1e-12);
    PlayerState p = freshCube();
    Rect r = playerRect(p);
    CHECK(r.x0 == -15.0 && r.x1 == 15.0 && r.y0 == 90.0 && r.y1 == 120.0);
    Rect in = playerInnerRect(p);
    CHECK_NEAR(in.x0, -4.5, 1e-12);
    CHECK_NEAR(in.y1, 109.5, 1e-12);
}

void testCubeJump() {
    SECTION("cube jump at 1x: apex and airtime derived from yStart / gravity / dt / 0.9");
    PlayerState p = freshCube();
    double const floorCentre = 105.0;
    // analytic: vy_k = yStart - k * g * dt (rounded to 1/1000 by setYVelocity), y += dt * 0.9 * vy
    double const gTick = p.gravity * kConstants.dt;
    double expectedApex = 0.0;
    for (int k = 0;; ++k) {
        double vy = std::round((p.yStart - k * gTick) * 1000.0) / 1000.0;
        if (vy <= 0.0) break;
        expectedApex += kConstants.dt * kConstants.verticalSlow * vy;
    }
    CHECK_NEAR(expectedApex, 60.0, 1.5);   // about two blocks (59.96 before rounding)
    double t = 0.0;
    p.jumpBuffered = true;
    p.held = true;
    bool jumped = updateJump(p, kConstants.dt, t);
    CHECK(jumped);
    CHECK_NEAR(p.yVelocity, 11.18, 1e-9);   // setYVelocity rounds 11.1800318 to 11.18
    CHECK(!p.onGround && p.boosted);
    p.jumpBuffered = false;
    p.held = false;
    integratePosition(p, kConstants.dt);
    double apex = p.y;
    int airTicks = 1;
    for (int i = 0; i < 400; ++i) {
        t += 1.0 / 240.0;
        updateJump(p, kConstants.dt, t);
        integratePosition(p, kConstants.dt);
        floorSnap(p, floorCentre, t);
        ++airTicks;
        apex = std::max(apex, p.y);
        if (p.onGround) break;
    }
    CHECK(p.onGround);
    CHECK_NEAR(apex - 105.0, expectedApex, 0.6);
    CHECK_MSG(airTicks >= 90 && airTicks <= 98, "airtime ticks = " + std::to_string(airTicks));
    std::printf("  cube jump 1x: apex %.3f units, %d ticks (%.1f ms), %.1f units of travel\n", apex - 105.0, airTicks,
                airTicks * 1000.0 / 240.0, airTicks * kConstants.dt * p.playerSpeed * p.speedMultiplier);

    SECTION("holding the button re-jumps on landing (auto-jump); mini jumps with 0.8 of the velocity");
    PlayerState q = freshCube(true);
    q.jumpBuffered = true;
    updateJump(q, kConstants.dt, 0.0);
    CHECK_NEAR(q.yVelocity, 11.18 * 0.8, 1e-9);
    PlayerState h = freshCube();
    h.jumpBuffered = true;
    h.held = true;
    int jumps = 0;
    double tt = 0.0;
    for (int i = 0; i < 600; ++i) {
        if (updateJump(h, kConstants.dt, tt)) ++jumps;
        integratePosition(h, kConstants.dt);
        floorSnap(h, 105.0, tt);
        tt += 1.0 / 240.0;
    }
    CHECK_MSG(jumps >= 5 && jumps <= 7, "auto-jumps in 600 ticks = " + std::to_string(jumps));

    SECTION("fall speed clamp at 15 for the non-boosted cube");
    PlayerState f = freshCube();
    f.onGround = false;
    f.y = 2000.0;
    for (int i = 0; i < 400; ++i) updateJump(f, kConstants.dt, 0.0);
    CHECK_NEAR(f.yVelocity, -15.0, 1e-9);
    CHECK(playerIsFallingBugged(f));
    PlayerState r = freshCube();
    r.yVelocity = 5.0;
    CHECK(!playerIsFallingBugged(r));
    r.yVelocity = 1.0;
    CHECK(playerIsFallingBugged(r));
}

void testBallRobotSpider() {
    SECTION("ball: a press flips gravity and keeps 0.3 of yStart (flip halves, then 0.6); the hold is consumed");
    PlayerState b = freshCube();
    b.mode = Gamemode::Ball;
    b.jumpBuffered = true;
    b.held = true;
    CHECK(updateJump(b, kConstants.dt, 1.0));
    CHECK(b.upsideDown);
    CHECK_NEAR(b.yVelocity, 11.18 * 0.5 * 0.6, 1e-9);
    CHECK(!b.jumpBuffered);
    CHECK(b.lastFlipTime == 1.0);
    // airborne ball gravity: 0.9582 * 0.6 per dt in the new direction (upward now)
    double before = b.yVelocity;
    updateJump(b, kConstants.dt, 1.0);
    CHECK_NEAR(b.yVelocity - before, 0.9582 * 0.25 * 0.6, 2e-3);

    SECTION("robot: half-height jump, the hold cancels gravity for 1.5 / (dt / 10) = 60 ticks, then 0.9 gravity");
    PlayerState r = freshCube();
    r.mode = Gamemode::Robot;
    r.robotHold = kConstants.robotHoldOnEnter;
    r.jumpBuffered = true;
    r.held = true;
    CHECK(!updateJump(r, kConstants.dt, 0.0));   // a hold alone does not jump the robot (needs the armed press)
    // updateJump clears onGround every tick (playerIsFallingBugged at vy = 0); the floor collision
    // restores it in GD - emulate the landing before the armed press
    r.onGround = true;
    r.yVelocity = 0.0;
    r.ringJumpArmed = true;
    CHECK(updateJump(r, kConstants.dt, 0.0));
    CHECK_NEAR(r.yVelocity, 11.18 * 0.5, 1e-9);
    CHECK(r.robotHold == 0.0 && !r.ringJumpArmed);
    int constantTicks = 0;
    for (int i = 0; i < 200; ++i) {
        double v = r.yVelocity;
        updateJump(r, kConstants.dt, 0.0);
        if (std::fabs(r.yVelocity - v) < 1e-9) ++constantTicks;
        else break;
    }
    // 60 increments of dt / 10 (0.025, not exactly representable) sum to just under 1.5: 61 ticks
    CHECK_MSG(constantTicks >= 60 && constantTicks <= 61, "robot hover ticks = " + std::to_string(constantTicks));
    double v0 = r.yVelocity;
    updateJump(r, kConstants.dt, 0.0);
    CHECK_NEAR(v0 - r.yVelocity, r.gravity * 0.25 * 0.9, 2e-3);

    SECTION("spider: updateJump leaves the ground jump to the engine (teleport) and applies 0.6 gravity in the air");
    PlayerState s = freshCube();
    s.mode = Gamemode::Spider;
    s.jumpBuffered = true;
    CHECK(!updateJump(s, kConstants.dt, 0.0));
    CHECK(s.onGround && s.yVelocity == 0.0);
    s.onGround = false;
    s.jumpBuffered = false;
    updateJump(s, kConstants.dt, 0.0);
    CHECK_NEAR(s.yVelocity, -0.9582 * 0.25 * 0.6, 2e-3);
}

void testShip() {
    SECTION("ship: hold thrust 0.4 g dt against gravity, release 1.2 g dt (0.8 when falling), caps -6.4 / 8");
    PlayerState s = freshCube();
    s.mode = Gamemode::Ship;
    s.onGround = false;
    s.y = 200.0;
    s.jumpBuffered = true;
    s.held = true;
    updateJump(s, kConstants.dt, 0.0);
    // held while "falling bugged" (vy < 2 g, including rest): v52 = 0.5 (2.2081 @0x38c65d), else 0.4
    CHECK_NEAR(s.yVelocity, std::round(0.9582 * 0.25 * 0.5 * 1000.0) / 1000.0, 1e-9);
    PlayerState s2 = freshCube();
    s2.mode = Gamemode::Ship;
    s2.yVelocity = 4.0;
    s2.jumpBuffered = true;
    updateJump(s2, kConstants.dt, 0.0);
    CHECK_NEAR(s2.yVelocity - 4.0, 0.9582 * 0.25 * 0.4, 2e-3);
    PlayerState r = freshCube();
    r.mode = Gamemode::Ship;
    r.onGround = false;
    r.y = 200.0;
    updateJump(r, kConstants.dt, 0.0);
    // released from rest: playerIsFallingBugged (vy < 2g) -> v51 = 0.8, v52 = 0.4
    CHECK_NEAR(r.yVelocity, -std::round(0.8 * 0.9582 * 0.25 * 0.4 * 1000.0) / 1000.0, 1e-9);
    PlayerState r2 = freshCube();
    r2.mode = Gamemode::Ship;
    r2.yVelocity = 5.0;   // rising fast: v51 = 1.2
    updateJump(r2, kConstants.dt, 0.0);
    CHECK_NEAR(5.0 - r2.yVelocity, 1.2 * 0.9582 * 0.25 * 0.4, 2e-3);
    for (int i = 0; i < 300; ++i) updateJump(s, kConstants.dt, 0.0);
    CHECK_NEAR(s.yVelocity, 8.0, 1e-9);
    for (int i = 0; i < 300; ++i) updateJump(r, kConstants.dt, 0.0);
    CHECK_NEAR(r.yVelocity, -6.4, 1e-9);

    SECTION("mini ship: the caps scale by 1 / 0.85 and the thrust by the same");
    PlayerState m = freshCube(true);
    m.mode = Gamemode::Ship;
    m.jumpBuffered = true;
    for (int i = 0; i < 400; ++i) updateJump(m, kConstants.dt, 0.0);
    CHECK_NEAR(m.yVelocity, std::round(8.0 / 0.85 * 1000.0) / 1000.0, 1e-9);   // 9.412 (setYVelocity rounds to 1/1000)

    SECTION("a boosted ship (accelerating) is not capped until the velocity is back inside the band");
    PlayerState b = freshCube();
    b.mode = Gamemode::Ship;
    b.yVelocity = 16.0;
    b.accelerating = true;
    updateJump(b, kConstants.dt, 0.0);
    CHECK(b.yVelocity > 15.0);
    CHECK(b.accelerating);
    b.yVelocity = 3.0;
    updateJump(b, kConstants.dt, 0.0);
    CHECK(!b.accelerating);

    SECTION("upside-down ship: the band is mirrored");
    PlayerState u = freshCube();
    u.mode = Gamemode::Ship;
    u.upsideDown = true;
    u.jumpBuffered = true;
    for (int i = 0; i < 400; ++i) updateJump(u, kConstants.dt, 0.0);
    CHECK_NEAR(u.yVelocity, -8.0, 1e-9);
}

void testUfoWaveSwing() {
    SECTION("ufo: a fresh press sets vy = 7 (mini 8 * 0.85), a hold does not repeat, gravity is half of the cube's");
    PlayerState u = freshCube();
    u.mode = Gamemode::Ufo;
    u.onGround = false;
    u.jumpBuffered = true;
    u.ringJumpArmed = true;
    updateJump(u, kConstants.dt, 0.0);
    double afterPress = u.yVelocity;
    CHECK_NEAR(afterPress, 7.0 - 1.2 * 0.9582 * 0.25 * 0.5, 2e-3);
    CHECK(!u.ringJumpArmed);
    updateJump(u, kConstants.dt, 0.0);
    CHECK(u.yVelocity < afterPress);
    PlayerState m = freshCube(true);
    m.mode = Gamemode::Ufo;
    m.jumpBuffered = true;
    m.ringJumpArmed = true;
    updateJump(m, kConstants.dt, 0.0);
    CHECK_NEAR(m.yVelocity, 8.0 * 0.85 - 1.2 * 0.9582 * 0.25 * 0.5 / 0.85, 2e-3);
    PlayerState f = freshCube();
    f.mode = Gamemode::Ufo;
    f.yVelocity = -5.0;
    updateJump(f, kConstants.dt, 0.0);
    CHECK_NEAR(-5.0 - f.yVelocity, 0.8 * 0.9582 * 0.25 * 0.5, 2e-3);   // falling: 0.8

    SECTION("wave: the position moves at 45 degrees (|dy| == |dx|), mini at 2:1, the velocity field = playerSpeed * speedMultiplier");
    PlayerState w = freshCube();
    w.mode = Gamemode::Wave;
    w.y = 200.0;
    w.jumpBuffered = true;
    updateJump(w, kConstants.dt, 0.0);
    CHECK_NEAR(w.yVelocity, 0.9 * 5.77000189, 1e-3);
    integratePosition(w, kConstants.dt);
    // gprl-sim/2: the position is a float (CCPoint) and m_playerSpeed a float, so the deltas carry
    // float rounding: dx within 1e-6 of the double figure, dy == dx within one float ulp at y = 200
    double const ulp200 = 1.0 / 65536.0;   // float spacing in [128, 256)
    double dx = w.x, dy = w.y - 200.0;
    CHECK_NEAR(dx, 0.25 * 0.9 * 5.77000189, 1e-6);
    CHECK(static_cast<double>(static_cast<float>(dx)) == dx);   // x stays on the float grid
    CHECK_NEAR(dy, dx, ulp200);
    w.jumpBuffered = false;
    updateJump(w, kConstants.dt, 0.0);
    integratePosition(w, kConstants.dt);
    CHECK_NEAR(w.y - (200.0 + dy), -dx, ulp200);
    PlayerState wm = freshCube(true);
    wm.mode = Gamemode::Wave;
    wm.y = 200.0;
    wm.jumpBuffered = true;
    updateJump(wm, kConstants.dt, 0.0);
    integratePosition(wm, kConstants.dt);
    CHECK_NEAR(wm.y - 200.0, 2.0 * dx, ulp200);
    PlayerState wu = freshCube();
    wu.mode = Gamemode::Wave;
    wu.upsideDown = true;
    wu.y = 200.0;
    wu.jumpBuffered = true;
    updateJump(wu, kConstants.dt, 0.0);
    integratePosition(wu, kConstants.dt);
    CHECK_NEAR(wu.y - 200.0, -dx, ulp200);

    SECTION("swing: a fresh press flips gravity and keeps 0.8 of the velocity; 0.4 g dt per tick (0.6 mini); cap 8");
    PlayerState s = freshCube();
    s.mode = Gamemode::Swing;
    s.yVelocity = -5.0;
    s.jumpBuffered = true;
    s.ringJumpArmed = true;
    updateJump(s, kConstants.dt, 2.0);
    CHECK(s.upsideDown && !s.ringJumpArmed);
    CHECK_NEAR(s.yVelocity, -5.0 * 0.8 + 0.9582 * 0.25 * 0.4, 2e-3);
    for (int i = 0; i < 400; ++i) updateJump(s, kConstants.dt, 2.0);
    CHECK_NEAR(s.yVelocity, 8.0, 1e-9);
}

void testFloatPositionDrift() {
    SECTION("H3: x accumulates in float like GD (CCPoint), 120 s at every speed; the /1 double sum drifts > 0.5 units");
    // GD's model (2.2081 PlayerObject::update 0x3894e0-0x38961e): dx = double(float m_playerSpeed) *
    // m_speedMultiplier * double(float dt), converted to float and added to the float x
    for (int s = 0; s <= 4; ++s) {
        Speed const sp = static_cast<Speed>(s);
        PlayerState p = freshCube();
        applySpeed(p, sp);
        CHECK(p.playerSpeed == static_cast<double>(static_cast<float>(speedParams(sp).playerSpeed)));
        float gdX = 0.f;
        double const gdDx = static_cast<double>(static_cast<float>(speedParams(sp).playerSpeed)) * speedParams(sp).speedMultiplier * 0.25;
        double doubleX = 0.0;
        double const doubleDx = 0.25 * speedParams(sp).playerSpeed * speedParams(sp).speedMultiplier;
        double maxDoubleDrift = 0.0;
        bool exact = true;
        for (int t = 1; t <= 28800; ++t) {
            integratePosition(p, kConstants.dt);
            gdX = gdX + static_cast<float>(gdDx);
            doubleX += doubleDx;
            if (p.x != static_cast<double>(gdX)) exact = false;
            maxDoubleDrift = std::max(maxDoubleDrift, std::fabs(doubleX - static_cast<double>(gdX)));
        }
        CHECK_MSG(exact, "speed " + std::to_string(s) + ": the engine's x left GD's float accumulation");
        CHECK_MSG(maxDoubleDrift > 0.5, "speed " + std::to_string(s) + ": the double sum drift " + std::to_string(maxDoubleDrift));
        std::printf("  speed %d: 120 s -> x %.4f (float, = GD), a double sum would have drifted up to %.3f units\n", s, p.x, maxDoubleDrift);
    }
    SECTION("H3: y integrates with the float factor dt * 0.9f and lands on the float grid");
    PlayerState q = freshCube();
    q.onGround = false;
    q.y = 300.0;
    q.yVelocity = 7.3;
    integratePosition(q, kConstants.dt);
    double const expect = static_cast<double>(300.f + static_cast<float>(static_cast<double>(0.25f * 0.9f) * 7.3));
    CHECK(q.y == expect);
    CHECK(gdFloat(q.y) == q.y);
}

void testPadsOrbsFlips() {
    SECTION("flipGravity halves vy, clears onGround and stamps the time; a no-op when already flipped");
    PlayerState p = freshCube();
    p.yVelocity = 10.0;
    flipGravity(p, true, 3.0);
    CHECK(p.upsideDown && p.yVelocity == 5.0 && !p.onGround && p.lastFlipTime == 3.0);
    flipGravity(p, true, 4.0);
    CHECK(p.yVelocity == 5.0 && p.lastFlipTime == 3.0);

    SECTION("pads: 16 * force * (0.8 mini) * (0.6 ball / spider / swing); the gravity pad = 12.8 then the flip halves it");
    PlayerState c = freshCube();
    propellPlayer(c, padMultiplier(PadKind::Yellow, c.mode, c.mini));
    CHECK_NEAR(c.yVelocity, 16.0, 1e-9);
    CHECK(c.boosted && !c.onGround);
    PlayerState cm = freshCube(true);
    propellPlayer(cm, padMultiplier(PadKind::Yellow, cm.mode, cm.mini));
    CHECK_NEAR(cm.yVelocity, 12.8, 1e-9);
    PlayerState b = freshCube();
    b.mode = Gamemode::Ball;
    propellPlayer(b, padMultiplier(PadKind::Yellow, b.mode, b.mini));
    CHECK_NEAR(b.yVelocity, 9.6, 1e-9);
    PlayerState pk = freshCube();
    propellPlayer(pk, padMultiplier(PadKind::Pink, pk.mode, pk.mini));
    CHECK_NEAR(pk.yVelocity, 10.4, 1e-9);
    PlayerState rd = freshCube();
    propellPlayer(rd, padMultiplier(PadKind::Red, rd.mode, rd.mini));
    CHECK_NEAR(rd.yVelocity, 20.0, 1e-9);
    PlayerState sh = freshCube();
    sh.mode = Gamemode::Ship;
    propellPlayer(sh, padMultiplier(PadKind::Red, sh.mode, sh.mini));
    CHECK_NEAR(sh.yVelocity, 10.08, 1e-9);
    PlayerState shp = freshCube();
    shp.mode = Gamemode::Ship;
    propellPlayer(shp, padMultiplier(PadKind::Pink, shp.mode, shp.mini));
    CHECK_NEAR(shp.yVelocity, 5.6, 1e-9);
    PlayerState uf = freshCube();
    uf.mode = Gamemode::Ufo;
    propellPlayer(uf, padMultiplier(PadKind::Red, uf.mode, uf.mini));
    CHECK_NEAR(uf.yVelocity, 9.6, 1e-9);
    PlayerState gp = freshCube();
    propellPlayer(gp, padMultiplier(PadKind::Gravity, gp.mode, gp.mini));
    CHECK_NEAR(gp.yVelocity, 12.8, 1e-9);
    flipGravity(gp, true, 0.0);
    CHECK_NEAR(gp.yVelocity, 6.4, 1e-9);
    PlayerState ud = freshCube();
    ud.upsideDown = true;
    propellPlayer(ud, padMultiplier(PadKind::Yellow, ud.mode, ud.mini));
    CHECK_NEAR(ud.yVelocity, -16.0, 1e-9);

    SECTION("orb factors (ringJump 0x398c00)");
    CHECK(orbFactor(OrbKind::Yellow, Gamemode::Cube, false) == 1.0);
    CHECK(orbFactor(OrbKind::Yellow, Gamemode::Robot, false) == 0.9);
    CHECK(orbFactor(OrbKind::Pink, Gamemode::Cube, false) == 0.72);
    CHECK(orbFactor(OrbKind::Pink, Gamemode::Ship, false) == 0.37);
    CHECK(orbFactor(OrbKind::Pink, Gamemode::Ufo, false) == 0.42);
    CHECK(orbFactor(OrbKind::Pink, Gamemode::Ball, false) == 0.77);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Cube, false) == 1.38);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Robot, false) == 1.28);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Ball, false) == 1.34);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Spider, false) == 1.34);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Ship, false) == 1.0);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Ship, true) == 1.4);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Ufo, false) == 1.02);
    CHECK(orbFactor(OrbKind::Red, Gamemode::Ufo, true) == 1.36);
    CHECK(orbFactor(OrbKind::Gravity, Gamemode::Cube, false) == 0.8);
    CHECK(orbFactor(OrbKind::Green, Gamemode::Ship, false) == 0.7);
    CHECK(orbFactor(OrbKind::Green, Gamemode::Cube, false) == 1.0);
    CHECK(blackOrbVelocity(Gamemode::Cube, false) == -15.0);
    CHECK(blackOrbVelocity(Gamemode::Cube, true) == 15.0);
    CHECK_NEAR(blackOrbVelocity(Gamemode::Spider, false), -16.5, 1e-9);
    CHECK(blackOrbVelocity(Gamemode::Ship, false) == -14.0);
    CHECK(blackOrbVelocity(Gamemode::Wave, false) == -14.0);
    CHECK_NEAR(blackOrbVelocity(Gamemode::Ufo, false), -11.2, 1e-9);

    SECTION("mode switches: entering or leaving a flying mode halves vy (ship -> ufo quarters); ball / robot / spider keep it");
    PlayerState s = freshCube();
    s.yVelocity = 8.0;
    switchMode(s, Gamemode::Ship, 1.0);
    CHECK(s.mode == Gamemode::Ship && s.yVelocity == 4.0 && !s.onGround && s.modeChangedTime == 1.0);
    switchMode(s, Gamemode::Ufo, 2.0);
    CHECK(s.yVelocity == 1.0);
    switchMode(s, Gamemode::Cube, 3.0);
    CHECK(s.yVelocity == 0.5);
    switchMode(s, Gamemode::Ball, 4.0);
    CHECK(s.yVelocity == 0.5);
    switchMode(s, Gamemode::Robot, 5.0);
    CHECK(s.yVelocity == 0.5 && s.robotHold == 1.5);
    switchMode(s, Gamemode::Robot, 6.0);
    CHECK(s.modeChangedTime == 5.0);
    switchMode(s, Gamemode::Wave, 7.0);
    CHECK(s.yVelocity == 0.25);
}

void testGeometry() {
    SECTION("rects, slopes and hazards");
    SimObject spike;
    spike.objectId = 8;
    spike.kind = ObjKind::Hazard;
    spike.x = 300.f; spike.y = 105.f;
    spike.rx = 297.f; spike.ry = 99.f; spike.rw = 6.f; spike.rh = 12.f;
    PlayerState p = freshCube();
    p.x = 281.0;
    CHECK(!hazardHits(spike, p));
    p.x = 282.0;
    CHECK(hazardHits(spike, p));
    p.x = 300.0;
    p.y = 127.0;   // bottom 112 > top 111
    CHECK(!hazardHits(spike, p));
    SimObject saw;
    saw.objectId = 88;
    saw.kind = ObjKind::Hazard;
    saw.x = 500.f; saw.y = 200.f;
    saw.rx = 500.f - 32.3f; saw.ry = 200.f - 32.3f; saw.rw = 64.6f; saw.rh = 64.6f;
    PlayerState q = freshCube();
    q.x = 500.0 - 32.3 - 15.0 + 0.5;   // the AABB corner overlaps, the circle does not
    q.y = 200.0 + 32.3 + 15.0 - 0.5;
    CHECK(!hazardHits(saw, q));
    q.x = 500.0 - 40.0;
    q.y = 200.0;
    CHECK(hazardHits(saw, q));
    SimObject rotated = spike;
    rotated.rotation = 45.f;
    rotated.rx = 300.f - 6.364f; rotated.ry = 105.f - 6.364f; rotated.rw = 12.728f; rotated.rh = 12.728f;
    PlayerState r = freshCube();
    r.x = 300.0 - 6.364 - 15.0 + 0.3;
    r.y = 105.0 + 6.364 + 15.0 - 0.3;   // the AABB corner only
    CHECK(!hazardHits(rotated, r));
    r.x = 300.0; r.y = 120.0;
    CHECK(hazardHits(rotated, r));

    SimObject sl;
    sl.kind = ObjKind::Slope;
    sl.slope = 1;   // "/" floor slope 30 x 30 at (600, 120)
    sl.x = 600.f; sl.y = 120.f;
    sl.rx = 585.f; sl.ry = 105.f; sl.rw = 30.f; sl.rh = 30.f;
    CHECK(!slopeFloorTop(sl) && slopeUphill(sl));
    CHECK_NEAR(slopeAngle(sl), std::atan(1.0), 1e-12);
    CHECK_NEAR(slopeYPos(sl, 585.0), 105.0, 1e-9);
    CHECK_NEAR(slopeYPos(sl, 600.0), 120.0, 1e-9);
    CHECK_NEAR(slopeYPos(sl, 615.0), 135.0, 1e-9);
    Rect broad = slopeBroadRect(sl);
    CHECK_NEAR(broad.x0, 570.0, 1e-9);
    CHECK_NEAR(broad.y1, 150.0, 1e-9);
    SimObject sl2 = sl;
    sl2.slope = 2;
    CHECK_NEAR(slopeYPos(sl2, 585.0), 135.0, 1e-9);
    CHECK_NEAR(slopeYPos(sl2, 615.0), 105.0, 1e-9);
    PlayerState w = freshCube();
    w.x = 590.0;
    w.y = 105.0;
    w.lastY = 105.0;
    SlopeContact c = slopeContact(sl, 0, w, 1.0);
    CHECK(c.contact && !c.death && !c.ceiling && !c.downhill);
    CHECK_NEAR(c.newY, slopeYPos(sl, 590.0) + 15.0 / std::cos(std::atan(1.0)), 1e-9);
    CHECK(c.slopeVelocity > 0.0);
    CHECK_NEAR(c.slopeYVelocity, 0.9 * 5.77000189, 1e-6);
    w.mode = Gamemode::Wave;
    SlopeContact cw = slopeContact(sl, 0, w, 1.0);
    CHECK(cw.contact && cw.death);
}

void testGdCollisionBoxes() {
    SECTION("gprl-extract/2 radius: GD's playerCircleCollision (centre inside the rect, or a rect CORNER closer than r), r scaled by max(scaleX, scaleY)");
    SimObject saw;
    saw.objectId = 1705;
    saw.kind = ObjKind::Hazard;
    saw.gdType = 2;
    saw.x = 500.f; saw.y = 200.f;
    saw.rx = 480.f; saw.ry = 180.f; saw.rw = 40.f; saw.rh = 40.f;
    saw.radius = 20.f;
    CHECK(gdCircleRadius(saw) == 20.0);
    PlayerState g = freshCube();
    g.x = 500.0;
    g.y = 233.0;   // the rect's bottom edge (y 218) crosses the circle (top 220) in its middle; corners 23.4 away
    CHECK(!hazardHits(saw, g));
    SimObject sawOld = saw;   // the /1 fallback (no radius, saw id): closest point -> a hit
    sawOld.radius = 0.f;
    sawOld.objectId = 1705;
    CHECK(hazardHits(sawOld, g));
    g.x = 525.0;
    g.y = 225.0;   // corner (510, 210): 14.1 from the centre
    CHECK(hazardHits(saw, g));
    g.x = 500.0;
    g.y = 210.0;   // the centre inside the rect
    CHECK(hazardHits(saw, g));
    SimObject scaled = saw;
    scaled.radius = 10.f;
    scaled.scaleX = 2.f;
    scaled.scaleY = 1.5f;
    CHECK(gdCircleRadius(scaled) == 20.0);
    g.x = 525.0;
    g.y = 225.0;
    CHECK(hazardHits(scaled, g));
    SimObject small = saw;
    small.radius = 10.f;   // scale 1 / 1: 10, the corner at 14.1 is out
    CHECK(!hazardHits(small, g));
    CHECK(circleHitsRect(0.0, 0.0, 5.0, Rect{3.0, 3.0, 10.0, 10.0}));    // corner at 4.24
    CHECK(!circleHitsRect(0.0, 0.0, 4.0, Rect{3.0, 3.0, 10.0, 10.0}));
    CHECK(!circleHitsRect(0.0, 0.0, 4.0, Rect{-10.0, 3.0, 10.0, 10.0}));  // the edge at 3 crosses the circle, no corner: GD misses it

    SECTION("gprl-extract/2 oriented: GD's own oriented box (centre / size / angle) decides; the AABB reconstruction is only a fallback");
    // a 6 x 30 hazard turned 45 degrees: the true box reaches 15 along its long diagonal, the /1
    // reconstruction (a square 18 x 18) only 9
    double const ang = -45.0 * 3.14159265358979323846 / 180.0;
    SimObject bar;
    bar.objectId = 1;
    bar.kind = ObjKind::Hazard;
    bar.gdType = 2;
    bar.rotation = 45.f;
    bar.x = 300.f; bar.y = 300.f;
    double const half = std::fabs(std::cos(ang)) * 3.0 + std::fabs(std::sin(ang)) * 15.0;   // the bounding rect's half size
    bar.rx = static_cast<float>(300.0 - half); bar.ry = static_cast<float>(300.0 - half);
    bar.rw = static_cast<float>(2.0 * half); bar.rh = static_cast<float>(2.0 * half);
    bar.oriented = true;
    bar.obbCx = 300.f; bar.obbCy = 300.f;
    bar.obbW = 6.f; bar.obbH = 30.f;
    bar.obbAngle = static_cast<float>(ang);
    PlayerState w = freshCube(true);
    w.mode = Gamemode::Wave;   // a 6 x 6 box
    // the long axis (0, 15) turned by -45 degrees points to the upper right: put the wave's lower-left
    // corner 11.8 units out along it - inside the bar, outside the reconstructed square
    w.x = 300.0 + 16.0 * std::sqrt(0.5);   // centre 16 out along the diagonal: the lower-left corner at 11.8
    w.y = 300.0 + 16.0 * std::sqrt(0.5);
    CHECK(hasOrientedBox(bar));
    CHECK(hazardHits(bar, w));
    SimObject legacy = bar;
    legacy.oriented = false;
    legacy.obbW = legacy.obbH = 0.f;
    CHECK(!hazardHits(legacy, w));   // the square reconstruction misses the bar's end
    // the AABB corner only: no hit with GD's box
    PlayerState c = freshCube(true);
    c.mode = Gamemode::Wave;
    c.x = 300.0 - half + 1.0;
    c.y = 300.0 + half - 1.0;
    CHECK(intersects(playerRect(c), objectRect(bar)));
    CHECK(!hazardHits(bar, c));
    // the same rule for a portal / pad (objectOverlaps)
    SimObject pad = bar;
    pad.kind = ObjKind::Pad;
    CHECK(!objectOverlaps(pad, playerRect(c)));
    CHECK(objectOverlaps(pad, playerRect(w)));
    SimObject plain = pad;
    plain.oriented = false;
    plain.obbW = plain.obbH = 0.f;
    CHECK(objectOverlaps(plain, playerRect(c)));   // without GD's box: the AABB
}

}  // namespace

int main() {
    testConstantsTable();
    testCubeJump();
    testBallRobotSpider();
    testShip();
    testUfoWaveSwing();
    testFloatPositionDrift();
    testPadsOrbsFlips();
    testGeometry();
    testGdCollisionBoxes();
    return gprl::test::finish("sim_physics_tests");
}
