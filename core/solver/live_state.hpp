#pragma once
// LiveStateSnapshot (docs/LIVE_ISOLATION_DESIGN.md §3.1, Appendix B; owner spec
// docs/LIVE_ISOLATION_SPEC.md "LIVE_STATE_MUTATION_DETECTED"). PURE C++20, no Geode includes;
// filled by src/solver/LiveCapture.cpp, compared here, host-tested in tests/live_state_tests.cpp.
//
// A snapshot is a flat set of NAMED fields of the live game (both players, the camera, the layer,
// and the activation bytes of the orbs / pads / portals around player 1). The engine takes one
// before and one after every analysis block (everything it does to clones in one call); GD runs
// nothing in between, so the expected difference is exactly zero. Floats compare BITWISE (a NaN
// equals itself, -0 differs from +0: anything a clone could write shows). The compare lists every
// differing field with its before / after text in the owner's format (field / before / after).
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gprl::solver::live {

constexpr char const* kVersion = "gprl-live-state/1";

/// Gamemode of a player as a small int (0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing).
constexpr char const* gamemodeName(uint8_t g) {
    switch (g) {
        case 0: return "cube";
        case 1: return "ship";
        case 2: return "ball";
        case 3: return "ufo";
        case 4: return "wave";
        case 5: return "robot";
        case 6: return "spider";
        case 7: return "swing";
    }
    return "?";
}

struct PlayerSnap {
    float posX = 0.f, posY = 0.f;          // getPosition()
    float mposX = 0.f, mposY = 0.f;        // m_position
    double yVelocity = 0.0;                // m_yVelocity
    bool upsideDown = false;               // m_isUpsideDown (gravity normal / inverted)
    uint8_t gamemode = 0;                  // mode flags -> gamemodeName
    float vehicleSize = 1.f;               // m_vehicleSize (mini)
    float playerSpeed = 0.f;               // m_playerSpeed
    double gravityConst = 0.0;             // m_gravity
    double yStart = 0.0;                   // m_yStart
    bool held = false;                     // m_holdingButtons[1]
    bool dead = false;                     // m_isDead
    bool onGround = false;                 // m_isOnGround
    bool visible = false;                  // isVisible()
    bool secondPlayer = false;             // m_isSecondPlayer
    int lastPortalId = 0;                  // m_lastActivatedPortal->m_objectID (0 = none)
    float lastPortalX = 0.f, lastPortalY = 0.f;   // m_lastPortalPos
    int rings = 0;                         // m_touchingRings->count()
    bool checkpointTimeout = false;        // m_checkpointTimeout (player 1 only; player 2 captured too, harmless)
};

struct CameraSnap {
    float posX = 0.f, posY = 0.f;          // m_gameState.m_cameraPosition
    float pos2X = 0.f, pos2Y = 0.f;        // m_cameraPosition2
    float zoom = 1.f, targetZoom = 1.f;    // m_cameraZoom, m_targetCameraZoom
    float offX = 0.f, offY = 0.f;          // m_cameraOffset
    float angle = 0.f;                     // m_cameraAngle
    float follow6X = 0.f, follow6Y = 0.f;      // m_unkPoint6  (layer +0x1e0 / +0x1e4)
    float follow8X = 0.f, follow8Y = 0.f;      // m_unkPoint8  (+0x1f0 / +0x1f4)
    float follow17X = 0.f, follow17Y = 0.f;    // m_unkPoint17 (+0x238 / +0x23c: the corridor lock)
    float follow18X = 0.f, follow18Y = 0.f;    // m_unkPoint18 (+0x240 / +0x244)
    float follow22X = 0.f, follow22Y = 0.f;    // m_unkPoint22 (+0x260 / +0x264)
    float padding = 0.f;                   // m_unkFloat2 (layer +0x2cc, written by playerWillSwitchMode)
    float easing = 0.f;                    // m_unkFloat3 (layer +0x2d0, written by playerWillSwitchMode)
    bool bool7 = false;                    // m_unkBool7
    bool freeMode = false;                 // m_isFreeMode (layer +0x311, written by playerWillSwitchMode)
    bool bool9 = false;                    // m_unkBool9 (layer +0x312, written by playerWillSwitchMode)
    float stepDiffX = 0.f, stepDiffY = 0.f;    // m_cameraStepDiff
    int edge0 = 0, edge1 = 0, edge2 = 0, edge3 = 0;   // m_cameraEdgeValue0-3
    uint32_t tweens = 0;                   // m_tweenActions.size()
    float flip = 0.f;                      // GJBaseGameLayer::m_cameraFlip
    float layerX = 0.f, layerY = 0.f, layerScale = 1.f;   // m_objectLayer position / scale (the rendered camera)
};

struct LayerSnap {
    bool dualMode = false;                 // m_isDualMode
    bool unlinkGravity = false;            // m_unkBool31 (layer +0x860)
    uint32_t dualGround = 0;               // m_dualRelated
    int lastPortal1Id = 0, lastPortal2Id = 0;          // m_lastActivatedPortal1/2 -> m_objectID (0 = null)
    uint64_t lastPortal1Ptr = 0, lastPortal2Ptr = 0;   // the pointers themselves (a swap to another object with the same id shows)
    float timeMod = 1.f;                   // m_timeModRelated
    bool timeModNoEffects = false;         // m_timeModRelated2
    bool betweenSteps = false;             // GJBaseGameLayer::m_isBetweenSteps
    float mirror = 0.f;                    // m_levelFlipping
    uint32_t activatedObjectIDs = 0;       // m_activatedObjectIDs size
    uint32_t stateObjects = 0;             // m_stateObjects size
    uint32_t checkpoints = 0;              // PlayLayer m_checkpointArray count
    bool playerDied = false;               // PlayLayer m_playerDied
    uint32_t items = 0;                    // effect manager m_itemCountMap size
};

/// One orb / pad / portal / collectible near player 1: its activation bytes.
struct ObjectSnap {
    int id = 0;                            // m_objectID (0 = unused slot)
    int uid = 0;                           // m_uniqueID
    float x = 0.f;
    uint8_t isActivated = 0;               // GameObject::m_isActivated
    uint8_t activated = 0;                 // EnhancedGameObject::m_activated
    uint8_t slot1 = 0, slot2 = 0;          // m_activatedByPlayer1 / 2
    uint8_t disabled = 0;                  // GameObject::m_isDisabled
    uint8_t powered = 0;                   // ring power (RingObject) / 0
    constexpr bool operator==(ObjectSnap const&) const = default;
};

constexpr int kMaxObjects = 128;
constexpr float kObjectsBehind = 130.f;    // §3.1: [P1 x - 130, P1 x + 600]
constexpr float kObjectsAhead = 600.f;

struct Snapshot {
    PlayerSnap p1, p2;
    CameraSnap camera;
    LayerSnap layer;
    std::array<ObjectSnap, kMaxObjects> objects{};
    int objectCount = 0;
    uint64_t objectsHash = 0;              // FNV-1a over the objects' bytes (quick compare)
    int captureStep = 0;                   // engine step the capture belongs to (log only)
};

/// FNV-1a 64 over the activation bytes of the captured objects (call after filling `objects`).
inline uint64_t objectsHash(Snapshot const& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (int i = 0; i < s.objectCount && i < kMaxObjects; ++i) {
        auto const& o = s.objects[static_cast<size_t>(i)];
        mix(static_cast<uint64_t>(static_cast<uint32_t>(o.uid)));
        mix(o.isActivated | (o.activated << 8) | (o.slot1 << 16) | (o.slot2 << 24) | (static_cast<uint64_t>(o.disabled) << 32) | (static_cast<uint64_t>(o.powered) << 40));
    }
    return h;
}

/// One differing field (the owner's block: field / before / after).
struct Diff {
    std::string field;
    std::string before;
    std::string after;
};

namespace detail {
inline std::string text(float v) { char b[32]; std::snprintf(b, sizeof b, "%.6g", static_cast<double>(v)); return b; }
inline std::string text(double v) { char b[40]; std::snprintf(b, sizeof b, "%.10g", v); return b; }
inline std::string text(bool v) { return v ? "true" : "false"; }
inline std::string text(int v) { return std::to_string(v); }
inline std::string text(uint32_t v) { return std::to_string(v); }
inline std::string text(uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v)); return b; }
inline bool same(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
inline bool same(double a, double b) { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }
template <class T> inline bool same(T a, T b) { return a == b; }

struct Collector {
    std::vector<Diff>& out;
    int maxFields;
    int total = 0;
    template <class T>
    void field(char const* name, T a, T b) {
        if (same(a, b)) return;
        ++total;
        if (static_cast<int>(out.size()) < maxFields) out.push_back({name, text(a), text(b)});
    }
    void gravity(char const* name, bool a, bool b) {
        if (a == b) return;
        ++total;
        if (static_cast<int>(out.size()) < maxFields) out.push_back({name, a ? "inverted" : "normal", b ? "inverted" : "normal"});
    }
    void gamemode(char const* name, uint8_t a, uint8_t b) {
        if (a == b) return;
        ++total;
        if (static_cast<int>(out.size()) < maxFields) out.push_back({name, gamemodeName(a), gamemodeName(b)});
    }
};

inline void comparePlayer(Collector& c, char const* p, PlayerSnap const& a, PlayerSnap const& b) {
    std::string pre = p;
    auto n = [&](char const* f) { static thread_local std::string s; s = pre + f; return s.c_str(); };
    c.field(n(".position.x"), a.posX, b.posX);
    c.field(n(".position.y"), a.posY, b.posY);
    c.field(n(".mposition.x"), a.mposX, b.mposX);
    c.field(n(".mposition.y"), a.mposY, b.mposY);
    c.field(n(".velocity.y"), a.yVelocity, b.yVelocity);
    c.gravity(n(".gravity"), a.upsideDown, b.upsideDown);
    c.gamemode(n(".gamemode"), a.gamemode, b.gamemode);
    c.field(n(".mini"), a.vehicleSize, b.vehicleSize);
    c.field(n(".speed"), a.playerSpeed, b.playerSpeed);
    c.field(n(".gravityConst"), a.gravityConst, b.gravityConst);
    c.field(n(".yStart"), a.yStart, b.yStart);
    c.field(n(".held"), a.held, b.held);
    c.field(n(".dead"), a.dead, b.dead);
    c.field(n(".onGround"), a.onGround, b.onGround);
    c.field(n(".visible"), a.visible, b.visible);
    c.field(n(".secondPlayer"), a.secondPlayer, b.secondPlayer);
    c.field(n(".portal.last"), a.lastPortalId, b.lastPortalId);
    c.field(n(".portal.lastPos.x"), a.lastPortalX, b.lastPortalX);
    c.field(n(".portal.lastPos.y"), a.lastPortalY, b.lastPortalY);
    c.field(n(".rings"), a.rings, b.rings);
    c.field(n(".checkpointTimeout"), a.checkpointTimeout, b.checkpointTimeout);
}
}  // namespace detail

/// Every differing field of `before` vs `after`, at most `maxFields` listed (the total count in
/// `*total` when given). Objects: the first differing object is named `objects.activation` with
/// "#id at x=..: bytes" texts; a changed object COUNT is `objects.count`.
inline std::vector<Diff> compare(Snapshot const& before, Snapshot const& after, int maxFields = 12, int* total = nullptr) {
    std::vector<Diff> out;
    detail::Collector c{out, maxFields};
    detail::comparePlayer(c, "player1", before.p1, after.p1);
    detail::comparePlayer(c, "player2", before.p2, after.p2);
    auto const& a = before.camera;
    auto const& b = after.camera;
    c.field("camera.position.x", a.posX, b.posX);
    c.field("camera.position.y", a.posY, b.posY);
    c.field("camera.position2.x", a.pos2X, b.pos2X);
    c.field("camera.position2.y", a.pos2Y, b.pos2Y);
    c.field("camera.zoom", a.zoom, b.zoom);
    c.field("camera.targetZoom", a.targetZoom, b.targetZoom);
    c.field("camera.offset.x", a.offX, b.offX);
    c.field("camera.offset.y", a.offY, b.offY);
    c.field("camera.angle", a.angle, b.angle);
    c.field("camera.follow6.x", a.follow6X, b.follow6X);
    c.field("camera.follow6.y", a.follow6Y, b.follow6Y);
    c.field("camera.follow8.x", a.follow8X, b.follow8X);
    c.field("camera.follow8.y", a.follow8Y, b.follow8Y);
    c.field("camera.follow17.x", a.follow17X, b.follow17X);
    c.field("camera.follow17.y", a.follow17Y, b.follow17Y);
    c.field("camera.follow18.x", a.follow18X, b.follow18X);
    c.field("camera.follow18.y", a.follow18Y, b.follow18Y);
    c.field("camera.follow22.x", a.follow22X, b.follow22X);
    c.field("camera.follow22.y", a.follow22Y, b.follow22Y);
    c.field("camera.padding", a.padding, b.padding);
    c.field("camera.easing", a.easing, b.easing);
    c.field("camera.bool7", a.bool7, b.bool7);
    c.field("camera.freeMode", a.freeMode, b.freeMode);
    c.field("camera.bool9", a.bool9, b.bool9);
    c.field("camera.stepDiff.x", a.stepDiffX, b.stepDiffX);
    c.field("camera.stepDiff.y", a.stepDiffY, b.stepDiffY);
    c.field("camera.edge0", a.edge0, b.edge0);
    c.field("camera.edge1", a.edge1, b.edge1);
    c.field("camera.edge2", a.edge2, b.edge2);
    c.field("camera.edge3", a.edge3, b.edge3);
    c.field("camera.tweens", a.tweens, b.tweens);
    c.field("camera.flip", a.flip, b.flip);
    c.field("camera.layer.x", a.layerX, b.layerX);
    c.field("camera.layer.y", a.layerY, b.layerY);
    c.field("camera.layer.scale", a.layerScale, b.layerScale);
    auto const& la = before.layer;
    auto const& lb = after.layer;
    c.field("layer.dualMode", la.dualMode, lb.dualMode);
    c.field("layer.unlinkGravity", la.unlinkGravity, lb.unlinkGravity);
    c.field("layer.dualGround", la.dualGround, lb.dualGround);
    c.field("layer.lastPortal1", la.lastPortal1Id, lb.lastPortal1Id);
    c.field("layer.lastPortal1.ptr", la.lastPortal1Ptr, lb.lastPortal1Ptr);
    c.field("layer.lastPortal2", la.lastPortal2Id, lb.lastPortal2Id);
    c.field("layer.lastPortal2.ptr", la.lastPortal2Ptr, lb.lastPortal2Ptr);
    c.field("layer.timeMod", la.timeMod, lb.timeMod);
    c.field("layer.timeModNoEffects", la.timeModNoEffects, lb.timeModNoEffects);
    c.field("layer.betweenSteps", la.betweenSteps, lb.betweenSteps);
    c.field("layer.mirror", la.mirror, lb.mirror);
    c.field("layer.activatedObjectIDs", la.activatedObjectIDs, lb.activatedObjectIDs);
    c.field("layer.stateObjects", la.stateObjects, lb.stateObjects);
    c.field("layer.checkpoints", la.checkpoints, lb.checkpoints);
    c.field("layer.playerDied", la.playerDied, lb.playerDied);
    c.field("layer.items", la.items, lb.items);
    // objects: the same census (count and ids) is expected on both sides of a block
    if (before.objectCount != after.objectCount) {
        c.field("objects.count", before.objectCount, after.objectCount);
    } else {
        for (int i = 0; i < before.objectCount && i < kMaxObjects; ++i) {
            auto const& oa = before.objects[static_cast<size_t>(i)];
            auto const& ob = after.objects[static_cast<size_t>(i)];
            if (oa == ob) continue;
            ++c.total;
            if (static_cast<int>(out.size()) < maxFields) {
                char ba[96], bb[96];
                std::snprintf(ba, sizeof ba, "#%d (uid %d) at x=%.1f: isActivated %u activated %u slot1 %u slot2 %u disabled %u powered %u", oa.id, oa.uid,
                              static_cast<double>(oa.x), oa.isActivated, oa.activated, oa.slot1, oa.slot2, oa.disabled, oa.powered);
                std::snprintf(bb, sizeof bb, "#%d (uid %d) at x=%.1f: isActivated %u activated %u slot1 %u slot2 %u disabled %u powered %u", ob.id, ob.uid,
                              static_cast<double>(ob.x), ob.isActivated, ob.activated, ob.slot1, ob.slot2, ob.disabled, ob.powered);
                out.push_back({"objects.activation", ba, bb});
            }
        }
    }
    if (total) *total = c.total;
    return out;
}

/// The owner's exact multi-line block for one field (docs/LIVE_ISOLATION_SPEC.md).
inline std::string ownerBlock(Diff const& d) {
    return "LIVE_STATE_MUTATION_DETECTED\nfield: " + d.field + "\nbefore: " + d.before + "\nafter: " + d.after;
}

}  // namespace gprl::solver::live
