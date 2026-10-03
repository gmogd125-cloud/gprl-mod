#pragma once
// The isolated simulator's physics constants and per-gamemode kinematics (docs/BACKGROUND_ANALYZER_
// DESIGN.md §4.2, `gprl-sim/1`). PURE C++20, no GD symbols.
//
// Every number is named and carries its source. Confirmation legend:
//   "2.2081 @0x..."   the float / double immediate was read from the Windows 2.2081 executable at
//                     that RVA (read-only disassembly with D:\claude-scratch\gprl-physics\floatdis.py,
//                     a gddis.py variant that resolves rip-relative constant loads; symbol names from
//                     D:\GeodeMods\_research\GeometryDash.bro)
//   "gdp 2.2"         taken from the camila314 2.2 decompile (D:\DecoForge-research\sources\camila314_gdp)
//                     and not re-read from the 2.2081 binary
//   "gdsim empirical" taken from the seanlnge Rust simulator (D:\DecoForge-research\sources\
//                     seanlnge_gd-simulate), an empirical second opinion, unconfirmed in 2.2081
// Function RVAs quoted below (win 2.2081): PlayerObject::update 0x388d80, updateJump 0x38b900,
// updateTimeMod 0x3a0d50, flipGravity 0x39a1d0, ringJump 0x398c00, propellPlayer 0x39f850,
// bumpPlayer 0x39f6a0, playerIsFallingBugged 0x39a430, hitGround 0x39bf30, collidedWithObjectInternal
// 0x391a70, toggleFlyMode 0x39a4f0, toggleBirdMode 0x39a820, toggleDartMode 0x39af90, toggleRollMode
// 0x39b570, toggleRobotMode 0x39b6f0, toggleSpiderMode 0x39ba70, toggleSwingMode 0x39ab20,
// togglePlayerScale 0x3a0700, GJBaseGameLayer::checkCollisions 0x2137f0, collisionCheckObjects
// 0x214960, GJBaseGameLayer::bumpPlayer 0x2179d0, getMinPortalY 0x213690, getMaxPortalY 0x213770.
#include <cstdint>

#include "engine.hpp"
#include "world.hpp"

namespace gprl::sim {

/// PlayerObject::updateTimeMod (gdp 2.2 PlayerObject_updateTimeMod.cpp; the same table is the Rust
/// SpeedProfile). m_playerSpeed / m_speedMultiplier / m_yStart / m_gravity per speed portal.
struct SpeedParams {
    double playerSpeed = 0.9;
    double speedMultiplier = 5.77000189;
    double yStart = 11.1800318;
    double gravity = 0.958199024;
};

struct Constants {
    // ---- step ----
    double dt = kStepDt;                     // 0.25 per 240 Hz tick, 1/60 s units (GD_PHYSICS_NOTES)
    double verticalSlow = 0.9;               // 2.2081 @0x388deb: update() multiplies dt by 0.9 for the y integration
    double ticksPerSecond = kTicksPerSecond;

    // ---- speeds (gdp 2.2 updateTimeMod; ids 200 / 201 / 202 / 203 / 1334) ----
    SpeedParams speeds[5] = {
        {0.7, 5.980002, 10.620032, 0.940199},        // Half
        {0.9, 5.77000189, 11.1800318, 0.958199024},  // Normal (also the fallback branch)
        {1.1, 5.870002, 11.420032, 0.957199},        // Double
        {1.3, 6.000002, 11.230032, 0.961199},        // Triple
        {1.6, 6.000002, 11.230032, 0.961199},        // Quadruple
    };

    // ---- updateJump (2.2081 0x38b900) ----
    double flyerGravity = 0.9582;            // 2.2081 @0x38ba86: ball / spider / flying modes use 0.9582 instead of m_gravity
    double miniJumpFactor = 0.8;             // 2.2081 @0x38babd: v16 for non-flyers when m_vehicleSize != 1
    double miniFlyerFactor = 0.85;           // 2.2081 @0x38c4ff: v16 for flyers when mini (caps and thrust divide by it)
    double airFactorCube = 1.0;              // float_b (also robot's base)
    double airFactorRobot = 0.9;             // 2.2081 @0x38bb2e
    double airFactorBallSpiderSwing = 0.6;   // 2.2081 @0x38bb0d / @0x38bb20
    double fallClamp = 15.0;                 // 2.2081 @0x38c303 (-15) / @0x38c30d (15): non-boosted ground-mode fall speed
    double robotJumpFactor = 0.5;            // 2.2081 @0x38bc44: robot yStart * 0.5
    double robotHoldMax = 1.5;               // 2.2081 @0x38c042: m_accelerationOrSpeed < 1.5 keeps the hover
    double robotHoldPerDt = 0.1;             // gdp 2.2 line 426: m_accelerationOrSpeed += dt / 10
    double robotHoldOnEnter = 1.5;           // 2.2081 toggleRobotMode @0x39b77a: m_accelerationOrSpeed = 1.5 (no hover until a jump)
    double ballJumpAfterFlip = 0.6;          // 2.2081 @0x38be86: ball vy *= 0.6 after flipGravity (which halves)
    double fallingBuggedGravityMult = 2.0;   // 2.2081 playerIsFallingBugged @0x39a488: falling when vy * flipMod < 2 * gravity
    double slopeJumpBonus = 0.25;            // 2.2081 @0x38bdda: + slopeVelocity * 0.25 on a jump from a slope
    double slopeJumpCap = 1.4;               // 2.2081 @0x38bd7d: capped at 1.4 * the plain jump
    double ufoSlopeBonus = 0.5;              // 2.2081 @0x38c7c8

    // ship (flyer branch, gdp lines 237-280; 2.2081 0x38c5a5-0x38c6ad)
    double shipThrust = -1.0;                // 2.2081 @0x38c5c7: v51 while held (sign: thrust against gravity)
    double shipBoostDecay = 0.8;             // v51 while accelerating and moving up (held) / moving up (released)
    double shipReleaseFalling = 0.8;         // v51 released, playerIsFallingBugged
    double shipReleaseRising = 1.2;          // 2.2081 @0x38c623: v51 released, rising
    double shipV52 = 0.4;                    // 2.2081 @0x38c645
    double shipV52HeldFalling = 0.5;         // 2.2081 @0x38c645-0x38c65d: v52 = 0.5 only when m_jumpBuffered (dl) && playerIsFallingBugged;
                                             // gdp 2.2 also uses 0.5 for a RELEASED boost - the 2.2081 binary does not (re-checked 2026-10-02, review M7)
    // ufo
    double ufoPress = 7.0;                   // 2.2081 @0x38c72f
    double ufoPressMini = 8.0;               // 2.2081 xmm13 @0x38c508 reused at 0x38c747 (mini: 8 * 0.85)
    double ufoGravityHalf = 0.5;             // 2.2081 @0x38c8d2
    double ufoFalling = 0.8;                 // v41 when falling bugged
    double ufoRising = 1.2;                  // 2.2081 @0x38c8a1
    // swing
    double swingAccel = 0.4;                 // 2.2081 @0x38c961
    double swingAccelMini = 0.6;             // 2.2081 @0x38c979
    double swingFlipKeep = 0.8;              // 2.2081 @0x38c92b: vy = old vy * 0.8 after the flip
    // wave
    double waveVelocityUnit = 8.0;           // 2.2081 @0x38c9fb (overwritten at once by playerSpeed * speedMultiplier)
    double waveMiniSlope = 2.0;              // 2.2081 update @0x38955d: dy doubled when mini (63.4 degrees)
    // flyer caps (gdp lines 282-303; 2.2081 0x38ca9f-0x38cb1e)
    double flyerCapUp = 8.0;                 // 2.2081 @0x38c508
    double flyerCapDownFactor = 0.8;         // v42: ship / ufo -8 * 0.8 = -6.4 (@0x38c51a); swing 1.0
    double swingCap = 8.0;

    // ---- gravity flip ----
    double flipGravityVelocityFactor = 0.5;  // 2.2081 PlayerObject::flipGravity @0x39a2dc
    double headHitFlipGrace = 0.1;           // gdp 2.2 line 621: a solid overhead within 0.1 s of a flip pushes instead of killing
    double floorFlipGrace = 0.1;             // 2.2081 checkCollisions @0x213b45: upside-down cube below the floor within 0.1 s of a flip
    double safeModeChangeTime = 0.1;         // gdp 2.2 collidedWithSlopeInternal line 111 (isSafeMode / isSafeFlip)
    double spiderFlipGrace = 0.04;           // gdp 2.2 line 633

    // ---- mode toggles ----
    double modeToggleVelocityFactor = 0.5;   // 2.2081 toggleFlyMode @0x39a556, toggleBirdMode @0x39a89d, toggleDartMode @0x39aff9, toggleSwingMode @0x39ab89 (roll / robot / spider: none)

    // ---- pads (2.2081 GJBaseGameLayer::bumpPlayer 0x2179d0 + PlayerObject::propellPlayer 0x39f850) ----
    double padBase = 16.0;                   // 2.2081 @0x39f8d0
    double padMini = 0.8;                    // 2.2081 @0x39f8ad
    double padBallSpiderSwing = 0.6;         // 2.2081 @0x39f912
    double padYellow = 1.0;                  // @0x217a62 default multiplier
    double padPinkShip = 0.35;               // @0x217a7d
    double padPinkUfo = 0.4;                 // @0x217a93
    double padPinkBall = 0.7;                // @0x217aa9
    double padPinkSpider = 0.7;              // @0x217abc
    double padPinkOther = 0.65;              // @0x217ac6
    double padRedShip = 0.63;                // @0x217af1 (normal size)
    double padRedShipMini = 0.95;            // @0x217ae7
    double padRedUfo = 0.6;                  // @0x217b17 (normal size)
    double padRedUfoMini = 0.98;             // @0x217b0d
    double padRedOther = 1.25;               // @0x217b21
    double padGravity = 0.8;                 // 2.2081 collisionCheckObjects @0x2159d5: propellPlayer(0.8) then flipGravity (halves)

    // ---- orbs (2.2081 PlayerObject::ringJump 0x398c00) ----
    double orbMini = 0.8;                    // @0x39922e
    double orbBall = 0.7;                    // @0x3995f1
    double orbSpider = 0.7;                  // @0x3995f1 (same branch)
    double orbSwing = 0.6;                   // @0x3995d6
    double orbGravityFactor = 0.8;           // @0x399254 (type 13 blue): then flipGravity (@0x39967c) halves it
    double orbGreenShip = 0.7;               // @0x39927f (type 29 green, ship only): flipGravity first (@0x39941e)
    double orbPinkShip = 0.37;               // @0x3992a6
    double orbPinkUfo = 0.42;                // @0x3992bc
    double orbPinkBall = 0.77;               // @0x3992d2
    double orbPinkOther = 0.72;              // @0x3992df
    double orbRedShip = 1.0;                 // normal-size ship: no multiplier (@0x399317 skips)
    double orbRedShipMini = 1.4;             // @0x39931d
    double orbRedUfo = 1.02;                 // @0x399342
    double orbRedUfoMini = 1.36;             // @0x39934c
    double orbRedBall = 1.34;                // @0x39935f
    double orbRedRobot = 1.28;               // @0x399372
    double orbRedSpider = 1.34;              // @0x399385
    double orbRedOther = 1.38;               // @0x39938f
    double orbYellowRobot = 0.9;             // @0x3993a2 (the yellow / default branch only)
    double orbBlackGround = -15.0;           // @0x3998a7 (cube / ball / robot; spider * 1.1 @0x3998e5)
    double orbBlackSpiderMult = 1.1;
    double orbBlackFlyer = -14.0;            // @0x3998fc (ship / wave / swing; ufo * 0.8 @0x39990d)
    double orbBlackUfoMult = 0.8;

    // ---- hitboxes and collision ----
    double playerSize = 30.0;                // GameObject m_width / m_height of the player (toggleDartMode(false) @0x39b30c writes 30)
    double spiderSize = 27.0;                // 2.2081 toggleSpiderMode @0x39baf8-0x39bb0c: m_width = m_height = base height = 27
    double waveSize = 10.0;                  // 2.2081 toggleDartMode @0x39b050-0x39b05a: m_width = m_height = 10 (base height 20 @0x39b046)
    double waveBaseHeight = 20.0;            // the floor-centre height used by checkCollisions for the wave
    double miniScale = 0.6;                  // 2.2081 togglePlayerScale @0x3a076c: m_vehicleSize = 0.6 (0x3f19999a)
    double innerHitboxScale = 0.3;           // 2.2081 collidedWithObjectInternal @0x393646 (getObjectRect(0.3, 0.3)): the death rect vs solids
    double snapThreshold = 10.0;             // 2.2081 @0x391bc1 (ground modes); both grow by unk_584 while m_wasOnSlope (@0x391c54)
    double snapThresholdFlyer = 6.0;         // 2.2081 @0x391c48 (flying modes, non-platformer)
    double slopeRectBroadphase = 2.0;        // 2.2081 collisionCheckObjects @0x214a0b: slopes are matched with getObjectRect(2, 2)
    double slopeExitInset = 1.0;             // gdp 2.2 collidedWithSlopeInternal lines 54-57
    double slopeVelocityAngleNum = 1.12;     // gdp 2.2 line 266 (Rust: same)
    double slopeVelocityMax = 1.54;          // gdp 2.2 line 266
    double slopeVelocityFlyerBall = 0.75;    // gdp 2.2 line 273
    double slopeLaunchMinFactor = 0.4;       // 2.2081 postCollision @0x38d85d: max(0.4, timeOnSlope * 10) below 0.1 s
    double slopeLaunchRamp = 10.0;           // 2.2081 postCollision @0x38d855
    double slopeLaunchFullTime = 0.1;        // 2.2081 postCollision @0x38d847
    double slopeNewScalar = 20.0;            // gdp 2.2 line 67: m_vehicleSize * 20 when switching floor / ceiling slopes
    double floorY = 90.0;                    // 2.2081 checkCollisions @0x2139ac and getMinPortalY @0x2136c4: the player's bottom rests at 90
    double ceilingDeathY = 2505.0;           // gdsim empirical (seanlnge ground_bounds.rs: 30 * 80 + 105); GD: y > m_maxGameplayY (+0x36a8) kills (checkCollisions @0x213c12), the value is not extracted in /1
    double spiderSearchRange = 3000.0;       // 2.2081 spiderTestJumpInternal @0x39456a: the teleport looks up to 3000 units away
    double snapJumpThresholdSlow = 1.0;      // gdp 2.2 checkSnapJumpToObject: 0.5x / 1x (mini 4x) tolerance 1, else 2
    double snapJumpThresholdFast = 2.0;
};

inline constexpr Constants kConstants{};

/// The speed table entry for a speed index.
constexpr SpeedParams const& speedParams(Speed s) {
    int i = static_cast<int>(s);
    if (i < 0 || i > 4) i = 1;
    return kConstants.speeds[i];
}

/// GD's m_playerSpeed value for a speed index (0.7 / 0.9 / 1.1 / 1.3 / 1.6).
constexpr double playerSpeedOf(Speed s) { return speedParams(s).playerSpeed; }

/// Units per second along x at a speed (dt * playerSpeed * speedMultiplier * 240 / 4): 251.16 /
/// 311.58 / 387.42 / 468 / 576 (the well-known GD figures).
constexpr double unitsPerSecond(Speed s) {
    return speedParams(s).playerSpeed * speedParams(s).speedMultiplier * kConstants.ticksPerSecond * kStepDt;
}

/// m_vehicleSize: 1.0 or 0.6 (mini).
constexpr double vehicleSize(bool mini) { return mini ? kConstants.miniScale : 1.0; }

/// updateJump's v16: the jump / orb / pad size factor. Flyers use 0.85, ground modes 0.8 when mini.
constexpr double sizeFactor(Gamemode mode, bool mini) {
    if (!mini) return 1.0;
    return flyingMode(mode) ? kConstants.miniFlyerFactor : kConstants.miniJumpFactor;
}

/// The player's base hitbox side (GameObject m_width == m_height) before the vehicle scale.
constexpr double playerBaseSize(Gamemode mode) {
    if (mode == Gamemode::Spider) return kConstants.spiderSize;
    if (mode == Gamemode::Wave) return kConstants.waveSize;
    return kConstants.playerSize;
}

/// Half of the player's collision rect side: 15 cube (9 mini), 13.5 spider (8.1), 5 wave (3).
constexpr double playerHalfSize(Gamemode mode, bool mini) {
    return playerBaseSize(mode) * vehicleSize(mini) * 0.5;
}

/// Half of the death rect vs solids (getObjectRect(0.3, 0.3)): 4.5 cube, 2.7 mini cube, 1.5 wave.
constexpr double playerInnerHalfSize(Gamemode mode, bool mini) {
    return playerHalfSize(mode, mini) * kConstants.innerHitboxScale;
}

/// The "height" field (+0xab0) checkCollisions uses for the floor centre: 30 (27 spider, 20 wave).
constexpr double playerBaseHeight(Gamemode mode) {
    if (mode == Gamemode::Spider) return kConstants.spiderSize;
    if (mode == Gamemode::Wave) return kConstants.waveBaseHeight;
    return kConstants.playerSize;
}

/// The player's centre y when its bottom rests on the floor line `floorY`
/// (checkCollisions @0x2139a8: floorY + h / 2 - (1 - size) * h / 2 = floorY + h * size / 2).
constexpr double floorCentreY(Gamemode mode, bool mini, double floorY) {
    return floorY + playerBaseHeight(mode) * vehicleSize(mini) * 0.5;
}

/// Mirror of the above for a ceiling line.
constexpr double ceilingCentreY(Gamemode mode, bool mini, double ceilingY) {
    return ceilingY - playerBaseHeight(mode) * vehicleSize(mini) * 0.5;
}

/// PlayerObject::flipMod(): +1 normal gravity, -1 upside down.
constexpr double flipMod(bool upsideDown) { return upsideDown ? -1.0 : 1.0; }

/// GD keeps the player's position in a cocos2d CCPoint (two floats): PlayerObject::update adds the
/// float-converted dx / dy to the float position (2.2081 0x3895e0-0x38961e: cvtpd2ps, ccpAdd,
/// setPosition) and every setPositionX / Y stores a float. The simulator keeps doubles everywhere
/// else but rounds each position write through this (gprl-sim/2; /1 accumulated x in double and
/// drifted > 0.5 units from GD after 20-40 s, which failed verification on long levels).
constexpr double gdFloat(double v) { return static_cast<double>(static_cast<float>(v)); }

/// Modes that share the ground / ceiling corridor code of checkCollisions (@0x2139c1-0x2139fe):
/// ship, ufo, wave, swing, ball, spider. Cube and robot have the floor only.
constexpr bool corridorMode(Gamemode g) {
    return flyingMode(g) || g == Gamemode::Ball || g == Gamemode::Spider;
}

/// The gravity updateJump applies: 0.9582 for ball / spider / flyers, m_gravity (speed dependent)
/// for cube / robot (2.2081 @0x38ba86 / gdp line 112).
constexpr double usedGravity(PlayerState const& p) {
    return (p.mode == Gamemode::Ball || p.mode == Gamemode::Spider || flyingMode(p.mode)) ? kConstants.flyerGravity : p.gravity;
}

/// updateJump's float_b: the air gravity factor per mode (gdp lines 309-315).
constexpr double airGravityFactor(Gamemode g) {
    if (g == Gamemode::Ball || g == Gamemode::Spider || g == Gamemode::Swing) return kConstants.airFactorBallSpiderSwing;
    if (g == Gamemode::Robot) return kConstants.airFactorRobot;
    return kConstants.airFactorCube;
}

/// PlayerObject::playerIsFallingBugged (2.2081 0x39a430): true when the velocity against gravity is
/// below 2 * m_gravity (the player is "falling" or barely rising). Swing uses the same test with the
/// doubled gravity cast to float; the sideways / platformer / reversed paths are not modelled.
inline bool playerIsFallingBugged(PlayerState const& p) {
    double twoG = 2.0 * p.gravity;
    if (p.mode == Gamemode::Swing) twoG = static_cast<double>(static_cast<float>(twoG));
    if (!p.upsideDown) return twoG > p.yVelocity;
    return p.yVelocity > -twoG;
}

/// Pad multiplier for GJBaseGameLayer::bumpPlayer (before the 16 base and the size / mode factors).
double padMultiplier(PadKind kind, Gamemode mode, bool mini);

/// Orb factor applied to yStart for the non-black orbs (before size, mode and the gravity halving).
double orbFactor(OrbKind kind, Gamemode mode, bool mini);

/// Black orb velocity (sign already includes the gravity direction).
double blackOrbVelocity(Gamemode mode, bool upsideDown);

/// PlayerObject::flipGravity(flip, noEffects): no-op when already in that state; else sets the flag,
/// halves vy, clears onGround / slope state and stamps lastFlipTime (2.2081 0x39a1d0).
void flipGravity(PlayerState& p, bool upsideDown, double totalTime);

/// PlayerObject::updateTimeMod(speed): m_playerSpeed / m_yStart / m_gravity / m_speedMultiplier.
/// m_playerSpeed is a float field (+0x9f4): the table value is stored float-rounded (0.9 ->
/// 0.89999997615814209), like GD.
void applySpeed(PlayerState& p, Speed s);

/// PlayerObject::hitGround(obj, notFlipped) (2.2081 0x39bf30): vy = 0, onGround = onGround2 = true,
/// lastLandTime = t. `noJump` = hitGroundNoJump (flags preserved, only vy and the ground object).
void hitGround(PlayerState& p, int groundIndex, double totalTime, bool noJump = false);

/// PlayerObject::propellPlayer (pads, 2.2081 0x39f850): vy = flipMod * 16 * mult * size (0.6 for
/// ball / spider / swing), boosted, off the ground and off any slope.
void propellPlayer(PlayerState& p, double mult);

/// PlayerObject::boostPlayer (orbs / slope launch, gdp boostPlayer): vy = amount, boosted,
/// accelerating, off the ground.
void boostPlayer(PlayerState& p, double amount);

/// PlayerObject::updateJump(dt) (2.2081 0x38b900): the per-mode velocity update including the
/// ground jump for cube / ball / robot (spider jumps are handled by the engine: they teleport).
/// `totalTime` is the step's time in seconds (robot hover bookkeeping). Returns true when a ground
/// jump was consumed this call (so the collision pass does not ring-jump with a stale armed flag).
bool updateJump(PlayerState& p, double dt, double totalTime);

/// The position integration of PlayerObject::update after updateJump (2.2081 0x3894c1-0x38961e):
/// dx = double(float m_playerSpeed) * m_speedMultiplier * double(float dt); dy = double(float dt *
/// 0.9f) * vy, except the wave which moves by +-|dx| (x2 mini) per step (@0x389507-0x389561); both
/// are converted to float and added to the FLOAT position (gdFloat). Writes lastX / lastY first.
void integratePosition(PlayerState& p, double dt);

/// Mode switch at a gamemode portal (portal_model.hpp plans transcribed to the player part):
/// every toggle that changes a flying flag halves vy and clears onGround; robot entry sets the
/// hover budget to 1.5; the hitbox follows the mode (spider 27, wave 10). No-op for the same mode.
void switchMode(PlayerState& p, Gamemode target, double totalTime);

}  // namespace gprl::sim
