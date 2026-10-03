#pragma once
// Rect, slope and hazard geometry for the isolated simulator (docs/BACKGROUND_ANALYZER_DESIGN.md
// §4.2). PURE C++20, no GD symbols. The object rects come from the extraction (SimObject::rx..rh =
// GameObject::getObjectRect()); the player's rects follow GameObject::getObjectRect(w, h) for a
// PlayerObject (m_width * m_scale centred on the position; 0.3 for the inner death rect).
#include <cstdint>
#include <vector>

#include "engine.hpp"
#include "world.hpp"

namespace gprl::sim {

struct Rect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;   // left, bottom, right, top
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
    double cx() const { return (x0 + x1) * 0.5; }
    double cy() const { return (y0 + y1) * 0.5; }
};

/// cocos2d CCRect::intersectsRect: closed intervals (touching edges intersect).
inline bool intersects(Rect const& a, Rect const& b) {
    return !(a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0);
}

/// The extracted collision rect of an object.
inline Rect objectRect(SimObject const& o) { return Rect{o.rx, o.ry, o.rx + o.rw, o.ry + o.rh}; }

/// The slope broad-phase rect GD uses in collisionCheckObjects: getObjectRect(2, 2), the rect
/// scaled by 2 about its centre (2.2081 @0x214a0b-0x214ac8).
Rect slopeBroadRect(SimObject const& o);

/// The player's full collision rect (30 x 30 cube, 18 mini; 27 spider; 10 wave).
Rect playerRect(PlayerState const& p);
/// The player's inner death rect vs solids: getObjectRect(0.3, 0.3).
Rect playerInnerRect(PlayerState const& p);

/// Hazard test as GJBaseGameLayer::checkCollisions does it (2.2081 0x2147c0-0x2148cc): the player's
/// FULL rect vs the hazard: a circle (m_objectRadius > 0, or a saw by object id) via the closest
/// point, an oriented hazard (m_shouldUseOuterOb) via GD's own oriented box when the extraction
/// carries it (SimObject::oriented / obb*), else via a box reconstructed from the AABB and the
/// rotation, else the AABB. The player's box is never rotated (its rotation is not simulated).
bool hazardHits(SimObject const& hazard, PlayerState const& p);

/// Object ids whose GD hitbox is a circle (gdclone object.json "Circle"): the saw family. Only a
/// fallback for worlds without SimObject::radius.
bool circularHazardId(int objectId);

/// GD's circle radius of an object with m_objectRadius > 0 (scaled by max(scaleX, scaleY) unless
/// both are 1, playerCircleCollision 0x211df0); 0 without a radius.
double gdCircleRadius(SimObject const& o);
/// playerCircleCollision's default test: the centre inside the rect or a rect corner closer than r.
bool circleHitsRect(double cx, double cy, double r, Rect const& playerRect);
/// The object has GD's oriented box (SimObject::oriented with a size from the extraction).
bool hasOrientedBox(SimObject const& o);
/// The player rect overlaps the object as GD tests it for portals / pads (and hazards without a
/// radius): the AABB, then GD's oriented box when the object is oriented.
bool objectOverlaps(SimObject const& o, Rect const& playerRect);

// ---- slopes (GameObject::slopeYPos, gdp 2.2) ----

/// The slope's solid part is at the top (orientations 3 / 4 of world.hpp): a ceiling slope.
constexpr bool slopeFloorTop(SimObject const& o) { return o.slope == 3 || o.slope == 4; }
/// GameObject::m_slopeUphill: the surface rises to the right (orientations 1 / 4).
constexpr bool slopeUphill(SimObject const& o) { return o.slope == 1 || o.slope == 4; }
/// GameObject::getSlopeAngle(): atan(height / width) in radians.
double slopeAngle(SimObject const& o);
/// GameObject::slopeYPos(playerX): the surface height at x (extended beyond the rect), +-4 for hazard
/// slopes (none in /1: hazard slopes are Hazard objects).
double slopeYPos(SimObject const& o, double playerX);

/// Result of a slope contact test.
struct SlopeContact {
    bool contact = false;      // the player is on / under the slope this step
    bool death = false;        // the contact kills (wave on a slope, floor/ceiling slope switch, hazard slope)
    double newY = 0.0;         // the player's centre y on the surface
    bool ceiling = false;      // the slope acts as a ceiling for the player's gravity
    bool downhill = false;     // the player travels down the surface (gdp's inverted "playerUphill")
    double slopeYVelocity = 0.0;   // rh * playerSpeed * speedMultiplier / rw
    double slopeVelocity = 0.0;    // m_slopeVelocity: min(1.12 / angle, 1.54) * slopeYVelocity * flipMod * (downhill ? -1 : 1) [* 0.75 flyers / ball]
    double playerRadOnSlope = 0.0; // playerRadius / cos(angle)
};

/// PlayerObject::collidedWithSlopeInternal for a static slope and a non-platformer player (gdp 2.2),
/// evaluated against the state after the position update. `slopeIndex` names the slope in
/// World::objects (for the new-slope rule); `totalTime` in seconds.
SlopeContact slopeContact(SimObject const& slope, int slopeIndex, PlayerState const& p, double totalTime);

/// PlayerObject::checkSnapJumpToObject (gdp 2.2): the cube's x snap when landing on the stair
/// patterns GD recognises. Mutates p.x and the snap bookkeeping.
void checkSnapJumpToObject(PlayerState& p, World const& world, int objectIndex);

}  // namespace gprl::sim
