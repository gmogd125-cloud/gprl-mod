#pragma once
// The isolated simulator's world model (docs/BACKGROUND_ANALYZER_DESIGN.md §3-§4, AN-D1).
//
// PURE C++20. Nothing under geode/core/sim may include a Geode, cocos2d or GD header
// (tests/sim_no_gd_symbols_tests.cpp fails the build otherwise). The mod fills a `World` from a
// READ-ONLY walk of the live level (src/analyzer/Extract.cpp) and hands it to the worker thread;
// from then on the simulator owns plain numbers only. Every field here is documented with the GD
// field it was read from so the extraction and the physics agree on meaning.
#include <cstdint>
#include <string>
#include <vector>

namespace gprl::sim {

// /2 (2026-10-02 review fixes, core/sim): float positions, the slope plateau exit, the flyer orb
// rule, ring state across restore, the slope snap / launch rules, restart states after a skipped
// stretch, span re-seeding in verify; the analyzer's coverage / time / verification figures with it
constexpr char const* kSimVersion = "gprl-sim/2";
constexpr char const* kAnalyzerVersion = "gprl-analyzer/2";
// /2 (2026-10-02, mod extraction gprl-extract/2): rects are GD's exact getObjectRect formula on the
// level's static fields, the anti-cheat spike is no longer a hazard, one start-state rule
constexpr char const* kGameplayHashVersion = "gprl-gameplay-hash/2";

constexpr int kTicksPerSecond = 240;
constexpr double kStepDt = 0.25;          // PlayerObject::update receives 1/60-second units (GD_PHYSICS_NOTES)
constexpr double kUnitsPerBlock = 30.0;

/// Gamemode as a small int (same numbering as core/solver/live_state.hpp gamemodeName).
enum class Gamemode : uint8_t { Cube = 0, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };
constexpr char const* gamemodeName(Gamemode g) {
    switch (g) {
        case Gamemode::Cube: return "cube";
        case Gamemode::Ship: return "ship";
        case Gamemode::Ball: return "ball";
        case Gamemode::Ufo: return "ufo";
        case Gamemode::Wave: return "wave";
        case Gamemode::Robot: return "robot";
        case Gamemode::Spider: return "spider";
        case Gamemode::Swing: return "swing";
    }
    return "?";
}
constexpr bool flyingMode(Gamemode g) { return g == Gamemode::Ship || g == Gamemode::Ufo || g == Gamemode::Wave || g == Gamemode::Swing; }

/// Speed as GD's index (object ids 200 / 201 / 202 / 203 / 1334).
enum class Speed : uint8_t { Half = 0, Normal = 1, Double = 2, Triple = 3, Quadruple = 4 };

/// What an object is for the physics, decided by the extraction from GameObject::m_objectType
/// (the 0..47 census of core/solver/isolation.hpp) plus m_objectID for the typeless cases.
enum class ObjKind : uint8_t {
    Solid = 0,         // type 0 Solid, 21 Breakable
    Hazard,            // type 2 Hazard, 47 AnimatedHazard (static rect)
    Slope,             // type 25 (orientation in `slope`)
    GravityPortal,     // type 3 (inverse = flipped), 4 (normal), 42 (toggle)
    GamemodePortal,    // types 5 6 16 19 26 27 33 41 (target mode in `mode`)
    SizePortal,        // 17 regular, 18 mini
    SpeedChange,       // ids 200 201 202 203 1334 (not a collision in GD: applied by x)
    Orb,               // 11 yellow, 12 pink, 13 gravity(blue), 29 green, 32 drop(black), 35 red, 43 spider, (36 custom, 37/38 dash, 46 teleport = unsupported)
    Pad,               // 8 yellow, 9 pink, 34 red, 10 gravity(blue), 44 spider
    StartPos,          // id 31
    Decoration,        // type 7: never copied into World::objects (counted in DecorationSummary)
    Unsupported,       // everything else that affects gameplay (§4.7): dual / mirror / teleport portals, triggers, collision blocks, special, ...
};

enum class OrbKind : uint8_t { Yellow = 0, Pink, Red, Gravity, Green, Black, Spider, Custom, Dash, GravityDash, Teleport };
enum class PadKind : uint8_t { Yellow = 0, Pink, Red, Gravity, Spider };

/// Slope orientation: which corner of the rect is the solid right angle.
/// 0 = not a slope; 1 = solid bottom-left (rises to the right, "/"), 2 = solid bottom-right ("\"),
/// 3 = solid top-left (ceiling "\"), 4 = solid top-right (ceiling "/").
using SlopeOrientation = uint8_t;

struct SimObject {
    int objectId = 0;        // GameObject::m_objectID
    int uniqueId = 0;        // GameObject::m_uniqueID (log / diagnostics only; never a key)
    uint8_t gdType = 1;      // GameObject::m_objectType (0..47)
    ObjKind kind = ObjKind::Unsupported;
    float x = 0.f, y = 0.f;                      // getPosition() (centre)
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f; // getObjectRect(): origin (bottom-left) and size, world units
    float rotation = 0.f;                        // getRotation() degrees
    float scaleX = 1.f, scaleY = 1.f;            // getScaleX / Y
    bool flipX = false, flipY = false;           // m_isFlipX / m_isFlipY
    SlopeOrientation slope = 0;
    Gamemode mode = Gamemode::Cube;              // GamemodePortal target
    Speed speed = Speed::Normal;                 // SpeedChange value
    bool flag = false;                           // GravityPortal: true = flipped (inverse) gravity; SizePortal: true = mini
    OrbKind orb = OrbKind::Yellow;
    PadKind pad = PadKind::Yellow;
    bool multiActivate = false;                  // EnhancedGameObject multi-activate (orbs / pads)
    bool hidden = false;                         // !isVisible() || m_isDisabled: no collision in GD
    uint32_t groupsHash = 0;                     // FNV over sorted group ids (gameplay hash input)
    uint16_t groupCount = 0;
    char const* unsupportedMechanic = nullptr;   // ObjKind::Unsupported: the §4.7 name (static string)
    // gprl-extract/2 (2026-10-02; added at the END with defaults = the /1 behaviour, not hash inputs):
    // GD's own collision facts from src/analyzer/Extract.cpp + core/analyzer_extract.hpp gdObjectRect
    float radius = 0.f;                          // GameObject::m_objectRadius: > 0 = GD never orients it (orbs / rings)
    bool oriented = false;                       // GD's m_shouldUseOuterOb rule (updateIsOriented): GD collides with the
                                                 // oriented box below; rx..rh is that box's bounding rect
    float obbCx = 0.f, obbCy = 0.f;              // the rect's centre: position + GD's box offset (getRealPosition + getBoxOffset)
    float obbW = 0.f, obbH = 0.f;                // oriented only: (spriteScale x m_width) x scale (updateOrientedBox), may be < 0
    float obbAngle = 0.f;                        // oriented only: radians, -(rotation) x 0.0174533 (OBB2D, counter-clockwise)

    float left() const { return rx; }
    float right() const { return rx + rw; }
    float bottom() const { return ry; }
    float top() const { return ry + rh; }
};

/// The player's state at a start: the level settings (LevelSettingsObject) or a StartPos (id 31).
struct StartState {
    float x = 0.f, y = 0.f;
    Gamemode mode = Gamemode::Cube;
    bool mini = false;
    bool upsideDown = false;
    Speed speed = Speed::Normal;
    bool dual = false;
    bool mirror = false;
    bool reversed = false;
    bool platformer = false;
    bool twoPlayer = false;
    float percent = 0.f;                         // of the level length (StartPos: by x)
};

/// A gameplay span the simulator does not model (§4.7). Coverage ends in it.
struct UnsupportedSpan {
    float x0 = 0.f, x1 = 0.f;
    std::string mechanic;                        // e.g. "dual_portal", "move_trigger", "teleport_portal"
    int objectId = 0;
};

/// Decoration is never simulated; this summary feeds the presentation fingerprint (level families).
struct DecorationSummary {
    int objects = 0;
    std::vector<uint16_t> perColumn;             // decoration objects per 30-unit column, index = floor(x / 30)
    std::vector<uint16_t> gameplayPerColumn;     // gameplay objects per column (the same axis)
};

struct World {
    int gdLevelId = 0;
    std::string levelHash;                       // sha256 of the level string (env::hashLevel), the exact version
    std::string gameplayHash;                    // gameplay_hash.hpp, kGameplayHashVersion
    float groundY = 0.f;                         // the floor line (PlayLayer ground): player bottom rests here
    float ceilingY = 0.f;                        // 0 = none (open sky); flying modes use the corridor rules
    float lengthX = 0.f;                         // level length in units (last object right edge + GD's end padding)
    float endX = 0.f;                            // x where the level completes (GD: lengthX + end offset)
    StartState start;
    std::vector<StartState> startPositions;      // id 31 objects, sorted by x
    std::vector<SimObject> objects;              // gameplay objects only, sorted by left() ascending
    std::vector<UnsupportedSpan> unsupported;    // sorted by x0
    DecorationSummary decoration;
    int gameplayObjects = 0;                     // = objects.size()
    int decorationObjects = 0;
    bool tooLarge = false;                       // over the object cap: the world holds nothing past the cap (Partial)
};

/// One physics step of the REAL player, captured passively (docs §4.5). Compared bitwise for
/// the discrete fields and within tolerance for the continuous ones.
struct RecordedTick {
    int step = 0;              // physics step of the attempt (1 = first step after reset)
    float x = 0.f, y = 0.f;    // PlayerObject position after the step
    float yVelocity = 0.f;     // m_yVelocity
    bool upsideDown = false;
    Gamemode mode = Gamemode::Cube;
    bool mini = false;
    Speed speed = Speed::Normal;
    bool onGround = false;
    bool held = false;         // m_holdingButtons[1] (jump)
    bool dead = false;
};

struct RecordedInput {
    int step = 0;              // the step the input was applied in (processCommands of that step)
    double subTick = 0.0;      // 0..1 within the step (CBF); 0 without CBF
    bool down = true;          // press / release
    uint8_t button = 1;        // 1 jump (platformer left/right are unsupported in /1)
};

struct RecordedAttempt {
    int attemptIndex = 0;
    StartState start;          // where the attempt began (level start or StartPos)
    int startPosIndex = -1;    // -1 = level start
    bool cbf = false;          // Click Between Frames active (sub-tick inputs)
    std::vector<RecordedInput> inputs;
    std::vector<RecordedTick> ticks;    // one per step; may stop early (quit)
    int endStep = 0;
    bool died = false;
    bool completed = false;
    float endPercent = 0.f;
};

}  // namespace gprl::sim
