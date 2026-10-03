#pragma once
// EXACT REPLAY PARITY (docs/SHIP_SOLVER.md §11.1; owner prompt "SHIP COUNTING / COMPENSATED TIMING
// WINDOW REWRITE" §1, 2026-10-03): before any window of an input is measured the solver proves
// that the recorded inputs, replayed from the saved state, reproduce the real run - the lockstep
// control and the shadow compared with the live player after every step, the delayed control of
// a compensation job compared with the history ring and then with the live player. The engine has
// done that since v0.5 (statesMatch / matchesSnapshot) but only kept a sentence about the first
// difference. This is the same comparison as a pure function with a STRUCTURED answer:
//
//   field        the first field that differs, in the fixed order below
//   real         the real player's value          (flags: 0 / 1, the gamemode: its ordinal)
//   replay       the replayed copy's value
//   delta        replay - real
//
// plus, kept by the caller, the tick (frame) of that first divergence. A result whose replay was
// not exact is `state_replay_failed`: no window, no rating evidence, never an estimate; its
// `parity` block says where the replay first left the real run.
//
// The tolerances are the engine's (position 1e-3 units, velocity 1e-6): unchanged. PURE C++20;
// host-tested in tests/parity_tests.cpp.
#include <cmath>
#include <cstdint>
#include <string>

namespace gprl::solver::parity {

/// What is compared, in comparison order (the first that differs is reported).
enum class Field : uint8_t {
    None = 0,
    X,
    Y,
    VelocityY,
    OnGround,
    Gravity,
    Gamemode,
    Dashing,
    OnSlope,
    Size,
    Speed,
    Held,
    Rings,
    LastX,
    LastY,
    Dual,
    Alive,        // one of the two died and the other did not
};
constexpr char const* name(Field f) {
    switch (f) {
        case Field::None: return "none";
        case Field::X: return "x";
        case Field::Y: return "y";
        case Field::VelocityY: return "y_velocity";
        case Field::OnGround: return "on_ground";
        case Field::Gravity: return "gravity";
        case Field::Gamemode: return "gamemode";
        case Field::Dashing: return "dashing";
        case Field::OnSlope: return "on_slope";
        case Field::Size: return "size";
        case Field::Speed: return "speed";
        case Field::Held: return "held";
        case Field::Rings: return "rings";
        case Field::LastX: return "last_x";
        case Field::LastY: return "last_y";
        case Field::Dual: return "dual";
        case Field::Alive: return "alive";
    }
    return "none";
}

/// The physical state of a player (or a copy) after a step, as plain values.
struct State {
    double x = 0.0, y = 0.0;
    double vy = 0.0;
    bool onGround = false;
    bool upsideDown = false;      // gravity flipped
    int gamemode = 0;             // any stable ordinal (the engine: modeChar)
    bool dashing = false;
    bool onSlope = false;
    double size = 1.0;            // m_vehicleSize (1 normal, 0.6 mini)
    double speed = 0.9;           // m_playerSpeed
    int held = 0;                 // held buttons as a bit set
    int rings = 0;                // touching rings
    double lastX = 0.0, lastY = 0.0;
    bool dual = false;
    bool alive = true;
};

struct Tolerances {
    double position = 1e-3;
    double velocity = 1e-6;
};
inline constexpr Tolerances kTolerances{};

struct Divergence {
    Field field = Field::None;
    double real = 0.0;
    double replay = 0.0;
    double delta = 0.0;
    bool any() const { return field != Field::None; }
};

/// The first field in which `replay` differs from `real` (Field::None = exact).
inline Divergence compare(State const& real, State const& replay, Tolerances const& tol = kTolerances) {
    auto num = [](Field f, double a, double b) { return Divergence{f, a, b, b - a}; };
    auto flag = [](Field f, bool a, bool b) { return Divergence{f, a ? 1.0 : 0.0, b ? 1.0 : 0.0, (b ? 1.0 : 0.0) - (a ? 1.0 : 0.0)}; };
    if (real.alive != replay.alive) return flag(Field::Alive, real.alive, replay.alive);
    if (std::fabs(real.x - replay.x) > tol.position) return num(Field::X, real.x, replay.x);
    if (std::fabs(real.y - replay.y) > tol.position) return num(Field::Y, real.y, replay.y);
    if (std::fabs(real.vy - replay.vy) > tol.velocity) return num(Field::VelocityY, real.vy, replay.vy);
    if (real.onGround != replay.onGround) return flag(Field::OnGround, real.onGround, replay.onGround);
    if (real.upsideDown != replay.upsideDown) return flag(Field::Gravity, real.upsideDown, replay.upsideDown);
    if (real.gamemode != replay.gamemode) return num(Field::Gamemode, real.gamemode, replay.gamemode);
    if (real.dashing != replay.dashing) return flag(Field::Dashing, real.dashing, replay.dashing);
    if (real.onSlope != replay.onSlope) return flag(Field::OnSlope, real.onSlope, replay.onSlope);
    if (real.size != replay.size) return num(Field::Size, real.size, replay.size);
    if (real.speed != replay.speed) return num(Field::Speed, real.speed, replay.speed);
    if (real.held != replay.held) return num(Field::Held, real.held, replay.held);
    if (real.rings != replay.rings) return num(Field::Rings, real.rings, replay.rings);
    if (std::fabs(real.lastX - replay.lastX) > tol.position) return num(Field::LastX, real.lastX, replay.lastX);
    if (std::fabs(real.lastY - replay.lastY) > tol.position) return num(Field::LastY, real.lastY, replay.lastY);
    if (real.dual != replay.dual) return flag(Field::Dual, real.dual, replay.dual);
    return {};
}

/// The first divergence of a replay, with the tick it happened at (the record a result carries).
struct Record {
    bool valid = true;            // false: the replay left the real run
    double tick = 0.0;            // frame (240 Hz ticks since the attempt start) of the first divergence
    Divergence first;
    void note(double atTick, Divergence const& d) {
        if (!valid || !d.any()) return;
        valid = false;
        tick = atTick;
        first = d;
    }
};

}  // namespace gprl::solver::parity
