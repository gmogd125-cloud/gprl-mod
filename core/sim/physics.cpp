// Per-gamemode kinematics of the isolated simulator: a transcription of PlayerObject::updateJump /
// update / flipGravity / propellPlayer / ringJump factors (sources in physics.hpp). PURE C++20.
#include "physics.hpp"

#include <algorithm>
#include <cmath>

namespace gprl::sim {

namespace {

constexpr Constants const& C = kConstants;

inline bool isFlyer(PlayerState const& p) { return flyingMode(p.mode); }

/// updateJump's "moving up relative to gravity" test for the ship boost decay (2.2081 0x38c5e1).
inline bool movingUpRelative(PlayerState const& p) {
    return p.upsideDown ? (p.yVelocity <= 0.0) : (p.yVelocity >= 0.0);
}

/// setYVelocity (2.2081 0x388d10) rounds the fractional part to 1/1000 (round(frac * 1000) / 1000)
/// via the CRT round; the integer part is kept exact. Reproduced so vy stays on GD's grid.
inline double gdRoundVelocity(double v) {
    double ip = static_cast<double>(static_cast<long long>(v));   // trunc toward zero like cvttsd2si
    if (v == ip) return v;
    double frac = v - ip;
    double r = std::round(frac * 1000.0) / 1000.0;
    return ip + r;
}

inline void setYVelocity(PlayerState& p, double v) { p.yVelocity = gdRoundVelocity(v); }
inline void addToYVelocity(PlayerState& p, double dv) { setYVelocity(p, p.yVelocity + dv); }

}  // namespace

double padMultiplier(PadKind kind, Gamemode mode, bool mini) {
    switch (kind) {
        case PadKind::Yellow: return C.padYellow;
        case PadKind::Pink:
            if (mode == Gamemode::Ship) return C.padPinkShip;
            if (mode == Gamemode::Ufo) return C.padPinkUfo;
            if (mode == Gamemode::Ball) return C.padPinkBall;
            if (mode == Gamemode::Spider) return C.padPinkSpider;
            return C.padPinkOther;
        case PadKind::Red:
            if (mode == Gamemode::Ship) return mini ? C.padRedShipMini : C.padRedShip;
            if (mode == Gamemode::Ufo) return mini ? C.padRedUfoMini : C.padRedUfo;
            return C.padRedOther;
        case PadKind::Gravity: return C.padGravity;
        case PadKind::Spider: return 0.0;   // no propell: flip + teleport
    }
    return C.padYellow;
}

double orbFactor(OrbKind kind, Gamemode mode, bool mini) {
    // 2.2081 ringJump 0x399243-0x3993aa: the factor applied to m_yStart
    switch (kind) {
        case OrbKind::Gravity: return C.orbGravityFactor;
        case OrbKind::Green: return mode == Gamemode::Ship ? C.orbGreenShip : 1.0;
        case OrbKind::Pink:
            if (mode == Gamemode::Ship) return C.orbPinkShip;
            if (mode == Gamemode::Ufo) return C.orbPinkUfo;
            if (mode == Gamemode::Ball) return C.orbPinkBall;
            return C.orbPinkOther;
        case OrbKind::Red:
            if (mode == Gamemode::Ship) return mini ? C.orbRedShipMini : C.orbRedShip;
            if (mode == Gamemode::Ufo) return mini ? C.orbRedUfoMini : C.orbRedUfo;
            if (mode == Gamemode::Ball) return C.orbRedBall;
            if (mode == Gamemode::Robot) return C.orbRedRobot;
            if (mode == Gamemode::Spider) return C.orbRedSpider;
            return C.orbRedOther;
        default:
            // yellow and everything else that reaches the default branch (@0x399399)
            return mode == Gamemode::Robot ? C.orbYellowRobot : 1.0;
    }
}

double blackOrbVelocity(Gamemode mode, bool upsideDown) {
    // 2.2081 0x399886-0x399915: eax = upsideDown ? -1 : 1; vy = eax * -15 (ground) / -14 (flyers)
    double sign = upsideDown ? -1.0 : 1.0;
    if (flyingMode(mode)) {
        double v = sign * C.orbBlackFlyer;
        if (mode == Gamemode::Ufo) v *= C.orbBlackUfoMult;
        return v;
    }
    double v = sign * C.orbBlackGround;
    if (mode == Gamemode::Spider) v *= C.orbBlackSpiderMult;
    return v;
}

void flipGravity(PlayerState& p, bool upsideDown, double totalTime) {
    if (p.upsideDown == upsideDown) return;                 // 0x39a1e9: no-op when already there
    p.upsideDown = upsideDown;
    p.lastFlipTime = totalTime;                              // 0x39a229: m_lastFlipTime = m_totalTime
    p.yVelocity = p.yVelocity * C.flipGravityVelocityFactor;  // 0x39a2dc (m_maybeReducedEffects == 0)
    p.onGround = false;                                      // 0x39a3cd
    // the collision logs are cleared (0x39a276-0x39a2c0): the slope / ground references restart
    p.onSlopeIndex = -1;
    p.lastGroundIndex = -1;
}

void applySpeed(PlayerState& p, Speed s) {
    SpeedParams const& sp = speedParams(s);
    p.speed = s;
    p.playerSpeed = gdFloat(sp.playerSpeed);                 // m_playerSpeed is a float (+0x9f4)
    p.speedMultiplier = sp.speedMultiplier;
    p.yStart = sp.yStart;
    p.gravity = sp.gravity;
}

void hitGround(PlayerState& p, int groundIndex, double totalTime, bool noJump) {
    // 2.2081 hitGround: m_yVelocity = 0 (@0x39c164), m_isOnGround2 = 1 (@0x39c403), m_lastLandTime
    // (@0x39c42a), m_isOnGround = 1 (@0x39c431). hitGroundNoJump restores the three flags.
    p.yVelocity = 0.0;
    p.lastGroundIndex = groundIndex;
    if (noJump) return;
    p.onGround = true;
    p.onGround2 = true;
    p.lastLandTime = totalTime;
}

void propellPlayer(PlayerState& p, double mult) {
    // 2.2081 0x39f850
    p.boosted = true;         // +0xa1c
    p.onGround2 = false;      // +0xa0c
    p.onGround = false;       // +0x9c1
    p.onSlopeIndex = -1;      // +0x9b0 / +0x9b1 cleared
    p.wasOnSlope = false;
    double size = p.mini ? C.padMini : 1.0;
    double v = flipMod(p.upsideDown) * (mult * C.padBase) * size;
    setYVelocity(p, v);
    if (p.mode == Gamemode::Ball || p.mode == Gamemode::Spider || p.mode == Gamemode::Swing) {
        p.yVelocity = p.yVelocity * C.padBallSpiderSwing;   // @0x39f912 (direct multiply, no rounding)
    }
}

void boostPlayer(PlayerState& p, double amount) {
    // gdp PlayerObject::boostPlayer
    p.boosted = true;
    p.onGround2 = false;
    p.onGround = false;
    p.accelerating = true;
    setYVelocity(p, amount);
}

bool updateJump(PlayerState& p, double dt, double totalTime) {
    (void)totalTime;
    bool const onGround = p.onGround;
    bool const jumpBufferedAndRingJump = p.jumpBuffered && (p.ringJumpArmed || p.mode != Gamemode::Robot);   // gdp line 110
    double const g = usedGravity(p);        // float_c = usedGravity * m_gravityMod (gravityMod = 1 in /1)
    double const fm = flipMod(p.upsideDown);
    bool jumped = false;

    if (isFlyer(p)) {
        // ---- flyers (gdp lines 121-307; 2.2081 0x38c4ff-0x38cb36) ----
        double v16 = p.mini ? C.miniFlyerFactor : 1.0;
        double capUp = C.flyerCapUp / v16;                        // v30
        double capDown = -(C.flyerCapUp * C.flyerCapDownFactor) / v16;   // v33 = -6.4 / v16
        double v42 = C.flyerCapDownFactor;                        // ship / ufo / wave
        // the boost ends once vy is back inside the normal band (0x38c527-0x38c59e)
        if (!p.upsideDown) {
            if (p.yVelocity >= 0.0 && p.yVelocity < capUp) p.accelerating = false;
            if (p.yVelocity <= 0.0 && p.yVelocity > capDown) p.accelerating = false;
        } else {
            if (p.yVelocity <= 0.0 && p.yVelocity > -capUp) p.accelerating = false;
            if (p.yVelocity >= 0.0 && p.yVelocity < -capDown) p.accelerating = false;
        }

        if (p.mode == Gamemode::Ship) {
            double v51;
            if (p.jumpBuffered) {
                v51 = (p.accelerating && movingUpRelative(p)) ? C.shipBoostDecay : C.shipThrust;
            } else if (p.accelerating) {
                // released while boosted: decay the boost whichever way it points (0x38c5e1-0x38c623)
                v51 = !movingUpRelative(p) ? C.shipThrust : (playerIsFallingBugged(p) ? C.shipReleaseFalling : C.shipReleaseRising);
            } else {
                v51 = playerIsFallingBugged(p) ? C.shipReleaseFalling : C.shipReleaseRising;
            }
            double v52 = (p.jumpBuffered && playerIsFallingBugged(p)) ? C.shipV52HeldFalling : C.shipV52;
            addToYVelocity(p, -(v51 * g * dt * fm * v52 / v16));
            if (p.jumpBuffered) p.onGround2 = false;
        } else if (p.mode == Gamemode::Ufo) {
            if (p.ringJumpArmed && p.jumpBuffered) {
                p.ringJumpArmed = false;
                double v34 = p.mini ? C.ufoPressMini : C.ufoPress;
                double v37 = fm * v34 * v16;
                bool higher = p.upsideDown ? (v37 < p.yVelocity) : (v37 > p.yVelocity);
                if (higher) {
                    setYVelocity(p, v37);
                    if ((p.wasOnSlope || p.onSlopeIndex >= 0) && p.slopeVelocity > 0.0) {
                        double cap = p.yVelocity * C.slopeJumpCap;
                        addToYVelocity(p, p.slopeVelocity * C.ufoSlopeBonus);
                        if (p.yVelocity > cap) setYVelocity(p, cap);
                    }
                }
                jumped = true;
            }
            double v41 = playerIsFallingBugged(p) ? C.ufoFalling : C.ufoRising;
            addToYVelocity(p, -(g * dt * fm * v41 * C.ufoGravityHalf / v16));
            if (p.jumpBuffered) p.onGround2 = false;
        } else if (p.mode == Gamemode::Swing) {
            if (p.ringJumpArmed && p.jumpBuffered) {
                p.ringJumpArmed = false;
                double before = p.yVelocity;
                flipGravity(p, !p.upsideDown, totalTime);
                setYVelocity(p, before * C.swingFlipKeep);
                jumped = true;
            }
            double accel = p.mini ? C.swingAccelMini : C.swingAccel;
            double fm2 = flipMod(p.upsideDown);
            addToYVelocity(p, -(g * dt * fm2 * accel));
            v42 = 1.0;
            v16 = 1.0;
            if (p.jumpBuffered) p.onGround2 = false;
        } else {
            // wave: m_yVelocity carries the direction; the position moves by +-|dx| (integratePosition)
            double dir = p.jumpBuffered ? 1.0 : -1.0;
            setYVelocity(p, fm * C.waveVelocityUnit * dir);
            setYVelocity(p, p.playerSpeed * p.speedMultiplier * fm * dir);
        }

        if (!p.accelerating && p.mode != Gamemode::Wave) {
            double fmNow = flipMod(p.upsideDown);
            if (fmNow > 0.0) {
                double lo = v42 * -C.flyerCapUp / v16;
                double hi = C.flyerCapUp / v16;
                if (p.yVelocity < lo) setYVelocity(p, lo);
                if (p.yVelocity > hi) setYVelocity(p, hi);
            } else {
                double lo = -C.flyerCapUp / v16;
                double hi = v42 * C.flyerCapUp / v16;
                if (p.yVelocity < lo) setYVelocity(p, lo);
                if (p.yVelocity > hi) setYVelocity(p, hi);
            }
        }
        if (playerIsFallingBugged(p)) p.boosted = false;
    } else {
        // ---- cube / ball / robot / spider (gdp lines 309-477) ----
        double const b = airGravityFactor(p.mode);
        double const size = p.mini ? C.miniJumpFactor : 1.0;
        if (p.onGround && jumpBufferedAndRingJump && !p.dashing) {
            if (p.mode == Gamemode::Spider) {
                // spiderTestJump: the engine performs the teleport; nothing happens here
                return false;
            }
            p.onGround2 = false;
            p.boosted = true;
            p.onGround = false;
            p.ringJumpArmed = false;
            p.touchedPad = false;
            p.robotHold = 0.0;
            double yStart = p.yStart;
            if (p.mode == Gamemode::Robot) yStart *= C.robotJumpFactor;
            setYVelocity(p, fm * yStart * size);
            if (p.wasOnSlope || p.onSlopeIndex >= 0) {
                if (p.slopeVelocity * fm > 0.0) {
                    double cap = p.yVelocity * C.slopeJumpCap;
                    addToYVelocity(p, p.slopeVelocity * C.slopeJumpBonus);
                    if (p.upsideDown) {
                        if (p.yVelocity < cap) setYVelocity(p, cap);
                    } else {
                        if (p.yVelocity > cap) setYVelocity(p, cap);
                    }
                }
            }
            if (p.mode == Gamemode::Ball) {
                flipGravity(p, !p.upsideDown, totalTime);
                p.jumpBuffered = false;
                p.yVelocity = p.yVelocity * C.ballJumpAfterFlip;   // direct multiply @0x38be86
            }
            jumped = true;
        } else {
            if (p.boosted) {
                double floatD = fm * g * dt * b;
                if (p.mode == Gamemode::Robot && p.jumpBuffered && !p.touchedPad && p.robotHold < C.robotHoldMax) {
                    p.robotHold += dt * C.robotHoldPerDt;
                    addToYVelocity(p, floatD);
                }
                addToYVelocity(p, -floatD);
                if (playerIsFallingBugged(p)) {
                    p.boosted = false;
                    p.onGround2 = false;
                }
            } else {
                if (playerIsFallingBugged(p)) p.onGround = false;
                addToYVelocity(p, -(g * dt * fm * b));
                if (p.upsideDown) {
                    if (p.yVelocity > C.fallClamp) setYVelocity(p, C.fallClamp);
                } else {
                    if (p.yVelocity < -C.fallClamp) setYVelocity(p, -C.fallClamp);
                }
                if (playerIsFallingBugged(p) && fm * p.yVelocity < -4.0) p.onGround2 = false;
            }
        }
    }
    (void)onGround;
    return jumped;
}

void integratePosition(PlayerState& p, double dt) {
    p.lastX = p.x;
    p.lastY = p.y;
    // 2.2081 PlayerObject::update: dt arrives as a float; xmm14 = double(dt) (@0x388de6), the y factor
    // is the float product dt * 0.9f (@0x388df3) widened (@0x38949b); dx = double(float m_playerSpeed)
    // * m_speedMultiplier * xmm14 (@0x3894e0-0x3894f5)
    float const dtF = static_cast<float>(dt);
    double const dx = p.playerSpeed * p.speedMultiplier * static_cast<double>(dtF);
    double dy;
    if (p.dashing) {
        dy = 0.0;                                   // unsupported (dash orbs end coverage)
    } else if (p.mode == Gamemode::Wave) {
        dy = std::fabs(dx) * flipMod(p.upsideDown) * (p.jumpBuffered ? 1.0 : -1.0);
        if (p.mini) dy *= C.waveMiniSlope;
    } else {
        dy = static_cast<double>(dtF * static_cast<float>(C.verticalSlow)) * p.yVelocity;
    }
    // cvtpd2ps dx / dy (@0x3895e0 / @0x3895e4), CCPoint float add, setPosition (@0x38961e)
    p.x = static_cast<double>(static_cast<float>(p.x) + static_cast<float>(dx));
    p.y = static_cast<double>(static_cast<float>(p.y) + static_cast<float>(dy));
}

void switchMode(PlayerState& p, Gamemode target, double totalTime) {
    if (p.mode == target) return;
    bool const leavingFlyer = flyingMode(p.mode);
    bool const enteringFlyer = flyingMode(target);
    // every flying toggle whose flag changes: vy *= 0.5, onGround = onGround2 = false,
    // m_gameModeChangedTime = t (toggleFlyMode 0x39a506-0x39a58a and the bird / dart / swing twins)
    if (leavingFlyer) {
        p.yVelocity = p.yVelocity * C.modeToggleVelocityFactor;
        p.onGround = false;
        p.onGround2 = false;
    }
    if (enteringFlyer) {
        p.yVelocity = p.yVelocity * C.modeToggleVelocityFactor;
        p.onGround = false;
        p.onGround2 = false;
    }
    p.modeChangedTime = totalTime;
    if (target == Gamemode::Robot) p.robotHold = C.robotHoldOnEnter;   // toggleRobotMode @0x39b77a
    p.mode = target;
    // the hitbox follows the mode through playerHalfSize(); slope state restarts
    p.onSlopeIndex = -1;
    p.wasOnSlope = false;
}

}  // namespace gprl::sim
