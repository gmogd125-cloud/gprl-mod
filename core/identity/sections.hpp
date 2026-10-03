#pragma once
// gprl-section-fp/2 (docs/LEVEL_FAMILY_DESIGN.md Â§2, FA-D4): the cut rule and the per-section
// gameplay hashes.
//
// Cut rule over World::objects: forced cuts at the centre x of every gamemode portal, speed change,
// size portal and every Unsupported object whose mechanic is a dual / mirror / teleport portal
// (0 < x < level end, duplicates within 0.01 units merged). A span between forced cuts longer than
// `maxSectionLength` is split into n = ceil(len / max) EQUAL pieces (so no piece is shorter than
// max / 2 = 300 > minSectionLength); a forced cut may still produce a shorter section (a portal
// right after another). Objects belong to the section whose range holds their centre x (the portal
// at a cut starts its section). The level end = World::endX, else lengthX, else the last right edge.
//
// Per section: hash = FNV-1a 64 over the sorted tuple hashes with x relative to xFrom (an added intro
// or a global shift keeps every section that starts at a portal), the MinHash signature, the
// gamemode / speed / mini entering the section (every mode / speed / size portal assigned to this
// or an earlier section applied in x order from World::start), the object count and the percent
// range (x / endX * 100).
#include <vector>

#include "identity.hpp"

namespace gprl::identity {

struct SectionCut {
    float xFrom = 0.f, xTo = 0.f;
    bool forced = true;            // starts at the level start or a forced cut (not a length split)
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool mini = false;
};

/// Where the level ends for the cut rule and the percent axis.
float levelEnd(World const& world);

/// The objects that force a cut.
bool isCutObject(SimObject const& o);

/// The ordered cuts of a world (never empty: an empty world has one section).
std::vector<SectionCut> cutSections(World const& world, IdentityConfig const& cfg = kIdentityConfig);

/// The section holding x: the last section whose xFrom <= x (x before the first = 0, x at or past
/// the end = the last).
int sectionIndexForX(std::vector<SectionCut> const& cuts, double x);

/// Sections with index, ranges, percents, hash, minhash, state and object count filled (the
/// presentation vector and readability stay zero: presentation.hpp fills them).
std::vector<SectionIdentity> sections(World const& world, IdentityConfig const& cfg = kIdentityConfig);

/// The same, reusing cuts and the per-object section assignment (`assignment[i]` = section of
/// world.objects[i], -1 = not hashed).
std::vector<SectionIdentity> sectionsFor(World const& world, std::vector<SectionCut> const& cuts, std::vector<int> const& assignment,
                                         IdentityConfig const& cfg = kIdentityConfig);

/// `assignment[i]` for every world object (-1 for StartPos objects).
std::vector<int> assignSections(World const& world, std::vector<SectionCut> const& cuts);

}  // namespace gprl::identity
