// analyzer_extract_tests: the pure half of the world extraction (core/analyzer_extract.hpp;
// docs/BACKGROUND_ANALYZER_DESIGN.md §3, §4.1, §4.7): classification by object id / GameObjectType,
// GD 2.2081's rect formula (getObjectRect / getBoxOffset / updateIsOriented / updateOrientedBox /
// OBB2D, replicated from the disassembly), GD's slope rules, slope corners, the group hash,
// unsupported spans, the object cap (while collecting and at the end), the decoration summary and
// the StartPos list.
#include "../core/analyzer_extract.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace gprl;
using namespace gprl::analyzer::extract;

namespace {

ObjectReading obj(int type, int id, float x, float y = 105.f) {
    ObjectReading r;
    r.gdType = type;
    r.objectId = id;
    r.uniqueId = id * 10 + static_cast<int>(x);
    r.x = x;
    r.y = y;
    r.rectExact = true;
    r.rect = {x - 15.f, y - 15.f, 30.f, 30.f};
    r.width = 30.f;
    r.height = 30.f;
    return r;
}

ObjectReading withGroups(ObjectReading r, std::initializer_list<int16_t> groups) {
    int i = 0;
    for (auto g : groups) r.groups[static_cast<size_t>(i++)] = g;
    r.groupCount = i;
    return r;
}

BuildParams params(float length = 3000.f) {
    BuildParams p;
    p.gdLevelId = 1234;
    p.levelHash = "abc";
    p.lengthX = length;
    p.endX = length;
    return p;
}

bool hasSpan(sim::World const& w, std::string const& mechanic, float x0, float x1) {
    for (auto const& s : w.unsupported) {
        if (s.mechanic == mechanic && std::fabs(s.x0 - x0) < 1e-3f && std::fabs(s.x1 - x1) < 1e-3f) return true;
    }
    return false;
}

int spansOf(sim::World const& w, std::string const& mechanic) {
    int n = 0;
    for (auto const& s : w.unsupported) n += s.mechanic == mechanic ? 1 : 0;
    return n;
}

}  // namespace

int main(int, char**) {
    SECTION("classification: object ids before types");
    {
        CHECK(classify(7, 31).use == Use::StartPos);
        auto s = classify(20, 200);
        CHECK(s.use == Use::Gameplay && s.kind == sim::ObjKind::SpeedChange && s.speed == sim::Speed::Half);
        CHECK(classify(20, 201).speed == sim::Speed::Normal);
        CHECK(classify(20, 202).speed == sim::Speed::Double);
        CHECK(classify(20, 203).speed == sim::Speed::Triple);
        CHECK(classify(7, 1334).speed == sim::Speed::Quadruple);   // id wins over the type
        auto move = classify(20, 901);
        CHECK(move.use == Use::Gameplay && move.kind == sim::ObjKind::Unsupported && move.trigger);
        CHECK(std::string(move.mechanic) == "move_trigger");
        CHECK(move.span == SpanRule::IfTargetsGameplay && std::string(move.targetMechanic) == "moved_by_trigger");
        for (int id : {901, 1346, 1347, 1049, 1268, 1815, 1611, 1811, 1817, 2067, 1585, 1814, 1917}) {
            auto c = classify(20, id);
            CHECK_MSG(c.kind == sim::ObjKind::Unsupported && c.use == Use::Gameplay && c.mechanic, std::to_string(id));
        }
        CHECK(classify(20, 1268).span == SpanRule::None);   // spawn acts through other triggers
        CHECK(classify(20, 1917).span == SpanRule::Here);   // reverse changes the player
        // harmless triggers (colour, pulse, alpha, song) are skipped: not simulated, not hashed
        for (int id : {899, 1006, 1007, 1934, 1913}) CHECK(classify(20, id).use == Use::Skip);
        CHECK(classify(45, 22).use == Use::Skip);
        CHECK(classify(1, 4242, true).use == Use::Skip);    // an unknown trigger id (m_isTrigger) is harmless
    }

    SECTION("classification: GameObjectType 0..47");
    {
        CHECK(classify(0, 1).kind == sim::ObjKind::Solid);
        CHECK(classify(21, 143).kind == sim::ObjKind::Solid);
        CHECK(classify(2, 8).kind == sim::ObjKind::Hazard);
        CHECK(classify(47, 1705).kind == sim::ObjKind::Hazard);
        CHECK(classify(25, 289).kind == sim::ObjKind::Slope);
        auto inv = classify(3, 11);
        CHECK(inv.kind == sim::ObjKind::GravityPortal && inv.flag);
        auto norm = classify(4, 10);
        CHECK(norm.kind == sim::ObjKind::GravityPortal && !norm.flag);
        CHECK(std::string(classify(42, 2926).mechanic) == "gravity_toggle_portal");
        struct { int type; sim::Gamemode mode; } portals[] = {{5, sim::Gamemode::Ship}, {6, sim::Gamemode::Cube}, {16, sim::Gamemode::Ball}, {19, sim::Gamemode::Ufo},
                                                              {26, sim::Gamemode::Wave}, {27, sim::Gamemode::Robot}, {33, sim::Gamemode::Spider}, {41, sim::Gamemode::Swing}};
        for (auto const& p : portals) {
            auto c = classify(p.type, 1000 + p.type);
            CHECK(c.kind == sim::ObjKind::GamemodePortal && c.mode == p.mode);
        }
        CHECK(classify(17, 101).kind == sim::ObjKind::SizePortal && !classify(17, 101).flag);
        CHECK(classify(18, 99).kind == sim::ObjKind::SizePortal && classify(18, 99).flag);
        struct { int type; sim::OrbKind orb; } orbs[] = {{11, sim::OrbKind::Yellow}, {12, sim::OrbKind::Pink}, {35, sim::OrbKind::Red}, {13, sim::OrbKind::Gravity},
                                                         {29, sim::OrbKind::Green}, {32, sim::OrbKind::Black}, {43, sim::OrbKind::Spider}};
        for (auto const& o : orbs) {
            auto c = classify(o.type, 36);
            CHECK(c.kind == sim::ObjKind::Orb && c.orb == o.orb);
        }
        for (int t : {36, 37, 38, 46}) CHECK(classify(t, 1).kind == sim::ObjKind::Unsupported);
        struct { int type; sim::PadKind pad; } pads[] = {{8, sim::PadKind::Yellow}, {9, sim::PadKind::Pink}, {34, sim::PadKind::Red}, {10, sim::PadKind::Gravity},
                                                         {44, sim::PadKind::Spider}};
        for (auto const& p : pads) {
            auto c = classify(p.type, 35);
            CHECK(c.kind == sim::ObjKind::Pad && c.pad == p.pad);
        }
        CHECK(std::string(classify(14, 45).mechanic) == "mirror_portal");
        CHECK(std::string(classify(15, 46).mechanic) == "mirror_portal");
        CHECK(classify(23, 286).span == SpanRule::UntilSolo);
        CHECK(classify(24, 287).span == SpanRule::None);
        CHECK(std::string(classify(28, 747).mechanic) == "teleport_portal");
        CHECK(std::string(classify(39, 1816).mechanic) == "collision_block");
        CHECK(std::string(classify(40, 1).mechanic) == "special_object");
        CHECK(classify(7, 1).use == Use::Decoration);
        for (int t : {22, 30, 31}) CHECK(classify(t, 1).use == Use::Skip);
        CHECK(std::string(classify(1, 1).mechanic) == "unknown_object_type");
        CHECK(std::string(classify(99, 1).mechanic) == "unknown_object_type");
    }

    SECTION("level settings: Geode Speed and start mode");
    {
        CHECK(speedFromGeodeSpeed(0) == sim::Speed::Normal);
        CHECK(speedFromGeodeSpeed(1) == sim::Speed::Half);
        CHECK(speedFromGeodeSpeed(2) == sim::Speed::Double);
        CHECK(speedFromGeodeSpeed(3) == sim::Speed::Triple);
        CHECK(speedFromGeodeSpeed(4) == sim::Speed::Quadruple);
        CHECK(gamemodeFromStartMode(0) == sim::Gamemode::Cube);
        CHECK(gamemodeFromStartMode(4) == sim::Gamemode::Wave);
        CHECK(gamemodeFromStartMode(7) == sim::Gamemode::Swing);
        CHECK(gamemodeFromStartMode(12) == sim::Gamemode::Cube);
    }

    SECTION("GD's float -> int truncation (cvttss2si) and the rotation-aligned rule (setRotation 0x197e00)");
    {
        CHECK(gdTruncate(1.9f) == 1 && gdTruncate(-1.9f) == -1 && gdTruncate(89.99f) == 89);
        CHECK(gdTruncate(std::nanf("")) == static_cast<int>(0x80000000u));
        CHECK(gdTruncate(3e9f) == static_cast<int>(0x80000000u) && gdTruncate(-3e9f) == static_cast<int>(0x80000000u));
        for (float r : {90.f, -90.f, 270.f, -270.f}) CHECK(gdRotationAligned(r));
        for (float r : {0.f, 180.f, -180.f, 360.f, 450.f, 90.5f, 89.99f}) CHECK_MSG(!gdRotationAligned(r), std::to_string(r));   // exact float equality, like GD
    }

    SECTION("updateIsOriented (0x1a1730): decoration never, solids / slopes only when no-touch, int(rotation) % 90, radius");
    {
        CHECK(!gdIsOriented(7, true, 45.f, 0.f));                 // decoration
        CHECK(!gdIsOriented(0, false, 45.f, 0.f));                // a rotated solid keeps an axis rect
        CHECK(gdIsOriented(0, true, 45.f, 0.f));                  // ... unless it is no-touch
        CHECK(!gdIsOriented(21, false, 30.f, 0.f) && !gdIsOriented(25, false, 30.f, 0.f));
        CHECK(gdIsOriented(2, false, 45.f, 0.f));                 // a rotated hazard
        CHECK(!gdIsOriented(2, false, 90.f, 0.f) && !gdIsOriented(2, false, -270.f, 0.f));
        CHECK(!gdIsOriented(2, false, 90.4f, 0.f));               // truncated to 90
        CHECK(gdIsOriented(2, false, 89.9f, 0.f));                // truncated to 89
        CHECK(!gdIsOriented(2, false, 0.6f, 0.f));                // truncated to 0
        CHECK(!gdIsOriented(11, false, 45.f, 12.f));              // orbs have a radius: never oriented
        CHECK(!gdIsOriented(2, false, 45.f, std::nanf("")));      // comiss with NaN jumps: not oriented
    }

    SECTION("getObjectRect(w, h) (0x1976c0): (scale x size) x sprite scale, |scale| only when mirrored, swapped when aligned");
    {
        RectInputs in;
        in.x = 100.f;
        in.y = 200.f;
        in.width = 30.f;
        in.height = 30.f;
        auto g = gdObjectRect(in);
        CHECK(!g.oriented && !g.aligned);
        CHECK(g.rect.x == 85.f && g.rect.y == 185.f && g.rect.w == 30.f && g.rect.h == 30.f);
        in.width = 10.f;
        in.height = 20.f;
        in.scaleX = 2.f;
        in.spriteWidthScale = 0.5f;
        in.spriteHeightScale = 0.25f;
        g = gdObjectRect(in);
        CHECK(g.rect.w == 10.f && g.rect.h == 5.f);              // (2 x 10) x 0.5, (1 x 20) x 0.25
        in.rotationX = in.rotationY = 90.f;                       // aligned: w / h swapped
        g = gdObjectRect(in);
        CHECK(g.aligned && g.rect.w == 5.f && g.rect.h == 10.f && g.rect.x == 97.5f && g.rect.y == 195.f);
        in.rotationX = in.rotationY = 180.f;                      // not aligned: unchanged
        CHECK(gdObjectRect(in).rect.w == 10.f);
        in.rotationX = in.rotationY = 0.f;
        in.scaleX = -2.f;                                         // a negative scale keeps its sign ...
        g = gdObjectRect(in);
        CHECK(g.rect.w == -10.f && g.rect.x == 105.f);
        Rect n = g.rect;
        CHECK(normalizeRect(n) && n.x == 95.f && n.w == 10.f);   // ... normalised for the simulator
        in.mirroredByScale = true;                                // ... unless m_isMirroredByScale (fabs)
        CHECK(gdObjectRect(in).rect.w == 10.f);
        // the exact float order: (scale x width) x spriteScale, rounding like GD's two mulss
        RectInputs f;
        f.width = 0.1f;
        f.height = 1.f;
        f.scaleX = 3.3f;
        f.spriteWidthScale = 0.7f;
        float expect = (3.3f * 0.1f) * 0.7f;
        CHECK(gdAxisRect(f, false, {}).w == expect);
    }

    SECTION("getBoxOffset (0x1a17d0): zero offsets stay zero; others follow the node's flip / scale / clockwise rotation");
    {
        RectInputs in;
        in.width = in.height = 30.f;
        in.customBoxOffsetX = 1e-8f;   // CCPoint::equals(CCPointZero): |d| < FLT_EPSILON
        auto z = gdBoxOffset(in);
        CHECK(z.x == 0.f && z.y == 0.f);
        in.customBoxOffsetX = 0.f;
        in.customBoxOffsetY = -10.f;
        auto p = gdBoxOffset(in);
        CHECK(p.x == 0.f && p.y == -10.f);
        in.flipY = true;               // GameObject::setFlipY negates the node's scaleY: the offset mirrors
        p = gdBoxOffset(in);
        CHECK_NEAR(p.y, 10.f, 1e-6);
        in.flipY = false;
        in.scaleY = 2.f;               // the node's scale applies too
        CHECK_NEAR(gdBoxOffset(in).y, -20.f, 1e-6);
        in.scaleY = 1.f;
        in.rotationX = in.rotationY = 90.f;   // clockwise: down -> left
        p = gdBoxOffset(in);
        CHECK_NEAR(p.x, -10.f, 1e-5);
        CHECK_NEAR(p.y, 0.f, 1e-5);
        auto g = gdObjectRect(in);     // the rect's centre carries the offset
        CHECK_NEAR(g.centre.x, -10.f, 1e-5);
        CHECK_NEAR(g.rect.x + g.rect.w * 0.5f, -10.f, 1e-4);
    }

    SECTION("updateOrientedBox + OBB2D (0x1a1570 / 0x6da80 / 0x6e270): the bounding rect of the rotated box");
    {
        RectInputs in;
        in.gdType = 2;                 // a hazard rotated by 45 degrees
        in.x = 100.f;
        in.y = 200.f;
        in.width = in.height = 30.f;
        in.rotationX = in.rotationY = 45.f;
        auto g = gdObjectRect(in);
        CHECK(g.oriented && !g.aligned);
        CHECK_NEAR(g.rect.w, 42.4264f, 1e-3);
        CHECK_NEAR(g.rect.h, 42.4264f, 1e-3);
        CHECK_NEAR(g.rect.x, 100.f - 21.2132f, 1e-3);
        CHECK_NEAR(g.obbAngle, -45.f * 0.0174532924f, 1e-7);
        CHECK(g.obbW == 30.f && g.obbH == 30.f);
        // corner order and float ops exactly as calculateWithCenter: c0 = (c - X) - Y, ...
        float a = (-45.f) * 0.0174532924f;
        float cs = ::cosf(a), sn = ::sinf(a), hw = 15.f;
        float minX = std::min({(100.f - cs * hw) - (-sn) * hw, (100.f + cs * hw) - (-sn) * hw, (100.f + cs * hw) + (-sn) * hw, (100.f - cs * hw) + (-sn) * hw});
        CHECK(g.rect.x == minX);
        // a solid rotated by 45 degrees keeps GD's axis rect (updateIsOriented skips types 0 / 21 / 25)
        in.gdType = 0;
        auto s = gdObjectRect(in);
        CHECK(!s.oriented && s.rect.w == 30.f);
        // GD's getBoundingRect quirk: min / max start at 0 (0 = "unset" for the minimum), so a box
        // entirely left of x = 0 keeps maxX = 0 - replicated, not corrected
        in.gdType = 2;
        in.x = -100.f;
        auto q = gdObjectRect(in);
        CHECK_NEAR(q.rect.x, -121.2132f, 1e-3);
        CHECK_NEAR(q.rect.w, 121.2132f, 1e-3);
        // no fabs in the oriented size: a negative scale flips the box, the bounding rect is the same
        in.x = 100.f;
        in.scaleX = -1.f;
        auto m = gdObjectRect(in);
        CHECK(m.obbW == -30.f);
        CHECK_NEAR(m.rect.w, 42.4264f, 1e-3);
    }

    SECTION("isFacingDown (0x1a1910) and determineSlopeDirection (0x19c2c0) replicas");
    {
        CHECK(!gdFacingDown(0.f, false) && gdFacingDown(0.f, true));
        CHECK(gdFacingDown(180.f, false) && !gdFacingDown(180.f, true));
        CHECK(gdFacingDown(-180.f, false));
        CHECK(!gdFacingDown(360.f, false) && !gdFacingDown(90.f, false) && gdFacingDown(90.f, true));
        CHECK(gdFacingDown(135.f, false) && !gdFacingDown(135.f, true) && gdFacingDown(-135.f, false));
        CHECK(!gdFacingDown(45.f, false) && gdFacingDown(45.f, true) && !gdFacingDown(269.9f, true));
        CHECK(!gdFacingDown(540.f, false));   // GD compares |rotation| with 180 without a modulo: kept
        int dir = -2;
        CHECK(gdSlopeUphill(0.f, false, false, &dir) && dir == 0);
        CHECK(!gdSlopeUphill(0.f, true, false, &dir) && dir == 2);
        CHECK(!gdSlopeUphill(0.f, false, true, &dir) && dir == 1);
        CHECK(gdSlopeUphill(0.f, true, true, &dir) && dir == 3);
        CHECK(gdSlopeUphill(180.f, false, false, &dir) && dir == 3);
        CHECK(!gdSlopeUphill(90.f, false, false, &dir) && dir == 4);
        CHECK(!gdSlopeUphill(-90.f, false, false, &dir) && dir == 5);
        CHECK(!gdSlopeUphill(270.f, false, false, &dir) && dir == 5);
        CHECK(!gdSlopeUphill(-270.f, false, false, &dir) && dir == 4);
        CHECK(gdSlopeUphill(90.f, true, false, &dir) && dir == 6);    // L3d5 -> L4b6 -> L4d4 -> L46c (dl) -> L4f5
        CHECK(gdSlopeUphill(-90.f, true, false, &dir) && dir == 7);   // ... L46c -> L474 (cl) -> L47c
        CHECK(gdSlopeUphill(90.f, false, true, &dir) && dir == 7);    // L394 -> L3bd -> L3f1 -> L48d -> L491 (dl) -> L47c
        CHECK(!gdSlopeUphill(-90.f, true, true, &dir) && dir == 4);   // L3d5 -> L3b1 -> L3b8 -> L405 (cl) -> L40e
        // for every unrotated / half-turned slope GD's two rules agree with the corner derivation
        for (float rot : {0.f, 180.f, -180.f, 360.f, -360.f}) {
            for (int f = 0; f < 4; ++f) {
                bool fx = f & 1, fy = f & 2;
                bool ok = false;
                auto gd = orientationFromGd(gdSlopeUphill(rot, fx, fy), gdFacingDown(rot, fy));
                CHECK_MSG(gd == orientationFromCorner(slopeCorner(rot, fx, fy, ok)), "rot " + std::to_string(rot) + " flips " + std::to_string(f));
            }
        }
    }

    SECTION("GD's anti-cheat spike is recognised by pointer or unique id");
    {
        static int objs[2] = {1, 2};   // two distinct objects (an array: the optimiser cannot share their slot)
        int const* a = &objs[0];
        int const* b = &objs[1];
        CHECK(isAnticheatSpike(a, 5, a, 7));
        CHECK(isAnticheatSpike(a, 7, b, 7));
        CHECK(!isAnticheatSpike(a, 5, b, 7));
        CHECK(!isAnticheatSpike(a, 5, nullptr, 5));
        CHECK(!isAnticheatSpike(nullptr, 5, b, 5));
    }

    SECTION("slope corners: the unrotated slope rises to the right with its right angle bottom-right");
    {
        bool ok = false;
        CHECK(slopeCorner(0.f, false, false, ok) == Corner::BottomRight && ok);
        CHECK(slopeCorner(0.f, true, false, ok) == Corner::BottomLeft);
        CHECK(slopeCorner(0.f, false, true, ok) == Corner::TopRight);
        CHECK(slopeCorner(0.f, true, true, ok) == Corner::TopLeft);
        CHECK(slopeCorner(90.f, false, false, ok) == Corner::BottomLeft && ok);    // clockwise quarter turn
        CHECK(slopeCorner(180.f, false, false, ok) == Corner::TopLeft);
        CHECK(slopeCorner(270.f, false, false, ok) == Corner::TopRight);
        CHECK(slopeCorner(-90.f, false, false, ok) == Corner::TopRight);
        CHECK(slopeCorner(360.f, false, false, ok) == Corner::BottomRight);
        slopeCorner(45.f, false, false, ok);
        CHECK(!ok);
        slopeCorner(90.5f, false, false, ok);
        CHECK(ok);
        // the simulator's encoding (core/sim/collision.hpp): uphill = 1 / 4, ceiling (floor top) = 3 / 4
        CHECK(orientationFromCorner(Corner::BottomRight) == 1);
        CHECK(orientationFromCorner(Corner::BottomLeft) == 2);
        CHECK(orientationFromCorner(Corner::TopRight) == 3);
        CHECK(orientationFromCorner(Corner::TopLeft) == 4);
        CHECK(orientationFromGd(true, false) == 1 && orientationFromGd(false, false) == 2);
        CHECK(orientationFromGd(false, true) == 3 && orientationFromGd(true, true) == 4);
        CHECK(sidewaysSlope(90.f) && sidewaysSlope(-270.f) && !sidewaysSlope(180.f) && !sidewaysSlope(0.f));
    }

    SECTION("group hash: order-independent, 0 without groups");
    {
        int16_t a[3] = {5, 1, 9};
        int16_t b[3] = {9, 5, 1};
        int16_t c[3] = {9, 5, 2};
        CHECK(groupsHash(a, 3) == groupsHash(b, 3));
        CHECK(groupsHash(a, 3) != groupsHash(c, 3));
        CHECK(groupsHash(a, 0) == 0u);
        CHECK(groupsHash(nullptr, 3) == 0u);
    }

    SECTION("builder: gameplay sorted by left edge, decoration counted per column, skipped things nowhere");
    {
        WorldBuilder b;
        CHECK(b.add(obj(0, 1, 300.f)) == Use::Gameplay);
        CHECK(b.add(obj(2, 8, 100.f)) == Use::Gameplay);
        CHECK(b.add(obj(7, 1, 45.f)) == Use::Decoration);
        CHECK(b.add(obj(7, 1, 50.f)) == Use::Decoration);
        CHECK(b.add(obj(7, 1, 95.f)) == Use::Decoration);
        CHECK(b.add(obj(20, 899, 10.f)) == Use::Skip);
        CHECK(b.add(obj(30, 1329, 10.f)) == Use::Skip);
        CHECK(b.add(obj(7, 31, 600.f)) == Use::StartPos);
        sim::StartState sp;
        sp.x = 600.f;
        sp.y = 105.f;
        b.addStartPos(sp);
        auto w = b.finish(params());
        CHECK(w.gdLevelId == 1234 && w.levelHash == "abc");
        CHECK(w.objects.size() == 2u);
        CHECK(w.gameplayObjects == 2);
        CHECK(w.objects[0].objectId == 8 && w.objects[1].objectId == 1);   // sorted by left()
        CHECK(w.objects[0].kind == sim::ObjKind::Hazard);
        CHECK(w.decorationObjects == 3 && w.decoration.objects == 3);
        CHECK(w.decoration.perColumn.size() == 4u);
        CHECK(w.decoration.perColumn[1] == 2 && w.decoration.perColumn[3] == 1);
        CHECK(w.decoration.gameplayPerColumn.size() == 11u);
        CHECK(w.startPositions.size() == 1u);
        CHECK_NEAR(w.startPositions[0].percent, 20.f, 1e-4);
        CHECK(w.unsupported.empty());
        CHECK(!w.tooLarge);
        CHECK_NEAR(w.groundY, 90.f, 1e-6);
        auto const& c = b.lastCounters();
        CHECK(c.seen == 8 && c.gameplay == 2 && c.decoration == 3 && c.rectsChecked == 2 && c.rectsUnchecked == 0 && c.rectsEstimateOff == 0);
        CHECK(c.harmlessTriggers == 1);
        CHECK(b.counters().seen == 0);   // the builder is empty after finish
    }

    SECTION("spans: Here = [x, max(next gamemode portal, x + 600)); dual until the solo portal");
    {
        WorldBuilder b;
        b.add(obj(5, 13, 1000.f));        // ship portal at 1000
        b.add(obj(14, 45, 200.f));        // mirror at 200 -> [185, 985) (600 beats nothing before the portal... 185+600 = 785 < 1000)
        b.add(obj(28, 747, 900.f));       // teleport at 900 -> [885, max(1485, 1000)) = [885, 1485)
        b.add(obj(23, 286, 1500.f));      // dual at 1500 -> until the solo portal at 2500
        b.add(obj(24, 287, 2500.f));
        b.add(obj(40, 1, 2900.f));        // special near the end -> [2885, 3485)
        auto w = b.finish(params());
        CHECK(hasSpan(w, "mirror_portal", 185.f, 1000.f));
        CHECK(hasSpan(w, "teleport_portal", 885.f, 1485.f));
        CHECK(hasSpan(w, "dual_portal", 1485.f, 2500.f));
        CHECK(hasSpan(w, "special_object", 2885.f, 3485.f));
        CHECK(spansOf(w, "solo_portal") == 0);
        for (size_t i = 1; i < w.unsupported.size(); ++i) CHECK(w.unsupported[i - 1].x0 <= w.unsupported[i].x0);
        // the solo portal is still a hashed gameplay object
        bool solo = false;
        for (auto const& o : w.objects) solo = solo || o.gdType == 24;
        CHECK(solo);
    }

    SECTION("spans: overlapping spans of one mechanic merge, different mechanics stay apart");
    {
        WorldBuilder b;
        b.add(obj(40, 1, 100.f));
        b.add(obj(40, 1, 300.f));
        b.add(obj(40, 1, 2000.f));
        b.add(obj(39, 1816, 350.f));
        auto w = b.finish(params());
        CHECK(spansOf(w, "special_object") == 2);
        CHECK(hasSpan(w, "special_object", 85.f, 885.f));
        CHECK(spansOf(w, "collision_block") == 1);
    }

    SECTION("move-like triggers: a span at every gameplay object of the target group, and at the trigger");
    {
        WorldBuilder b;
        auto mover = obj(20, 901, 50.f);
        mover.targetGroupId = 7;
        mover.isTrigger = true;
        b.add(mover);
        b.add(withGroups(obj(0, 1, 1200.f), {7, 3}));   // a moved block
        b.add(withGroups(obj(0, 1, 2000.f), {4}));      // another group: untouched
        auto decoMover = obj(20, 901, 2500.f);
        decoMover.targetGroupId = 9;                     // group 9 holds no gameplay object
        decoMover.isTrigger = true;
        b.add(decoMover);
        b.add(withGroups(obj(7, 1, 2600.f), {9}));      // decoration is not a gameplay group
        auto w = b.finish(params());
        CHECK(hasSpan(w, "moved_by_trigger", 1185.f, 1785.f));
        CHECK(hasSpan(w, "move_trigger", 50.f, 650.f));
        CHECK(spansOf(w, "move_trigger") == 1);          // the decorative mover makes no span
        CHECK(spansOf(w, "moved_by_trigger") == 1);
        // both triggers are gameplay objects for the hash
        int triggers = 0;
        for (auto const& o : w.objects) triggers += o.objectId == 901 ? 1 : 0;
        CHECK(triggers == 2);
        CHECK(b.lastCounters().gameplayTriggers == 2);
        auto toggle = obj(20, 1049, 10.f);
        toggle.targetGroupId = 3;
        b.add(toggle);
        b.add(withGroups(obj(12, 36, 700.f), {3}));     // a toggled pink orb
        auto w2 = b.finish(params());
        CHECK(hasSpan(w2, "toggled_by_trigger", 685.f, 1285.f));
        CHECK(hasSpan(w2, "toggle_trigger", 10.f, 610.f));
    }

    SECTION("slopes: orientation from rotation + flips; hazard / odd rotations are unsupported");
    {
        WorldBuilder b;
        auto s0 = obj(25, 289, 100.f);
        b.add(s0);
        auto s1 = obj(25, 289, 200.f);
        s1.flipX = true;
        b.add(s1);
        auto s2 = obj(25, 289, 300.f);
        s2.rotation = 30.f;
        b.add(s2);
        auto s3 = obj(25, 289, 400.f);
        s3.slopeHazard = true;
        b.add(s3);
        auto s4 = obj(25, 289, 500.f);           // flipped vertically: GD's rules give a ceiling slope "\"
        s4.flipY = true;
        s4.haveLive = true;                      // GD's live m_slopeUphill disagrees (a runtime rerun): only counted
        s4.live.flipY = true;
        s4.live.haveSlopeUphill = true;
        s4.live.slopeUphill = true;
        s4.rectExact = false;
        b.add(s4);
        auto s5 = obj(25, 289, 600.f);           // both flips: ceiling "/"
        s5.flipX = true;
        s5.flipY = true;
        b.add(s5);
        auto s6 = obj(25, 289, 700.f);
        s6.rotation = 90.f;
        b.add(s6);
        auto w = b.finish(params());
        CHECK(w.objects.size() == 7u);
        CHECK(w.objects[0].kind == sim::ObjKind::Slope && w.objects[0].slope == 1);
        CHECK(w.objects[1].kind == sim::ObjKind::Slope && w.objects[1].slope == 2);
        CHECK(w.objects[2].kind == sim::ObjKind::Unsupported && std::string(w.objects[2].unsupportedMechanic) == "rotated_slope");
        CHECK(w.objects[3].kind == sim::ObjKind::Unsupported && std::string(w.objects[3].unsupportedMechanic) == "hazard_slope");
        CHECK(w.objects[4].kind == sim::ObjKind::Slope && w.objects[4].slope == 3);
        CHECK(w.objects[5].slope == 4);
        CHECK(w.objects[6].kind == sim::ObjKind::Unsupported && std::string(w.objects[6].unsupportedMechanic) == "sideways_slope");
        CHECK(spansOf(w, "rotated_slope") == 1 && spansOf(w, "hazard_slope") == 1 && spansOf(w, "sideways_slope") == 1);
        CHECK(b.lastCounters().slopesFromGd == 4 && b.lastCounters().slopesDerivationOff == 0);
        CHECK(b.lastCounters().slopesLiveOff == 1);
    }

    SECTION("rects: GD's formula on the static fields always; GD's clean cache is only the formula's check");
    {
        WorldBuilder b;
        auto r = obj(0, 1, 100.f);
        r.rectExact = false;                    // GD has not computed this rect yet: nothing to compare
        r.rect = {};
        b.add(r);
        b.add(obj(0, 1, 300.f));                // GD's rect == the formula
        auto off = obj(2, 8, 500.f);
        off.rect = {497.f, 90.f, 6.f, 12.f};    // a cache that disagrees with the formula: counted, never used
        b.add(off);
        auto w = b.finish(params());
        auto const& c = b.lastCounters();
        CHECK(c.rectsUnchecked == 1 && c.rectsChecked == 2 && c.rectsEstimateOff == 1);
        CHECK(c.rectMismatches.size() == 1u && c.rectMismatches[0].find("#8 type 2") == 0);
        CHECK(w.objects[0].rx == 85.f && w.objects[0].rw == 30.f);
        CHECK(w.objects[2].rw == 30.f);          // the formula, whatever the cache says (hash-stable)
    }

    SECTION("the check uses GD's LIVE fields; the World keeps the STATIC ones (a scale trigger ran)");
    {
        WorldBuilder b;
        auto r = obj(2, 8, 500.f);              // static: scale 1 -> 30 x 30
        r.haveLive = true;
        r.live.scaleX = 2.f;                    // a scale trigger doubled it before the walk
        r.live.scaleY = 2.f;
        r.rect = {470.f, 75.f, 60.f, 60.f};     // GD's cache = the formula on the live scale
        b.add(r);
        auto ori = obj(2, 8, 700.f);            // GD's own oriented flag + cached box offset are used for the check
        ori.rotation = ori.rotationY = 45.f;
        ori.haveRotationY = true;
        ori.haveLive = true;
        ori.live.rotationX = ori.live.rotationY = 45.f;
        ori.live.oriented = true;
        RectInputs li = staticInputs(ori, 700.f, 105.f);
        auto lg = gdObjectRect(li, true, false, {});
        ori.rect = lg.rect;
        b.add(ori);
        auto w = b.finish(params());
        auto const& c = b.lastCounters();
        CHECK(c.rectsChecked == 2 && c.rectsEstimateOff == 0);
        CHECK(c.rectsOriented == 1 && c.orientedFlagOff == 0);
        CHECK(w.objects[0].rw == 30.f);          // static
        CHECK(w.objects[1].oriented && w.objects[1].obbW == 30.f);
        CHECK_NEAR(w.objects[1].obbAngle, -45.f * 0.0174532924f, 1e-7);
        CHECK(w.objects[1].obbCx == 700.f && w.objects[1].obbCy == 105.f);
    }

    SECTION("SimObject facts: radius, box-offset centre");
    {
        WorldBuilder b;
        auto orb = obj(11, 36, 300.f);
        orb.objectRadius = 12.f;
        orb.boxOffsetY = -5.f;
        orb.rect = {285.f, 85.f, 30.f, 30.f};
        b.add(orb);
        auto w = b.finish(params());
        CHECK(w.objects[0].radius == 12.f && !w.objects[0].oriented);
        CHECK(w.objects[0].obbCx == 300.f && w.objects[0].obbCy == 100.f);
        CHECK(w.objects[0].ry == 85.f);
        CHECK(b.lastCounters().rectsEstimateOff == 0);
    }

    SECTION("hidden = the static no-touch flag; runtime visibility is only counted (stable gameplay hash)");
    {
        WorldBuilder b;
        auto culled = obj(0, 1, 1000.f);
        culled.hiddenRuntime = true;     // off screen / toggled now: NOT a World property
        auto noTouch = obj(0, 1, 100.f);
        noTouch.noTouch = true;
        auto passable = obj(0, 1, 2000.f);
        passable.passable = true;
        b.add(culled);
        b.add(noTouch);
        b.add(passable);
        auto w = b.finish(params());
        CHECK(w.objects[0].hidden);      // x = 100
        CHECK(!w.objects[1].hidden);     // x = 1000
        CHECK(w.objects[2].kind == sim::ObjKind::Unsupported && std::string(w.objects[2].unsupportedMechanic) == "passable_block");
        CHECK(spansOf(w, "passable_block") == 1);
        CHECK(b.lastCounters().hidden == 1 && b.lastCounters().hiddenRuntime == 1);
    }

    SECTION("moved objects: the World keeps m_startPosition (stable hash); a field that disagrees everywhere is reverted");
    {
        WorldBuilder b;
        auto moved = obj(0, 1, 700.f, 200.f);   // a move trigger carried it from (400, 105)
        moved.haveStart = true;
        moved.startX = 400.f;
        moved.startY = 105.f;
        b.add(moved);
        for (int i = 0; i < 20; ++i) {
            auto still = obj(0, 1, 1000.f + 30.f * i);
            still.haveStart = true;
            still.startX = still.x;
            still.startY = still.y;
            b.add(still);
        }
        auto w = b.finish(params());
        CHECK(b.lastCounters().movedNow == 1);
        CHECK(!b.lastCounters().startPositionIgnored);
        CHECK_NEAR(w.objects[0].x, 400.f, 1e-4);
        CHECK_NEAR(w.objects[0].rx, 385.f, 1e-4);   // estimated at the start position
        CHECK_NEAR(w.objects[0].ry, 90.f, 1e-4);
        // a field that never matches (not the start position after all): everything reverted
        for (int i = 0; i < 20; ++i) {
            auto o = obj(0, 1, 100.f + 30.f * i);
            o.haveStart = true;
            o.startX = 5000.f;
            o.startY = -5000.f;
            b.add(o);
        }
        auto w2 = b.finish(params());
        CHECK(b.lastCounters().startPositionIgnored);
        CHECK_NEAR(w2.objects[0].x, 100.f, 1e-4);
        CHECK_NEAR(w2.objects[0].rx, 85.f, 1e-4);
    }

    SECTION("the object cap: the world keeps the first N by x and marks the rest too_large");
    {
        WorldBuilder b;
        for (int i = 9; i >= 0; --i) b.add(obj(0, 1, 100.f * static_cast<float>(i) + 15.f));
        auto p = params(1000.f);
        p.maxObjects = 6;
        auto w = b.finish(p);
        CHECK(w.tooLarge);
        CHECK(w.objects.size() == 6u);
        CHECK(w.gameplayObjects == 6);
        CHECK(hasSpan(w, "too_large", 600.f, 1000.f));
    }

    SECTION("the object cap applies WHILE collecting: nothing past the leftmost dropped object is kept");
    {
        ExtractConstants k;
        k.maxObjects = 6;
        WorldBuilder b(k);
        for (int i = 0; i < 10; ++i) b.add(obj(0, 1, 100.f * static_cast<float>(i) + 15.f));   // ascending: 600.. dropped
        CHECK(b.counters().gameplay == 6 && b.counters().capDropped == 4);
        auto w = b.finish(params(1000.f));
        CHECK(w.tooLarge && w.objects.size() == 6u);
        CHECK(hasSpan(w, "too_large", 600.f, 1000.f));
        WorldBuilder d(k);
        for (int i = 9; i >= 0; --i) d.add(obj(0, 1, 100.f * static_cast<float>(i) + 15.f));   // descending: the LEFT ones are dropped
        auto w2 = d.finish(params(1000.f));
        CHECK(w2.tooLarge && w2.objects.empty());                // an arbitrary subset is never kept
        CHECK(hasSpan(w2, "too_large", 0.f, 1000.f));
        CHECK(d.lastCounters().capDropped == 4);
    }

    SECTION("level end: endX defaults to the length");
    {
        WorldBuilder b;
        auto p = params(5000.f);
        p.endX = 0.f;
        auto w = b.finish(p);
        CHECK_NEAR(w.endX, 5000.f, 1e-4);
        CHECK(w.objects.empty() && w.unsupported.empty());
    }

    return test::finish("analyzer_extract_tests");
}
