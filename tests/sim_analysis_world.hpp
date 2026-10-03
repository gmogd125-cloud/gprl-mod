#pragma once
// Synthetic worlds for the analysis-side simulator tests (search / windows / verify / hash / job).
// Owned by the analysis tests; the physics tests keep their own helper (copied by agreement, never
// shared, so the two builders do not depend on each other's test files).
//
// Conventions (GD's own): the floor line is y = kGround (90, PlayLayer ground: the player's
// bottom rests there, a regular cube's centre at 105); every y below is RELATIVE to the ground:
// a block is a 30 x 30 solid whose bottom sits `yBottom` above the ground; a spike is a hazard
// rect 9 x 12 standing on the ground at x; an orb is centred `yAbove` above the ground; a wall is
// a column of blocks from the ground up (impossible to jump when tall).
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../core/sim/world.hpp"

namespace simworld {

using namespace gprl::sim;

constexpr float kGround = 90.f;
constexpr float kCubeHalf = 15.f;

inline World base(float endX = 1500.f, int levelId = 1) {
    World w;
    w.gdLevelId = levelId;
    w.levelHash = std::string(64, 'a');
    w.groundY = kGround;
    w.ceilingY = 0.f;
    w.lengthX = endX;
    w.endX = endX;
    w.start.x = 0.f;
    w.start.y = kGround + kCubeHalf;
    w.start.mode = Gamemode::Cube;
    w.start.speed = Speed::Normal;
    return w;
}

inline SimObject block(float x, float yBottom, int uniqueId = 0) {
    SimObject o;
    o.objectId = 1;
    o.uniqueId = uniqueId;
    o.gdType = 0;
    o.kind = ObjKind::Solid;
    o.x = x;
    o.y = kGround + yBottom + 15.f;
    o.rx = x - 15.f;
    o.ry = kGround + yBottom;
    o.rw = 30.f;
    o.rh = 30.f;
    return o;
}

inline SimObject spike(float x, int uniqueId = 0) {
    SimObject o;
    o.objectId = 8;
    o.uniqueId = uniqueId;
    o.gdType = 2;
    o.kind = ObjKind::Hazard;
    o.x = x;
    o.y = kGround + 15.f;
    o.rx = x - 4.5f;
    o.ry = kGround;
    o.rw = 9.f;
    o.rh = 12.f;
    return o;
}

inline SimObject orb(float x, float yAbove, OrbKind kind = OrbKind::Yellow) {
    SimObject o;
    o.objectId = kind == OrbKind::Yellow ? 36 : (kind == OrbKind::Pink ? 141 : 84);
    o.gdType = 11;
    o.kind = ObjKind::Orb;
    o.orb = kind;
    o.x = x;
    o.y = kGround + yAbove;
    o.rx = x - 15.f;
    o.ry = kGround + yAbove - 15.f;
    o.rw = 30.f;
    o.rh = 30.f;
    return o;
}

inline SimObject portal(float x, Gamemode mode) {
    SimObject o;
    o.objectId = mode == Gamemode::Ship ? 13 : 12;
    o.gdType = mode == Gamemode::Ship ? 5 : 6;
    o.kind = ObjKind::GamemodePortal;
    o.mode = mode;
    o.x = x;
    o.y = kGround + 45.f;
    o.rx = x - 15.f;
    o.ry = kGround;
    o.rw = 30.f;
    o.rh = 90.f;
    return o;
}

inline SimObject speedChange(float x, Speed speed) {
    SimObject o;
    o.objectId = speed == Speed::Half ? 200 : (speed == Speed::Normal ? 201 : (speed == Speed::Double ? 202 : (speed == Speed::Triple ? 203 : 1334)));
    o.gdType = 1;
    o.kind = ObjKind::SpeedChange;
    o.speed = speed;
    o.x = x;
    o.y = kGround + 15.f;
    o.rx = x - 15.f;
    o.ry = kGround;
    o.rw = 30.f;
    o.rh = 30.f;
    return o;
}

inline SimObject decoration(float x, float y) {
    SimObject o;
    o.objectId = 1000;
    o.gdType = 7;
    o.kind = ObjKind::Decoration;
    o.x = x;
    o.y = y;
    o.rx = x - 15.f;
    o.ry = y - 15.f;
    o.rw = 30.f;
    o.rh = 30.f;
    return o;
}

/// A 30 x 30 hazard (a saw-less "hazard block") whose bottom sits `yBottom` above the ground.
inline SimObject hazardBlock(float x, float yBottom) {
    SimObject o;
    o.objectId = 9;
    o.gdType = 2;
    o.kind = ObjKind::Hazard;
    o.x = x;
    o.y = kGround + yBottom + 15.f;
    o.rx = x - 15.f;
    o.ry = kGround + yBottom;
    o.rw = 30.f;
    o.rh = 30.f;
    return o;
}

/// A column of SOLID blocks from the ground up (impossible to jump when tall). Relies on the
/// engine's wall death (collidedWithObjectInternal tail: the inner rect vs the block side).
inline void addWall(World& w, float x, int blocksTall) {
    for (int i = 0; i < blocksTall; ++i) w.objects.push_back(block(x, 30.f * i));
}

/// A column of HAZARDS from the ground up: impossible to pass whatever the solid-collision
/// details (hazards kill through the player's full rect). The search tests use this one so the
/// unsolved-span logic is tested independently of the wall-death physics.
inline void addHazardWall(World& w, float x, int blocksTall) {
    for (int i = 0; i < blocksTall; ++i) w.objects.push_back(hazardBlock(x, 30.f * i));
}

inline void addUnsupported(World& w, float x0, float x1, char const* mechanic) {
    UnsupportedSpan u;
    u.x0 = x0;
    u.x1 = x1;
    u.mechanic = mechanic;
    w.unsupported.push_back(u);
    std::sort(w.unsupported.begin(), w.unsupported.end(), [](UnsupportedSpan const& a, UnsupportedSpan const& b) { return a.x0 < b.x0; });
}

inline void finalize(World& w) {
    std::stable_sort(w.objects.begin(), w.objects.end(), [](SimObject const& a, SimObject const& b) { return a.left() < b.left(); });
    int id = 1;
    for (auto& o : w.objects)
        if (o.uniqueId == 0) o.uniqueId = id++;
    w.gameplayObjects = 0;
    for (auto const& o : w.objects)
        if (o.kind != ObjKind::Decoration) ++w.gameplayObjects;
}

inline World emptyWorld(float endX = 600.f) {
    World w = base(endX);
    finalize(w);
    return w;
}

inline World spikeWorld(float spikeX = 300.f, float endX = 900.f) {
    World w = base(endX);
    w.objects.push_back(spike(spikeX));
    finalize(w);
    return w;
}

inline World tripleSpikeWorld(float firstX = 300.f, float endX = 900.f) {
    World w = base(endX);
    w.objects.push_back(spike(firstX));
    w.objects.push_back(spike(firstX + 30.f));
    w.objects.push_back(spike(firstX + 60.f));
    finalize(w);
    return w;
}

inline World wallWorld(float wallX = 600.f, float endX = 1500.f) {
    World w = base(endX);
    addHazardWall(w, wallX, 10);
    finalize(w);
    return w;
}

inline World solidWallWorld(float wallX = 600.f, float endX = 1500.f) {
    World w = base(endX);
    addWall(w, wallX, 10);
    finalize(w);
    return w;
}

/// Loads a World fixture written as tagged CSV lines (tests/fixtures/sim/deadlocked-world.csv: W
/// world, T start, P StartPos, S span, O object; '#' comments; floats %.9g = an exact round trip).
/// Returns false when the file is missing or malformed.
inline bool loadWorldCsv(std::string const& path, World& w) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    static std::set<std::string> interned;   // SimObject::unsupportedMechanic is a static C string
    w = World{};
    std::string line;
    auto split = [](std::string const& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == ',') {
                out.push_back(cur);
                cur.clear();
            }
            else if (c != '\r') cur.push_back(c);
        }
        out.push_back(cur);
        return out;
    };
    auto fl = [](std::string const& s) { return std::strtof(s.c_str(), nullptr); };
    auto in = [](std::string const& s) { return std::atoi(s.c_str()); };
    auto startOf = [&](std::vector<std::string> const& c) {
        StartState s;
        s.x = fl(c[1]);
        s.y = fl(c[2]);
        s.mode = static_cast<Gamemode>(in(c[3]));
        s.mini = in(c[4]) != 0;
        s.upsideDown = in(c[5]) != 0;
        s.speed = static_cast<Speed>(in(c[6]));
        s.dual = in(c[7]) != 0;
        s.mirror = in(c[8]) != 0;
        s.reversed = in(c[9]) != 0;
        s.platformer = in(c[10]) != 0;
        s.twoPlayer = in(c[11]) != 0;
        s.percent = fl(c[12]);
        return s;
    };
    bool haveWorld = false;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c = split(line);
        if (c[0] == "W" && c.size() >= 8) {
            w.gdLevelId = in(c[1]);
            w.levelHash = std::string(64, 'a');
            w.groundY = fl(c[2]);
            w.ceilingY = fl(c[3]);
            w.lengthX = fl(c[4]);
            w.endX = fl(c[5]);
            w.gameplayObjects = in(c[6]);
            w.decorationObjects = in(c[7]);
            haveWorld = true;
        }
        else if (c[0] == "T" && c.size() >= 13) w.start = startOf(c);
        else if (c[0] == "P" && c.size() >= 13) w.startPositions.push_back(startOf(c));
        else if (c[0] == "S" && c.size() >= 5) {
            UnsupportedSpan s;
            s.x0 = fl(c[1]);
            s.x1 = fl(c[2]);
            s.objectId = in(c[3]);
            s.mechanic = c[4];
            w.unsupported.push_back(s);
        }
        else if (c[0] == "O" && c.size() >= 33) {
            SimObject o;
            o.objectId = in(c[1]);
            o.gdType = static_cast<uint8_t>(in(c[2]));
            o.kind = static_cast<ObjKind>(in(c[3]));
            o.x = fl(c[4]);
            o.y = fl(c[5]);
            o.rx = fl(c[6]);
            o.ry = fl(c[7]);
            o.rw = fl(c[8]);
            o.rh = fl(c[9]);
            o.rotation = fl(c[10]);
            o.scaleX = fl(c[11]);
            o.scaleY = fl(c[12]);
            o.flipX = in(c[13]) != 0;
            o.flipY = in(c[14]) != 0;
            o.slope = static_cast<SlopeOrientation>(in(c[15]));
            o.mode = static_cast<Gamemode>(in(c[16]));
            o.speed = static_cast<Speed>(in(c[17]));
            o.flag = in(c[18]) != 0;
            o.orb = static_cast<OrbKind>(in(c[19]));
            o.pad = static_cast<PadKind>(in(c[20]));
            o.multiActivate = in(c[21]) != 0;
            o.hidden = in(c[22]) != 0;
            o.groupsHash = static_cast<uint32_t>(std::strtoul(c[23].c_str(), nullptr, 10));
            o.groupCount = static_cast<uint16_t>(in(c[24]));
            o.radius = fl(c[25]);
            o.oriented = in(c[26]) != 0;
            o.obbCx = fl(c[27]);
            o.obbCy = fl(c[28]);
            o.obbW = fl(c[29]);
            o.obbH = fl(c[30]);
            o.obbAngle = fl(c[31]);
            if (!c[32].empty()) o.unsupportedMechanic = interned.insert(c[32]).first->c_str();
            o.uniqueId = static_cast<int>(w.objects.size()) + 1;
            w.objects.push_back(o);
        }
        else return false;
    }
    return haveWorld && !w.objects.empty();
}

}  // namespace simworld
