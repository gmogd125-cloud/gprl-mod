#pragma once
// gprl-presentation-fp/2 (docs/LEVEL_FAMILY_DESIGN.md Â§3): the per-section presentation feature
// vector, the readability penalty and the digest over the quantized vectors.
//
// Membership: a decoration object / trigger belongs to the section holding its centre x (the same
// rule as gameplay objects); obstruction and clutter use every decoration rect that overlaps the
// section's x range. "Drawn in front" compares (zLayer, zOrder) of the decoration with the GD
// default z of the gameplay object's kind (SimObject carries no z): blocks / hazards / slopes
// (T1 = 5, order 2), portals (5, 10), orbs / pads (B1 = 3, order 12), speed changes (B2 = 1, order
// -6) - the default_z_layer / default_z_order of opstic gdclone's assets/data/object.json.
// Decoration below the low-opacity threshold never obstructs (it tints).
//
// Fake objects: decoration (passable) whose objectId is a collidable sprite - the 337 ids that
// carry a hitbox (Box / Slope / Circle) in gdclone's object.json: blocks, spikes, saws, slopes,
// orbs, pads, portals, speed changes, collision blocks - or that the extraction flagged hazardLook.
#include <vector>

#include "identity.hpp"
#include "sections.hpp"

namespace gprl::identity {

enum class SpriteKind : uint8_t { None = 0, Box, Slope, Circle };

/// The gdclone hitbox kind of an object id (None = decoration-only sprite).
SpriteKind collidableSpriteKind(int objectId);

/// Decoration that looks like gameplay but has no collision.
bool isFakeObject(DecoObject const& d);

/// GD default (zLayer, zOrder) of a gameplay object's kind.
void gameplayZ(SimObject const& o, int& zLayer, int& zOrder);

/// Decoration drawn in front of a gameplay object.
bool drawnInFront(DecoObject const& d, SimObject const& o);

/// Union area of axis-aligned rects (exact sweep; rects as {x0, y0, x1, y1}).
struct Rect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};
double unionArea(std::vector<Rect> rects);

/// The Â§3 vector of one section. `sectionObjects` = the gameplay objects assigned to it.
PresentationVector presentationOf(World const& world, DecoSet const& deco, std::vector<SectionCut> const& cuts, int index,
                                  std::vector<SimObject const*> const& sectionObjects, IdentityConfig const& cfg = kIdentityConfig);

/// 16 hex: FNV-1a 64 over (section count, then per section the 11 features as round(f * quantum)).
std::string presentationFingerprint(std::vector<SectionIdentity> const& sections, IdentityConfig const& cfg = kIdentityConfig);

}  // namespace gprl::identity
