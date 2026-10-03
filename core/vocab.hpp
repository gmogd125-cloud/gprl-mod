#pragma once
// Shared vocabularies, mirrored from shared/src/domain/core.ts, gamemode.ts and
// shared/src/telemetry/schema.ts (SPEC §6, §9, §12). The string names ARE the wire format: they
// must serialise identically on both sides. Pure C++20, no Geode or cocos includes (host-tested).
#include <cstdint>
#include <string_view>

namespace gprl {

// ---- gameplay vocabularies (domain/core.ts, domain/gamemode.ts) ----

enum class Gamemode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };
constexpr int kGamemodeCount = 8;

enum class Speed : uint8_t { Slow, Normal, Fast, Faster, Fastest };
constexpr int kSpeedCount = 5;

enum class Gravity : uint8_t { Normal, Flipped };

enum class InputKind : uint8_t { Press, Release };

// Buttons the mod can observe. Values equal GD's PlayerButton enum (1 = jump, 2 = left, 3 = right).
enum class Button : uint8_t { Jump = 1, Left = 2, Right = 3 };

enum class AttemptEndReason : uint8_t { Death, Complete, Exit, Restart };

// ---- fingerprint vocabularies (telemetry/schema.ts TimingFingerprint) ----

enum class Trajectory : uint8_t { Rising, Apex, Falling, Grounded, Ceiling };
enum class HorizontalState : uint8_t { Ground, Air, Ceiling, Dash };
enum class PortalTransition : uint8_t { None, Gravity, Gamemode, Speed, Size, Mirror, Dual };
enum class InputDirection : uint8_t { Up, Down, Flip, None };

// ---- environment / session vocabularies (schema.ts EnvironmentEvent, api/contracts.ts) ----

/// Integrity summary, same vocabulary as the moderation UI (domain/states.ts IntegrityStatus and
/// telemetry/schema.ts ENVIRONMENT_INTEGRITY). Client-side summary only; the server re-classifies.
enum class Integrity : uint8_t { Clean, Warnings, Flagged, Unknown };
enum class SessionEndReason : uint8_t { LevelExit, GameExit, Timeout };

/// Local trust classification of the running environment (SPEC §21). NOT part of the telemetry
/// schema: the server re-classifies from the mod list; the mod only uses this for the HUD, the
/// popup and the informational `attempt_end.legit` flag.
enum class TrustState : uint8_t { Allowed, NoclipModified, Botting, PhysicsChanged, UnknownMod };

// ---- names (wire strings) ----

constexpr std::string_view name(Gamemode g) {
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
    return "cube";
}

constexpr std::string_view name(Speed s) {
    switch (s) {
        case Speed::Slow: return "slow";
        case Speed::Normal: return "normal";
        case Speed::Fast: return "fast";
        case Speed::Faster: return "faster";
        case Speed::Fastest: return "fastest";
    }
    return "normal";
}

constexpr std::string_view name(Gravity g) { return g == Gravity::Flipped ? "flipped" : "normal"; }
constexpr std::string_view name(InputKind k) { return k == InputKind::Release ? "release" : "press"; }

constexpr std::string_view name(Button b) {
    switch (b) {
        case Button::Jump: return "jump";
        case Button::Left: return "left";
        case Button::Right: return "right";
    }
    return "jump";
}

constexpr std::string_view name(AttemptEndReason r) {
    switch (r) {
        case AttemptEndReason::Death: return "death";
        case AttemptEndReason::Complete: return "complete";
        case AttemptEndReason::Exit: return "exit";
        case AttemptEndReason::Restart: return "restart";
    }
    return "exit";
}

constexpr std::string_view name(Trajectory t) {
    switch (t) {
        case Trajectory::Rising: return "rising";
        case Trajectory::Apex: return "apex";
        case Trajectory::Falling: return "falling";
        case Trajectory::Grounded: return "grounded";
        case Trajectory::Ceiling: return "ceiling";
    }
    return "grounded";
}

constexpr std::string_view name(HorizontalState h) {
    switch (h) {
        case HorizontalState::Ground: return "ground";
        case HorizontalState::Air: return "air";
        case HorizontalState::Ceiling: return "ceiling";
        case HorizontalState::Dash: return "dash";
    }
    return "ground";
}

constexpr std::string_view name(PortalTransition p) {
    switch (p) {
        case PortalTransition::None: return "none";
        case PortalTransition::Gravity: return "gravity";
        case PortalTransition::Gamemode: return "gamemode";
        case PortalTransition::Speed: return "speed";
        case PortalTransition::Size: return "size";
        case PortalTransition::Mirror: return "mirror";
        case PortalTransition::Dual: return "dual";
    }
    return "none";
}

constexpr std::string_view name(InputDirection d) {
    switch (d) {
        case InputDirection::Up: return "up";
        case InputDirection::Down: return "down";
        case InputDirection::Flip: return "flip";
        case InputDirection::None: return "none";
    }
    return "none";
}

constexpr std::string_view name(Integrity i) {
    switch (i) {
        case Integrity::Clean: return "clean";
        case Integrity::Warnings: return "warnings";
        case Integrity::Flagged: return "flagged";
        case Integrity::Unknown: return "unknown";
    }
    return "unknown";
}

constexpr std::string_view name(SessionEndReason r) {
    switch (r) {
        case SessionEndReason::LevelExit: return "level_exit";
        case SessionEndReason::GameExit: return "game_exit";
        case SessionEndReason::Timeout: return "timeout";
    }
    return "level_exit";
}

constexpr std::string_view name(TrustState t) {
    switch (t) {
        case TrustState::Allowed: return "allowed";
        case TrustState::NoclipModified: return "noclip_modified";
        case TrustState::Botting: return "botting";
        case TrustState::PhysicsChanged: return "physics_changed";
        case TrustState::UnknownMod: return "unknown_mod";
    }
    return "allowed";
}

// ---- parsers (wire string -> enum). Generic: walks the enum's range using name(). ----

namespace detail {
template <typename Enum, int Count>
constexpr bool parseEnum(std::string_view s, Enum& out) {
    for (int i = 0; i < Count; ++i) {
        if (name(static_cast<Enum>(i)) == s) {
            out = static_cast<Enum>(i);
            return true;
        }
    }
    return false;
}
}  // namespace detail

inline bool parse(std::string_view s, Gamemode& out) { return detail::parseEnum<Gamemode, kGamemodeCount>(s, out); }
inline bool parse(std::string_view s, Speed& out) { return detail::parseEnum<Speed, kSpeedCount>(s, out); }
inline bool parse(std::string_view s, Gravity& out) { return detail::parseEnum<Gravity, 2>(s, out); }
inline bool parse(std::string_view s, InputKind& out) { return detail::parseEnum<InputKind, 2>(s, out); }
inline bool parse(std::string_view s, AttemptEndReason& out) { return detail::parseEnum<AttemptEndReason, 4>(s, out); }
inline bool parse(std::string_view s, Trajectory& out) { return detail::parseEnum<Trajectory, 5>(s, out); }
inline bool parse(std::string_view s, HorizontalState& out) { return detail::parseEnum<HorizontalState, 4>(s, out); }
inline bool parse(std::string_view s, PortalTransition& out) { return detail::parseEnum<PortalTransition, 7>(s, out); }
inline bool parse(std::string_view s, InputDirection& out) { return detail::parseEnum<InputDirection, 4>(s, out); }
inline bool parse(std::string_view s, Integrity& out) { return detail::parseEnum<Integrity, 4>(s, out); }
inline bool parse(std::string_view s, SessionEndReason& out) { return detail::parseEnum<SessionEndReason, 3>(s, out); }
inline bool parse(std::string_view s, TrustState& out) { return detail::parseEnum<TrustState, 5>(s, out); }

inline bool parse(std::string_view s, Button& out) {
    if (s == "jump") { out = Button::Jump; return true; }
    if (s == "left") { out = Button::Left; return true; }
    if (s == "right") { out = Button::Right; return true; }
    return false;
}

/// GD 2.2 horizontal speed in units per second (SPEED_UNITS_PER_SECOND in shared core.ts).
constexpr double unitsPerSecond(Speed s) {
    switch (s) {
        case Speed::Slow: return 251.16;
        case Speed::Normal: return 311.58;
        case Speed::Fast: return 387.42;
        case Speed::Faster: return 468.0;
        case Speed::Fastest: return 576.0;
    }
    return 311.58;
}

/// Physics tick length. GD runs 240 physics ticks per second (GD_PHYSICS_NOTES.md).
constexpr double kTicksPerSecond = 240.0;
constexpr double kTickMs = 1000.0 / kTicksPerSecond;

/// Frames at 240 FPS are a DISPLAY conversion of a window stored in ms (ARCHITECTURE §5,
/// shared telemetry/units.ts msToFrames240).
constexpr double framesAt240(double ms) { return ms / kTickMs; }

}  // namespace gprl
