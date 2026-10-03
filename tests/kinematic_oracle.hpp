#pragma once
// DEV FIXTURE physics (NOT Geometry Dash): a deterministic 240 Hz world for the v2 solver host
// tests (docs/TIMING_SOLVER_V2.md §5). Wave (+-v slope by speed and mini), ship (thrust / gravity,
// velocity caps, solid floor and ceiling), cube (jump impulse, gravity, ground), corridors (floor /
// ceiling polylines), spikes (rects), portals at x (speed, gravity, mini, gamemode), an optional
// sub-tick split of a step at an input's fraction (Click Between Frames), snapshot / restore of its
// state and trajectory output. It implements IPhysicsOracle so the reference drivers
// (runAgainstOracle, runSAAgainstOracle) run the planners against it.
//
// Trial semantics (mirrors the engine, SOLVER_DESIGN §3.3 and TIMING_SOLVER_V2 §2.3):
//   - an input at tMs applies at the start of tick round(tMs / tick) without sub-tick placement,
//     inside the tick at its fraction with it; several inputs in one tick keep their schedule order
//   - death = end of the tick the player died in (Outcome::died(end ms, object id))
//   - Resynced = after every input that differs from the REFERENCE schedule was applied, the state
//     equals the reference run's state at the same tick for `convergeTicks` consecutive ticks
//     (Outcome::resynced(end ms of the first tick of that streak))
//   - Survived = alive at the horizon: the earliest time any input differs from the reference +
//     horizonSeconds (never before the last differing input + one tick)
//   - snapshot / restore (AUDIT §4, §17): `snapshotAt` captures the state after k ticks, `setBase`
//     makes every trial start from such a (possibly perturbed) state instead of the level start,
//     `replayFrom` replays the ORIGINAL inputs from it and compares every tick with the recorded
//     reference run (the engine's lockstep control rule, D7) - exact equality, no tolerance.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../core/solver/oracle.hpp"

namespace gprl::test::kin {

enum class Mode : uint8_t { Cube, Ship, Wave };

struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int id = 8;
    double vx = 0.0;   // DEV FIXTURE moving hazard (docs/SHIP_SOLVER.md §7 #9): the rect moves vx units per tick
    double vy = 0.0;
};

struct Portal {
    enum class Kind : uint8_t { Speed, Gravity, Mini, GameMode };
    double x = 0.0;
    Kind kind = Kind::Speed;
    double speed = 1.0;        // Speed
    bool value = false;        // Gravity: flipped, Mini: mini
    Mode mode = Mode::Wave;    // GameMode
    int id = 200;
};

/// Piecewise-linear boundary y(x); outside the points the end values hold. Empty = none.
struct Polyline {
    std::vector<std::pair<double, double>> pts;
    bool empty() const { return pts.empty(); }
    double at(double x) const {
        if (pts.empty()) return 0.0;
        if (x <= pts.front().first) return pts.front().second;
        if (x >= pts.back().first) return pts.back().second;
        auto it = std::upper_bound(pts.begin(), pts.end(), x, [](double v, auto const& p) { return v < p.first; });
        auto const& b = *it;
        auto const& a = *(it - 1);
        double u = (x - a.first) / (b.first - a.first);
        return a.second + u * (b.second - a.second);
    }
};

struct World {
    Mode startMode = Mode::Wave;
    double baseVx = 1.3;          // x units per tick at speed 1
    double startSpeed = 1.0;
    double waveSlope = 1.0;       // |vy| / vx for a normal wave (mini: x2)
    double shipThrust = 0.08;     // vy change per tick while holding (units / tick^2)
    double shipGravity = 0.07;
    double shipMaxVy = 1.6;
    double cubeJump = 2.6;        // vy set on a grounded press
    double cubeGravity = 0.09;
    double groundY = 0.0;         // cube ground (no floor below it)
    double half = 3.0;            // hitbox half size (mini: x0.6)
    Polyline floor;               // lethal for wave / cube, solid (slide) for ship
    Polyline ceil;
    std::vector<Rect> spikes;
    std::vector<Portal> portals;  // applied at the end of the tick the player crosses x
    double startY = 0.0;
    int corridorFloorId = 1;
    int corridorCeilId = 2;
};

struct State {
    int64_t tick = 0;          // ticks completed
    double x = 0.0, y = 0.0, vy = 0.0;
    Mode mode = Mode::Wave;
    double speed = 1.0;
    bool flipped = false;
    bool mini = false;
    bool holding = false;
    bool onGround = false;
    bool dead = false;
    int killer = -1;
    size_t portalsPassed = 0;

    bool samePhysics(State const& o) const {
        return std::fabs(x - o.x) < 1e-9 && std::fabs(y - o.y) < 1e-9 && std::fabs(vy - o.vy) < 1e-9 && mode == o.mode && speed == o.speed
            && flipped == o.flipped && mini == o.mini && holding == o.holding && onGround == o.onGround;
    }
    /// Bit-for-bit equality of every field (the snapshot round trip and the control rule).
    bool identical(State const& o) const {
        return tick == o.tick && x == o.x && y == o.y && vy == o.vy && mode == o.mode && speed == o.speed && flipped == o.flipped && mini == o.mini
            && holding == o.holding && onGround == o.onGround && dead == o.dead && killer == o.killer && portalsPassed == o.portalsPassed;
    }
};

class KinematicOracle : public solver::IPhysicsOracle {
public:
    explicit KinematicOracle(World w, bool subtick = false) : m_w(std::move(w)), m_subtick(subtick) {}

    void setReference(solver::InputSchedule s) {
        m_ref = std::move(s);
        m_refStates.clear();
        m_refValid = false;
    }
    void setSubtick(bool on) { m_subtick = on; }
    void setConvergeTicks(int n) { m_converge = n; }
    /// Every later trial starts from `base` (a snapshot, see snapshotAt) instead of the level
    /// start; the schedule's inputs before base.tick are part of the snapshot. nullopt = the start.
    void setBase(std::optional<State> base) { m_base = std::move(base); }
    World const& world() const { return m_w; }
    World& world() { return m_w; }
    int trials() const { return m_trials; }
    void resetCount() { m_trials = 0; }
    int64_t steps() const { return m_steps; }

    State initial() const {
        State s;
        s.x = 0.0;
        s.y = m_w.startY;
        s.mode = m_w.startMode;
        s.speed = m_w.startSpeed;
        s.onGround = m_w.startMode == Mode::Cube && m_w.startY <= m_w.groundY + 1e-12;
        return s;
    }

    /// One tick with the inputs that apply in it: (fraction in [0,1), down), in order.
    void step(State& s, std::vector<std::pair<double, bool>> const& inputs) const {
        if (s.dead) return;
        double done = 0.0;
        for (auto const& [frac, down] : inputs) {
            double f = m_subtick ? std::clamp(frac, 0.0, 1.0) : 0.0;
            if (f > done + 1e-12) {
                advance(s, f - done);
                done = f;
                if (s.dead) break;
            }
            press(s, down);
        }
        if (!s.dead && done < 1.0) advance(s, 1.0 - done);
        if (!s.dead) portals(s);
        ++s.tick;
    }

    /// The whole run of `schedule` from the start to `endTick` (exclusive), every state after each tick.
    std::vector<State> run(solver::InputSchedule const& schedule, int64_t endTick) const { return runFrom(initial(), schedule, endTick); }

    /// Snapshot: the state after `tick` ticks of `schedule` from the level start (a copy of every field).
    State snapshotAt(solver::InputSchedule const& schedule, int64_t tick) const {
        auto states = run(schedule, tick);
        return states.empty() ? initial() : states.back();
    }

    /// Restore: continue `schedule` from `s` (its inputs before s.tick are part of the snapshot)
    /// to `endTick` (exclusive); every state after each tick.
    std::vector<State> runFrom(State s, solver::InputSchedule const& schedule, int64_t endTick) const {
        std::vector<State> out;
        auto evs = events(schedule);
        size_t next = 0;
        while (next < evs.size() && evs[next].tick < s.tick) ++next;
        for (int64_t k = s.tick; k < endTick && !s.dead; ++k) {
            std::vector<std::pair<double, bool>> in;
            while (next < evs.size() && evs[next].tick == k) {
                in.emplace_back(evs[next].frac, evs[next].down);
                ++next;
            }
            step(s, in);
            out.push_back(s);
        }
        return out;
    }

    /// The control rule (D7) for a restored snapshot: replay the ORIGINAL inputs (the reference
    /// schedule) from `base` to `endTick` and compare every tick with the recorded reference run,
    /// field for field, no tolerance. Returns the first tick that differs (-1 = exact everywhere).
    int64_t replayMismatchFrom(State const& base, int64_t endTick) {
        ensureReference(endTick + 1);
        auto replay = runFrom(base, m_ref, endTick);
        for (auto const& st : replay) {
            size_t k = static_cast<size_t>(st.tick - 1);
            if (k >= m_refStates.size() || !st.identical(m_refStates[k])) return st.tick;
        }
        return -1;
    }

    /// A deviation sample of a trial from the REFERENCE run after one tick (trial minus reference).
    struct Dev {
        double frame = 0.0;   // frame at the END of the tick
        double dy = 0.0;
        double dvy = 0.0;
        bool discrete = false;   // mode, speed, gravity, size, hold and ground flag equal the reference's (rejoin.hpp)
        bool exact = false;      // samePhysics with the reference
        bool dead = false;       // the trial died in this tick (the sample is its state at the death)
    };

    solver::Outcome trial(solver::SnapshotId base, solver::InputSchedule const& schedule, double horizonSeconds) override {
        return trialImpl(base, schedule, horizonSeconds, nullptr, 0.0);
    }

    /// trial() plus the deviation from the reference run per tick from `fromMs` on (the
    /// compensation planner's response model, docs/SHIP_SOLVER.md §4.2).
    solver::Outcome trialTrace(solver::InputSchedule const& schedule, double horizonSeconds, double fromMs, std::vector<Dev>& dev) {
        dev.clear();
        return trialImpl(0, schedule, horizonSeconds, &dev, fromMs);
    }

    solver::Outcome trialImpl(solver::SnapshotId base, solver::InputSchedule const& schedule, double horizonSeconds, std::vector<Dev>* dev, double fromMs) {
        (void)base;
        ++m_trials;
        double const tick = kTickMs;
        // earliest difference to the reference and the last differing input
        double firstDiff = std::numeric_limits<double>::infinity();
        double lastDiff = -std::numeric_limits<double>::infinity();
        size_t n = std::min(schedule.inputs.size(), m_ref.inputs.size());
        for (size_t i = 0; i < n; ++i) {
            double a = schedule.inputs[i].tMs, b = m_ref.inputs[i].tMs;
            if (std::fabs(a - b) > 1e-9) {
                firstDiff = std::min({firstDiff, a, b});
                lastDiff = std::max({lastDiff, a, b});
            }
        }
        if (!std::isfinite(firstDiff)) {
            firstDiff = schedule.inputs.empty() ? 0.0 : schedule.inputs.front().tMs;
            lastDiff = firstDiff;
        }
        double endMs = std::max(firstDiff + horizonSeconds * 1000.0, lastDiff + tick);
        int64_t endTick = static_cast<int64_t>(std::ceil(endMs / tick - 1e-9));
        ensureReference(endTick + 2);
        State s = m_base ? *m_base : initial();
        auto evs = events(schedule);
        size_t next = 0;
        while (next < evs.size() && evs[next].tick < s.tick) ++next;   // already part of the snapshot
        int64_t lastDiffTick = static_cast<int64_t>(std::floor(lastDiff / tick + (m_subtick ? 0.0 : 0.5) + 1e-9));
        int streak = 0;
        int64_t streakStart = -1;
        for (int64_t k = s.tick; k < endTick; ++k) {
            std::vector<std::pair<double, bool>> in;
            while (next < evs.size() && evs[next].tick == k) {
                in.emplace_back(evs[next].frac, evs[next].down);
                ++next;
            }
            step(s, in);
            ++m_steps;
            if (dev && static_cast<double>(k + 1) * tick >= fromMs - 1e-9 && static_cast<size_t>(k) < m_refStates.size()) {
                auto const& r = m_refStates[static_cast<size_t>(k)];
                bool const discrete = s.mode == r.mode && s.speed == r.speed && s.flipped == r.flipped && s.mini == r.mini && s.holding == r.holding
                                   && s.onGround == r.onGround;
                dev->push_back({static_cast<double>(k + 1), s.y - r.y, s.vy - r.vy, discrete, !s.dead && s.samePhysics(r), s.dead});
            }
            if (s.dead) return solver::Outcome::died(static_cast<double>(k + 1) * tick, s.killer);
            if (k >= lastDiffTick && static_cast<size_t>(k) < m_refStates.size()) {
                if (s.samePhysics(m_refStates[static_cast<size_t>(k)])) {
                    if (streak == 0) streakStart = k;
                    if (++streak >= m_converge) return solver::Outcome::resynced(static_cast<double>(streakStart + 1) * tick);
                }
                else streak = 0;
            }
        }
        return solver::Outcome::survived();
    }

    double historyStartMs(solver::SnapshotId) const override { return 0.0; }
    std::string name() const override { return "kinematic-240hz (DEV FIXTURE physics, not GD)"; }

    /// Brute-force pass test of one schedule (the ground truth the planners are compared with).
    bool passes(solver::InputSchedule const& schedule, double horizonSeconds) { return trial(0, schedule, horizonSeconds).passed(); }

private:
    struct Ev {
        int64_t tick;
        double frac;
        bool down;
        size_t order;
    };

    std::vector<Ev> events(solver::InputSchedule const& schedule) const {
        std::vector<Ev> out;
        for (size_t i = 0; i < schedule.inputs.size(); ++i) {
            double f = schedule.inputs[i].tMs / kTickMs;
            Ev e;
            if (m_subtick) {
                e.tick = static_cast<int64_t>(std::floor(f + 1e-9));
                e.frac = f - static_cast<double>(e.tick);
                if (e.frac < 1e-9) e.frac = 0.0;
            }
            else {
                e.tick = static_cast<int64_t>(std::floor(f + 0.5 + 1e-9));
                e.frac = 0.0;
            }
            e.down = schedule.inputs[i].down;
            e.order = i;
            out.push_back(e);
        }
        std::stable_sort(out.begin(), out.end(), [](Ev const& a, Ev const& b) { return a.tick < b.tick || (a.tick == b.tick && a.frac < b.frac); });
        return out;
    }

    void ensureReference(int64_t endTick) {
        if (m_refValid && static_cast<int64_t>(m_refStates.size()) >= endTick) return;
        // the reference never dies before the look-ahead in a sensible test; keep the states it has
        m_refStates = run(m_ref, endTick);
        m_refValid = true;
    }

    double hitHalf(State const& s) const { return s.mini ? m_w.half * 0.6 : m_w.half; }

    void press(State& s, bool down) const {
        s.holding = down;
        if (s.mode == Mode::Cube && down && s.onGround) {
            s.vy = m_w.cubeJump * (s.flipped ? -1.0 : 1.0);
            s.onGround = false;
        }
    }

    void advance(State& s, double frac) const {
        double vx = m_w.baseVx * s.speed;
        double dir = s.flipped ? -1.0 : 1.0;
        switch (s.mode) {
            case Mode::Wave: {
                double slope = m_w.waveSlope * (s.mini ? 2.0 : 1.0);
                s.vy = (s.holding ? 1.0 : -1.0) * dir * vx * slope;
                s.y += s.vy * frac;
                break;
            }
            case Mode::Ship: {
                s.vy += (s.holding ? m_w.shipThrust : -m_w.shipGravity) * dir * frac;
                s.vy = std::clamp(s.vy, -m_w.shipMaxVy, m_w.shipMaxVy);
                s.y += s.vy * frac;
                break;
            }
            case Mode::Cube: {
                if (!s.onGround) {
                    s.vy -= m_w.cubeGravity * dir * frac;
                    s.y += s.vy * frac;
                    if (!s.flipped && s.y <= m_w.groundY) {
                        s.y = m_w.groundY;
                        s.vy = 0.0;
                        s.onGround = true;
                    }
                }
                break;
            }
        }
        s.x += vx * frac;
        collide(s);
    }

    void collide(State& s) const {
        double h = hitHalf(s);
        for (auto const& r0 : m_w.spikes) {
            Rect r = r0;
            if (r0.vx != 0.0 || r0.vy != 0.0) {
                // the hazard's position at this tick (time-dependent geometry)
                double const dt = static_cast<double>(s.tick);
                r.x0 += r0.vx * dt; r.x1 += r0.vx * dt;
                r.y0 += r0.vy * dt; r.y1 += r0.vy * dt;
            }
            if (s.x + h > r.x0 && s.x - h < r.x1 && s.y + h > r.y0 && s.y - h < r.y1) {
                s.dead = true;
                s.killer = r.id;
                return;
            }
        }
        if (!m_w.floor.empty()) {
            double f = m_w.floor.at(s.x);
            if (s.y - h <= f) {
                if (s.mode == Mode::Ship) {
                    s.y = f + h;
                    if (s.vy < 0) s.vy = 0.0;
                }
                else {
                    s.dead = true;
                    s.killer = m_w.corridorFloorId;
                    return;
                }
            }
        }
        if (!m_w.ceil.empty()) {
            double c = m_w.ceil.at(s.x);
            if (s.y + h >= c) {
                if (s.mode == Mode::Ship) {
                    s.y = c - h;
                    if (s.vy > 0) s.vy = 0.0;
                }
                else {
                    s.dead = true;
                    s.killer = m_w.corridorCeilId;
                }
            }
        }
    }

    void portals(State& s) const {
        while (s.portalsPassed < m_w.portals.size() && s.x >= m_w.portals[s.portalsPassed].x) {
            auto const& p = m_w.portals[s.portalsPassed];
            switch (p.kind) {
                case Portal::Kind::Speed: s.speed = p.speed; break;
                case Portal::Kind::Gravity: s.flipped = p.value; break;
                case Portal::Kind::Mini: s.mini = p.value; break;
                case Portal::Kind::GameMode:
                    s.mode = p.mode;
                    s.onGround = false;
                    break;
            }
            ++s.portalsPassed;
        }
    }

    World m_w;
    bool m_subtick = false;
    int m_converge = 16;
    solver::InputSchedule m_ref;
    std::vector<State> m_refStates;
    bool m_refValid = false;
    std::optional<State> m_base;
    int m_trials = 0;
    int64_t m_steps = 0;
};

/// A reference path of `schedule` in free space (no floor / ceiling / spikes): the y after every tick.
inline std::vector<State> freePath(World w, solver::InputSchedule const& schedule, int64_t ticks, bool subtick = false) {
    w.floor = {};
    w.ceil = {};
    w.spikes.clear();
    KinematicOracle o(std::move(w), subtick);
    return o.run(schedule, ticks);
}

/// Corridor around a free path: floor = y - below(x), ceiling = y + above(x) (each + the hitbox
/// half, so `below` / `above` are the clearances the reference run has).
template <class Below, class Above>
inline void corridorAround(World& w, std::vector<State> const& path, Below&& below, Above&& above, int every = 1) {
    w.floor.pts.clear();
    w.ceil.pts.clear();
    for (size_t i = 0; i < path.size(); i += static_cast<size_t>(std::max(1, every))) {
        w.floor.pts.emplace_back(path[i].x, path[i].y - below(path[i].x) - w.half);
        w.ceil.pts.emplace_back(path[i].x, path[i].y + above(path[i].x) + w.half);
    }
}

}  // namespace gprl::test::kin
