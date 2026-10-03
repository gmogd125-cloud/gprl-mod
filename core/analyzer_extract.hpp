#pragma once
// Background level analyzer: the PURE half of the world extraction (docs/BACKGROUND_ANALYZER_
// DESIGN.md §3, §4.1, §4.7). PURE C++20, no Geode / cocos / GD include; host-tested in
// tests/analyzer_extract_tests.cpp.
//
// READ-ONLY RULE (AN-D1 / AN-D11): src/analyzer/Extract.cpp walks PlayLayer::m_objects in time
// slices and only READS binding members into `ObjectReading` (plain numbers). Everything that
// decides what an object IS for the simulator lives here: the GameObjectType / object-id
// classification, the slope orientation, GD's own collision-rect formula (replicated from the
// 2.2081 disassembly, never called), the §4.7 unsupported spans, the decoration summary and the
// object cap.
//
// gprl-extract/2 (2026-10-02 review): every World / hash input is a STATIC field of the level
// (m_startPosition, m_startRotationX / Y, m_startScaleX / Y, m_startFlipX / Y, the object's
// definition sizes), never a CCNode getter (getScaleX / getRotation / getOpacity carry trigger
// offsets, fades and pulses) and never GD's rect cache (filled lazily: present or not depending on
// when the object was first collided). The rect is GD's exact getObjectRect formula on those static
// fields (gdObjectRect below); GD's clean cache is only a CHECK of the formula with GD's live
// fields (BuildCounters::rectsChecked / rectsEstimateOff, logged "estimate check 0 of N").
//
// Classification order (classify()): StartPos id 31 -> speed object ids 200/201/202/203/1334 ->
// the gameplay-trigger id table (move / rotate / toggle / spawn / ...) -> GameObjectType 0..47.
// Every other trigger (colour, pulse, alpha, camera, song, sfx, shader, ...) is harmless for the
// physics and is skipped (it is neither simulated nor hashed: a colour edit keeps the gameplay
// hash, AN-D7).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sim/world.hpp"

namespace gprl::analyzer::extract {

// /2 (2026-10-02): GD's exact rect formula on static fields, the anti-cheat spike skipped, static
// slope facts, one start-state rule, the object cap applied while collecting
constexpr char const* kVersion = "gprl-extract/2";

/// GD 2.2081 facts the extraction relies on (one place).
struct ExtractConstants {
    float groundY = 90.f;          // the floor's top in level units: a first-row block is centred at y = 105
                                   // and player 1 spawns at y = 105 standing on it (cube, 30 units tall)
    float defaultStartX = 0.f;     // the World's level-start x - ONE rule for every visit (StartPos or not;
                                   // y: the simulator's floorCentreY on groundY, 105 for a normal cube). The
                                   // mod logs player 1's live spawn when it differs (in-game check only).
    float spanMinLength = 600.f;   // §4.7: an unsupported span lasts at least this far
    float columnWidth = 30.f;      // DecorationSummary column (one block)
    int maxColumns = 65536;
    int maxObjects = 400000;       // AN-D12 memory cap per world (gameplay objects), applied WHILE collecting
    int maxDecoObjects = 400000;   // the same cap for the identity's decoration set (Extract.cpp readPassive)
    float slopeRotationTolerance = 1.f;   // degrees from a multiple of 90
    float rectCheckTolerance = 0.05f;     // the formula vs GD's clean cache (units)
};
constexpr ExtractConstants kConstants{};

enum class Use : uint8_t {
    Gameplay = 0,    // goes into World::objects (simulated and hashed)
    Decoration,      // type 7: counted in the DecorationSummary only
    StartPos,        // id 31: World::startPositions
    Skip,            // collectibles, harmless triggers: nothing
};

/// How an object turns into an UnsupportedSpan (§4.7).
enum class SpanRule : uint8_t {
    None = 0,            // no span by itself (logic triggers act through other triggers; solo portals)
    Here,                // [x, max(next gamemode portal x, x + 600))
    UntilSolo,           // dual portal: [x, next solo portal x) or to the level end
    IfTargetsGameplay,   // move-like trigger: a Here span at the trigger only when its target group holds gameplay objects
};

struct Classified {
    Use use = Use::Skip;
    sim::ObjKind kind = sim::ObjKind::Unsupported;
    sim::OrbKind orb = sim::OrbKind::Yellow;
    sim::PadKind pad = sim::PadKind::Yellow;
    sim::Gamemode mode = sim::Gamemode::Cube;
    sim::Speed speed = sim::Speed::Normal;
    bool flag = false;                 // gravity portal: flipped; size portal: mini
    char const* mechanic = nullptr;    // ObjKind::Unsupported: the §4.7 name
    SpanRule span = SpanRule::None;
    char const* targetMechanic = nullptr;   // gameplay objects in the trigger's target group get a Here span with this name
    bool trigger = false;              // an EffectGameObject trigger (by id)
};

/// A gameplay-affecting trigger (GD 2.2 object ids; the ones marked "to verify" come from the
/// community object list and were not checked against the 2.2081 binary).
struct TriggerInfo {
    int id;
    char const* mechanic;
    SpanRule span;
    char const* targetMechanic;
};

inline constexpr TriggerInfo kGameplayTriggers[] = {
    // objects move / turn / scale / follow: the trigger and every gameplay object of its target group
    {901, "move_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {1346, "rotate_trigger", SpanRule::IfTargetsGameplay, "rotated_by_trigger"},
    {1347, "follow_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {1814, "follow_player_y_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {2067, "scale_trigger", SpanRule::IfTargetsGameplay, "scaled_by_trigger"},
    {1585, "animate_trigger", SpanRule::IfTargetsGameplay, "animated_by_trigger"},
    {3016, "advanced_follow_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {3660, "edit_advanced_follow_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {3661, "retarget_advanced_follow_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {3006, "area_move_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},
    {3007, "area_rotate_trigger", SpanRule::IfTargetsGameplay, "rotated_by_trigger"},
    {3008, "area_scale_trigger", SpanRule::IfTargetsGameplay, "scaled_by_trigger"},
    {3033, "keyframe_animation_trigger", SpanRule::IfTargetsGameplay, "moved_by_trigger"},   // to verify
    // objects appear / disappear
    {1049, "toggle_trigger", SpanRule::IfTargetsGameplay, "toggled_by_trigger"},
    {1595, "touch_trigger", SpanRule::IfTargetsGameplay, "toggled_by_trigger"},
    {3643, "toggle_block", SpanRule::Here, "toggled_by_trigger"},
    // logic: acts only through the triggers it activates (those carry their own spans)
    {1268, "spawn_trigger", SpanRule::None, nullptr},
    {1815, "collision_trigger", SpanRule::None, nullptr},
    {3609, "instant_collision_trigger", SpanRule::None, nullptr},
    {1611, "count_trigger", SpanRule::None, nullptr},
    {1811, "instant_count_trigger", SpanRule::None, nullptr},
    {1817, "pickup_trigger", SpanRule::None, nullptr},
    {1912, "random_trigger", SpanRule::None, nullptr},
    {2068, "advanced_random_trigger", SpanRule::None, nullptr},
    {3607, "sequence_trigger", SpanRule::None, nullptr},
    {1812, "on_death_trigger", SpanRule::None, nullptr},
    {1616, "stop_trigger", SpanRule::None, nullptr},
    {3618, "reset_trigger", SpanRule::None, nullptr},
    {3619, "item_edit_trigger", SpanRule::None, nullptr},
    {3620, "item_compare_trigger", SpanRule::None, nullptr},
    {3614, "timer_trigger", SpanRule::None, nullptr},
    {3615, "time_event_trigger", SpanRule::None, nullptr},
    {3617, "time_control_trigger", SpanRule::None, nullptr},
    {3604, "event_trigger", SpanRule::None, nullptr},
    // the player itself changes (gravity, controls, time, direction, the end)
    {1917, "reverse_trigger", SpanRule::Here, nullptr},
    {1932, "player_control_trigger", SpanRule::Here, nullptr},
    {2066, "gravity_trigger", SpanRule::Here, nullptr},
    {1935, "time_warp_trigger", SpanRule::Here, nullptr},
    {2899, "options_trigger", SpanRule::Here, nullptr},
    {3600, "end_trigger", SpanRule::Here, nullptr},
    {2900, "rotate_gameplay_trigger", SpanRule::Here, nullptr},   // to verify
    {3022, "teleport_trigger", SpanRule::Here, nullptr},          // to verify
};

inline TriggerInfo const* gameplayTrigger(int objectId) {
    for (auto const& t : kGameplayTriggers) {
        if (t.id == objectId) return &t;
    }
    return nullptr;
}

/// Speed object ids (not collisions in GD: applied by x, GD_PHYSICS_NOTES "Speed portals").
inline bool speedFromObjectId(int objectId, sim::Speed& out) {
    switch (objectId) {
        case 200: out = sim::Speed::Half; return true;
        case 201: out = sim::Speed::Normal; return true;
        case 202: out = sim::Speed::Double; return true;
        case 203: out = sim::Speed::Triple; return true;
        case 1334: out = sim::Speed::Quadruple; return true;
    }
    return false;
}

/// Geode's `Speed` enum (LevelSettingsObject::m_startSpeed): Normal 0, Slow 1, Fast 2, Faster 3, Fastest 4.
inline sim::Speed speedFromGeodeSpeed(int geodeSpeed) {
    switch (geodeSpeed) {
        case 1: return sim::Speed::Half;
        case 2: return sim::Speed::Double;
        case 3: return sim::Speed::Triple;
        case 4: return sim::Speed::Quadruple;
        default: return sim::Speed::Normal;
    }
}

/// LevelSettingsObject::m_startMode: 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing.
inline sim::Gamemode gamemodeFromStartMode(int startMode) {
    if (startMode < 0 || startMode > 7) return sim::Gamemode::Cube;
    return static_cast<sim::Gamemode>(startMode);
}

inline Classified unsupported(char const* mechanic, SpanRule span) {
    Classified c;
    c.use = Use::Gameplay;
    c.kind = sim::ObjKind::Unsupported;
    c.mechanic = mechanic;
    c.span = span;
    return c;
}

/// What an object is for the simulator. `isTrigger` = GameObject::m_isTrigger (an unknown trigger
/// id is harmless and skipped; its id is logged by the caller).
inline Classified classify(int gdType, int objectId, bool isTrigger = false) {
    Classified c;
    if (objectId == 31) {
        c.use = Use::StartPos;
        c.kind = sim::ObjKind::StartPos;
        return c;
    }
    sim::Speed speed;
    if (speedFromObjectId(objectId, speed)) {
        c.use = Use::Gameplay;
        c.kind = sim::ObjKind::SpeedChange;
        c.speed = speed;
        return c;
    }
    if (auto t = gameplayTrigger(objectId)) {
        c = unsupported(t->mechanic, t->span);
        c.targetMechanic = t->targetMechanic;
        c.trigger = true;
        return c;
    }
    auto gameplay = [&](sim::ObjKind k) {
        c.use = Use::Gameplay;
        c.kind = k;
        return c;
    };
    auto orb = [&](sim::OrbKind k) {
        c.orb = k;
        return gameplay(sim::ObjKind::Orb);
    };
    auto pad = [&](sim::PadKind k) {
        c.pad = k;
        return gameplay(sim::ObjKind::Pad);
    };
    auto portal = [&](sim::Gamemode m) {
        c.mode = m;
        return gameplay(sim::ObjKind::GamemodePortal);
    };
    switch (gdType) {
        case 0: case 21: return gameplay(sim::ObjKind::Solid);           // Solid, Breakable
        case 2: case 47: return gameplay(sim::ObjKind::Hazard);          // Hazard, AnimatedHazard (static rect)
        case 25: return gameplay(sim::ObjKind::Slope);
        case 3: c.flag = true; return gameplay(sim::ObjKind::GravityPortal);    // InverseGravityPortal
        case 4: c.flag = false; return gameplay(sim::ObjKind::GravityPortal);   // NormalGravityPortal
        case 42: return unsupported("gravity_toggle_portal", SpanRule::Here);
        case 5: return portal(sim::Gamemode::Ship);
        case 6: return portal(sim::Gamemode::Cube);
        case 16: return portal(sim::Gamemode::Ball);
        case 19: return portal(sim::Gamemode::Ufo);
        case 26: return portal(sim::Gamemode::Wave);
        case 27: return portal(sim::Gamemode::Robot);
        case 33: return portal(sim::Gamemode::Spider);
        case 41: return portal(sim::Gamemode::Swing);
        case 17: c.flag = false; return gameplay(sim::ObjKind::SizePortal);     // RegularSizePortal
        case 18: c.flag = true; return gameplay(sim::ObjKind::SizePortal);      // MiniSizePortal
        case 11: return orb(sim::OrbKind::Yellow);
        case 12: return orb(sim::OrbKind::Pink);
        case 35: return orb(sim::OrbKind::Red);
        case 13: return orb(sim::OrbKind::Gravity);
        case 29: return orb(sim::OrbKind::Green);
        case 32: return orb(sim::OrbKind::Black);    // DropRing
        case 43: return orb(sim::OrbKind::Spider);
        case 36: c.orb = sim::OrbKind::Custom; return unsupported("custom_orb", SpanRule::Here);
        case 37: c.orb = sim::OrbKind::Dash; return unsupported("dash_orb", SpanRule::Here);
        case 38: c.orb = sim::OrbKind::GravityDash; return unsupported("gravity_dash_orb", SpanRule::Here);
        case 46: c.orb = sim::OrbKind::Teleport; return unsupported("teleport_orb", SpanRule::Here);
        case 8: return pad(sim::PadKind::Yellow);
        case 9: return pad(sim::PadKind::Pink);
        case 34: return pad(sim::PadKind::Red);
        case 10: return pad(sim::PadKind::Gravity);
        case 44: return pad(sim::PadKind::Spider);
        case 14: case 15: return unsupported("mirror_portal", SpanRule::Here);
        case 23: return unsupported("dual_portal", SpanRule::UntilSolo);
        case 24: return unsupported("solo_portal", SpanRule::None);   // ends a dual span; hashed, no span of its own
        case 28: return unsupported("teleport_portal", SpanRule::Here);
        case 39: return unsupported("collision_block", SpanRule::Here);
        case 40: return unsupported("special_object", SpanRule::Here);
        case 7:
            c.use = Use::Decoration;
            c.kind = sim::ObjKind::Decoration;
            return c;
        case 22: case 30: case 31:   // SecretCoin, Collectible, UserCoin: not gameplay
            c.use = Use::Skip;
            return c;
        case 20: case 45:            // Modifier / EnterEffectObject: triggers not in the gameplay table are harmless
            c.use = Use::Skip;
            c.trigger = true;
            return c;
        default:
            break;
    }
    if (isTrigger) {
        c.use = Use::Skip;
        c.trigger = true;
        return c;
    }
    return unsupported("unknown_object_type", SpanRule::Here);
}

// ---- geometry ----

struct Rect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;   // origin (bottom-left) + size
};

struct Point {
    float x = 0.f, y = 0.f;
};

// ---- GD 2.2081's collision rect, replicated from the disassembly (never called: getObjectRect /
// getObjectRect2 / getOrientedBox / getBoxOffset write GD's caches) ----
//
//   getObjectRect()            0x1976a0  -> vtable +0x498 getObjectRect2(m_spriteWidthScale +0x394,
//                                           m_spriteHeightScale +0x398)
//   getObjectRect2(w, h)       0x197850  m_isObjectRectDirty (+0x368) clear: return m_objectRect
//                                        (+0x358); else clear the flag and: m_shouldUseOuterOb
//                                        (+0x2e8) ? (m_isOrientedBoxDirty +0x369 ? updateOrientedBox)
//                                        m_orientedBox->getBoundingRect() : vtable +0x488
//                                        getObjectRect(w, h); the result is stored in m_objectRect
//   getObjectRect(w, h)        0x1976c0  CCSize((m_isMirroredByScale +0x36d ? |m_scaleX| : m_scaleX)
//                                        (+0x488) * m_width (+0x2fc), (.. m_scaleY +0x48c) * m_height
//                                        (+0x300)) * (w, h); swapped when m_isRotationAligned (+0x390);
//                                        centre = getRealPosition() + getBoxOffset(); CCRect(centre -
//                                        size * 0.5, size)  (0.5 = the float at rva 0x622b08)
//   getRealPosition()          0x197b20  CCPoint((float)m_positionX (+0x3b8), (float)m_positionY (+0x3c0))
//   getBoxOffset()             0x1a17d0  m_customBoxOffset (+0x2cc) equals CCPointZero: that zero point;
//                                        else (when its dirty flag +0x2d4 is set) parent->convertToNodeSpace
//                                        (convertToWorldSpace(offset)) - the same of (0, 0), i.e. the offset
//                                        through the node's own scale / rotation, cached in +0x2d8
//   updateIsOriented()         0x1a1730  (PlayLayer::addObject, every object at load) m_shouldUseOuterOb =
//                                        type not 7, (type 0 / 21 / 25 only when m_isNoTouch +0x4c6),
//                                        (int)getRotation() % 90 != 0, and !(m_objectRadius (+0x38c) > 0)
//   updateOrientedBox()        0x1a1570  OBB2D(centre, (m_spriteWidthScale * m_width) * m_scaleX,
//                                        (m_spriteHeightScale * m_height) * m_scaleY, -getObjectRotation()
//                                        * 0.0174533) - no fabs here; getObjectRotation (0x1a14f0) =
//                                        m_startRotationX (+0x3d4) + m_rotationXOffset (+0x2a8)
//   OBB2D::calculateWithCenter 0x6da80   X = (cosf(a), sinf(a)) * (w * 0.5), Y = (-sinf(a), cosf(a)) *
//                                        (h * 0.5); corners (c - X) - Y, (c + X) - Y, (c + X) + Y, (c - X) + Y
//   OBB2D::getBoundingRect     0x6e270   min / max over the 4 corners starting from 0 with 0 as the
//                                        "unset" sentinel of the minimum (GD's quirk, kept)
//   GameObject::setRotation    0x197e00  m_isRotationAligned = rotation is exactly 90, -90, 270 or -270
//   (and setRotationX 0x197f50)          (floats at rva 0x623294 / 0x6239c0 / 0x623424 / 0x623ad0)
//
// Every product / sum below is done in float in GD's order (MSVC x64: SSE scalar, no contraction),
// so the static rect equals GD's for the same inputs; sinf / cosf are the UCRT's like GD's imports.

/// cvttss2si: float -> int truncating toward zero, out of range / NaN -> INT_MIN ("integer indefinite").
inline int gdTruncate(float v) {
    if (!(v > -2147483648.f && v < 2147483648.f)) return static_cast<int>(0x80000000u);
    return static_cast<int>(v);
}

/// GameObject::setRotation / setRotationX: the rect's width and height are swapped for these.
inline bool gdRotationAligned(float rotationX) { return rotationX == 90.f || rotationX == -90.f || rotationX == 270.f || rotationX == -270.f; }

/// GameObject::updateIsOriented (0x1a1730): GD tests this object with its oriented box (and its
/// collision rect is the OBB's bounding rect). `nodeRotation` = CCNode::getRotation() (vtable +0x158)
/// = the static m_startRotationX at load.
inline bool gdIsOriented(int gdType, bool noTouch, float nodeRotation, float objectRadius) {
    if (gdType == 7) return false;
    if ((gdType == 0 || gdType == 21 || gdType == 25) && !noTouch) return false;
    int r = gdTruncate(nodeRotation);
    if (r % 90 == 0) return false;
    if (!(0.f >= objectRadius)) return false;   // comiss 0, radius; jb: radius > 0 (or NaN) = never oriented
    return true;
}

/// The inputs of GD's rect functions for ONE state of an object (the World uses the level's STATIC
/// values; the check uses GD's live ones). Extract.cpp names the GameObject field of each.
struct RectInputs {
    float x = 0.f, y = 0.f;                           // getRealPosition (live) / m_startPosition (static)
    float width = 0.f, height = 0.f;                  // m_width / m_height
    float spriteWidthScale = 1.f, spriteHeightScale = 1.f;   // m_spriteWidthScale / m_spriteHeightScale
    float scaleX = 1.f, scaleY = 1.f;                 // m_scaleX / m_scaleY (live) / m_startScaleX / Y (static)
    float rotationX = 0.f, rotationY = 0.f;           // getObjectRotation / node rotation Y (live) / m_startRotationX / Y (static)
    bool flipX = false, flipY = false;                // m_isFlipX / Y (live) / m_startFlipX / Y (static): the node's scale sign
    bool mirroredByScale = false;                     // m_isMirroredByScale
    float customBoxOffsetX = 0.f, customBoxOffsetY = 0.f;   // m_customBoxOffset
    float objectRadius = 0.f;                         // m_objectRadius
    int gdType = 0;                                   // m_objectType
    bool noTouch = false;                             // m_isNoTouch
};

/// getBoxOffset (0x1a17d0) without the node: CCPoint::equals(CCPointZero) (|d| < FLT_EPSILON) gives
/// (0, 0); otherwise the offset through the linear part of cocos2d-x 2.2's nodeToParentTransform
/// with the node's scale (GameObject::setRScaleX 0x198510: scale = (flip ? -1 : 1) * m_scaleX, so a
/// flipped object mirrors its offset) and rotation (CC_DEGREES_TO_RADIANS, clockwise):
/// a = cos(ry) sX, b = sin(ry) sX, c = -sin(rx) sY, d = cos(rx) sY with rx/ry = -rotation X/Y.
/// GD goes through the world transform and back, so the last bits may differ (BuildCounters counts
/// the cached offsets it could compare).
inline Point gdBoxOffset(RectInputs const& in) {
    constexpr float kEps = 1.192092896e-07f;   // FLT_EPSILON
    if (std::fabs(in.customBoxOffsetX) < kEps && std::fabs(in.customBoxOffsetY) < kEps) return {};
    float sX = (in.flipX ? -1.f : 1.f) * in.scaleX;
    float sY = (in.flipY ? -1.f : 1.f) * in.scaleY;
    float cx = 1.f, sx = 0.f, cy = 1.f, sy = 0.f;
    if (in.rotationX != 0.f || in.rotationY != 0.f) {
        float radX = -(in.rotationX * 0.01745329252f);
        float radY = -(in.rotationY * 0.01745329252f);
        cx = ::cosf(radX);
        sx = ::sinf(radX);
        cy = ::cosf(radY);
        sy = ::sinf(radY);
    }
    float a = cy * sX, b = sy * sX, c = -sx * sY, d = cx * sY;
    double ox = in.customBoxOffsetX, oy = in.customBoxOffsetY;
    Point p;
    p.x = static_cast<float>(static_cast<double>(a) * ox + static_cast<double>(c) * oy);
    p.y = static_cast<float>(static_cast<double>(b) * ox + static_cast<double>(d) * oy);
    return p;
}

/// One object's rect as GD computes it, with the facts the simulator may use.
struct GdRect {
    Rect rect;                 // getObjectRect(): what GD collides the player with (or the OBB's bounding rect)
    bool oriented = false;     // m_shouldUseOuterOb
    bool aligned = false;      // m_isRotationAligned
    Point centre;              // getRealPosition() + getBoxOffset()
    Point boxOffset;
    float obbW = 0.f, obbH = 0.f;   // updateOrientedBox's size (oriented only)
    float obbAngle = 0.f;           // radians (oriented only)
};

/// getObjectRect(w, h) (0x1976c0) with w / h = m_spriteWidthScale / m_spriteHeightScale.
inline Rect gdAxisRect(RectInputs const& in, bool aligned, Point centre) {
    float sx = in.mirroredByScale ? std::fabs(in.scaleX) : in.scaleX;
    float sy = in.mirroredByScale ? std::fabs(in.scaleY) : in.scaleY;
    float w = sx * in.width;
    float h = sy * in.height;
    w = w * in.spriteWidthScale;
    h = h * in.spriteHeightScale;
    if (aligned) std::swap(w, h);
    Rect r;
    r.x = centre.x - w * 0.5f;
    r.y = centre.y - h * 0.5f;
    r.w = w;
    r.h = h;
    return r;
}

/// updateOrientedBox (0x1a1570) + OBB2D::calculateWithCenter (0x6da80) + OBB2D::getBoundingRect
/// (0x6e270), GD's sentinel quirk included.
inline Rect gdOrientedRect(RectInputs const& in, Point centre, float* obbW = nullptr, float* obbH = nullptr, float* obbAngle = nullptr) {
    float w = (in.spriteWidthScale * in.width) * in.scaleX;
    float h = (in.spriteHeightScale * in.height) * in.scaleY;
    float angle = (-in.rotationX) * 0.0174532924f;   // float(0.0174533), rva 0x6229bc
    float cs = ::cosf(angle), sn = ::sinf(angle);
    float hw = w * 0.5f, hh = h * 0.5f;
    Point X{cs * hw, sn * hw};
    Point Y{(-sn) * hh, cs * hh};
    Point corners[4] = {
        {(centre.x - X.x) - Y.x, (centre.y - X.y) - Y.y},
        {(centre.x + X.x) - Y.x, (centre.y + X.y) - Y.y},
        {(centre.x + X.x) + Y.x, (centre.y + X.y) + Y.y},
        {(centre.x - X.x) + Y.x, (centre.y - X.y) + Y.y},
    };
    float minX = 0.f, maxX = 0.f, minY = 0.f, maxY = 0.f;
    for (auto const& p : corners) {
        maxX = p.x > maxX ? p.x : maxX;                     // maxss
        if (minX > p.x || minX == 0.f) minX = p.x;          // comiss ja / ucomiss == 0 sentinel
        maxY = p.y > maxY ? p.y : maxY;
        if (minY > p.y || minY == 0.f) minY = p.y;
    }
    if (obbW) *obbW = w;
    if (obbH) *obbH = h;
    if (obbAngle) *obbAngle = angle;
    return {minX, minY, maxX - minX, maxY - minY};
}

/// The whole rule with GD's own flags given (the check against the cache passes GD's live
/// m_shouldUseOuterOb / m_isRotationAligned / cached box offset).
inline GdRect gdObjectRect(RectInputs const& in, bool oriented, bool aligned, Point boxOffset) {
    GdRect g;
    g.oriented = oriented;
    g.aligned = aligned;
    g.boxOffset = boxOffset;
    g.centre = {in.x + boxOffset.x, in.y + boxOffset.y};   // CCPoint::operator+
    if (oriented) g.rect = gdOrientedRect(in, g.centre, &g.obbW, &g.obbH, &g.obbAngle);
    else g.rect = gdAxisRect(in, aligned, g.centre);
    return g;
}

/// The whole rule with the flags derived from the inputs as GD derives them at load.
inline GdRect gdObjectRect(RectInputs const& in) {
    return gdObjectRect(in, gdIsOriented(in.gdType, in.noTouch, in.rotationX, in.objectRadius), gdRotationAligned(in.rotationX), gdBoxOffset(in));
}

/// GameObject::isFacingDown (0x1a1910): from getObjectRotation() (truncated) and isFlipY().
inline bool gdFacingDown(float objectRotation, bool flipY) {
    int rot = gdTruncate(objectRotation);
    if (rot % 90 != 0) {
        uint32_t u = static_cast<uint32_t>(rot);
        bool down = (u - 91u) <= 178u || (u + 269u) <= 178u;   // [91, 269] or [-269, -91]
        return flipY ? !down : down;
    }
    int a = rot < 0 ? -rot : rot;
    return flipY ? a != 180 : a == 180;
}

/// GameObject::determineSlopeDirection (0x19c2c0), the m_slopeUphill (+0x440) half, from
/// isFlipX() / isFlipY() / getObjectRotation(); `direction` gets m_slopeDirection (+0x444) or -1
/// where GD leaves it unchanged. The branch structure is the disassembly's, label by label.
inline bool gdSlopeUphill(float objectRotation, bool flipX, bool flipY, int* direction = nullptr) {
    int rot = gdTruncate(objectRotation);
    float r = static_cast<float>(rot % 360);
    bool const al = std::fabs(r) == 180.f;
    bool const dl = r == 90.f || r == -270.f;
    bool const cl = r == -90.f || r == 270.f;
    bool const sil = flipX, dil = flipY;
    int dir = -1;
    bool uphill = false;
    if (sil) goto L3d5;
    if (dil) goto L394;
    if (r == 0.f) goto L3e2;
    goto L3c2;
L394:
    if (r == 0.f) goto L4ba;
    if (al) goto L4db;
    if (!sil) goto L3bd;
    if (!dil) goto L3b8;
L3b1:
    if (r == 0.f) goto L3c6;
L3b8:
    if (sil) goto L405;
L3bd:
    if (dil) goto L3f1;
L3c2:
    if (!al) goto L3f1;
L3c6:
    dir = 3;
    goto L4ff;
L3d5:
    if (!dil) goto L4b6;
    if (!al) goto L3b1;
L3e2:
    dir = 0;
    goto L4ff;
L3f1:
    if (sil) goto L405;
    if (dil) goto L48d;
    if (dl) goto L40e;
    goto L432;
L405:
    if (!dil) goto L428;
    if (!cl) goto L428;
L40e:
    dir = 4;
    goto L506;
L428:
    if (sil) goto L43b;
    if (dil) goto L48d;
L432:
    if (cl) goto L444;
    if (!sil) goto L488;
L43b:
    if (!dil) goto L45e;
    if (!dl) goto L45e;
L444:
    dir = 5;
    goto L506;
L45e:
    if (!sil) goto L488;
    if (dil) goto L506;
L46c:
    if (dl) goto L4f5;
L474:
    if (!cl) goto L506;
L47c:
    dir = 7;
    goto L4ff;
L488:
    if (!dil) goto L491;
L48d:
    if (cl) goto L4f5;
L491:
    if (sil) goto L4af;
    if (!dil) goto L506;
    if (dl) goto L47c;
    goto L506;
L4af:
    if (dil) goto L506;
    goto L474;
L4b6:
    if (!al) goto L4d4;
L4ba:
    dir = 1;
    goto L506;
L4d4:
    if (r == 0.f) goto L4db;
    goto L46c;
L4db:
    dir = 2;
    goto L506;
L4f5:
    dir = 6;
L4ff:
    uphill = true;
L506:
    if (direction) *direction = dir;
    return uphill;
}

/// GD's anti-cheat spike (GJBaseGameLayer::m_anticheatSpike, a type-2 hazard PlayLayer moves onto
/// player 1 every step) is never a level object: the same object, or one with its unique id.
inline bool isAnticheatSpike(void const* object, int uniqueId, void const* spike, int spikeUniqueId) {
    if (!object || !spike) return false;
    return object == spike || uniqueId == spikeUniqueId;
}

/// A rect with a negative size (GD keeps CCRect's sign: a negative scale without m_isMirroredByScale)
/// as origin + |size| for the simulator. Returns true when it had to be normalised.
inline bool normalizeRect(Rect& r) {
    bool neg = false;
    if (r.w < 0.f) {
        r.x += r.w;
        r.w = -r.w;
        neg = true;
    }
    if (r.h < 0.f) {
        r.y += r.h;
        r.h = -r.h;
        neg = true;
    }
    return neg;
}

/// The corner of a slope's rect that holds the right angle (the solid corner), from rotation +
/// flips. GD's unrotated, unflipped slope rises to the right ("/") with its right angle at the
/// bottom-right. `supported` = false for rotations that are not a multiple of 90 degrees.
enum class Corner : uint8_t { BottomLeft = 1, BottomRight = 2, TopLeft = 3, TopRight = 4 };

inline Corner slopeCorner(float rotationDeg, bool flipX, bool flipY, bool& supported, float tolerance = kConstants.slopeRotationTolerance) {
    double q = static_cast<double>(rotationDeg) / 90.0;
    double r = std::round(q);
    supported = std::fabs(q - r) * 90.0 <= tolerance;
    int quarters = static_cast<int>(std::fmod(std::fmod(r, 4.0) + 4.0, 4.0));
    int cx = 1, cy = -1;   // bottom-right
    if (flipX) cx = -cx;
    if (flipY) cy = -cy;
    for (int i = 0; i < quarters; ++i) {
        // clockwise quarter turn: (x, y) -> (y, -x)
        int nx = cy, ny = -cx;
        cx = nx;
        cy = ny;
    }
    if (cy < 0) return cx < 0 ? Corner::BottomLeft : Corner::BottomRight;
    return cx < 0 ? Corner::TopLeft : Corner::TopRight;
}

/// SimObject::slope as the simulator reads it (core/sim/collision.hpp): slopeUphill = 1 / 4 (the
/// surface rises to the right, GameObject::m_slopeUphill), slopeFloorTop = 3 / 4 (the solid part is
/// on top: a ceiling slope, GameObject::isFacingDown()). So 1 = floor "/", 2 = floor "\",
/// 3 = ceiling "\", 4 = ceiling "/". ONE place: adjust here only.
inline sim::SlopeOrientation orientationFromGd(bool uphill, bool facingDown) {
    if (facingDown) return uphill ? 4 : 3;
    return uphill ? 1 : 2;
}

/// The same from the right-angle corner (the derivation used when GD's own facts were not read):
/// bottom-right = floor rising to the right, bottom-left = floor falling, top-left = ceiling rising,
/// top-right = ceiling falling.
inline sim::SlopeOrientation orientationFromCorner(Corner c) {
    switch (c) {
        case Corner::BottomRight: return orientationFromGd(true, false);
        case Corner::BottomLeft: return orientationFromGd(false, false);
        case Corner::TopLeft: return orientationFromGd(true, true);
        case Corner::TopRight: return orientationFromGd(false, true);
    }
    return 0;
}

/// A slope turned by an odd number of quarter turns (90 / 270 degrees): GD keeps a separate slope
/// direction for these (m_slopeDirection); /1 does not model them.
inline bool sidewaysSlope(float rotationDeg) {
    double q = std::round(static_cast<double>(rotationDeg) / 90.0);
    return std::fmod(std::fabs(q), 2.0) == 1.0;
}

/// FNV-1a 32 over the sorted group ids (world.hpp SimObject::groupsHash).
inline uint32_t groupsHash(int16_t const* ids, int count) {
    if (!ids || count <= 0) return 0;
    std::array<int16_t, 10> sorted{};
    int n = std::min(count, 10);
    for (int i = 0; i < n; ++i) sorted[static_cast<size_t>(i)] = ids[i];
    std::sort(sorted.begin(), sorted.begin() + n);
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) {
        uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(sorted[static_cast<size_t>(i)]));
        for (int b = 0; b < 2; ++b) {
            h ^= (v >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    }
    return h;
}

// ---- the builder ----

/// GD's LIVE values of the rect inputs, read only to check the formula against GD's clean rect
/// cache (never a World input: they change with move / rotate / scale triggers and fades).
struct LiveRectState {
    float scaleX = 1.f, scaleY = 1.f;      // m_scaleX / m_scaleY
    float rotationX = 0.f, rotationY = 0.f;   // m_startRotationX + m_rotationXOffset (getObjectRotation) / Y + m_rotationYOffset
    bool flipX = false, flipY = false;     // m_isFlipX / m_isFlipY
    bool aligned = false;                  // m_isRotationAligned
    bool oriented = false;                 // m_shouldUseOuterOb
    bool boxOffsetCached = false;          // !m_boxOffsetCalculated (GD's dirty flag of m_boxOffset is clear)
    Point boxOffset;                       // m_boxOffset (GD's cached getBoxOffset when the custom offset is non-zero)
    bool haveSlopeUphill = false;          // slopes: m_slopeUphill was read
    bool slopeUphill = false;              // m_slopeUphill as GD holds it now (determineSlopeDirection may rerun at runtime)
};

/// Everything the mod READ from one GameObject (Extract.cpp names the binding member of each).
/// The geometry fields are the level's STATIC values (see the header comment); `live` is a check.
struct ObjectReading {
    int objectId = 0;                  // m_objectID
    int uniqueId = 0;                  // m_uniqueID
    int gdType = 1;                    // m_objectType
    float x = 0.f, y = 0.f;            // getRealPosition(): (float)m_positionX / m_positionY - where it is NOW
    bool haveStart = false;            // startX / startY were read
    float startX = 0.f, startY = 0.f;  // m_startPosition: where the level placed it (before move triggers)
    float rotation = 0.f;              // m_startRotationX (static)
    float rotationY = 0.f;             // m_startRotationY (static; the box offset's second axis)
    bool haveRotationY = false;        // rotationY was read (else = rotation)
    float scaleX = 1.f, scaleY = 1.f;  // m_startScaleX / m_startScaleY (static)
    bool flipX = false, flipY = false; // m_startFlipX / m_startFlipY (static)
    float width = 0.f, height = 0.f;   // m_width / m_height
    float spriteWidthScale = 1.f, spriteHeightScale = 1.f;   // m_spriteWidthScale / m_spriteHeightScale
    bool mirroredByScale = false;      // m_isMirroredByScale
    float boxOffsetX = 0.f, boxOffsetY = 0.f;   // m_customBoxOffset
    float objectRadius = 0.f;          // m_objectRadius
    bool rectExact = false;            // m_isObjectRectDirty == false: m_objectRect is GD's own rect (a CHECK only)
    Rect rect;                         // m_objectRect (when rectExact)
    bool haveLive = false;             // `live` was read
    LiveRectState live;
    bool noTouch = false;              // m_isNoTouch: the editor's "no touch" - no player collision (static)
    bool passable = false;             // m_isPassable: a block the player passes from below (unsupported in /1)
    bool hiddenRuntime = false;        // !isVisible() || m_isDisabled, as read now: DIAGNOSTIC ONLY (see add())
    bool multiActivate = false;        // EnhancedGameObject::m_isMultiActivate
    bool isTrigger = false;            // GameObject::m_isTrigger
    int targetGroupId = 0;             // EffectGameObject::m_targetGroupID (triggers)
    bool slopeHazard = false;          // GameObject::m_slopeIsHazard
    std::array<int16_t, 10> groups{};  // m_groups[0..m_groupCount)
    int groupCount = 0;                // m_groupCount
};

/// The STATIC rect inputs of a reading at (x, y) (the World's position rule decides which point).
inline RectInputs staticInputs(ObjectReading const& r, float x, float y) {
    RectInputs in;
    in.x = x;
    in.y = y;
    in.width = r.width;
    in.height = r.height;
    in.spriteWidthScale = r.spriteWidthScale;
    in.spriteHeightScale = r.spriteHeightScale;
    in.scaleX = r.scaleX;
    in.scaleY = r.scaleY;
    in.rotationX = r.rotation;
    in.rotationY = r.haveRotationY ? r.rotationY : r.rotation;
    in.flipX = r.flipX;
    in.flipY = r.flipY;
    in.mirroredByScale = r.mirroredByScale;
    in.customBoxOffsetX = r.boxOffsetX;
    in.customBoxOffsetY = r.boxOffsetY;
    in.objectRadius = r.objectRadius;
    in.gdType = r.gdType;
    in.noTouch = r.noTouch;
    return in;
}

/// GD's rect from its LIVE fields at the live position (the check against the clean cache): GD's
/// own m_shouldUseOuterOb / m_isRotationAligned / cached box offset when read, else derived.
inline GdRect liveRect(ObjectReading const& r) {
    RectInputs in = staticInputs(r, r.x, r.y);
    if (!r.haveLive) return gdObjectRect(in);
    in.scaleX = r.live.scaleX;
    in.scaleY = r.live.scaleY;
    in.rotationX = r.live.rotationX;
    in.rotationY = r.live.rotationY;
    in.flipX = r.live.flipX;
    in.flipY = r.live.flipY;
    Point box = gdBoxOffset(in);
    constexpr float kEps = 1.192092896e-07f;
    bool custom = !(std::fabs(in.customBoxOffsetX) < kEps && std::fabs(in.customBoxOffsetY) < kEps);
    if (custom && r.live.boxOffsetCached) box = r.live.boxOffset;
    return gdObjectRect(in, r.live.oriented, r.live.aligned, box);
}

/// The level-wide facts (Extract.cpp: GJBaseGameLayer::m_levelLength / m_endPortal, the level
/// settings, player 1 at the level start).
struct BuildParams {
    int gdLevelId = 0;
    std::string levelHash;
    float groundY = kConstants.groundY;
    float ceilingY = 0.f;
    float lengthX = 0.f;
    float endX = 0.f;
    sim::StartState start;
    int maxObjects = kConstants.maxObjects;
};

struct BuildCounters {
    int seen = 0;
    int gameplay = 0;
    int decoration = 0;
    int skipped = 0;
    int harmlessTriggers = 0;
    int gameplayTriggers = 0;
    int startPositions = 0;
    // GD's rect formula (gdObjectRect) gives every World rect; GD's own cache is only a check:
    int rectsChecked = 0;              // clean caches compared with the formula on GD's LIVE fields
    int rectsEstimateOff = 0;          // ... that differed by more than kConstants.rectCheckTolerance (must stay 0)
    int rectsUnchecked = 0;            // dirty caches (GD has not computed the rect yet: nothing to compare)
    std::vector<std::string> rectMismatches;   // first few: "#id type t: GD (x y w h) formula (x y w h)"
    int rectsOriented = 0;             // static rects that are the oriented box's bounding rect
    int rectsNegative = 0;             // rects with a negative size (normalised for the simulator)
    int boxOffsetsChecked = 0;         // static box offsets compared with GD's cached m_boxOffset
    int boxOffsetsOff = 0;             // ... that differed by more than the tolerance
    int orientedFlagOff = 0;           // GD's m_shouldUseOuterOb != the static updateIsOriented rule (same rotation)
    int hidden = 0;                    // no-touch objects (World: SimObject::hidden)
    int hiddenRuntime = 0;             // diagnostic: objects that read !isVisible() || m_isDisabled
    int slopesFromGd = 0;              // slopes oriented by GD's determineSlopeDirection / isFacingDown rules (static fields)
    int slopesDerivationOff = 0;       // ... where the corner derivation disagreed (log check)
    int slopesLiveOff = 0;             // ... where GD's live m_slopeUphill differed (rotate triggers / runtime reruns)
    int movedNow = 0;                  // gameplay objects away from m_startPosition when read (a move trigger ran)
    bool startPositionIgnored = false; // most objects disagreed with m_startPosition: the field was not trusted
    int capDropped = 0;                // gameplay objects over the cap, not kept (counted while collecting)
    int unsupportedObjects = 0;
    int spans = 0;
    std::vector<int> unknownTriggerIds;   // first few ids of skipped m_isTrigger objects not in any table
};

class WorldBuilder {
public:
    WorldBuilder() : WorldBuilder(kConstants) {}   // not explicit: usable in aggregate `State{}` members
    explicit WorldBuilder(ExtractConstants const& k) : m_k(k) {}

    void reset() { *this = WorldBuilder(m_k); }

    /// One object. Everything that enters the World is a STATIC property of the level, so the
    /// gameplay hash (core/sim/gameplay_hash, which covers `hidden` and the rects) is the same on
    /// every visit: the position is m_startPosition, the rect is GD's formula on the static fields
    /// (gdObjectRect), `hidden` = the editor's no-touch flag; runtime visibility (culling, toggles,
    /// used orbs) is only counted - toggles are unsupported spans instead. GD's clean rect cache is
    /// compared with the formula on GD's live fields (rectsChecked / rectsEstimateOff): the in-game
    /// log line "estimate check 0 of N" proves the formula. Gameplay objects over the cap are
    /// counted, not kept (capDropped; finish() cuts the World at the leftmost of them).
    Use add(ObjectReading const& r) {
        ++m_c.seen;
        Classified c = classify(r.gdType, r.objectId, r.isTrigger);
        float const px = r.haveStart ? r.startX : r.x;   // the level's own position (static)
        float const py = r.haveStart ? r.startY : r.y;
        switch (c.use) {
            case Use::Decoration: {
                ++m_c.decoration;
                bumpColumn(m_decoColumns, px);
                return c.use;
            }
            case Use::StartPos:
                return c.use;   // the caller reads the StartPos settings and calls addStartPos
            case Use::Skip: {
                ++m_c.skipped;
                if (c.trigger) {
                    ++m_c.harmlessTriggers;
                    if (r.isTrigger && m_c.unknownTriggerIds.size() < 16 && !knownHarmlessTrigger(r.objectId)
                        && std::find(m_c.unknownTriggerIds.begin(), m_c.unknownTriggerIds.end(), r.objectId) == m_c.unknownTriggerIds.end())
                        m_c.unknownTriggerIds.push_back(r.objectId);
                }
                return c.use;
            }
            case Use::Gameplay: break;
        }
        // AN-D12 memory cap, applied while collecting (a 2 M-object level must not grow the vector
        // past the cap first): counted, the World is cut at the leftmost dropped object in finish()
        GdRect g = gdObjectRect(staticInputs(r, px, py));
        Rect rect = g.rect;
        bool const negative = normalizeRect(rect);
        if (m_k.maxObjects >= 0 && static_cast<int>(m_objects.size()) >= m_k.maxObjects) {
            ++m_c.capDropped;
            m_capMinLeft = std::min(m_capMinLeft, rect.w > 0.f ? rect.x : px);
            return Use::Gameplay;
        }
        if (negative) ++m_c.rectsNegative;
        if (g.oriented) ++m_c.rectsOriented;
        sim::SimObject o;
        o.objectId = r.objectId;
        o.uniqueId = r.uniqueId;
        o.gdType = static_cast<uint8_t>(std::clamp(r.gdType, 0, 255));
        o.kind = c.kind;
        o.x = px;
        o.y = py;
        o.rotation = r.rotation;
        o.scaleX = r.scaleX;
        o.scaleY = r.scaleY;
        o.flipX = r.flipX;
        o.flipY = r.flipY;
        o.mode = c.mode;
        o.speed = c.speed;
        o.flag = c.flag;
        o.orb = c.orb;
        o.pad = c.pad;
        o.multiActivate = r.multiActivate;
        o.unsupportedMechanic = c.mechanic;
        o.rx = rect.x;
        o.ry = rect.y;
        o.rw = rect.w;
        o.rh = rect.h;
        setGdFacts(o, r, g);
        // a move trigger has already moved it: the World keeps where the LEVEL put it (the same on
        // every visit); finish() reverts to the position as read (the rect recomputed there) if
        // m_startPosition turns out not to mean what it should (most objects "moved")
        bool moved = r.haveStart && (std::fabs(r.startX - r.x) > 0.01f || std::fabs(r.startY - r.y) > 0.01f);
        if (moved) {
            Moved m;
            m.index = m_objects.size();
            m.x = r.x;
            m.y = r.y;
            m.g = gdObjectRect(staticInputs(r, r.x, r.y));
            m.rect = m.g.rect;
            normalizeRect(m.rect);
            m_moved.push_back(m);
            ++m_c.movedNow;
        }
        // the formula's proof: GD's clean cache vs the formula on GD's LIVE fields (never a World input)
        if (r.rectExact) {
            ++m_c.rectsChecked;
            GdRect lg = liveRect(r);
            Rect lr = lg.rect;
            float off = std::max({std::fabs(lr.x - r.rect.x), std::fabs(lr.y - r.rect.y), std::fabs(lr.w - r.rect.w), std::fabs(lr.h - r.rect.h)});
            if (!(off <= m_k.rectCheckTolerance)) {
                ++m_c.rectsEstimateOff;
                if (m_c.rectMismatches.size() < 8) {
                    char b[240];
                    std::snprintf(b, sizeof b, "#%d type %d rot %.1f scale %.2f/%.2f%s: GD (%.2f %.2f %.2f %.2f) formula (%.2f %.2f %.2f %.2f)", r.objectId, r.gdType,
                                  static_cast<double>(r.haveLive ? r.live.rotationX : r.rotation), static_cast<double>(r.haveLive ? r.live.scaleX : r.scaleX),
                                  static_cast<double>(r.haveLive ? r.live.scaleY : r.scaleY), lg.oriented ? " oriented" : (lg.aligned ? " aligned" : ""),
                                  static_cast<double>(r.rect.x), static_cast<double>(r.rect.y), static_cast<double>(r.rect.w), static_cast<double>(r.rect.h),
                                  static_cast<double>(lr.x), static_cast<double>(lr.y), static_cast<double>(lr.w), static_cast<double>(lr.h));
                    m_c.rectMismatches.emplace_back(b);
                }
            }
        } else {
            ++m_c.rectsUnchecked;
        }
        if (r.haveLive) {
            bool const sameState = r.live.rotationX == r.rotation && r.live.scaleX == r.scaleX && r.live.scaleY == r.scaleY && r.live.flipX == r.flipX
                                   && r.live.flipY == r.flipY;
            if (sameState && r.live.boxOffsetCached && (g.boxOffset.x != 0.f || g.boxOffset.y != 0.f || r.live.boxOffset.x != 0.f || r.live.boxOffset.y != 0.f)) {
                ++m_c.boxOffsetsChecked;
                float off = std::max(std::fabs(g.boxOffset.x - r.live.boxOffset.x), std::fabs(g.boxOffset.y - r.live.boxOffset.y));
                if (!(off <= m_k.rectCheckTolerance)) ++m_c.boxOffsetsOff;
            }
            if (r.live.rotationX == r.rotation && r.live.oriented != g.oriented) ++m_c.orientedFlagOff;
        }
        if (c.kind == sim::ObjKind::Slope) {
            bool supported = true;
            Corner corner = slopeCorner(r.rotation, r.flipX, r.flipY, supported, m_k.slopeRotationTolerance);
            if (r.slopeHazard) {
                o.kind = sim::ObjKind::Unsupported;
                o.unsupportedMechanic = "hazard_slope";
                c.span = SpanRule::Here;
            } else if (!supported) {
                o.kind = sim::ObjKind::Unsupported;
                o.unsupportedMechanic = "rotated_slope";
                c.span = SpanRule::Here;
            } else if (sidewaysSlope(r.rotation)) {
                o.kind = sim::ObjKind::Unsupported;
                o.unsupportedMechanic = "sideways_slope";
                c.span = SpanRule::Here;
            } else {
                // GD's own rules (determineSlopeDirection / isFacingDown) on the STATIC rotation and
                // flips win; the corner derivation and GD's live m_slopeUphill are only checked
                bool const uphill = gdSlopeUphill(r.rotation, r.flipX, r.flipY);
                bool const down = gdFacingDown(r.rotation, r.flipY);
                o.slope = orientationFromGd(uphill, down);
                ++m_c.slopesFromGd;
                if (o.slope != orientationFromCorner(corner)) ++m_c.slopesDerivationOff;
                if (r.haveLive && r.live.haveSlopeUphill && r.live.slopeUphill != uphill) ++m_c.slopesLiveOff;
            }
        }
        if (r.passable && o.kind == sim::ObjKind::Solid) {
            o.kind = sim::ObjKind::Unsupported;
            o.unsupportedMechanic = "passable_block";
            c.span = SpanRule::Here;
        }
        o.hidden = r.noTouch;
        if (o.hidden) ++m_c.hidden;
        if (r.hiddenRuntime) ++m_c.hiddenRuntime;
        int gc = std::clamp(r.groupCount, 0, 10);
        o.groupCount = static_cast<uint16_t>(gc);
        o.groupsHash = groupsHash(r.groups.data(), gc);
        if (o.kind == sim::ObjKind::Unsupported) ++m_c.unsupportedObjects;
        if (c.trigger) ++m_c.gameplayTriggers;
        float spanX = o.rw > 0.f ? o.rx : o.x;
        if (c.span == SpanRule::Here || c.span == SpanRule::UntilSolo) m_pending.push_back({spanX, c.span, o.unsupportedMechanic, o.objectId});
        if (c.span == SpanRule::IfTargetsGameplay && r.targetGroupId > 0) m_targeting.push_back({o.x, r.targetGroupId, c.mechanic, c.targetMechanic, o.objectId});
        else if (c.targetMechanic && r.targetGroupId > 0) m_targeting.push_back({o.x, r.targetGroupId, nullptr, c.targetMechanic, o.objectId});
        if (gc > 0 && !c.trigger) {
            GroupRef g;
            g.x = spanX;
            g.objectId = o.objectId;
            g.n = static_cast<uint8_t>(gc);
            for (int i = 0; i < gc; ++i) g.ids[static_cast<size_t>(i)] = r.groups[static_cast<size_t>(i)];
            m_groupRefs.push_back(g);
        }
        if (o.gdType == 24) m_soloXs.push_back(o.x);
        if (o.kind == sim::ObjKind::GamemodePortal) m_portalXs.push_back(o.x);
        m_objects.push_back(o);
        ++m_c.gameplay;
        return Use::Gameplay;
    }

    /// A StartPos (id 31) with its settings (StartPosObject::m_startSettings).
    void addStartPos(sim::StartState s) {
        ++m_c.startPositions;
        m_startPositions.push_back(s);
    }

    /// Sorts, applies the cap, builds the §4.7 spans and the decoration summary. The builder is
    /// empty afterwards. World::gameplayHash is left for sim::gameplayHash (the caller).
    sim::World finish(BuildParams const& p) {
        sim::World w;
        w.gdLevelId = p.gdLevelId;
        w.levelHash = p.levelHash;
        w.groundY = p.groundY;
        w.ceilingY = p.ceilingY;
        w.lengthX = p.lengthX > 0.f ? p.lengthX : 0.f;
        w.endX = p.endX > 0.f ? p.endX : w.lengthX;
        w.start = p.start;
        float const levelEnd = std::max(w.lengthX, w.endX);

        // m_startPosition self-check: in a level that was just loaded nearly every object sits at
        // its start position; when most do not, the field is not the start position: revert
        if (!m_moved.empty() && static_cast<double>(m_moved.size()) > std::max(16.0, 0.5 * static_cast<double>(m_objects.size()))) {
            for (auto const& m : m_moved) {
                if (m.index >= m_objects.size()) continue;
                auto& o = m_objects[m.index];
                o.x = m.x;
                o.y = m.y;
                o.rx = m.rect.x;
                o.ry = m.rect.y;
                o.rw = m.rect.w;
                o.rh = m.rect.h;
                o.obbCx = m.g.centre.x;
                o.obbCy = m.g.centre.y;
            }
            m_c.startPositionIgnored = true;
        }

        std::stable_sort(m_objects.begin(), m_objects.end(), [](sim::SimObject const& a, sim::SimObject const& b) { return a.left() < b.left(); });
        std::vector<sim::UnsupportedSpan> spans;
        if (m_c.capDropped > 0) {
            // over the cap while collecting: nothing at or past the leftmost dropped object is kept
            // (the kept objects there would be an arbitrary subset), the rest is too_large
            float cut = m_capMinLeft;
            auto it = std::lower_bound(m_objects.begin(), m_objects.end(), cut, [](sim::SimObject const& o, float x) { return o.left() < x; });
            m_objects.erase(it, m_objects.end());
            w.tooLarge = true;
            sim::UnsupportedSpan s;
            s.x0 = cut;
            s.x1 = std::max(levelEnd, cut);
            s.mechanic = "too_large";
            spans.push_back(std::move(s));
        }
        if (static_cast<int>(m_objects.size()) > p.maxObjects && p.maxObjects >= 0) {
            float cut = m_objects[static_cast<size_t>(p.maxObjects)].left();
            m_objects.resize(static_cast<size_t>(p.maxObjects));
            w.tooLarge = true;
            sim::UnsupportedSpan s;
            s.x0 = cut;
            s.x1 = std::max(levelEnd, cut);
            s.mechanic = "too_large";
            spans.push_back(std::move(s));
        }

        std::sort(m_portalXs.begin(), m_portalXs.end());
        std::sort(m_soloXs.begin(), m_soloXs.end());
        auto nextAfter = [](std::vector<float> const& xs, float x) -> float {
            auto it = std::upper_bound(xs.begin(), xs.end(), x);
            return it == xs.end() ? -1.f : *it;
        };
        auto addSpan = [&](float x0, SpanRule rule, char const* mechanic, int objectId) {
            sim::UnsupportedSpan s;
            s.x0 = x0;
            if (rule == SpanRule::UntilSolo) {
                float solo = nextAfter(m_soloXs, x0);
                s.x1 = solo > x0 ? solo : std::max(levelEnd, x0 + m_k.spanMinLength);
            } else {
                float portal = nextAfter(m_portalXs, x0);
                s.x1 = std::max(x0 + m_k.spanMinLength, portal > x0 ? portal : x0);
            }
            s.mechanic = mechanic ? mechanic : "unsupported";
            s.objectId = objectId;
            spans.push_back(std::move(s));
        };
        for (auto const& sp : m_pending) addSpan(sp.x, sp.rule, sp.mechanic, sp.objectId);

        // move-like triggers: which groups hold gameplay objects, and which groups are targeted
        if (!m_targeting.empty()) {
            std::unordered_map<int, char const*> targeted;   // group -> the first target mechanic
            for (auto const& t : m_targeting) targeted.emplace(t.group, t.targetMechanic);
            std::unordered_set<int> gameplayGroups;
            for (auto const& g : m_groupRefs) {
                char const* hit = nullptr;
                for (int i = 0; i < g.n; ++i) {
                    int id = g.ids[static_cast<size_t>(i)];
                    gameplayGroups.insert(id);
                    if (!hit) {
                        auto it = targeted.find(id);
                        if (it != targeted.end() && it->second) hit = it->second;
                    }
                }
                if (hit) addSpan(g.x, SpanRule::Here, hit, g.objectId);
            }
            for (auto const& t : m_targeting) {
                if (t.mechanic && gameplayGroups.count(t.group)) addSpan(t.x, SpanRule::Here, t.mechanic, t.objectId);
            }
        }

        // sorted by x0; overlapping spans of the same mechanic merge (the last span of a mechanic is
        // the only candidate: every earlier one either ended before it or was merged into it)
        std::stable_sort(spans.begin(), spans.end(), [](auto const& a, auto const& b) { return a.x0 < b.x0; });
        std::vector<sim::UnsupportedSpan> merged;
        std::unordered_map<std::string, size_t> lastOf;
        for (auto& s : spans) {
            auto it = lastOf.find(s.mechanic);
            if (it != lastOf.end() && s.x0 <= merged[it->second].x1) {
                merged[it->second].x1 = std::max(merged[it->second].x1, s.x1);
                continue;
            }
            lastOf[s.mechanic] = merged.size();
            merged.push_back(std::move(s));
        }
        w.unsupported = std::move(merged);
        m_c.spans = static_cast<int>(w.unsupported.size());

        std::stable_sort(m_startPositions.begin(), m_startPositions.end(), [](auto const& a, auto const& b) { return a.x < b.x; });
        for (auto& s : m_startPositions) s.percent = w.lengthX > 0.f ? std::clamp(s.x / w.lengthX * 100.f, 0.f, 100.f) : 0.f;
        w.startPositions = std::move(m_startPositions);

        w.decoration.objects = m_c.decoration;
        w.decoration.perColumn = std::move(m_decoColumns);
        std::vector<uint16_t> gp;
        for (auto const& o : m_objects) bumpColumn(gp, o.x);
        w.decoration.gameplayPerColumn = std::move(gp);
        w.objects = std::move(m_objects);
        w.gameplayObjects = static_cast<int>(w.objects.size());
        w.decorationObjects = m_c.decoration;

        BuildCounters keep = m_c;
        ExtractConstants k = m_k;
        *this = WorldBuilder(k);
        m_last = std::move(keep);
        return w;
    }

    BuildCounters const& counters() const { return m_c; }
    /// The counters of the last finish() (the builder is reset by it).
    BuildCounters const& lastCounters() const { return m_last; }

private:
    struct PendingSpan {
        float x;
        SpanRule rule;
        char const* mechanic;
        int objectId;
    };
    struct Targeting {
        float x;
        int group;
        char const* mechanic;         // non-null: the trigger itself gets a span when the group holds gameplay objects
        char const* targetMechanic;   // the span name for the moved objects
        int objectId;
    };
    struct GroupRef {
        float x = 0.f;
        int objectId = 0;
        std::array<int16_t, 10> ids{};
        uint8_t n = 0;
    };
    struct Moved {
        size_t index = 0;   // into m_objects before the sort
        float x = 0.f, y = 0.f;
        GdRect g;           // the static formula at the position as read
        Rect rect;          // g.rect normalised
    };

    /// GD's rect facts the simulator may use (SimObject fields added at the end of the struct).
    static void setGdFacts(sim::SimObject& o, ObjectReading const& r, GdRect const& g) {
        o.radius = r.objectRadius;
        o.oriented = g.oriented;
        o.obbCx = g.centre.x;
        o.obbCy = g.centre.y;
        o.obbW = g.oriented ? g.obbW : 0.f;
        o.obbH = g.oriented ? g.obbH : 0.f;
        o.obbAngle = g.oriented ? g.obbAngle : 0.f;
    }

    static bool knownHarmlessTrigger(int id) {
        // colour / pulse / alpha / shake / camera / song / sfx / shader / bg / ground / label / ui triggers
        static constexpr int kHarmless[] = {29, 30, 104, 105, 221, 717, 718, 743, 744, 899, 900, 915, 1006, 1007, 1520, 1612, 1613, 1615, 1818, 1819, 1913,
                                            1914, 1916, 1934, 2015, 2016, 2062, 2901, 2903, 2904, 2905, 2907, 2909, 2910, 2911, 2912, 2913, 2914, 2915, 2916, 2917,
                                            2919, 2920, 2921, 2922, 2923, 2924, 2925, 2999, 3029, 3030, 3031, 3032, 3602, 3603, 3605, 3606, 3608, 3612, 3613, 3662,
                                            22, 23, 24, 25, 26, 27, 28, 32, 33, 55, 56, 57, 58, 59, 1915, 3009, 3010, 3011, 3012, 3013, 3014, 3015, 3017, 3018,
                                            3019, 3020, 3021, 3023, 3024};
        for (int h : kHarmless) {
            if (h == id) return true;
        }
        return false;
    }

    void bumpColumn(std::vector<uint16_t>& cols, float x) const {
        long long idx = x <= 0.f || !std::isfinite(x) ? 0 : static_cast<long long>(std::floor(x / m_k.columnWidth));
        if (idx >= m_k.maxColumns) idx = m_k.maxColumns - 1;
        if (idx < 0) idx = 0;
        size_t i = static_cast<size_t>(idx);
        if (cols.size() <= i) cols.resize(i + 1, 0);
        if (cols[i] < 0xffff) ++cols[i];
    }

    ExtractConstants m_k;
    BuildCounters m_c;
    BuildCounters m_last;
    std::vector<sim::SimObject> m_objects;
    std::vector<sim::StartState> m_startPositions;
    std::vector<PendingSpan> m_pending;
    std::vector<Targeting> m_targeting;
    std::vector<GroupRef> m_groupRefs;
    std::vector<Moved> m_moved;
    std::vector<float> m_portalXs;
    std::vector<float> m_soloXs;
    std::vector<uint16_t> m_decoColumns;
    float m_capMinLeft = std::numeric_limits<float>::infinity();   // leftmost gameplay object dropped by the cap
};

}  // namespace gprl::analyzer::extract
