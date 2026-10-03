#pragma once
// Live-state isolation of the lockstep clones (docs/LIVE_ISOLATION_DESIGN.md §2.3 / Appendix A;
// owner spec docs/LIVE_ISOLATION_SPEC.md). PURE C++20, no Geode includes; host-tested in
// tests/isolation_tests.cpp.
//
// A stepped clone runs GD's own GJBaseGameLayer::collisionCheckObjects (win 0x214960). Its
// per-object-type branches were classified from the disassembly: which ones only touch the clone
// (PASS), which reach layer / camera / dual writers that cannot be hooked (BLOCK_*), and which are
// dropped from the clone's object list (FILTER). The engine never decides a treatment anywhere
// else: `treatmentOf(objectType)` is the single source.
//
// GameObjectType values (GD 2.2081): 0 Solid, 1 Unknown, 2 Hazard, 3 InverseGravityPortal,
// 4 NormalGravityPortal, 5 ShipPortal, 6 CubePortal, 7 Decoration, 8 YellowJumpPad, 9 PinkJumpPad,
// 10 GravityPad, 11 YellowJumpRing, 12 PinkJumpRing, 13 GravityRing, 14 InverseMirrorPortal,
// 15 NormalMirrorPortal, 16 BallPortal, 17 RegularSizePortal, 18 MiniSizePortal, 19 UfoPortal,
// 20 Modifier, 21 Breakable, 22 SecretCoin, 23 DualPortal, 24 SoloPortal, 25 Slope, 26 WavePortal,
// 27 RobotPortal, 28 TeleportPortal, 29 GreenRing, 30 Collectible, 31 UserCoin, 32 DropRing,
// 33 SpiderPortal, 34 RedJumpPad, 35 RedJumpRing, 36 CustomRing, 37 DashRing, 38 GravityDashRing,
// 39 CollisionObject, 40 Special, 41 SwingPortal, 42 GravityTogglePortal, 43 SpiderOrb,
// 44 SpiderPad, 45 EnterEffectObject, 46 TeleportOrb, 47 AnimatedHazard.
#include <cstdint>

namespace gprl::solver::isolation {

constexpr char const* kVersion = "gprl-isolation/2";   // /2: gamemode portals modelled on the clone (step 2)

enum class Treatment : uint8_t {
    /// GD's branch runs for the clone; every write inside it is clone-local or hooked away.
    Pass,
    /// Gamemode portals: the branch calls playerWillSwitchMode (camera / ground / dual writes
    /// inline in the layer). Not run for a clone; the engine replays the PLAYER part of the branch
    /// on the clone itself (portal_model.hpp, kModelGamemodePortals) or invalidates the clone.
    BlockModel,
    /// Visual-only live writes (mirror portals): the clone never activates them.
    BlockIgnore,
    /// Dual / solo / teleport portals: unisolatable layer rewrites; the clone becomes invalid.
    BlockInvalid,
    /// Collectibles and touch triggers: dropped from the clone's object list, never seen.
    Filter,
    /// Not a GD object type: dropped, and an overlap makes the clone invalid.
    FilterInvalid,
    /// Orbs: GD's branch is replaced by the clone-local transcription (playerTouchedRing hook).
    Replace,
};

constexpr int kObjectTypeCount = 48;

/// The treatment of a GameObjectType for a stepped clone (Appendix A census).
constexpr Treatment treatmentOf(int objectType) {
    switch (objectType) {
        case 0: case 21:                       // Solid, Breakable (transient scratch lists)
        case 2: case 47:                       // Hazard, AnimatedHazard (clone death via the claim)
        case 7: case 39:                       // Decoration, CollisionObject (skipped by GD itself)
        case 3: case 4: case 42:               // gravity portals (flipGravity hooked clone-local)
        case 8: case 9: case 34: case 44:      // jump pads (bumpPlayer: clone fields only)
        case 10:                               // gravity pad
        case 17: case 18:                      // size portals (lightningFlash hooked away)
        case 25:                               // Slope
        case 40:                               // Special (force blocks etc.)
            return Treatment::Pass;
        case 5: case 6: case 16: case 19: case 26: case 27: case 33: case 41:
            return Treatment::BlockModel;      // ship, cube, ball, ufo, wave, robot, spider, swing portals
        case 14: case 15:
            return Treatment::BlockIgnore;     // mirror portals
        case 23: case 24: case 28:
            return Treatment::BlockInvalid;    // dual, solo, teleport portals
        case 20: case 45: case 22: case 30: case 31:
            return Treatment::Filter;          // touch triggers, coins, collectibles
        case 11: case 12: case 13: case 29: case 32: case 35: case 36: case 37: case 38: case 43: case 46:
            return Treatment::Replace;         // orbs (46 = teleport orb: + teleportPlayer tripwire)
        default:
            return Treatment::FilterInvalid;   // 1 and anything outside 0..47
    }
}

/// Whether a gamemode portal is modelled on the clone (step 2, core/solver/portal_model.hpp) or
/// invalidates it (the step 1 fallback, kept as the build-time switch).
constexpr bool kModelGamemodePortals = true;

/// Functions a clone step must never reach (layer / camera / dual / checkpoint writers). Reaching one
/// means the census missed a branch: the clone becomes Invalid("unisolated_path: <name>").
constexpr char const* kTripwires[] = {
    "playerWillSwitchMode", "updateDualGround", "animateInDualGroundNew", "updateStaticCameraPos",
    "destroyObject", "storeTriggeredID", "triggerActivated", "removeCheckpoint",
    "toggleDualMode", "teleportPlayer",
};

}  // namespace gprl::solver::isolation
