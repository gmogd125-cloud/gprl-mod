// Rect / slope / hazard geometry of the isolated simulator (collision.hpp). PURE C++20.
#include "collision.hpp"

#include <algorithm>
#include <cmath>

#include "physics.hpp"

namespace gprl::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Rotation reduced to [0, 360).
double normDeg(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

/// True when the rotation is a multiple of 90 degrees (the AABB is exact then).
bool axisAligned(double rotation) {
    double r = normDeg(rotation);
    double m = std::fmod(r, 90.0);
    return m < 1e-3 || m > 90.0 - 1e-3;
}

struct Obb {
    double cx, cy;      // centre
    double hw, hh;      // half extents (unrotated)
    double c, s;        // cos / sin of the rotation
};

/// Reconstruct the unrotated half extents of a box whose rotated AABB is (W, H) at angle t:
///   W = w|c| + h|s|, H = w|s| + h|c|  ->  solved when |c| != |s|; at 45 degrees the pair is
/// ambiguous and a square (w = h = W / (|c| + |s|)) is assumed (documented deviation).
Obb obbFromAabb(SimObject const& o) {
    double t = normDeg(o.rotation) * kPi / 180.0;
    double c = std::cos(t), s = std::sin(t);
    double ac = std::fabs(c), as = std::fabs(s);
    double W = o.rw, H = o.rh;
    double w, h;
    double det = ac * ac - as * as;
    if (std::fabs(det) > 1e-3) {
        w = (W * ac - H * as) / det;
        h = (H * ac - W * as) / det;
        if (w < 0.5 || h < 0.5) { w = h = W / (ac + as); }
    } else {
        w = h = W / (ac + as);
    }
    return Obb{o.rx + o.rw * 0.5, o.ry + o.rh * 0.5, w * 0.5, h * 0.5, c, s};
}

/// Separating-axis test between an OBB and an axis-aligned rect (OBB2D::overlaps1Way both ways).
bool obbIntersectsRect(Obb const& b, Rect const& r) {
    double bx[4], by[4];
    double const ex[2] = {b.hw * b.c, b.hw * b.s};
    double const ey[2] = {-b.hh * b.s, b.hh * b.c};
    bx[0] = b.cx - ex[0] - ey[0]; by[0] = b.cy - ex[1] - ey[1];
    bx[1] = b.cx + ex[0] - ey[0]; by[1] = b.cy + ex[1] - ey[1];
    bx[2] = b.cx + ex[0] + ey[0]; by[2] = b.cy + ex[1] + ey[1];
    bx[3] = b.cx - ex[0] + ey[0]; by[3] = b.cy - ex[1] + ey[1];
    double rx[4] = {r.x0, r.x1, r.x1, r.x0};
    double ry[4] = {r.y0, r.y0, r.y1, r.y1};
    double axes[4][2] = {{1.0, 0.0}, {0.0, 1.0}, {b.c, b.s}, {-b.s, b.c}};
    for (auto const& a : axes) {
        double bmin = 1e300, bmax = -1e300, rmin = 1e300, rmax = -1e300;
        for (int i = 0; i < 4; ++i) {
            double pb = bx[i] * a[0] + by[i] * a[1];
            double pr = rx[i] * a[0] + ry[i] * a[1];
            bmin = std::min(bmin, pb); bmax = std::max(bmax, pb);
            rmin = std::min(rmin, pr); rmax = std::max(rmax, pr);
        }
        if (bmax < rmin || rmax < bmin) return false;
    }
    return true;
}

}  // namespace

Rect slopeBroadRect(SimObject const& o) {
    double cx = o.rx + o.rw * 0.5, cy = o.ry + o.rh * 0.5;
    double hw = o.rw * 0.5 * kConstants.slopeRectBroadphase, hh = o.rh * 0.5 * kConstants.slopeRectBroadphase;
    return Rect{cx - hw, cy - hh, cx + hw, cy + hh};
}

Rect playerRect(PlayerState const& p) {
    double h = playerHalfSize(p.mode, p.mini);
    return Rect{p.x - h, p.y - h, p.x + h, p.y + h};
}

Rect playerInnerRect(PlayerState const& p) {
    double h = playerInnerHalfSize(p.mode, p.mini);
    return Rect{p.x - h, p.y - h, p.x + h, p.y + h};
}

bool circularHazardId(int id) {
    // gdclone object.json hitbox type "Circle" (37 ids): saws and the small rotating hazards
    static constexpr int kIds[] = {88, 89, 98, 183, 184, 185, 186, 187, 188, 397, 398, 399, 675, 676, 677, 678, 679, 680,
                                   740, 741, 742, 1582, 1583, 1619, 1620, 1701, 1702, 1703, 1705, 1706, 1707, 1708, 1709,
                                   1710, 1734, 1735, 1736};
    for (int k : kIds) {
        if (k == id) return true;
    }
    return false;
}

double gdCircleRadius(SimObject const& o) {
    if (!(o.radius > 0.f)) return 0.0;
    // playerCircleCollision @0x211e52-0x211e95 / @0x211efa-0x211f32: m_objectRadius as is when both
    // m_scaleX and m_scaleY are 1, else max(scaleX, scaleY) * m_objectRadius (float math)
    float const r = (o.scaleX == 1.f && o.scaleY == 1.f) ? o.radius : std::max(o.scaleX, o.scaleY) * o.radius;
    return static_cast<double>(r);
}

bool circleHitsRect(double cx, double cy, double r, Rect const& pr) {
    // GJBaseGameLayer::playerCircleCollision (2.2081 0x211df0), the default branch (the level setting
    // byte at [+0xdb0]+0x1cf off, @0x211e15): a hit when the object's centre lies inside the player's
    // rect (CCRect::containsPoint, closed) or a CORNER of the rect lies closer than r (r > |d|,
    // @0x211fcc-0x212062). A circle grazing the middle of an edge does not hit.
    if (cx >= pr.x0 && cx <= pr.x1 && cy >= pr.y0 && cy <= pr.y1) return true;
    double const xs[2] = {pr.x0, pr.x1}, ys[2] = {pr.y0, pr.y1};
    for (double x : xs)
        for (double y : ys) {
            float const d = static_cast<float>(std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (static_cast<float>(r) > std::fabs(d)) return true;
        }
    return false;
}

bool orientedBoxHitsRect(SimObject const& o, Rect const& pr) {
    // m_shouldUseOuterOb with GD's own oriented box (gprl-extract/2: updateOrientedBox centre / size /
    // angle): OBB2D::overlaps1Way both ways (0x2148b4 / 0x2148c3). The player's box is tested
    // unrotated (its rotation is not simulated: a known gap).
    double const a = static_cast<double>(o.obbAngle);
    Obb const b{static_cast<double>(o.obbCx), static_cast<double>(o.obbCy), std::fabs(static_cast<double>(o.obbW)) * 0.5,
                std::fabs(static_cast<double>(o.obbH)) * 0.5, std::cos(a), std::sin(a)};
    return obbIntersectsRect(b, pr);
}

bool hasOrientedBox(SimObject const& o) { return o.oriented && (o.obbW != 0.f || o.obbH != 0.f); }

bool objectOverlaps(SimObject const& o, Rect const& pr) {
    if (!intersects(pr, objectRect(o))) return false;   // rx..rh = the OBB's bounding rect for an oriented object
    if (hasOrientedBox(o)) return orientedBoxHitsRect(o, pr);
    return true;
}

bool hazardHits(SimObject const& hz, PlayerState const& p) {
    Rect pr = playerRect(p);
    Rect hr = objectRect(hz);
    if (!intersects(pr, hr)) return false;            // the AABB broad phase (0x214813-0x214863)
    if (hz.radius > 0.f) {
        // m_objectRadius > 0 (gprl-extract/2): GD's own circle test with GD's radius
        return circleHitsRect(static_cast<double>(hz.x), static_cast<double>(hz.y), gdCircleRadius(hz), pr);
    }
    if (circularHazardId(hz.objectId)) {
        // fallback for a world without the radius (/1 extraction, hand-built tests): the circle
        // (radius = half the rect) vs the rect by its closest point
        double r = std::min(hz.rw, hz.rh) * 0.5;
        double cx = hz.x, cy = hz.y;
        double qx = std::clamp(cx, pr.x0, pr.x1), qy = std::clamp(cy, pr.y0, pr.y1);
        double dx = cx - qx, dy = cy - qy;
        return dx * dx + dy * dy <= r * r;
    }
    if (hasOrientedBox(hz)) return orientedBoxHitsRect(hz, pr);
    if (!hz.oriented && !axisAligned(hz.rotation)) {
        // fallback for a world without GD's oriented-box facts: the box is reconstructed from the
        // AABB and the rotation
        return obbIntersectsRect(obbFromAabb(hz), pr);
    }
    return true;
}

double slopeAngle(SimObject const& o) {
    if (o.rw <= 0.0) return kPi * 0.5;
    return std::atan(static_cast<double>(o.rh) / static_cast<double>(o.rw));
}

double slopeYPos(SimObject const& o, double playerX) {
    double left = o.rx, right = o.rx + o.rw, bottom = o.ry, top = o.ry + o.rh;
    double ratio = o.rw > 0.0 ? o.rh / o.rw : 0.0;
    double result;
    if (left < playerX) {
        double distanceFromRight = playerX - right;
        result = slopeUphill(o) ? top + distanceFromRight * ratio : bottom - distanceFromRight * ratio;
    } else {
        double distanceFromLeft = left - playerX;
        result = !slopeUphill(o) ? top + distanceFromLeft * ratio : bottom - distanceFromLeft * ratio;
    }
    return result;
}

SlopeContact slopeContact(SimObject const& o, int slopeIndex, PlayerState const& p, double totalTime) {
    SlopeContact r;
    Rect pr = playerRect(p);
    Rect orct = objectRect(o);
    double const playerRadius = pr.height() * 0.5;
    bool const floorTop = slopeFloorTop(o);
    bool const flying = flyingMode(p.mode);
    double const fm = flipMod(p.upsideDown);
    double const upsideMod = fm;
    // gdp line 19-20 with goingLeft = false, sideways = false, static slope: "playerUphill" in the
    // decompile is true when the player travels DOWN the surface (the m_slopeVelocity sign proves it)
    bool const downhill = (!slopeUphill(o)) != p.upsideDown;
    double const slopeYVelocity = (static_cast<double>(o.rh) * p.playerSpeed * p.speedMultiplier) / std::max(static_cast<double>(o.rw), 1e-6);
    double floatG = downhill ? (p.wasOnSlope ? 4.0 : 1.0) : 0.0;
    bool const slopeTopRelated = downhill && p.currentSlopeTop == floorTop && p.upsideDown == floorTop;
    if (p.wasOnSlope && p.slopeVelocity * fm > 0.0 && (slopeTopRelated || p.currentSlopeYVelocity > slopeYVelocity)) return r;
    double const angle = slopeAngle(o);
    double const playerRadOnSlope = playerRadius / std::cos(angle);
    double const playerRadOnPrevSlope = p.wasOnSlope ? playerRadius / std::cos(p.slopeAngle) : playerRadius;
    double const onSlopeThreshold = p.y - upsideMod * (playerRadOnPrevSlope + floatG);
    if (p.wasOnSlope) {
        if (p.upsideDown && onSlopeThreshold < orct.y0) return r;
        if (!p.upsideDown && onSlopeThreshold > orct.y1) return r;
    } else {
        Rect exitRect = orct;
        exitRect.y0 += kConstants.slopeExitInset;
        exitRect.y1 -= kConstants.slopeExitInset;
        if (!intersects(pr, exitRect)) return r;
    }
    bool const isNewSlope = p.wasOnSlope && p.onSlopeIndex != slopeIndex && p.currentSlopeTop != floorTop;
    double const newSlopeScalar = isNewSlope ? vehicleSize(p.mini) * kConstants.slopeNewScalar : 0.0;
    double newPlayerY = slopeYPos(o, p.x) + (playerRadOnSlope - newSlopeScalar) * (floorTop ? -1.0 : 1.0);
    if (floorTop) {
        newPlayerY = std::max(newPlayerY, orct.y0 - playerRadius + newSlopeScalar);
        newPlayerY = std::min(newPlayerY, orct.y1);
    } else {
        newPlayerY = std::min(newPlayerY, orct.y1 + playerRadius - newSlopeScalar);
        newPlayerY = std::max(newPlayerY, orct.y0);
    }
    bool const slopeUpsideDown = p.upsideDown != floorTop;
    bool collided = false;
    if (slopeUpsideDown) {
        double floatP = downhill ? 0.0 : floatG;
        if (flying && p.jumpBuffered) floatP = p.wasOnSlope ? 2.0 : 1.0;
        collided = p.onSlopeIndex < 0 && !isNewSlope && (!flying || !downhill) && upsideMod * p.y > upsideMod * (newPlayerY - floatP);
        if (upsideMod * p.y > upsideMod * newPlayerY) collided = true;
        if (collided && !flying && p.mode != Gamemode::Ball) {
            // a ground-mode player meets a ceiling slope: pushed off when safe (fresh mode change /
            // flip, or barely inside), killed otherwise (gdp lines 110-131)
            bool safe = (p.modeChangedTime >= 0.0 && totalTime - p.modeChangedTime < kConstants.safeModeChangeTime) ||
                        (p.lastFlipTime >= 0.0 && totalTime - p.lastFlipTime < kConstants.safeModeChangeTime);
            if (safe || (upsideMod * p.y - 2.0) <= upsideMod * newPlayerY) {
                r.contact = true;
                r.ceiling = true;
                r.newY = safe ? p.y : newPlayerY;   // GD: setPositionY only when !safe... (notSafe) - see engine
                r.downhill = downhill;
                r.slopeYVelocity = slopeYVelocity;
                r.playerRadOnSlope = playerRadOnSlope;
                r.slopeVelocity = 0.0;
                return r;
            }
            r.contact = true;
            r.death = true;
            return r;
        }
    } else {
        bool boolH = downhill ? (!isNewSlope && p.onSlopeIndex < 0 && (!p.boosted || flying) && (p.mode != Gamemode::Ship || p.jumpBuffered)) : false;
        collided = true;
        if (upsideMod * p.y >= upsideMod * newPlayerY) {
            collided = false;
            if (boolH && upsideMod * p.y < upsideMod * (newPlayerY + floatG)) {
                collided = p.mode == Gamemode::Ufo ? (upsideMod * p.yVelocity <= 0.0) : true;
            }
        }
    }
    if (collided && (isNewSlope || p.mode == Gamemode::Wave)) {
        r.contact = true;
        r.death = true;
        return r;
    }
    // The plateau exit (gdp lines 161-165; 2.2081 0x390415-0x390488): while the player was on a
    // slope, a surface y equal to the previous contact's (m_unk3d0 holds double(float newPlayerY))
    // means the player is past the slope's end (newPlayerY clamped to the rect's top) -> no contact
    // this step; postCollision then sees wasOnSlope && !onSlope and launches. Without it /1 kept the
    // player glued to the clamped top for as long as the doubled broad-phase rect overlapped.
    if (p.wasOnSlope && p.slopeContactY == gdFloat(newPlayerY)) return r;
    if (!collided) return r;
    if (downhill && p.mode == Gamemode::Wave && p.jumpBuffered && p.upsideDown == floorTop) return r;
    r.contact = true;
    r.ceiling = slopeUpsideDown;
    r.newY = newPlayerY;
    r.downhill = downhill;
    r.slopeYVelocity = slopeYVelocity;
    r.playerRadOnSlope = playerRadOnSlope;
    double mult = std::min(kConstants.slopeVelocityAngleNum / std::max(angle, 1e-6), kConstants.slopeVelocityMax);
    r.slopeVelocity = mult * slopeYVelocity * fm * (downhill ? -1.0 : 1.0);
    if (flying || p.mode == Gamemode::Ball) r.slopeVelocity *= kConstants.slopeVelocityFlyerBall;
    return r;
}

void checkSnapJumpToObject(PlayerState& p, World const& world, int objectIndex) {
    if (objectIndex < 0 || objectIndex >= static_cast<int>(world.objects.size())) return;
    SimObject const& obj = world.objects[static_cast<size_t>(objectIndex)];
    double const posX = p.x;
    double const objPosX = obj.x;
    if (p.snappedObjectIndex >= 0 && p.snappedObjectIndex != objectIndex && p.snappedObjectIndex < static_cast<int>(world.objects.size())) {
        SimObject const& prev = world.objects[static_cast<size_t>(p.snappedObjectIndex)];
        double threshold, bigStair, downStair, littleStair;
        bool const normalSize = !p.mini;
        if (p.speed == Speed::Normal) {
            threshold = 1.0; bigStair = 90.0; downStair = 150.0; littleStair = normalSize ? 120.0 : 90.0;
        } else if (p.speed == Speed::Half) {
            threshold = 1.0; bigStair = 60.0; downStair = 120.0; littleStair = 90.0;
        } else if (p.speed == Speed::Double) {
            threshold = 2.0; bigStair = 120.0; downStair = 195.0; littleStair = normalSize ? 150.0 : 90.0;
        } else if (p.speed == Speed::Triple) {
            threshold = 2.0; bigStair = 135.0; downStair = 225.0; littleStair = 90.0;
        } else if (normalSize) {
            threshold = 2.0; bigStair = 135.0; downStair = 225.0; littleStair = 180.0;
        } else {
            threshold = 1.0; bigStair = 90.0; downStair = 150.0; littleStair = 120.0;
        }
        double const blockLength = flipMod(p.upsideDown) * 30.0;
        double const dx = obj.x - prev.x;
        double const dy = obj.y - prev.y;
        bool const match = (std::fabs(dx - littleStair) <= threshold && std::fabs(dy - blockLength) <= threshold) ||
                           (std::fabs(dx - downStair) <= threshold && std::fabs(dy + blockLength) <= threshold) ||
                           (std::fabs(dx - bigStair) <= threshold && std::fabs(dy - blockLength * 2.0) <= threshold);
        if (match) {
            double newPosX = objPosX + p.snapDistance;
            if (std::fabs(newPosX - posX) > threshold) newPosX = newPosX <= posX ? posX - threshold : posX + threshold;
            p.x = gdFloat(newPosX);                    // setPositionX: a float
        }
    }
    p.snappedObjectIndex = objectIndex;
    p.snapDistance = posX - objPosX;
}

}  // namespace gprl::sim
