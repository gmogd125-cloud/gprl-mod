// core/identity host tests (docs/LEVEL_FAMILY_DESIGN.md §9): a synthetic "Sonic Wave-like" level
// (cube intro, ship corridor, mirror portal, wave corridors with slanted spike / block walls, speed
// changes, a mini portal, a gravity portal, decoration on several z-layers, triggers) and nine
// derived worlds. Pins: gameplay fingerprint equality / inequality, section hashes, Jaccard
// estimates, presentation digests and readability ordering, the StartPos list, determinism and
// object-order independence, shift invariance of portal sections, the JSON key list; and the golden
// identities in tests/fixtures/identity/<case>.json + cases.json (the TypeScript matcher's input).
//
// Usage: level_identity_tests.exe <repo root> [--write]
//   --write   (re)generate every fixture (only when every other check passes). Run
//             `npx prettier --write tests/fixtures/identity` afterwards; the compare is structural
//             (canonical JSON), so the layout never matters.
#include "test_util.hpp"

#include "../core/identity/gameplay_fingerprint.hpp"
#include "../core/identity/identity.hpp"
#include "../core/identity/presentation.hpp"
#include "../core/identity/sections.hpp"
#include "../core/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::identity;
using sim::ObjKind;
using sim::OrbKind;
using sim::PadKind;

namespace {

std::string g_root;
bool g_write = false;

struct Level {
    World world;
    DecoSet deco;
    IdentityInput input;
};

/// A tiny level builder: GD-style centres (15 + 30k), rects derived from a width / height.
struct Builder {
    Level lv;
    int nextUnique = 1;

    SimObject& add(int objectId, uint8_t gdType, ObjKind kind, float cx, float cy, float w, float h) {
        SimObject o;
        o.objectId = objectId;
        o.uniqueId = nextUnique++;
        o.gdType = gdType;
        o.kind = kind;
        o.x = cx;
        o.y = cy;
        o.rx = cx - w / 2;
        o.ry = cy - h / 2;
        o.rw = w;
        o.rh = h;
        lv.world.objects.push_back(o);
        return lv.world.objects.back();
    }
    void block(float cx, float cy) { add(1, 0, ObjKind::Solid, cx, cy, 30, 30); }
    void spike(float cx, float cy, float rotation = 0.f) { add(8, 2, ObjKind::Hazard, cx, cy, 30, 30).rotation = rotation; }
    void smallSpike(float cx, float cy, float rotation = 0.f) { add(39, 2, ObjKind::Hazard, cx, cy, 30, 15).rotation = rotation; }
    void slope(float cx, float cy, uint8_t orientation) { add(289, 25, ObjKind::Slope, cx, cy, 30, 30).slope = orientation; }
    void orb(int id, OrbKind k, float cx, float cy) { add(id, 11, ObjKind::Orb, cx, cy, 30, 30).orb = k; }
    void pad(int id, PadKind k, float cx, float cy) { add(id, 8, ObjKind::Pad, cx, cy, 30, 10).pad = k; }
    void portal(int id, Gamemode mode, float cx, float cy) { add(id, 5, ObjKind::GamemodePortal, cx, cy, 25, 75).mode = mode; }
    void gravity(int id, bool flip, float cx, float cy) { add(id, flip ? 3 : 4, ObjKind::GravityPortal, cx, cy, 25, 75).flag = flip; }
    void sizePortal(int id, bool mini, float cx, float cy) { add(id, mini ? 18 : 17, ObjKind::SizePortal, cx, cy, 25, 75).flag = mini; }
    void speed(int id, Speed s, float cx, float cy) { add(id, 1, ObjKind::SpeedChange, cx, cy, 30, 30).speed = s; }
    void unsupported(int id, char const* mechanic, float cx, float cy) { add(id, 1, ObjKind::Unsupported, cx, cy, 25, 75).unsupportedMechanic = mechanic; }
    void startPos(float cx, float cy, Gamemode mode, Speed s, bool mini) {
        auto& o = add(31, 1, ObjKind::StartPos, cx, cy, 30, 30);
        o.mode = mode;
        o.speed = s;
        o.flag = mini;
        sim::StartState st;
        st.x = cx;
        st.y = cy;
        st.mode = mode;
        st.speed = s;
        st.mini = mini;
        lv.world.startPositions.push_back(st);
    }
    void deco(int id, float cx, float cy, float w, float h, int zLayer, int zOrder, bool noGlow, float opacity = 1.f, bool hazardLook = false) {
        DecoObject d;
        d.objectId = id;
        d.rx = cx - w / 2;
        d.ry = cy - h / 2;
        d.rw = w;
        d.rh = h;
        d.zLayer = zLayer;
        d.zOrder = zOrder;
        d.noGlow = noGlow;
        d.opacityHint = opacity;
        d.hazardLook = hazardLook;
        d.passable = true;
        lv.deco.deco.push_back(d);
    }
    void trigger(int id, TriggerObject::Kind kind, float x, float duration) {
        TriggerObject t;
        t.objectId = id;
        t.x = x;
        t.kind = kind;
        t.duration = duration;
        lv.deco.triggers.push_back(t);
    }
    /// A corridor of floor + ceiling blocks every 30 units from x0 to x1 (floor rising `slope` per
    /// column, `gap` between floor and ceiling) with inward spikes on every `spikeEvery`-th column.
    void corridor(float x0, float x1, float floorY, float slope, float gap, int spikeEvery) {
        int n = static_cast<int>((x1 - x0) / 30.f);
        for (int i = 0; i < n; ++i) {
            float x = x0 + 15.f + 30.f * i;
            float fy = floorY + slope * i;
            block(x, fy);
            block(x, fy + gap);
            if (spikeEvery > 0 && i % spikeEvery == 1) {
                spike(x, fy + 30.f);
                spike(x, fy + gap - 30.f, 180.f);
            }
        }
    }
    Level finish(float endX, std::string levelHash, int gdId, int copiedFrom, std::string name) {
        auto& w = lv.world;
        std::sort(w.objects.begin(), w.objects.end(), [](SimObject const& a, SimObject const& b) {
            if (a.left() != b.left()) return a.left() < b.left();
            if (a.ry != b.ry) return a.ry < b.ry;
            return a.objectId < b.objectId;
        });
        w.groundY = 0.f;
        w.lengthX = endX;
        w.endX = endX;
        w.gameplayObjects = static_cast<int>(w.objects.size());
        w.decorationObjects = static_cast<int>(lv.deco.deco.size());
        w.levelHash = levelHash;
        w.gdLevelId = gdId;
        lv.input.levelHash = levelHash;
        lv.input.gdLevelId = gdId;
        lv.input.copiedFromGdId = copiedFrom;
        lv.input.nameHint = std::move(name);
        return lv;
    }
};

// ---- the Sonic Wave-like gameplay (3000 units, 80 % at the x = 2400 speed change) ----

void gameplayBase(Builder& b, bool buffed) {
    // cube intro [0, 700): spikes, a block step, a yellow orb, a yellow pad, a slope, a triple spike
    b.spike(165, 15);
    b.pad(35, PadKind::Yellow, 225, 5);
    b.spike(315, 15);
    b.orb(36, OrbKind::Yellow, 345, 75);
    b.block(465, 15);
    b.block(495, 15);
    b.spike(495, 45);
    b.slope(525, 15, 1);
    b.block(555, 15);
    b.block(555, 45);
    b.spike(615, 15);
    b.spike(645, 15);
    b.spike(675, 15);
    // ship [700, 1300): floor + ceiling, spikes both sides, a flipped-gravity portal
    b.portal(12, Gamemode::Ship, 700, 105);
    for (float x = 715; x < 1300; x += 30) {
        b.block(x, 15);
        b.block(x, 255);
    }
    b.spike(835, 45);
    b.spike(985, 45);
    b.spike(1135, 45);
    b.spike(925, 225, 180);
    b.spike(1075, 225, 180);
    b.gravity(10, true, 1000, 135);
    // mirror portal (an Unsupported mechanic that cuts) and the rest of the ship [1300, 1500)
    b.unsupported(45, "mirror_portal", 1300, 135);
    for (float x = 1315; x < 1500; x += 30) {
        b.block(x, 15);
        b.block(x, 255);
    }
    b.spike(1405, 45);
    b.spike(1465, 225, 180);
    // wave [1500, 1800): rising corridor
    b.portal(660, Gamemode::Wave, 1500, 135);
    b.corridor(1500, 1800, 15, 12, 180, 3);
    // triple speed [1800, 2100): falling corridor
    b.speed(203, Speed::Triple, 1800, 150);
    b.corridor(1800, 2100, 135, -12, 180, 2);
    // mini [2100, 2400): flat narrow corridor with a normal-gravity portal
    b.sizePortal(101, true, 2100, 120);
    b.corridor(2100, 2400, 45, 0, 120, 2);
    b.gravity(11, false, 2250, 105);
    // quadruple speed [2400, 3000): rising then falling corridor, last objects at ~2900
    b.speed(1334, Speed::Quadruple, 2400, 150);
    b.corridor(2400, 2700, 15, 9, 150, 2);
    b.corridor(2700, 2900, 105, -9, 150, 3);
    if (buffed) {
        // extra small spikes in the middle of the last corridor: the buffed 80-100 %
        for (int i = 0; i < 8; ++i) {
            float x = 2445.f + 60.f * i;
            float floorY = x < 2700.f ? 15.f + 9.f * ((x - 2400.f) / 30.f) : 105.f - 9.f * ((x - 2700.f) / 30.f);
            b.smallSpike(x, floorY + 75.f);
        }
    }
}

void decoBase(Builder& b) {
    // background panels behind everything (B3)
    for (int k = 0; k < 8; ++k) b.deco(503, 200.f + 400.f * k, 160, 400, 320, -1, 1, true);
    // foreground pieces (T2, glow) overlapping gameplay in the ship, the first wave and the last section
    b.deco(1000, 835, 15, 60, 60, 7, 3, false);
    b.deco(1000, 1135, 15, 60, 60, 7, 3, false);
    b.deco(1000, 1620, 100, 90, 60, 7, 3, false);
    b.deco(1000, 2520, 80, 60, 60, 7, 3, false);
    // non-overlapping foreground pieces above the corridors, glow alternating
    for (int k = 0; k < 15; ++k) b.deco(1001, 100.f + 200.f * k, 330, 40, 40, 7, 1, k % 2 == 0);
    // low-opacity tints on the gameplay layer
    b.deco(1002, 1150, 135, 120, 120, 5, 1, true, 0.3f);
    b.deco(1002, 2250, 105, 120, 120, 5, 1, true, 0.3f);
    // fake spikes: a hazard sprite without collision in the wave and the last section
    b.deco(8, 1665, 150, 30, 30, 5, 2, true, 1.f, true);
    b.deco(8, 2565, 130, 30, 30, 5, 2, true, 1.f, true);
    // triggers
    b.trigger(899, TriggerObject::Color, 100, 0.5f);
    b.trigger(899, TriggerObject::Color, 1500, 0.1f);     // flash (section 4)
    b.trigger(1006, TriggerObject::Pulse, 1850, 0.2f);    // flash (section 5)
    b.trigger(1007, TriggerObject::Alpha, 2450, 1.0f);
    b.trigger(1520, TriggerObject::Shake, 2500, 0.5f);    // flash (section 7)
    b.trigger(1913, TriggerObject::CameraZoom, 1500, 0.5f);
    b.trigger(1916, TriggerObject::CameraOffset, 2400, 0.5f);
}

void decoCleaner(Builder& b) {
    for (int k = 0; k < 8; ++k) b.deco(503, 200.f + 400.f * k, 160, 400, 320, -1, 1, true);
    for (int k = 0; k < 15; ++k) b.deco(1001, 100.f + 200.f * k, 330, 40, 40, 7, 1, k % 2 == 0);
    b.deco(1002, 1150, 135, 120, 120, 5, 1, true, 0.3f);
    b.deco(1002, 2250, 105, 120, 120, 5, 1, true, 0.3f);
    b.trigger(899, TriggerObject::Color, 100, 0.5f);
    b.trigger(1007, TriggerObject::Alpha, 2450, 1.0f);
    b.trigger(1913, TriggerObject::CameraZoom, 1500, 0.5f);
    b.trigger(1916, TriggerObject::CameraOffset, 2400, 0.5f);
}

void decoMessier(Builder& b) {
    decoBase(b);
    // opaque foreground panels over whole corridor stretches (ship, falling wave, last section)
    b.deco(1000, 1000, 135, 300, 270, 7, 5, false);
    b.deco(1000, 1950, 130, 240, 200, 7, 5, false);
    b.deco(1000, 2800, 120, 200, 200, 7, 5, false);
    // more fake hazards
    b.deco(8, 1380, 120, 30, 30, 5, 2, true, 1.f, true);
    b.deco(39, 1950, 60, 30, 15, 5, 2, true, 1.f, true);
    b.deco(8, 2750, 110, 30, 30, 5, 2, true, 1.f, true);
    b.deco(88, 2850, 100, 40, 40, 5, 2, true, 1.f, false);
    // more flashes
    b.trigger(1006, TriggerObject::Pulse, 900, 0.1f);
    b.trigger(899, TriggerObject::Color, 1350, 0.2f);
    b.trigger(1006, TriggerObject::Pulse, 1950, 0.15f);
    b.trigger(1520, TriggerObject::Shake, 2700, 0.3f);
}

void decoNew(Builder& b) {
    for (int k = 0; k < 6; ++k) b.deco(1002, 250.f + 500.f * k, 160, 500, 320, 1, 2, false);
    b.deco(1001, 925, 225, 80, 50, 9, 2, false);
    b.deco(1001, 1740, 190, 70, 70, 9, 2, false, 0.4f);
    b.deco(1001, 2200, 60, 60, 60, 9, 2, false);
    for (int k = 0; k < 20; ++k) b.deco(1000, 60.f + 150.f * k, 290, 30, 30, 7, 2, false, k % 3 == 0 ? 0.4f : 1.f);
    b.deco(88, 1200, 135, 40, 40, 5, 2, false, 1.f, false);
    b.trigger(1006, TriggerObject::Pulse, 300, 0.25f);
    b.trigger(899, TriggerObject::Color, 700, 2.0f);
    b.trigger(1914, TriggerObject::CameraRotate, 1800, 1.0f);
    b.trigger(1520, TriggerObject::Shake, 2600, 0.4f);
}

void gameplayUnrelated(Builder& b) {
    b.spike(105, 15);
    b.spike(255, 15);
    b.block(345, 15);
    b.spike(345, 45);
    b.portal(660, Gamemode::Wave, 400, 135);
    b.corridor(400, 1000, 15, 6, 210, 2);
    b.speed(202, Speed::Double, 1000, 150);
    b.corridor(1000, 1600, 135, -6, 210, 3);
    b.sizePortal(101, true, 1600, 120);
    b.corridor(1600, 2400, 45, 4, 120, 2);
}

void decoUnrelated(Builder& b) {
    for (int k = 0; k < 5; ++k) b.deco(503, 250.f + 500.f * k, 160, 500, 320, -1, 1, true);
    b.deco(1000, 700, 120, 60, 60, 7, 3, false);
    b.deco(1000, 1900, 100, 60, 60, 7, 3, false);
    b.trigger(899, TriggerObject::Color, 400, 0.2f);
    b.trigger(1913, TriggerObject::CameraZoom, 1000, 1.0f);
}

Level makeBase(std::string const& hash = "b0000000000000000000000000000000000000000000000000000000000000ba", int gdId = 1001, int copiedFrom = 0,
               std::string name = "Sonic Wave-like (base)") {
    Builder b;
    gameplayBase(b, false);
    decoBase(b);
    return b.finish(3000, hash, gdId, copiedFrom, std::move(name));
}

struct Case {
    std::string id;
    std::string description;
    std::string expectedRelationship;
    std::string expectedConfidence;
    std::string note;
    Level level;
    LevelIdentity identity;
    json::Value json;
};

std::vector<Case> buildCases() {
    std::vector<Case> cases;
    auto push = [&cases](std::string id, std::string description, std::string rel, std::string conf, std::string note, Level lv) {
        Case c;
        c.id = std::move(id);
        c.description = std::move(description);
        c.expectedRelationship = std::move(rel);
        c.expectedConfidence = std::move(conf);
        c.note = std::move(note);
        c.level = std::move(lv);
        cases.push_back(std::move(c));
    };
    push("base", "the synthetic Sonic Wave-like level: cube intro, ship corridor, mirror portal, wave corridors, speed changes, mini portal, decoration + triggers",
         "founder", "confirmed", "the family founder every other case is matched against", makeBase());
    push("exact_copy", "byte-identical copy: same level hash, objects, decoration, name and ids", "exact_match", "confirmed", "",
         makeBase());
    push("renamed", "renamed re-upload: nameHint and levelHash differ, nothing else", "gameplay_identical_copy", "confirmed", "",
         makeBase("b2000000000000000000000000000000000000000000000000000000000000b2", 1001, 0, "Sonic Wave-like (renamed)"));
    {
        Builder b;
        gameplayBase(b, false);
        decoBase(b);
        b.startPos(1950, 135, Gamemode::Wave, Speed::Triple, false);
        push("startpos_65", "StartPos version: one StartPos object at 65 % (x 1950, wave, triple speed), everything else equal", "startpos_derivative",
             "confirmed", "FA-D8: a fingerprint-equal member that adds StartPos objects the candidate lacks; startposRange [65]",
             b.finish(3000, "b3000000000000000000000000000000000000000000000000000000000000b3", 1003, 1001, "Sonic Wave-like StartPos 65%"));
    }
    {
        Builder b;
        gameplayBase(b, false);
        decoNew(b);
        push("new_decoration", "entirely new decoration (other objects, layers, glow, opacity, triggers) over identical gameplay", "presentation_variant",
             "confirmed", "", b.finish(3000, "b4000000000000000000000000000000000000000000000000000000000000b4", 1004, 1001, "Sonic Wave-like redecorated"));
    }
    {
        Builder b;
        gameplayBase(b, false);
        decoCleaner(b);
        push("cleaner_decoration", "cleaner decoration: no foreground over gameplay, no fake hazards, no flash triggers", "presentation_variant", "confirmed",
             "lower readability penalty than the base in every changed section",
             b.finish(3000, "b5000000000000000000000000000000000000000000000000000000000000b5", 1005, 1001, "Sonic Wave-like clean"));
    }
    {
        Builder b;
        gameplayBase(b, false);
        decoMessier(b);
        push("messier_decoration", "messier decoration: opaque foreground panels over corridors, more fake hazards, more flashes", "presentation_variant",
             "confirmed", "higher readability penalty than the base in every changed section",
             b.finish(3000, "b6000000000000000000000000000000000000000000000000000000000000b6", 1006, 1001, "Sonic Wave-like messy"));
    }
    {
        Builder b;
        gameplayBase(b, true);
        decoBase(b);
        push("buffed_80_100", "buffed 80-100 %: eight extra small spikes narrow the last corridor (x 2445-2865); sections 0-6 equal, section 7 changed",
             "modified_version", "partial_derivative",
             "buffed_version when the matcher has hazard density / window evidence; G ~ 0.8 (section 7 = 600 of 3000 units unmatched)",
             b.finish(3000, "b7000000000000000000000000000000000000000000000000000000000000b7", 1007, 1001, "Sonic Wave-like buffed"));
    }
    push("metadata_only", "metadata-only re-upload: gdLevelId and copiedFromGdId differ (and the level hash), objects and decoration equal",
         "gameplay_identical_copy", "confirmed", "", makeBase("b8000000000000000000000000000000000000000000000000000000000000b8", 1008, 999, "Sonic Wave-like (base)"));
    {
        Builder b;
        gameplayUnrelated(b);
        decoUnrelated(b);
        push("unrelated_wave", "an unrelated wave level (2500 units, other corridors and portal positions)", "unrelated", "unrelated", "",
             b.finish(2500, "c9000000000000000000000000000000000000000000000000000000000000c9", 2001, 0, "Unrelated wave level"));
    }
    for (auto& c : cases) {
        c.identity = compute(c.level.world, c.level.deco, c.level.input);
        c.json = toJson(c.identity);
    }
    return cases;
}

Case const& find(std::vector<Case> const& cases, std::string const& id) {
    for (auto const& c : cases) {
        if (c.id == id) return c;
    }
    std::printf("  no case %s\n", id.c_str());
    return cases.front();
}

bool hex16Lower(std::string const& s) {
    if (s.size() != 16) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)) || std::isupper(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool sameSectionHashes(LevelIdentity const& a, LevelIdentity const& b) {
    if (a.sections.size() != b.sections.size()) return false;
    for (size_t i = 0; i < a.sections.size(); ++i) {
        if (a.sections[i].hash != b.sections[i].hash) return false;
    }
    return true;
}

bool samePresentation(PresentationVector const& a, PresentationVector const& b) {
    return a.decoDensity == b.decoDensity && a.obstruction == b.obstruction && a.clutter == b.clutter && a.fakeObjects == b.fakeObjects
        && a.hiddenGameplay == b.hiddenGameplay && a.flashes == b.flashes && a.cameraEffects == b.cameraEffects && a.glowShare == b.glowShare
        && a.opacityLow == b.opacityLow && a.previewSeconds == b.previewSeconds && a.indicators == b.indicators;
}

// ---- unit checks of the primitives ----

void testPrimitives() {
    SECTION("splitmix64, quantize, hex16, union area, sprite table, jaccard, similarity, readability");
    CHECK(splitmix64(0) == 0xe220a8397b1dcdafull);   // the reference first output of SplitMix64 seeded with 0
    CHECK(hex16(0) == "0000000000000000");
    CHECK(hex16(0xe220a8397b1dcdafull) == "e220a8397b1dcdaf");
    CHECK(quantize(1.25, 0.1) == 13);
    CHECK(quantize(-0.04, 0.1) == 0);
    CHECK(quantize(-1.26, 0.1) == -13);
    CHECK(quantize(std::nan(""), 0.1) == 0);
    CHECK_NEAR(unionArea({{0, 0, 10, 10}, {5, 5, 15, 15}}), 175.0, 1e-9);
    CHECK_NEAR(unionArea({{0, 0, 10, 10}, {0, 0, 10, 10}}), 100.0, 1e-9);
    CHECK_NEAR(unionArea({{0, 0, 10, 10}, {20, 0, 30, 10}}), 200.0, 1e-9);
    CHECK_NEAR(unionArea({}), 0.0, 1e-12);
    CHECK(collidableSpriteKind(8) == SpriteKind::Box);
    CHECK(collidableSpriteKind(1) == SpriteKind::Box);
    CHECK(collidableSpriteKind(36) == SpriteKind::Box);
    CHECK(collidableSpriteKind(660) == SpriteKind::Box);
    CHECK(collidableSpriteKind(289) == SpriteKind::Slope);
    CHECK(collidableSpriteKind(1717) == SpriteKind::Slope);
    CHECK(collidableSpriteKind(88) == SpriteKind::Circle);
    CHECK(collidableSpriteKind(503) == SpriteKind::None);
    CHECK(collidableSpriteKind(1000) == SpriteKind::None);
    CHECK(collidableSpriteKind(0) == SpriteKind::None);
    DecoObject fake;
    fake.objectId = 8;
    CHECK(isFakeObject(fake));
    fake.passable = false;
    CHECK(!isFakeObject(fake));
    DecoObject plain;
    plain.objectId = 1000;
    CHECK(!isFakeObject(plain));
    plain.hazardLook = true;
    CHECK(isFakeObject(plain));

    MinHash a{}, b{};
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<uint32_t>(i);
        b[i] = i < 16 ? static_cast<uint32_t>(i) : 1000u + static_cast<uint32_t>(i);
    }
    CHECK_NEAR(jaccardEstimate(a, a), 1.0, 1e-12);
    CHECK_NEAR(jaccardEstimate(a, b), 0.25, 1e-12);
    // minhash of a set is position-wise <= the minhash of any subset's superset... and the estimate
    // of a set against itself is exactly 1
    std::vector<uint64_t> set1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<uint64_t> set2 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    auto m1 = minhashOf(set1), m2 = minhashOf(set2);
    CHECK_NEAR(jaccardEstimate(m1, m1), 1.0, 1e-12);
    for (size_t i = 0; i < m1.size(); ++i) CHECK(m2[i] <= m1[i]);
    double j = jaccardEstimate(m1, m2);
    CHECK_MSG(j > 0.5 && j < 1.0, "jaccard of a 10/12 subset = " + std::to_string(j));
    MinHash empty = minhashOf({});
    for (uint32_t v : empty) CHECK(v == 0xffffffffu);
    // the sorted-tuple hash is order independent and the empty hash is the FNV basis
    std::vector<uint64_t> h1 = {5, 3, 9}, h2 = {9, 5, 3};
    CHECK(hashSortedTuples(h1) == hashSortedTuples(h2));
    std::vector<uint64_t> none;
    CHECK(hashSortedTuples(none) == 14695981039346656037ull);

    PresentationVector p0, p1;
    p0.previewSeconds = 3.0;
    p1.previewSeconds = 3.0;
    CHECK_NEAR(presentationSimilarity(p0, p1), 1.0, 1e-12);
    CHECK_NEAR(readability(p0), 0.0, 1e-12);
    p1.obstruction = 1.0;
    CHECK_NEAR(presentationSimilarity(p0, p1), 1.0 - 1.0 / 11.0, 1e-12);
    CHECK_NEAR(readability(p1), 0.3, 1e-12);
    PresentationVector p2;
    p2.previewSeconds = 0.0;
    CHECK_NEAR(readability(p2), 0.1, 1e-12);
    PresentationVector worst;
    worst.obstruction = 1;
    worst.flashes = 100;
    worst.fakeObjects = 100;
    worst.hiddenGameplay = 100;
    worst.cameraEffects = 100;
    worst.clutter = 1;
    worst.previewSeconds = 0;
    CHECK_NEAR(readability(worst), 1.0, 1e-12);
    // the tuple hash: x relative to the section start, 0.1-unit quantisation
    SimObject o;
    o.objectId = 8;
    o.rx = 100.f;
    o.ry = 15.f;
    o.rw = 30.f;
    o.rh = 30.f;
    SimObject shifted = o;
    shifted.rx = 400.f;
    CHECK(tupleHash(o, 0.0) == tupleHash(shifted, 300.0));
    CHECK(tupleHash(o, 0.0) != tupleHash(shifted, 0.0));
    SimObject nudged = o;
    nudged.rx = 100.04f;
    CHECK(tupleHash(o, 0.0) == tupleHash(nudged, 0.0));
    nudged.rx = 100.3f;
    CHECK(tupleHash(o, 0.0) != tupleHash(nudged, 0.0));
    SimObject startPos = o;
    startPos.kind = ObjKind::StartPos;
    startPos.objectId = 31;
    CHECK(!hashedForIdentity(startPos));
    CHECK(hashedForIdentity(o));
}

void testEmptyWorld() {
    SECTION("empty world: one section, basis hashes, no crash");
    World w;
    DecoSet d;
    IdentityInput in;
    auto id = compute(w, d, in);
    CHECK(id.sections.size() == 1);
    CHECK(id.gameplayFingerprint == "cbf29ce484222325");
    CHECK(id.sections[0].hash == "cbf29ce484222325");
    CHECK(id.sections[0].objects == 0);
    CHECK(id.startPos.empty());
    CHECK(id.gameplayObjects == 0);
    auto j = toJson(id);
    CHECK(json::canonical(j) == json::canonical(toJson(compute(w, d, in))));
}

void testBaseStructure(Case const& base) {
    SECTION("base: cut rule, state per section, fingerprint format, key list");
    auto const& id = base.identity;
    CHECK(hex16Lower(id.gameplayFingerprint));
    CHECK(hex16Lower(id.presentationFingerprint));
    CHECK(id.levelHash == base.level.input.levelHash);
    CHECK(id.gdLevelId == 1001);
    CHECK(id.copiedFromGdId == 0);
    CHECK(id.nameHint == "Sonic Wave-like (base)");
    CHECK_NEAR(id.lengthX, 3000.0, 1e-6);
    CHECK(id.gameplayObjects == static_cast<int>(base.level.world.objects.size()));
    CHECK(id.decorationObjects == static_cast<int>(base.level.deco.deco.size()));
    CHECK(id.startPos.empty());
    float const xFrom[] = {0, 350, 700, 1300, 1500, 1800, 2100, 2400};
    float const xTo[] = {350, 700, 1300, 1500, 1800, 2100, 2400, 3000};
    Gamemode const modes[] = {Gamemode::Cube, Gamemode::Cube, Gamemode::Ship, Gamemode::Ship, Gamemode::Wave, Gamemode::Wave, Gamemode::Wave, Gamemode::Wave};
    Speed const speeds[] = {Speed::Normal, Speed::Normal, Speed::Normal, Speed::Normal, Speed::Normal, Speed::Triple, Speed::Triple, Speed::Quadruple};
    bool const minis[] = {false, false, false, false, false, false, true, true};
    CHECK_MSG(id.sections.size() == 8, "sections = " + std::to_string(id.sections.size()));
    int total = 0;
    for (size_t i = 0; i < id.sections.size() && i < 8; ++i) {
        auto const& s = id.sections[i];
        CHECK(s.index == static_cast<int>(i));
        CHECK_NEAR(s.xFrom, xFrom[i], 1e-3);
        CHECK_NEAR(s.xTo, xTo[i], 1e-3);
        CHECK_NEAR(s.pctFrom, xFrom[i] / 3000.0 * 100.0, 1e-9);
        CHECK_NEAR(s.pctTo, xTo[i] / 3000.0 * 100.0, 1e-9);
        CHECK(s.gamemode == modes[i]);
        CHECK(s.speed == speeds[i]);
        CHECK(s.mini == minis[i]);
        CHECK(hex16Lower(s.hash));
        CHECK(s.objects > 0);
        CHECK(s.readability >= 0.0 && s.readability <= 1.0);
        total += s.objects;
        // every presentation value is finite and the shares are in range
        auto const& p = s.presentation;
        CHECK(p.obstruction >= 0.0 && p.obstruction <= 1.0);
        CHECK(p.clutter >= 0.0 && p.clutter <= 1.0);
        CHECK(p.glowShare >= 0.0 && p.glowShare <= 1.0);
        CHECK(p.opacityLow >= 0.0 && p.opacityLow <= 1.0);
        CHECK(p.previewSeconds >= 0.0 && p.previewSeconds <= 3.0);
        CHECK(p.decoDensity > 0.0);
    }
    CHECK(total == id.gameplayObjects);
    // the hashes of the eight sections are pairwise different
    std::set<std::string> hashes;
    for (auto const& s : id.sections) hashes.insert(s.hash);
    CHECK(hashes.size() == id.sections.size());
    // presentation facts of the base decoration
    CHECK(id.sections[2].presentation.obstruction > 0.0);    // foreground pieces over the ship floor
    CHECK(id.sections[0].presentation.obstruction == 0.0);   // nothing in front in the intro
    CHECK(id.sections[4].presentation.fakeObjects > 0.0);
    CHECK(id.sections[4].presentation.flashes > 0.0);
    CHECK(id.sections[4].presentation.cameraEffects > 0.0);
    CHECK(id.sections[1].presentation.flashes == 0.0);
    CHECK(id.sections[1].presentation.previewSeconds == 3.0);   // a length split: fully previewed
    CHECK(id.sections[4].presentation.previewSeconds < 3.0);    // the wave portal -> first spike
    CHECK(id.sections[2].presentation.glowShare > 0.0 && id.sections[2].presentation.glowShare < 1.0);
    CHECK(id.sections[2].presentation.opacityLow > 0.0);   // the 0.3-opacity tint at x 1150
    CHECK(id.sections[6].presentation.opacityLow > 0.0);   // and at x 2250
    CHECK(id.sections[3].presentation.opacityLow == 0.0);
    CHECK(id.sections[0].presentation.indicators == 0.0);
    CHECK(id.sections[0].presentation.clutter > 0.9);          // the background panels fill the screen

    // JSON: the exact key list the TypeScript side reads
    auto const& obj = base.json.asObject();
    char const* keys[] = {"gameplayFingerprintVersion", "sectionVersion", "presentationVersion", "levelHash", "gameplayFingerprint",
                          "presentationFingerprint", "gdLevelId", "copiedFromGdId", "nameHint", "sections", "startPos", "gameplayObjects",
                          "decorationObjects", "lengthX"};
    CHECK(obj.size() == 14);
    for (size_t i = 0; i < obj.size() && i < 14; ++i) CHECK_MSG(obj[i].first == keys[i], obj[i].first + " != " + keys[i]);
    // /2 since 2026-10-02: the extraction's inputs changed (GD's exact rect on static fields)
    CHECK(base.json.getString("gameplayFingerprintVersion") == "gprl-gameplay-fp/2");
    CHECK(base.json.getString("sectionVersion") == "gprl-section-fp/2");
    CHECK(base.json.getString("presentationVersion") == "gprl-presentation-fp/2");
    auto const& s0 = base.json["sections"][0];
    char const* skeys[] = {"index", "xFrom", "xTo", "pctFrom", "pctTo", "hash", "minhash", "gamemode", "speed", "mini", "objects", "presentation", "readability"};
    CHECK(s0.asObject().size() == 13);
    for (size_t i = 0; i < s0.asObject().size() && i < 13; ++i) CHECK_MSG(s0.asObject()[i].first == skeys[i], s0.asObject()[i].first + " != " + skeys[i]);
    CHECK(s0["minhash"].asArray().size() == 64);
    CHECK(s0["gamemode"].asString() == "cube");
    CHECK(base.json["sections"][4]["gamemode"].asString() == "wave");
    CHECK(base.json["sections"][7]["speed"].asInt() == 4);
    char const* pkeys[] = {"decoDensity", "obstruction", "clutter", "fakeObjects", "hiddenGameplay", "flashes", "cameraEffects", "glowShare", "opacityLow",
                           "previewSeconds", "indicators"};
    auto const& p0 = s0["presentation"].asObject();
    CHECK(p0.size() == 11);
    for (size_t i = 0; i < p0.size() && i < 11; ++i) CHECK_MSG(p0[i].first == pkeys[i], p0[i].first + " != " + pkeys[i]);
}

void testRelations(std::vector<Case> const& cases) {
    SECTION("gameplay fingerprints and section hashes across the nine variants");
    auto const& base = find(cases, "base").identity;
    for (char const* same : {"exact_copy", "renamed", "startpos_65", "new_decoration", "cleaner_decoration", "messier_decoration", "metadata_only"}) {
        auto const& c = find(cases, same);
        CHECK_MSG(c.identity.gameplayFingerprint == base.gameplayFingerprint, std::string(same) + " gameplay fingerprint");
        CHECK_MSG(sameSectionHashes(c.identity, base), std::string(same) + " section hashes");
        CHECK_MSG(c.identity.gameplayObjects == base.gameplayObjects, std::string(same) + " gameplay object count");
        for (size_t i = 0; i < c.identity.sections.size() && i < base.sections.size(); ++i) {
            CHECK(c.identity.sections[i].minhash == base.sections[i].minhash);
            CHECK_NEAR(jaccardEstimate(c.identity.sections[i].minhash, base.sections[i].minhash), 1.0, 1e-12);
        }
    }
    for (char const* diff : {"buffed_80_100", "unrelated_wave"}) {
        auto const& c = find(cases, diff);
        CHECK_MSG(c.identity.gameplayFingerprint != base.gameplayFingerprint, std::string(diff) + " gameplay fingerprint differs");
    }
    // the exact copy and the renamed / metadata copies are identical except for the inputs
    auto const& exact = find(cases, "exact_copy");
    CHECK(json::canonical(exact.json) == json::canonical(find(cases, "base").json));
    auto const& renamed = find(cases, "renamed").identity;
    CHECK(renamed.nameHint != base.nameHint && renamed.presentationFingerprint == base.presentationFingerprint);
    auto const& meta = find(cases, "metadata_only").identity;
    CHECK(meta.gdLevelId == 1008 && meta.copiedFromGdId == 999 && meta.presentationFingerprint == base.presentationFingerprint);

    // buffed 80-100: sections before 80 % equal, the last changed, Jaccard in (0.3, 0.95)
    auto const& buffed = find(cases, "buffed_80_100").identity;
    CHECK(buffed.sections.size() == base.sections.size());
    for (size_t i = 0; i + 1 < buffed.sections.size() && i + 1 < base.sections.size(); ++i) {
        CHECK_MSG(buffed.sections[i].hash == base.sections[i].hash, "buffed section " + std::to_string(i) + " equal");
        CHECK_NEAR(jaccardEstimate(buffed.sections[i].minhash, base.sections[i].minhash), 1.0, 1e-12);
    }
    if (!buffed.sections.empty() && buffed.sections.size() == base.sections.size()) {
        auto const& b7 = buffed.sections.back();
        auto const& s7 = base.sections.back();
        CHECK(b7.xFrom == 2400.f && s7.xFrom == 2400.f);
        CHECK(b7.hash != s7.hash);
        CHECK(b7.objects == s7.objects + 8);
        double j = jaccardEstimate(b7.minhash, s7.minhash);
        CHECK_MSG(j >= 0.3 && j <= 0.95, "buffed section 7 jaccard = " + std::to_string(j));
        // true Jaccard = |base| / (|base| + 8)
        double trueJ = static_cast<double>(s7.objects) / static_cast<double>(s7.objects + 8);
        CHECK_MSG(std::fabs(j - trueJ) < 0.2, "estimate " + std::to_string(j) + " vs true " + std::to_string(trueJ));
    }

    // unrelated: < 10 % of the section length shares a hash with the base
    auto const& unrelated = find(cases, "unrelated_wave").identity;
    CHECK(unrelated.sections.size() >= 4);
    std::set<std::string> baseHashes;
    for (auto const& s : base.sections) baseHashes.insert(s.hash);
    double shared = 0.0, total = 0.0;
    for (auto const& s : unrelated.sections) {
        double len = static_cast<double>(s.xTo - s.xFrom);
        total += len;
        if (baseHashes.count(s.hash)) shared += len;
    }
    CHECK_MSG(total > 0.0 && shared / total < 0.1, "unrelated shared length share = " + std::to_string(total > 0.0 ? shared / total : -1.0));
    // and no unrelated section looks similar by MinHash either
    for (auto const& s : unrelated.sections) {
        for (auto const& t : base.sections) {
            double j = jaccardEstimate(s.minhash, t.minhash);
            CHECK_MSG(j < 0.3, "unrelated vs base jaccard = " + std::to_string(j));
        }
    }
}

void testPresentation(std::vector<Case> const& cases) {
    SECTION("presentation digests, readability ordering, similarity");
    auto const& base = find(cases, "base").identity;
    for (char const* v : {"new_decoration", "cleaner_decoration", "messier_decoration"}) {
        auto const& c = find(cases, v).identity;
        CHECK_MSG(c.presentationFingerprint != base.presentationFingerprint, std::string(v) + " presentation digest differs");
    }
    for (char const* v : {"exact_copy", "renamed", "startpos_65", "metadata_only"}) {
        auto const& c = find(cases, v).identity;
        CHECK_MSG(c.presentationFingerprint == base.presentationFingerprint, std::string(v) + " presentation digest equal");
    }
    auto const& cleaner = find(cases, "cleaner_decoration").identity;
    auto const& messier = find(cases, "messier_decoration").identity;
    CHECK(cleaner.sections.size() == base.sections.size() && messier.sections.size() == base.sections.size());
    int changedClean = 0, changedMessy = 0;
    double meanBase = 0.0, meanClean = 0.0, meanMessy = 0.0;
    for (size_t i = 0; i < base.sections.size() && i < cleaner.sections.size() && i < messier.sections.size(); ++i) {
        auto const& b = base.sections[i];
        auto const& c = cleaner.sections[i];
        auto const& m = messier.sections[i];
        meanBase += b.readability;
        meanClean += c.readability;
        meanMessy += m.readability;
        if (!samePresentation(c.presentation, b.presentation)) {
            ++changedClean;
            CHECK_MSG(c.readability < b.readability, "cleaner section " + std::to_string(i) + ": " + std::to_string(c.readability) + " < " + std::to_string(b.readability));
        } else {
            CHECK(c.readability == b.readability);
        }
        if (!samePresentation(m.presentation, b.presentation)) {
            ++changedMessy;
            CHECK_MSG(m.readability > b.readability, "messier section " + std::to_string(i) + ": " + std::to_string(m.readability) + " > " + std::to_string(b.readability));
        } else {
            CHECK(m.readability == b.readability);
        }
        // similarity: identical vectors = 1; the variants stay well above an unrelated-looking 0
        CHECK_NEAR(presentationSimilarity(b.presentation, b.presentation), 1.0, 1e-12);
        double sc = presentationSimilarity(b.presentation, c.presentation), sm = presentationSimilarity(b.presentation, m.presentation);
        CHECK(sc > 0.5 && sc <= 1.0);
        CHECK(sm > 0.5 && sm <= 1.0);
        if (!samePresentation(c.presentation, b.presentation)) CHECK(sc < 1.0);
        if (!samePresentation(m.presentation, b.presentation)) CHECK(sm < 1.0);
    }
    CHECK_MSG(changedClean >= 3, "cleaner changed sections = " + std::to_string(changedClean));
    CHECK_MSG(changedMessy >= 3, "messier changed sections = " + std::to_string(changedMessy));
    CHECK(meanClean < meanBase && meanBase < meanMessy);
    // the messier level hides gameplay behind opaque panels
    CHECK(messier.sections[2].presentation.hiddenGameplay > base.sections[2].presentation.hiddenGameplay);
    CHECK(messier.sections[2].presentation.obstruction > base.sections[2].presentation.obstruction);
    CHECK(cleaner.sections[2].presentation.obstruction == 0.0);
    CHECK(cleaner.sections[4].presentation.fakeObjects == 0.0 && cleaner.sections[4].presentation.flashes == 0.0);
    // the buffed level keeps the base decoration: its presentation vector only moves where the new
    // spikes change obstruction / preview, never in the untouched sections
    auto const& buffed = find(cases, "buffed_80_100").identity;
    for (size_t i = 0; i + 1 < buffed.sections.size() && i + 1 < base.sections.size(); ++i) CHECK(samePresentation(buffed.sections[i].presentation, base.sections[i].presentation));
}

void testStartPos(std::vector<Case> const& cases) {
    SECTION("StartPos list");
    auto const& sp = find(cases, "startpos_65").identity;
    CHECK(sp.startPos.size() == 1);
    if (!sp.startPos.empty()) {
        CHECK_NEAR(sp.startPos[0].percent, 65.0, 1e-9);
        CHECK_NEAR(sp.startPos[0].x, 1950.0, 1e-6);
        CHECK(sp.startPos[0].mode == Gamemode::Wave);
        CHECK(sp.startPos[0].speed == Speed::Triple);
        CHECK(!sp.startPos[0].mini);
    }
    auto const& j = find(cases, "startpos_65").json;
    CHECK(j["startPos"].asArray().size() == 1);
    CHECK(j["startPos"][0]["mode"].asString() == "wave");
    CHECK(j["startPos"][0]["speed"].asInt() == 3);
    CHECK_NEAR(j["startPos"][0]["percent"].asNumber(), 65.0, 1e-9);
    char const* keys[] = {"x", "percent", "mode", "mini", "speed", "upsideDown", "dual"};
    auto const& o = j["startPos"][0].asObject();
    CHECK(o.size() == 7);
    for (size_t i = 0; i < o.size() && i < 7; ++i) CHECK(o[i].first == keys[i]);
    // the StartPos object (id 31) is in the world's object list and is not hashed
    int startPosObjects = 0;
    for (auto const& ob : find(cases, "startpos_65").level.world.objects) startPosObjects += ob.kind == ObjKind::StartPos ? 1 : 0;
    CHECK(startPosObjects == 1);
    CHECK(sp.gameplayObjects == find(cases, "base").identity.gameplayObjects);
    // a world whose extraction listed no StartState falls back to the id-31 objects
    Level lv = find(cases, "startpos_65").level;
    lv.world.startPositions.clear();
    auto fallback = compute(lv.world, lv.deco, lv.input);
    CHECK(fallback.startPos.size() == 1);
    if (!fallback.startPos.empty()) {
        CHECK_NEAR(fallback.startPos[0].percent, 65.0, 1e-9);
        CHECK(fallback.startPos[0].mode == Gamemode::Wave);
    }
}

void testDeterminism(std::vector<Case> const& cases) {
    SECTION("determinism, object-order independence, shift invariance of portal sections");
    auto const& base = find(cases, "base");
    auto again = compute(base.level.world, base.level.deco, base.level.input);
    CHECK(json::canonical(toJson(again)) == json::canonical(base.json));
    std::mt19937 rng(42);
    for (int round = 0; round < 5; ++round) {
        Level lv = base.level;
        std::shuffle(lv.world.objects.begin(), lv.world.objects.end(), rng);
        std::shuffle(lv.deco.deco.begin(), lv.deco.deco.end(), rng);
        std::shuffle(lv.deco.triggers.begin(), lv.deco.triggers.end(), rng);
        std::shuffle(lv.world.startPositions.begin(), lv.world.startPositions.end(), rng);
        auto shuffled = compute(lv.world, lv.deco, lv.input);
        CHECK_MSG(json::canonical(toJson(shuffled)) == json::canonical(base.json), "shuffle round " + std::to_string(round));
    }
    // a global shift: the intro pieces move, every section that starts at a portal keeps its hash
    Level shifted = base.level;
    for (auto& o : shifted.world.objects) {
        o.x += 300.f;
        o.rx += 300.f;
    }
    for (auto& d : shifted.deco.deco) d.rx += 300.f;
    for (auto& t : shifted.deco.triggers) t.x += 300.f;
    shifted.world.endX += 300.f;
    shifted.world.lengthX += 300.f;
    auto sh = compute(shifted.world, shifted.deco, shifted.input);
    CHECK(sh.gameplayFingerprint != base.identity.gameplayFingerprint);
    CHECK(sh.sections.size() == base.identity.sections.size());
    for (size_t i = 2; i < sh.sections.size() && i < base.identity.sections.size(); ++i) {
        CHECK_MSG(sh.sections[i].hash == base.identity.sections[i].hash, "shifted section " + std::to_string(i));
        CHECK_NEAR(sh.sections[i].xFrom, base.identity.sections[i].xFrom + 300.f, 1e-3);
        CHECK(sh.sections[i].gamemode == base.identity.sections[i].gamemode);
    }
    if (sh.sections.size() >= 2) {
        CHECK_NEAR(sh.sections[0].xTo, 500.0, 1e-3);   // [0, 1000) split in two equal pieces
        CHECK(sh.sections[0].hash != base.identity.sections[0].hash);
    }
    // a shift smaller than the quantum does nothing anywhere
    Level nudged = base.level;
    for (auto& o : nudged.world.objects) o.rx += 0.02f;
    auto nd = compute(nudged.world, nudged.deco, nudged.input);
    CHECK(nd.gameplayFingerprint == base.identity.gameplayFingerprint);
    CHECK(sameSectionHashes(nd, base.identity));
    // a hidden flag, a rotation, a scale or a group change all change the fingerprint
    for (int what = 0; what < 4; ++what) {
        Level lv = base.level;
        auto& o = lv.world.objects[10];
        if (what == 0) o.hidden = true;
        if (what == 1) o.rotation += 90.f;
        if (what == 2) o.scaleX = 1.5f;
        if (what == 3) o.groupsHash = 12345u;
        auto v = compute(lv.world, lv.deco, lv.input);
        CHECK_MSG(v.gameplayFingerprint != base.identity.gameplayFingerprint, "change " + std::to_string(what));
    }
}

void testCutRuleEdges() {
    SECTION("cut rule edges: two portals at one x, a portal right after another, a long span, max length");
    Builder b;
    b.spike(105, 15);
    b.portal(12, Gamemode::Ship, 300, 105);
    b.speed(202, Speed::Double, 300, 150);        // same x: one cut, both apply
    b.block(400, 15);
    b.sizePortal(101, true, 340, 120);            // 40 units after: a forced short section
    b.block(500, 15);
    b.spike(1700, 15);
    b.block(1750, 15);
    Level lv = b.finish(1900, "e1", 1, 0, "edges");
    auto cuts = cutSections(lv.world);
    // bounds: 0 | 300 | 340 | 1900 -> [0,300) [300,340) and [340,1900) = 1560 -> 3 x 520
    CHECK_MSG(cuts.size() == 5, "cuts = " + std::to_string(cuts.size()));
    if (cuts.size() == 5) {
        CHECK_NEAR(cuts[0].xTo, 300.0, 1e-3);
        CHECK_NEAR(cuts[1].xFrom, 300.0, 1e-3);
        CHECK_NEAR(cuts[1].xTo, 340.0, 1e-3);
        CHECK(cuts[1].gamemode == Gamemode::Ship && cuts[1].speed == Speed::Double && !cuts[1].mini);
        CHECK(cuts[2].mini && cuts[2].gamemode == Gamemode::Ship);
        CHECK(cuts[2].forced && !cuts[3].forced && !cuts[4].forced);
        CHECK_NEAR(cuts[2].xTo, 860.0, 1e-3);
        CHECK_NEAR(cuts[3].xTo, 1380.0, 1e-3);
        CHECK_NEAR(cuts[4].xTo, 1900.0, 1e-3);
        for (auto const& c : cuts) CHECK(c.xTo - c.xFrom <= 600.f + 1e-3f);
    }
    CHECK(sectionIndexForX(cuts, -5.0) == 0);
    CHECK(sectionIndexForX(cuts, 300.0) == 1);
    CHECK(sectionIndexForX(cuts, 339.99) == 1);
    CHECK(sectionIndexForX(cuts, 340.0) == 2);
    CHECK(sectionIndexForX(cuts, 5000.0) == 4);
    // a cut object at or past the level end never cuts
    Builder b2;
    b2.spike(105, 15);
    b2.portal(12, Gamemode::Ship, 500, 105);
    Level lv2 = b2.finish(500, "e2", 1, 0, "end");
    CHECK(cutSections(lv2.world).size() == 1);
    // a 600-unit span is a single section, 601 splits in two
    Builder b3;
    b3.spike(105, 15);
    Level lv3 = b3.finish(600, "e3", 1, 0, "six");
    CHECK(cutSections(lv3.world).size() == 1);
    Builder b4;
    b4.spike(105, 15);
    Level lv4 = b4.finish(601, "e4", 1, 0, "sixone");
    CHECK(cutSections(lv4.world).size() == 2);
}

// ---- goldens ----

std::string fixtureDir() { return g_root + "/tests/fixtures/identity/"; }

json::Value fixtureOf(Case const& c) {
    json::Value f = json::Value::object();
    f.set("$comment", "DEV FIXTURE: synthetic Sonic Wave-like level written by geode/tests/level_identity_tests.cpp (run_tests.ps1 -Write); generated data, not gameplay");
    f.set("kind", "gprl.level-identity/1");
    f.set("case", c.id);
    f.set("description", c.description);
    f.set("identity", c.json);
    return f;
}

json::Value casesIndex(std::vector<Case> const& cases) {
    json::Value f = json::Value::object();
    f.set("$comment", "DEV FIXTURE: index of the level-identity golden cases (geode/tests/level_identity_tests.cpp -Write); expectedRelationship / expectedConfidence = what shared/src/level-family/match.ts should decide for the case against `base`");
    f.set("kind", "gprl.level-identity/cases");
    json::Value versions = json::Value::object();
    versions.set("gameplayFingerprint", kGameplayFingerprintVersion);
    versions.set("section", kSectionVersion);
    versions.set("presentation", kPresentationVersion);
    f.set("versions", std::move(versions));
    f.set("fixtureLayout", "each <id>.json = { $comment, kind, case, description, identity: LevelIdentity } with identity = identity::toJson");
    f.set("base", "base");
    f.set("config", configJson());
    json::Value arr = json::Value::array();
    for (auto const& c : cases) {
        json::Value e = json::Value::object();
        e.set("id", c.id);
        e.set("file", c.id + ".json");
        e.set("description", c.description);
        e.set("vs", c.id == "base" ? json::Value(nullptr) : json::Value("base"));
        e.set("expectedRelationship", c.expectedRelationship);
        e.set("expectedConfidence", c.expectedConfidence);
        if (!c.note.empty()) e.set("note", c.note);
        if (c.id == "startpos_65") {
            json::Value r = json::Value::array();
            r.push(65.0);
            e.set("startposRange", std::move(r));
        }
        if (c.id == "buffed_80_100") {
            json::Value changed = json::Value::array();
            changed.push(7);
            e.set("changedSections", std::move(changed));
            e.set("subKind", "buffed_version");
        }
        arr.push(std::move(e));
    }
    f.set("cases", std::move(arr));
    return f;
}

bool writeFile(std::string const& path, std::string const& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

void compareFixture(std::string const& path, json::Value const& want, std::string const& what) {
    std::string text = gprl::test::readFile(path);
    if (text.empty()) {
        CHECK_MSG(false, what + ": fixture missing at " + path + " (generate with tests/run_tests.ps1 -Write -Only level_identity_tests)");
        return;
    }
    json::Value stored;
    json::ParseError err;
    if (!json::parse(text, stored, &err)) {
        CHECK_MSG(false, what + ": fixture does not parse: " + err.message);
        return;
    }
    CHECK_MSG(json::canonical(stored) == json::canonical(want),
              what + " golden differs from the fixture (regenerate with tests/run_tests.ps1 -Write -Only level_identity_tests only if the identity change is intended)");
}

void testGoldens(std::vector<Case> const& cases) {
    SECTION(g_write ? "goldens: writing tests/fixtures/identity" : "goldens: comparing tests/fixtures/identity");
    if (!g_write) {
        for (auto const& c : cases) compareFixture(fixtureDir() + c.id + ".json", fixtureOf(c), c.id);
        compareFixture(fixtureDir() + "cases.json", casesIndex(cases), "cases.json");
        return;
    }
    if (gprl::test::g_failures) {
        std::printf("  --write: NOT written, %d check(s) failed\n", gprl::test::g_failures);
        CHECK_MSG(false, "--write refused: fix the failures first");
        return;
    }
    int written = 0;
    for (auto const& c : cases) {
        std::string path = fixtureDir() + c.id + ".json";
        bool ok = writeFile(path, json::stringifyPretty(fixtureOf(c)) + "\n");
        CHECK_MSG(ok, "--write failed: " + path);
        if (ok) ++written;
    }
    bool ok = writeFile(fixtureDir() + "cases.json", json::stringifyPretty(casesIndex(cases)) + "\n");
    CHECK_MSG(ok, "--write failed: cases.json");
    std::printf("  --write: %d case fixture(s) + cases.json written to %s\n", written, fixtureDir().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--write") g_write = true;
        else if (g_root.empty()) g_root = a;
        else {
            std::printf("usage: level_identity_tests <repo root> [--write] (unknown argument '%s')\n", argv[i]);
            return 2;
        }
    }
    if (g_root.empty()) {
        std::printf("usage: level_identity_tests <repo root> [--write]\n");
        return 2;
    }
    testPrimitives();
    testEmptyWorld();
    testCutRuleEdges();
    auto cases = buildCases();
    testBaseStructure(find(cases, "base"));
    testRelations(cases);
    testPresentation(cases);
    testStartPos(cases);
    testDeterminism(cases);
    testGoldens(cases);
    return gprl::test::finish("level_identity_tests");
}
