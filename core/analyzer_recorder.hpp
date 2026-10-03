#pragma once
// Background level analyzer: the PURE bookkeeping behind the passive recorder and the
// frame-pressure sampler (docs/BACKGROUND_ANALYZER_DESIGN.md §4.5 "RecordedTick", AN-D3, AN-D12).
// PURE C++20, no Geode / cocos / GD include; host-tested in tests/analyzer_recorder_tests.cpp.
//
// READ-ONLY RULE (the whole analyzer, AN-D1 / AN-D11): the mod side (src/analyzer/Recorder.cpp)
// only READS player 1 at the hook points and hands plain numbers to the classes below. Nothing
// here can reach the game; nothing the mod adds for the analyzer ever writes a GD field.
//
// Tick boundaries. GD runs one or more processCommands per 240 TPS tick (half ticks, Click Between
// Frames splits: GD_PHYSICS_NOTES "Half ticks"). The recorder therefore counts WHOLE ticks from
// the sum of player 1's PlayerObject::update deltas (1/60 s units, 0.25 per tick): at every
// processCommands pre-hook, when a whole tick has been simulated since the last record, the state
// read there is the END state of that tick (after its collisions and rotation - the same place
// the live solver's ring captures, "after collisions of the previous step"). An input's step is
// the tick in progress; its sub-tick is the fraction of that tick already simulated (0 on a tick
// boundary, 0.5 in a GD half tick, arbitrary under CBF) - the same figure as the tracker's
// SubTickClock (core/classify.hpp), computed from the same deltas.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "sim/world.hpp"

namespace gprl::analyzer {

struct RecorderLimits {
    int maxTicksPerAttempt = 60000;    // 4 minutes at 240 TPS (owner cap)
    int maxAttemptsPerVisit = 40;      // per level visit: the best ones (completed, then longest)
    int minTicksToKeep = 24;           // 0.1 s: shorter attempts verify nothing
    int extraCompletedAfterCap = 10;   // completed attempts still handed over after the cap
    double tickUnits = 0.25;           // one 240 TPS tick in PlayerObject::update units (1/60 s)
    double tickEpsilon = 1e-4;         // float sums of sub-step deltas
};
constexpr RecorderLimits kRecorderLimits{};

/// What the mod READ from player 1 at a tick boundary (src/analyzer/Recorder.cpp names the
/// binding member of every field).
struct PlayerReading {
    float x = 0.f, y = 0.f;            // getPosition()
    double yVelocity = 0.0;            // m_yVelocity
    bool upsideDown = false;           // m_isUpsideDown
    sim::Gamemode mode = sim::Gamemode::Cube;   // m_isShip / m_isBall / m_isBird / m_isDart / m_isRobot / m_isSpider / m_isSwing
    bool mini = false;                 // m_vehicleSize < 1
    sim::Speed speed = sim::Speed::Normal;      // m_playerSpeed
    bool onGround = false;             // m_isOnGround
    bool held = false;                 // m_holdingButtons[1]
    bool dead = false;                 // m_isDead
};

inline sim::RecordedTick toTick(PlayerReading const& r, int step) {
    sim::RecordedTick t;
    t.step = step;
    t.x = r.x;
    t.y = r.y;
    t.yVelocity = static_cast<float>(r.yVelocity);
    t.upsideDown = r.upsideDown;
    t.mode = r.mode;
    t.mini = r.mini;
    t.speed = r.speed;
    t.onGround = r.onGround;
    t.held = r.held;
    t.dead = r.dead;
    return t;
}

/// Side facts of a recorded attempt the World resolves later (the StartPos index needs the
/// extracted StartPos list, which may not exist yet when the attempt starts).
struct AttemptMeta {
    bool hasStartPos = false;
    float startPosX = 0.f;             // StartPosObject getPositionX() of the active StartPos
    bool truncated = false;            // the tick cap cut it
};

/// One attempt of player 1, recorded passively.
class AttemptRecorder {
public:
    AttemptRecorder() : AttemptRecorder(kRecorderLimits) {}   // not explicit: usable in aggregate `State{}` members
    explicit AttemptRecorder(RecorderLimits const& lim) : m_lim(lim) {}

    /// Attempt start (after PlayLayer::resetLevel): the state GD placed player 1 in.
    void begin(sim::StartState const& start, int attemptIndex, bool cbf, AttemptMeta meta = {}) {
        m_attempt = sim::RecordedAttempt{};
        m_attempt.attemptIndex = attemptIndex;
        m_attempt.start = start;
        m_attempt.startPosIndex = -1;
        m_attempt.cbf = cbf;
        m_attempt.ticks.reserve(2048);
        m_meta = meta;
        m_meta.truncated = false;
        m_elapsed = 0.0;
        m_active = true;
    }

    bool active() const { return m_active; }
    void abandon() { m_active = false; m_attempt = {}; }

    /// PlayerObject::update of player 1 (post-hook), dt in 1/60 s units.
    void onPlayerUpdate(double dt) {
        if (!m_active || !(dt > 0.0) || !std::isfinite(dt)) return;
        m_elapsed += dt;
    }

    /// processCommands pre-hook: true when a whole tick finished since the last record (the caller
    /// then reads player 1 and calls recordTick; reading only then keeps the hook cheap).
    bool wantsTick() const { return m_active && m_elapsed >= m_lim.tickUnits - m_lim.tickEpsilon; }

    /// The end state of the tick that just finished (call only when wantsTick()).
    void recordTick(PlayerReading const& r) {
        if (!wantsTick()) return;
        m_elapsed -= m_lim.tickUnits;
        if (m_elapsed < m_lim.tickEpsilon) m_elapsed = 0.0;
        // a frame that ran several whole ticks between two pre-hooks cannot happen in GD (one
        // processCommands per sub-step); a stalled sum is clamped so a hitch never fakes ticks
        if (m_elapsed >= m_lim.tickUnits) m_elapsed = std::fmod(m_elapsed, m_lim.tickUnits);
        if (static_cast<int>(m_attempt.ticks.size()) >= m_lim.maxTicksPerAttempt) {
            m_meta.truncated = true;
            return;
        }
        int step = static_cast<int>(m_attempt.ticks.size()) + 1;
        m_attempt.ticks.push_back(toTick(r, step));
    }

    /// pushButton / releaseButton of player 1 (jump), BEFORE GD applies it.
    void onInput(bool down) {
        if (!m_active || m_meta.truncated) return;
        sim::RecordedInput in;
        in.step = static_cast<int>(m_attempt.ticks.size()) + 1;   // the tick in progress
        in.subTick = subTick();
        in.down = down;
        in.button = 1;
        if (in.step > m_lim.maxTicksPerAttempt) return;
        m_attempt.inputs.push_back(in);
    }

    /// Fraction of the tick in progress already simulated, [0, 1).
    double subTick() const {
        if (!(m_lim.tickUnits > 0.0)) return 0.0;
        double f = m_elapsed / m_lim.tickUnits;
        if (f < 1e-9) return 0.0;
        return std::min(f, 0.999999);
    }

    /// Attempt end. `final` = player 1 read right after the death / completion (the end state of
    /// the tick it happened in), recorded when that tick finished; nullptr = no reading.
    sim::RecordedAttempt finish(bool died, bool completed, float endPercent, PlayerReading const* final = nullptr) {
        if (!m_active) return {};
        if (final && wantsTick()) recordTick(*final);
        m_active = false;
        m_attempt.died = died;
        m_attempt.completed = completed;
        m_attempt.endPercent = endPercent;
        m_attempt.endStep = static_cast<int>(m_attempt.ticks.size());
        sim::RecordedAttempt out = std::move(m_attempt);
        m_attempt = {};
        return out;
    }

    int ticks() const { return static_cast<int>(m_attempt.ticks.size()); }
    int inputs() const { return static_cast<int>(m_attempt.inputs.size()); }
    AttemptMeta const& meta() const { return m_meta; }
    double elapsedUnits() const { return m_elapsed; }

private:
    RecorderLimits m_lim;
    sim::RecordedAttempt m_attempt;
    AttemptMeta m_meta;
    double m_elapsed = 0.0;
    bool m_active = false;
};

/// "Completed first, then longer" (the attempts worth verifying).
inline bool betterAttempt(sim::RecordedAttempt const& a, sim::RecordedAttempt const& b) {
    if (a.completed != b.completed) return a.completed;
    return a.ticks.size() > b.ticks.size();
}

/// An attempt waiting for the hand-over, with the facts the World resolves later.
struct KeptAttempt {
    sim::RecordedAttempt attempt;
    AttemptMeta meta;
};

/// The per-visit keep policy (owner cap: 40 attempts per level visit, keep the longest / completed
/// ones). Before the World exists the attempts wait here and the best `maxAttemptsPerVisit` are
/// kept; at the hand-over they all go to the worker at once (`takeAll`), and from then on every
/// new attempt is admitted alone (`admit`) while the visit has handed over fewer than the cap, plus
/// up to `extraCompletedAfterCap` completed ones (a job cannot give an attempt back).
class AttemptStore {
public:
    AttemptStore() : AttemptStore(kRecorderLimits) {}
    explicit AttemptStore(RecorderLimits const& lim) : m_lim(lim) {}

    void reset() {
        m_kept.clear();
        m_handedOver = false;
        m_handed = 0;
        m_extraCompleted = 0;
        m_dropped = 0;
    }

    /// Before the hand-over: true = kept (maybe evicting the worst one).
    bool offer(sim::RecordedAttempt a, AttemptMeta meta = {}) {
        if (static_cast<int>(a.ticks.size()) < m_lim.minTicksToKeep) {
            ++m_dropped;
            return false;
        }
        if (static_cast<int>(m_kept.size()) < m_lim.maxAttemptsPerVisit) {
            m_kept.push_back({std::move(a), meta});
            return true;
        }
        auto worst = std::min_element(m_kept.begin(), m_kept.end(), [](auto const& x, auto const& y) { return betterAttempt(y.attempt, x.attempt); });
        if (worst == m_kept.end() || !betterAttempt(a, worst->attempt)) {
            ++m_dropped;
            return false;
        }
        *worst = {std::move(a), meta};
        ++m_dropped;   // the evicted one
        return true;
    }

    /// The hand-over (the World was submitted): everything kept, best first.
    std::vector<KeptAttempt> takeAll() {
        std::stable_sort(m_kept.begin(), m_kept.end(), [](auto const& x, auto const& y) { return betterAttempt(x.attempt, y.attempt); });
        std::vector<KeptAttempt> out = std::move(m_kept);
        m_kept.clear();
        m_handedOver = true;
        m_handed += static_cast<int>(out.size());
        return out;
    }

    /// After the hand-over: true = give this attempt to the job (it is counted).
    bool admit(sim::RecordedAttempt const& a) {
        if (static_cast<int>(a.ticks.size()) < m_lim.minTicksToKeep) {
            ++m_dropped;
            return false;
        }
        if (m_handed < m_lim.maxAttemptsPerVisit) {
            ++m_handed;
            return true;
        }
        if (a.completed && m_extraCompleted < m_lim.extraCompletedAfterCap) {
            ++m_extraCompleted;
            ++m_handed;
            return true;
        }
        ++m_dropped;
        return false;
    }

    bool handedOver() const { return m_handedOver; }
    int kept() const { return static_cast<int>(m_kept.size()); }
    int handed() const { return m_handed; }
    int dropped() const { return m_dropped; }
    std::vector<KeptAttempt> const& keptAttempts() const { return m_kept; }

private:
    RecorderLimits m_lim;
    std::vector<KeptAttempt> m_kept;
    bool m_handedOver = false;
    int m_handed = 0;
    int m_extraCompleted = 0;
    int m_dropped = 0;
};

// ---- frame pressure (AN-D12) ----

struct PressureConfig {
    int window = 30;                   // "the last 30 frames"
    double factor = 1.5;               // average frame time > 1.5 x the target = pressure
    double emaAlpha = 0.1;             // the EMA shown in the status (not the rule)
    double gapResetMs = 250.0;         // a longer gap is a pause / load / alt-tab: the window restarts
    double minTargetMs = 1.0;          // target clamps (an uncapped FPS setting must not make
    double maxTargetMs = 50.0;         //  every frame "pressure")
};
constexpr PressureConfig kPressure{};

/// Target frame time in ms from CCDirector's animation interval (seconds per frame, GD's FPS cap),
/// the display refresh rate (Hz, 0 = unknown) and GD's Vertical Sync option (GameVar "0030"):
///   VSync on  -> the slower of the FPS cap and the refresh rate (the swap waits for the display),
///   VSync off -> the FPS cap alone (the display does not limit the frame rate; review fix
///                2026-10-02: a 240 FPS cap on a 60 Hz monitor targets 4.17 ms, not 16.7 ms).
/// Without a usable FPS cap: the refresh rate when VSync is on, else 60 FPS. Clamped.
inline double targetFrameMs(double animationIntervalSec, double refreshHz, bool vsync, PressureConfig const& c = kPressure) {
    double t = 0.0;
    if (animationIntervalSec > 0.0 && std::isfinite(animationIntervalSec)) t = animationIntervalSec * 1000.0;
    if (vsync && refreshHz > 1.0 && std::isfinite(refreshHz)) t = std::max(t, 1000.0 / refreshHz);
    if (!(t > 0.0)) t = 1000.0 / 60.0;
    return std::clamp(t, c.minTargetMs, c.maxTargetMs);
}

/// Frame-time sampler fed once per rendered level frame (PlayLayer::postUpdate) with the wall time
/// since the previous one.
class FramePressure {
public:
    FramePressure() : FramePressure(kPressure) {}
    explicit FramePressure(PressureConfig const& c) : m_cfg(c), m_target(1000.0 / 60.0) {}

    void setTargetMs(double t) { m_target = std::clamp(t, m_cfg.minTargetMs, m_cfg.maxTargetMs); }
    double targetMs() const { return m_target; }

    void frame(double frameMs) {
        if (!(frameMs >= 0.0) || !std::isfinite(frameMs)) return;
        if (frameMs > m_cfg.gapResetMs) {
            reset();
            return;
        }
        m_window.push_back(frameMs);
        m_sum += frameMs;
        while (static_cast<int>(m_window.size()) > m_cfg.window) {
            m_sum -= m_window.front();
            m_window.pop_front();
        }
        m_ema = m_frames == 0 ? frameMs : m_ema + m_cfg.emaAlpha * (frameMs - m_ema);
        ++m_frames;
    }

    /// The window is full and its average is over factor x target.
    bool pressure() const {
        if (static_cast<int>(m_window.size()) < m_cfg.window) return false;
        return averageMs() > m_cfg.factor * m_target;
    }

    double averageMs() const { return m_window.empty() ? 0.0 : m_sum / static_cast<double>(m_window.size()); }
    double emaMs() const { return m_ema; }
    int samples() const { return static_cast<int>(m_window.size()); }

    void reset() {
        m_window.clear();
        m_sum = 0.0;
        m_frames = 0;
        m_ema = 0.0;
    }

private:
    PressureConfig m_cfg;
    double m_target;
    std::deque<double> m_window;
    double m_sum = 0.0;
    double m_ema = 0.0;
    int m_frames = 0;
};

}  // namespace gprl::analyzer
