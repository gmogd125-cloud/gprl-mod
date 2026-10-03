// The isolated physics engine (engine.hpp): GD's step order transcribed from the 2.2081 binary and
// the camila314 2.2 decompile. PURE C++20, no GD symbols. Sources per rule are quoted inline;
// the constants live in physics.hpp.
//
// One step (GJBaseGameLayer::update, GD_PHYSICS_NOTES "Step loop"):
//   0. speed objects whose x the player passed -> updateTimeMod (the queued time-mod pop)
//   1. processCommands: the queued press / release (pushButton ring-jumps every touching ring)
//   2. resetTouchedRings(false): the touching set becomes "rings overlapped during the last pass"
//   3. PlayerObject::update: updateJump (velocities, ground jump, spider teleport) -> position
//   4. checkCollisions (0x2137f0): floor / ceiling corridor -> collisionCheckObjects (slopes,
//      portals, pads, rings) -> solids (collidedWithObjectInternal) -> hazards -> postCollision
//   5. rotation (cosmetic, not modelled) and the tick advance
#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "collision.hpp"
#include "physics.hpp"

namespace gprl::sim {

namespace {

constexpr Constants const& C = kConstants;
constexpr double kScanMargin = 90.0;     // x window around the player: slopes are matched with a doubled rect
constexpr double kHashNearX = 600.0;     // activations within +-600 units take part in stateHash

inline uint64_t fnv1a(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}

inline int64_t quant(double v, double step) { return static_cast<int64_t>(std::llround(v / step)); }

/// setYVelocity rounding (physics.cpp keeps its own copy; the engine needs it for a few direct writes).
inline double gdRound(double v) {
    double ip = static_cast<double>(static_cast<long long>(v));
    if (v == ip) return v;
    return ip + std::round((v - ip) * 1000.0) / 1000.0;
}

/// GameObject::isFacingDown for an orb / pad: rotated by 180 or flipped vertically.
inline bool facingDown(SimObject const& o) {
    double r = std::fmod(std::fabs(o.rotation), 360.0);
    bool rotated = r > 90.0 && r < 270.0;
    return rotated != o.flipY;
}

}  // namespace

struct Engine::Impl {
    World const* world = nullptr;
    PlayerState p;
    int tick = 0;
    double subTick = 0.0;
    std::vector<uint8_t> queued;          // pending inputs in order (1 = press, 0 = release)
    std::vector<int> activated;           // sorted World::objects indices used this run
    std::vector<int> touchingRings;       // sorted: rings overlapped during the previous collision pass
    std::vector<int> touchedThisStep;     // rings overlapped during the current pass
    int cursor = 0;                       // first index whose left() >= x - margin - maxWidth
    double maxWidth = 0.0;
    double floorY = C.floorY;

    explicit Impl(World const* w) : world(w) {
        for (auto const& o : w->objects) maxWidth = std::max(maxWidth, static_cast<double>(o.rw));
        if (w->groundY > 0.f) floorY = w->groundY;
    }

    // ---- activation bookkeeping ----
    bool isActivated(int i) const { return std::binary_search(activated.begin(), activated.end(), i); }
    void activate(int i) {
        auto it = std::lower_bound(activated.begin(), activated.end(), i);
        if (it == activated.end() || *it != i) activated.insert(it, i);
    }
    void deactivate(int i) {
        auto it = std::lower_bound(activated.begin(), activated.end(), i);
        if (it != activated.end() && *it == i) activated.erase(it);
    }
    static void addSorted(std::vector<int>& v, int i) {
        auto it = std::lower_bound(v.begin(), v.end(), i);
        if (it == v.end() || *it != i) v.insert(it, i);
    }
    static void removeSorted(std::vector<int>& v, int i) {
        auto it = std::lower_bound(v.begin(), v.end(), i);
        if (it != v.end() && *it == i) v.erase(it);
    }

    double now() const { return (tick + subTick) / C.ticksPerSecond; }

    void die(uint8_t reason, int killer) {
        if (p.dead) return;
        p.dead = true;
        p.deathReason = reason;
        p.killerIndex = killer;
    }

    // ---- the scan window ----
    void advanceCursor() {
        auto const& objs = world->objects;
        double const minLeft = p.x - kScanMargin - maxWidth;
        while (cursor < static_cast<int>(objs.size()) && objs[static_cast<size_t>(cursor)].left() < minLeft) ++cursor;
        // Every object before the cursor ends left of the scan window (left < x - margin - maxWidth,
        // so right < x - margin) and is never visited again: its activation can never matter
        // (single- or multi-activate alike). Dropping it keeps snapshots and stateHash bounded by the
        // objects near the player instead of growing with every orb / pad / portal of the level (M3).
        if (!activated.empty() && activated.front() < cursor) {
            activated.erase(activated.begin(), std::lower_bound(activated.begin(), activated.end(), cursor));
        }
    }
    template <typename F>
    void forNearby(F&& f) {
        auto const& objs = world->objects;
        double const hi = p.x + kScanMargin;
        double const lo = p.x - kScanMargin;
        for (int i = cursor; i < static_cast<int>(objs.size()); ++i) {
            SimObject const& o = objs[static_cast<size_t>(i)];
            if (o.left() > hi) break;
            if (o.right() < lo) continue;
            if (o.hidden) continue;
            if (!f(i, o)) break;
        }
    }

    // ---- 0. speed objects by x (GD_PHYSICS_NOTES: updateTimeMod at the start of a sub-step) ----
    void applySpeedObjects() {
        forNearby([&](int i, SimObject const& o) {
            if (o.kind == ObjKind::SpeedChange && o.x <= p.x && !isActivated(i)) {
                activate(i);
                applySpeed(p, o.speed);
            }
            return true;
        });
    }

    // ---- 1. processCommands ----
    void applyInput(bool down, double t) {
        if (down) {
            // pushButton (GD_PHYSICS_NOTES): clear m_ringRelatedSet, set m_jumpBuffered / m_stateRingJump
            // and their copies, ringJump every touching ring
            p.held = true;
            p.jumpBuffered = true;
            p.ringJumpArmed = true;
            p.ringUsedSincePress = -1;
            std::vector<int> rings = touchingRings;
            for (int r : rings) ringJump(r, t);
        } else {
            p.held = false;
            p.jumpBuffered = false;
            p.ringJumpArmed = false;
        }
    }

    // ---- the spider teleport (spiderTestJumpInternal 0x3943f0 modelled, not transcribed) ----
    void spiderJump(double t) {
        bool const newUpsideDown = !p.upsideDown;
        double const dir = newUpsideDown ? 1.0 : -1.0;    // the new "down" direction
        Rect pr = playerRect(p);
        double const half = playerHalfSize(p.mode, p.mini);
        double bestDist = C.spiderSearchRange;
        double landingY = 0.0;
        int landingIndex = -1;
        bool found = false;
        forNearby([&](int i, SimObject const& o) {
            if (o.kind != ObjKind::Solid && o.kind != ObjKind::Slope) return true;
            Rect r = objectRect(o);
            if (r.x1 < pr.x0 || r.x0 > pr.x1) return true;
            double dist, land;
            if (dir > 0.0) {
                if (r.y0 < pr.y1 - 1e-6) return true;
                dist = r.y0 - pr.y1;
                land = r.y0 - half;
            } else {
                if (r.y1 > pr.y0 + 1e-6) return true;
                dist = pr.y0 - r.y1;
                land = r.y1 + half;
            }
            if (dist < bestDist) {
                bestDist = dist;
                landingY = land;
                landingIndex = i;
                found = true;
            }
            return true;
        });
        // the corridor floor / ceiling counts as a surface
        if (dir < 0.0) {
            double fc = floorCentreY(p.mode, p.mini, floorY);
            double dist = p.y - fc;
            if (dist >= -1e-6 && dist < bestDist) { bestDist = dist; landingY = fc; landingIndex = -1; found = true; }
        } else if (world->ceilingY > 0.f) {
            double cc = ceilingCentreY(p.mode, p.mini, world->ceilingY);
            double dist = cc - p.y;
            if (dist >= -1e-6 && dist < bestDist) { bestDist = dist; landingY = cc; landingIndex = -1; found = true; }
        }
        p.jumpBuffered = false;        // assumed: a hold is one spider jump (GD: spiderTestJumpInternal not read)
        p.ringJumpArmed = false;
        p.lastSpiderFlipTime = t;
        if (found) {
            // hazards crossed by the teleport kill (damagingObjectsInRect @0x3948c4 is consulted by GD;
            // its exact use is not transcribed - modelled as a death)
            Rect swept{pr.x0, std::min(p.y, landingY) - half, pr.x1, std::max(p.y, landingY) + half};
            int killer = -1;
            forNearby([&](int i, SimObject const& o) {
                if (o.kind != ObjKind::Hazard) return true;
                if (intersects(swept, objectRect(o))) { killer = i; return false; }
                return true;
            });
            flipGravity(p, newUpsideDown, t);
            p.y = gdFloat(landingY);
            if (killer >= 0) { die(1, killer); return; }
            hitGround(p, landingIndex, t);
            p.onSlopeIndex = -1;
            p.wasOnSlope = false;
        } else {
            flipGravity(p, newUpsideDown, t);
            p.onGround = false;
        }
    }

    // ---- rings (PlayerObject::ringJump 0x398c00) ----
    bool ringJump(int i, double t) {
        if (p.dead) return false;
        if (p.ringUsedSincePress == i) return false;                 // m_ringRelatedSet
        if (!(p.ringJumpArmed && p.jumpBuffered) || p.dashing) return false;   // m_stateRingJump2 && m_stateJumpBuffered && !m_isDashing
        SimObject const& o = world->objects[static_cast<size_t>(i)];
        OrbKind const kind = o.orb;
        if (kind == OrbKind::Custom || kind == OrbKind::Dash || kind == OrbKind::GravityDash || kind == OrbKind::Teleport) return false;   // unsupported in /1
        if (p.touchedRingThisStep) return false;                      // m_touchedRing: one plain ring per step
        if (isActivated(i) && !o.multiActivate) return false;         // an activated ring is never touched again
        p.ringUsedSincePress = i;
        p.touchedRingThisStep = true;
        removeSorted(touchingRings, i);                               // 0x398e3a: m_touchingRings->removeObject
        removeSorted(touchedThisStep, i);                             // so the next resetTouchedRings prune keeps it out
        activate(i);                                                  // 0x399cbd: activatedByPlayer
        if (kind == OrbKind::Spider) {
            bool fd = facingDown(o);
            if (fd != p.upsideDown) flipGravity(p, fd, t);
            spiderJump(t);
            return true;
        }
        p.ringJumpArmed = false;                                      // 0x3991ee
        if (kind == OrbKind::Black) {
            p.yVelocity = gdRound(blackOrbVelocity(p.mode, p.upsideDown));
            p.accelerating = true;                                    // 0x399982
            if (p.mode == Gamemode::Ball || p.mode == Gamemode::Swing) p.jumpBuffered = false;
            return true;
        }
        p.boosted = true;                                             // 0x3991fe
        p.onGround2 = false;
        p.onGround = false;
        double const size = p.mini ? C.orbMini : 1.0;
        double const f = orbFactor(kind, p.mode, p.mini);
        if (kind == OrbKind::Green) flipGravity(p, !p.upsideDown, t); // 0x39941e: before the velocity
        p.yVelocity = gdRound(flipMod(p.upsideDown) * (p.yStart * f) * size);
        if (p.mode == Gamemode::Ball) { p.yVelocity *= C.orbBall; p.jumpBuffered = false; }
        else if (p.mode == Gamemode::Spider) { p.yVelocity *= C.orbSpider; p.jumpBuffered = false; }
        else if (p.mode == Gamemode::Swing) { p.yVelocity *= C.orbSwing; p.jumpBuffered = false; }
        if (kind == OrbKind::Gravity) flipGravity(p, !p.upsideDown, t);   // 0x39967c: after the velocity (halves it)
        if (kind == OrbKind::Red) p.accelerating = true;              // 0x39987a
        return true;
    }

    // ---- pads (GJBaseGameLayer::bumpPlayer 0x2179d0 / collisionCheckObjects gravity pad 0x2158f9) ----
    void pad(int i, SimObject const& o, double t) {
        switch (o.pad) {
            case PadKind::Gravity: {
                bool const padUp = !facingDown(o);
                if (p.upsideDown == padUp) return;                    // 0x21591a: already pointing that way
                activate(i);
                propellPlayer(p, C.padGravity);
                flipGravity(p, padUp, t);
                return;
            }
            case PadKind::Spider: {
                activate(i);
                p.touchedPad = true;
                bool fd = facingDown(o);
                if (fd != p.upsideDown) flipGravity(p, fd, t);
                spiderJump(t);
                return;
            }
            default: {
                activate(i);
                p.touchedPad = true;                                  // PlayerObject::bumpPlayer 0x39f6c6
                propellPlayer(p, padMultiplier(o.pad, p.mode, p.mini));
                p.accelerating = (o.pad == PadKind::Red);             // 0x39f81e / 0x39f831
                return;
            }
        }
    }

    // ---- 4a. floor / ceiling (checkCollisions 0x213963-0x214162) ----
    void floorAndCeiling(double t) {
        double const floorCentre = floorCentreY(p.mode, p.mini, floorY);
        double const hh = playerBaseHeight(p.mode) * vehicleSize(p.mini) * 0.5;
        bool const corridor = corridorMode(p.mode);
        if (p.y < floorCentre && !corridor) {
            if (!p.upsideDown) {
                if (!p.boosted) {                                     // 0x213bab: m_maybeIsBoosted == 0
                    p.y = gdFloat(floorCentre);
                    hitGround(p, -1, t);
                }
            } else {
                if (p.lastFlipTime >= 0.0 && t - p.lastFlipTime < C.floorFlipGrace) {   // 0x213b45
                    p.y = gdFloat(floorCentre);
                    hitGround(p, -1, t, true);
                    p.onGround2 = false;
                } else {
                    die(3, -1);
                    return;
                }
            }
        } else {
            double const miniCorr = (1.0 - vehicleSize(p.mini)) * playerBaseHeight(p.mode) * 0.5;
            if (p.y > C.ceilingDeathY + miniCorr) {                   // 0x213c12: y > m_maxGameplayY
                die(4, -1);
                return;
            }
        }
        if (!corridor) return;
        double const botCentre = floorCentre;
        if (world->ceilingY > 0.f) {
            double const topCentre = ceilingCentreY(p.mode, p.mini, world->ceilingY);
            if (p.y > topCentre) {
                if (p.wasOnSlope && p.y > topCentre + hh) { die(3, -1); return; }   // 0x213ca3
                p.y = gdFloat(topCentre);
                if (p.yVelocity > 0.0) hitGround(p, -1, t);           // 0x213e31
                return;
            }
        }
        if (p.y < botCentre) {
            if (p.wasOnSlope && p.y < botCentre - hh) { die(3, -1); return; }       // 0x213ef8
            p.y = gdFloat(botCentre);
            bool const shipOrSwing = p.mode == Gamemode::Ship || p.mode == Gamemode::Swing;
            if (!shipOrSwing || p.yVelocity < 0.0) hitGround(p, -1, t);   // 0x2140df-0x214109
        }
    }

    // ---- 4b. slopes (collidedWithSlopeInternal, gdp 2.2) ----
    void slope(int i, SimObject const& o, double t) {
        if (!intersects(playerRect(p), slopeBroadRect(o))) return;
        SlopeContact c = slopeContact(o, i, p, t);
        if (!c.contact) return;
        if (c.death) { die(2, i); return; }
        bool const flying = flyingMode(p.mode);
        double const fm = flipMod(p.upsideDown);
        if (c.ceiling && !flying && p.mode != Gamemode::Ball) {
            // gdp lines 110-121: pushed off a ceiling slope (safe mode change / flip, or shallow)
            if (c.newY != p.y) {
                p.y = gdFloat(c.newY);
                p.yVelocity = p.upsideDown ? std::max(p.yVelocity, 2.0) : std::min(p.yVelocity, -2.0);
            }
            p.onGround = false;
            p.onGround2 = false;
            return;
        }
        double const half = playerHalfSize(p.mode, p.mini);
        p.onSlopeIndex = i;
        p.currentSlopeTop = slopeFloorTop(o);
        p.slopeAngle = slopeAngle(o);
        p.slopeRadiusExtra = c.playerRadOnSlope - half;
        p.slopeContactY = gdFloat(c.newY);                            // m_unk3d0 = newPlayerY (gdp line 199)
        p.slopeDownhill = c.downhill;                                 // m_slopeFlipGravityRelated (gdp line 202)
        p.y = gdFloat(c.newY);
        if (!p.wasOnSlope) p.slopeStartTime = t;
        bool const skipGround = c.ceiling && (flying || p.mode == Gamemode::Ball) && !p.jumpBuffered && c.downhill && p.wasOnSlope;   // bool_i
        if (!skipGround) {
            if (c.ceiling) {
                if (fm * p.yVelocity > 0.0) p.yVelocity = 0.0;        // gdp line 224
                p.onGround = false;
                p.onGround2 = false;
            } else {
                double const old = p.yVelocity;
                hitGround(p, i, t);
                if (fm * old > fm * 5.0) p.yVelocity = old;           // gdp line 231
            }
        }
        if (c.ceiling && flying && c.downhill && !p.jumpBuffered && fm * p.yVelocity > fm * -2.0) {
            p.yVelocity = fm * -2.0;                                   // gdp line 258
        } else if (!c.ceiling && p.jumpBuffered && flying && !c.downhill && fm * p.yVelocity < fm * 2.0) {
            p.yVelocity = fm * 2.0;                                    // gdp line 260 (bool_b)
        }
        p.currentSlopeYVelocity = c.slopeYVelocity;
        p.slopeVelocity = c.slopeVelocity;
    }

    // ---- 4c. solids (collidedWithObjectInternal 0x391a70, static objects, non-platformer) ----
    void solid(int i, SimObject const& o, double t) {
        Rect pr = playerRect(p);
        Rect const orct = objectRect(o);
        if (!intersects(pr, orct)) return;
        double const gm = flipMod(p.upsideDown);
        double const h = pr.height();
        bool const flying = flyingMode(p.mode);
        double snap = flying ? C.snapThresholdFlyer : C.snapThreshold;
        // right after a slope the threshold grows by unk_584 (playerRadOnSlope - radius: 6.21 on 45
        // degrees): 2.2081 collidedWithObjectInternal @0x391c54-0x391c76 (m_wasOnSlope ? += gm * [+0x680]),
        // gdp lines 39-40 (gprl-sim/2; /1 used the plain 10 / 6 and walled where GD lands)
        if (p.wasOnSlope) snap += p.slopeRadiusExtra;
        double const dyRel = (p.y - p.lastY) * gm;
        bool boolJ = dyRel > 0.0;
        // gravity-relative frame: relY = y * gm; the landing face is the block face the player's
        // gravity-facing edge meets, the far face the other one
        double const faceRel = gm > 0.0 ? orct.y1 : -orct.y0;
        double const farRel = gm > 0.0 ? orct.y0 : -orct.y1;
        double const bottomRel = p.y * gm - h * 0.5;
        double const topRel = p.y * gm + h * 0.5;
        bool const groundMode = !flying && p.mode != Gamemode::Ball;
        bool canSnap = false;
        if (p.mode == Gamemode::Wave) {
            boolJ = true;                                              // gdp line 91: the wave never lands
        } else {
            double const bottomBefore = bottomRel - dyRel;
            canSnap = (bottomRel + snap <= farRel) || (bottomBefore + snap <= farRel);   // the block is overhead
            bool const landable = !((bottomRel + snap < faceRel) && (bottomBefore + snap < faceRel));
            if (landable) {
                double const vyRel = p.yVelocity * gm;
                if (vyRel > 0.0 && !p.wasOnSlope) return;              // rising away from the surface: no collision (gdp line 174)
                p.y = gdFloat(gm > 0.0 ? orct.y1 + h * 0.5 : orct.y0 - h * 0.5);
                if (p.mode == Gamemode::Cube) checkSnapJumpToObject(p, *world, i);
                hitGround(p, i, t, canSnap);
                return;
            }
            boolJ = true;                                              // gdp line 164: the top is out of reach -> a wall / ceiling
            if (!groundMode) {
                // block 2: the far face as a ceiling for flyers / ball (gdp lines 343-507)
                double const topBefore = topRel - dyRel;
                bool const deep = (farRel < topRel - snap) && (farRel < topBefore - snap);
                if (!deep) {
                    double const vyRel = p.yVelocity * gm;
                    if (vyRel >= 0.0 || p.wasOnSlope) {
                        p.y = gdFloat(gm > 0.0 ? orct.y0 - h * 0.5 : orct.y1 + h * 0.5);
                        hitGround(p, i, t, canSnap);
                        return;
                    }
                }
            }
        }
        if (!boolJ) return;
        // the tail (gdp lines 612-653): the inner death rect vs the block
        Rect const inner = playerInnerRect(p);
        if (!intersects(orct, inner)) return;
        if (canSnap && p.lastFlipTime >= 0.0 && t - p.lastFlipTime < C.headHitFlipGrace) {
            p.y = gdFloat(gm > 0.0 ? orct.y0 - h * 0.5 : orct.y1 + h * 0.5);
            hitGround(p, i, t);
            p.onGround2 = false;
            return;
        }
        if (p.mode == Gamemode::Spider && orct.x1 < inner.x1 && p.lastSpiderFlipTime >= 0.0 && t - p.lastSpiderFlipTime < C.spiderFlipGrace) return;
        die(2, i);
    }

    // ---- 4e. postCollision (0x38d580): slope exit ----
    void postCollision(double t) {
        bool const onSlopeNow = p.onSlopeIndex >= 0;
        if (p.wasOnSlope && !onSlopeNow) {                            // 0x38d708-0x38d72c
            p.slopeEndTime = t;
            double const fm = flipMod(p.upsideDown);
            double const sv = p.slopeVelocity;
            double const vy = p.yVelocity;
            bool const cst = p.currentSlopeTop;
            // the launch gate reads m_isCurrentSlopeTop, not the gravity (0x38d742-0x38d7f3: the
            // same test on both gravity branches): a floor-top (ceiling) slope launches a negative
            // m_slopeVelocity, any other a positive one (gprl-sim/2; /1 used flipMod, wrong for a
            // flyer under a ceiling slope or an upside-down player on a floor slope)
            bool const gate = cst ? (sv < 0.0 && vy > sv) : (sv > 0.0 && sv > vy);
            bool const flying = flyingMode(p.mode);
            bool const groundJumped = p.boosted && !flying && p.mode != Gamemode::Ball;   // 0x38d7f5-0x38d829
            if (gate && !groundJumped) {
                double const dur = t - p.slopeStartTime;
                double const factor = dur < C.slopeLaunchFullTime ? std::max(C.slopeLaunchMinFactor, dur * C.slopeLaunchRamp) : 1.0;
                double const launch = gdFloat(sv * factor);           // cvtpd2ps @0x38d878
                // 0x38d8e0-0x38d92c: boost only when the launch beats the current velocity, compared in
                // the direction the slope pushes (m_slopeFlipGravityRelated = the contact was downhill)
                bool const pushesDown = p.upsideDown ? (cst || !p.slopeDownhill) : (cst && p.slopeDownhill);
                bool const beats = pushesDown ? (vy > launch) : (launch > vy);
                if (beats) boostPlayer(p, launch);                     // 0x38d966
            } else {
                // 0x38da3d-0x38db0c: slide off a downhill slope
                bool const svDown = p.upsideDown ? sv > 0.0 : sv < 0.0;
                if (svDown && !p.boosted && !p.jumpBuffered && !p.ringJumpArmed && cst == p.upsideDown) {
                    p.onGround2 = false;                               // 0x38da9c
                    p.yVelocity = gdRound(-fm * p.currentSlopeYVelocity);   // 0x38dab4-0x38db07
                    p.onGround = false;                                // 0x38db0c
                }
            }
            p.currentSlopeYVelocity = 0.0;                             // 0x38d9ad
            p.slopeVelocity = 0.0;                                     // 0x38d9b4
        }
    }

    // ---- one (part of a) step ----
    void stepInternal(double fraction) {
        double const t = now();
        if (!p.dead) {
            double const dt = C.dt * fraction;
            advanceCursor();
            applySpeedObjects();
            for (uint8_t q : queued) applyInput(q != 0, t);
            queued.clear();
            // resetTouchedRings(false): prune to the rings overlapped during the last pass
            touchingRings = touchedThisStep;
            touchedThisStep.clear();
            // PlayerObject::update
            p.wasOnSlope = p.onSlopeIndex >= 0;
            p.onSlopeIndex = -1;
            updateJump(p, dt, t);
            if (!p.dead && p.mode == Gamemode::Spider && p.onGround && p.jumpBuffered && !p.dashing) spiderJump(t);
            if (!p.dead) {
                integratePosition(p, dt);
                p.touchedRingThisStep = false;                         // cleared at the end of update (0x389f09)
                advanceCursor();
                checkCollisions(t);
            }
        }
        subTick += fraction;
        if (subTick >= 1.0 - 1e-9) {
            ++tick;
            subTick = 0.0;
        }
    }

    void checkCollisions(double t) {
        floorAndCeiling(t);
        if (p.dead) return;
        // collisionCheckObjects: slopes, portals, pads, rings (x order; GD: section order)
        forNearby([&](int i, SimObject const& o) {
            switch (o.kind) {
                case ObjKind::Slope:
                    slope(i, o, t);
                    break;
                case ObjKind::GamemodePortal:
                case ObjKind::GravityPortal:
                case ObjKind::SizePortal:
                case ObjKind::Pad:
                case ObjKind::Orb: {
                    bool const used = isActivated(i);
                    bool const overlap = objectOverlaps(o, playerRect(p));   // GD's oriented box for an oriented portal / pad
                    if (!overlap) {
                        if (used && o.multiActivate) deactivate(i);   // re-armed once the player left it
                        break;
                    }
                    if (used) break;                                   // hasBeenActivatedByPlayer
                    if (o.kind == ObjKind::GamemodePortal) { activate(i); switchMode(p, o.mode, t); }
                    else if (o.kind == ObjKind::GravityPortal) { activate(i); flipGravity(p, o.flag, t); }
                    else if (o.kind == ObjKind::SizePortal) { activate(i); p.mini = o.flag; }
                    else if (o.kind == ObjKind::Pad) pad(i, o, t);
                    else {
                        // playerTouchedRing (0x217e40): addToTouchedRings, then ringJump only for cube /
                        // ball / robot / spider (GD_PHYSICS_NOTES "Activation slots"): a flyer holding into
                        // an orb does not use it - it needs a press while touching (pushButton)
                        addSorted(touchedThisStep, i);
                        addSorted(touchingRings, i);
                        if (!flyingMode(p.mode)) ringJump(i, t);
                    }
                    break;
                }
                default:
                    break;
            }
            return !p.dead;
        });
        if (p.dead) return;
        // solids
        forNearby([&](int i, SimObject const& o) {
            if (o.kind == ObjKind::Solid) solid(i, o, t);
            return !p.dead;
        });
        if (p.dead) return;
        // hazards: the full player rect
        forNearby([&](int i, SimObject const& o) {
            if (o.kind == ObjKind::Hazard && hazardHits(o, p)) { die(1, i); return false; }
            return true;
        });
        if (p.dead) return;
        postCollision(t);
    }

    void reset(StartState const& s) {
        p = PlayerState{};
        p.x = s.x;
        p.y = s.y;
        p.mode = s.mode;
        p.mini = s.mini;
        p.upsideDown = s.upsideDown;
        applySpeed(p, s.speed);
        if (s.mode == Gamemode::Robot) p.robotHold = C.robotHoldOnEnter;
        tick = 0;
        subTick = 0.0;
        queued.clear();
        activated.clear();
        touchingRings.clear();
        touchedThisStep.clear();
        cursor = 0;
        // speed objects already behind the start are consumed (their effect is the start speed)
        auto const& objs = world->objects;
        for (int i = 0; i < static_cast<int>(objs.size()); ++i) {
            if (objs[static_cast<size_t>(i)].kind == ObjKind::SpeedChange && objs[static_cast<size_t>(i)].x <= p.x) activated.push_back(i);
        }
        advanceCursor();
    }
};

// ---------------------------------------------------------------------------------------------

Engine::Engine(World const* world) : m_impl(new Impl(world)) {}
Engine::~Engine() { delete m_impl; }

void Engine::reset(StartState const& start) { m_impl->reset(start); }

void Engine::queueInput(bool down) { m_impl->queued.push_back(down ? 1 : 0); }

void Engine::applyQueuedInput() {
    double const t = m_impl->now();
    // processCommands runs BEFORE the next sub-step's resetTouchedRings: the press sees the whole
    // m_touchingRings (the prune happens at the start of the following stepPart, like a whole step)
    for (uint8_t q : m_impl->queued) m_impl->applyInput(q != 0, t);
    m_impl->queued.clear();
}

void Engine::step() { m_impl->stepInternal(1.0); }

void Engine::stepPart(double fraction) {
    if (fraction <= 0.0) return;
    if (fraction > 1.0) fraction = 1.0;
    double const room = 1.0 - m_impl->subTick;
    if (fraction > room + 1e-9) fraction = room;
    m_impl->stepInternal(fraction);
}

PlayerState const& Engine::player() const { return m_impl->p; }
World const& Engine::world() const { return *m_impl->world; }
int Engine::tick() const { return m_impl->tick; }
double Engine::frame() const { return m_impl->tick + m_impl->subTick; }

double Engine::percent() const {
    double const end = m_impl->world->endX;
    if (end <= 0.0) return 0.0;
    double pc = m_impl->p.x / end * 100.0;
    return std::clamp(pc, 0.0, 100.0);
}

bool Engine::dead() const { return m_impl->p.dead; }
bool Engine::completed() const { return !m_impl->p.dead && m_impl->world->endX > 0.0 && m_impl->p.x >= m_impl->world->endX; }

bool Engine::inUnsupportedSpan() const {
    double const x = m_impl->p.x;
    for (auto const& s : m_impl->world->unsupported) {
        if (s.x0 > x) break;
        if (x >= s.x0 && x < s.x1) return true;
    }
    return false;
}

int Engine::nearestAhead(float range) const {
    auto const& objs = m_impl->world->objects;
    double const x = m_impl->p.x;
    auto it = std::upper_bound(objs.begin(), objs.end(), x, [](double v, SimObject const& o) { return v < o.left(); });
    if (it == objs.end()) return -1;
    if (it->left() - x > static_cast<double>(range)) return -1;
    return static_cast<int>(it - objs.begin());
}

EngineSnapshot Engine::save() const {
    EngineSnapshot s;
    s.player = m_impl->p;
    s.tick = m_impl->tick;
    s.subTick = m_impl->subTick;
    s.activatedIndices = m_impl->activated;
    // both ring sets (gprl-sim/2): a press at the next step's processCommands ring-jumps every ring
    // in m_touchingRings (the rings of the previous pass too), and only then does resetTouchedRings
    // prune it to m_touchedRings. /1 saved only the last pass, so restore(save()) was not an identity.
    s.touchingRings = m_impl->touchingRings;
    s.touchedRings = m_impl->touchedThisStep;
    s.nextObject = m_impl->cursor;
    return s;
}

void Engine::restore(EngineSnapshot const& s) {
    m_impl->p = s.player;
    m_impl->tick = s.tick;
    m_impl->subTick = s.subTick;
    m_impl->activated = s.activatedIndices;
    m_impl->touchingRings = s.touchingRings;
    m_impl->touchedThisStep = s.touchedRings;
    m_impl->cursor = std::clamp(s.nextObject, 0, static_cast<int>(m_impl->world->objects.size()));
    m_impl->queued.clear();
    m_impl->advanceCursor();
}

uint64_t Engine::stateHash() const {
    PlayerState const& p = m_impl->p;
    uint64_t h = 1469598103934665603ull;
    h = fnv1a(h, static_cast<uint64_t>(m_impl->tick));
    h = fnv1a(h, static_cast<uint64_t>(quant(m_impl->subTick, 1.0 / 64.0)));
    h = fnv1a(h, static_cast<uint64_t>(quant(p.y, 0.5)));
    h = fnv1a(h, static_cast<uint64_t>(quant(p.yVelocity, 0.05)));
    uint64_t flags = 0;
    flags |= static_cast<uint64_t>(p.mode) << 0;
    flags |= static_cast<uint64_t>(p.speed) << 4;
    flags |= static_cast<uint64_t>(p.mini) << 8;
    flags |= static_cast<uint64_t>(p.upsideDown) << 9;
    flags |= static_cast<uint64_t>(p.onGround) << 10;
    flags |= static_cast<uint64_t>(p.held) << 11;
    flags |= static_cast<uint64_t>(p.jumpBuffered) << 12;
    flags |= static_cast<uint64_t>(p.ringJumpArmed) << 13;
    flags |= static_cast<uint64_t>(p.boosted) << 14;
    flags |= static_cast<uint64_t>(p.accelerating) << 15;
    flags |= static_cast<uint64_t>(p.dead) << 16;
    flags |= static_cast<uint64_t>(p.touchedPad) << 17;
    flags |= static_cast<uint64_t>(p.wasOnSlope) << 18;
    flags |= static_cast<uint64_t>(p.onSlopeIndex >= 0) << 19;
    h = fnv1a(h, flags);
    h = fnv1a(h, static_cast<uint64_t>(quant(p.robotHold, 0.025)));
    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(p.onSlopeIndex)));
    auto const& objs = m_impl->world->objects;
    for (int i : m_impl->activated) {
        SimObject const& o = objs[static_cast<size_t>(i)];
        if (std::fabs(static_cast<double>(o.x) - p.x) <= kHashNearX) h = fnv1a(h, static_cast<uint64_t>(i) + 0x9e3779b97f4a7c15ull);
    }
    for (int i : m_impl->touchedThisStep) h = fnv1a(h, static_cast<uint64_t>(i) + 0x7f4a7c159e3779b9ull);
    for (int i : m_impl->touchingRings) h = fnv1a(h, static_cast<uint64_t>(i) + 0x3c6ef372fe94f82bull);
    return h;
}

bool Engine::flying() const { return flyingMode(m_impl->p.mode); }
/// m_touchingRings as GD holds it between steps (what the live clone solver's settle fact reads).
bool Engine::touchingRing() const { return !m_impl->touchingRings.empty(); }

std::string Engine::describe() const {
    PlayerState const& p = m_impl->p;
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "t%d+%.3f x=%.3f y=%.3f vy=%.4f %s%s%s spd=%d g=%s %s%s%s%s%s dead=%d/%d killer=%d slope=%d",
                  m_impl->tick, m_impl->subTick, p.x, p.y, p.yVelocity, gamemodeName(p.mode), p.mini ? " mini" : "",
                  p.upsideDown ? " flipped" : "", static_cast<int>(p.speed), p.onGround ? "ground" : "air",
                  p.held ? "H" : "-", p.jumpBuffered ? "J" : "-", p.ringJumpArmed ? "R" : "-", p.boosted ? "B" : "-",
                  p.accelerating ? "A" : "-", p.dead ? 1 : 0, p.deathReason, p.killerIndex, p.onSlopeIndex);
    return buf;
}

}  // namespace gprl::sim
