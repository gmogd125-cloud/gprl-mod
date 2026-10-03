#pragma once
// Performance budget of the lockstep clone engine as ONE config object with a version, plus the
// pure arithmetic that turns measured cost into per-frame budgets (docs/SOLVER_DESIGN.md §6 and
// §12). PURE C++20, no Geode includes; host-tested in tests/tuning_tests.cpp.
//
// Every number the engine uses to decide "how much to simulate this frame" lives here, so a
// change is one diff and the Geode log names the version it ran with (`budget=gprl-clone-budget/3`
// in the ready line).
//
// budget/3 (v0.7.0, docs/TIMING_SOLVER_V2.md §3.2, §3.6, §2.12): the history ring is 2048 steps
// (8.5 s at one step per tick; the sequence-adjusted jobs replay up to ~3 s old snapshots), the
// SA job budget (SABudget: the M4 SequenceConfig budget fields, re-tuned), the replay circuit
// breaker thresholds and the debug-trace caps.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gprl::solver::budget {

/// History ring size in steps (a GD step is 1 or 0.5 ticks). v0.6.x: 1024 (2.1 s worst case).
/// timeline.hpp and CloneEngine read it from here.
constexpr int kHistorySteps = 2048;

struct BudgetConfig {
    char const* version = "gprl-clone-budget/3";
    int historySteps = kHistorySteps;
    // jobs and clones
    int maxJobs = 24;                 // open measurements (v0.4.x: 8; wave / 10 cps spam skipped 3 in 4)
    int maxClones = 320;              // hidden PlayerObjects at most (created lazily, never all at once)
    int poolWarm = 48;                // clones created at level setup (inside the loading screen)
    int cloneCreatesPerFrame = 3;     // PlayerObject::create per rendered frame after the warm-up (~0.7 ms each)
    int spawnDeadlineSteps = 24;      // a pass's shifts still without a clone this many steps later are reported NotTested (pool)
    // clone steps per rendered frame: adaptive from the measured cost per clone step
    double targetSimMsPerFrame = 2.0; // the step budget aims at this much clone sim per rendered frame
    int minStepsPerFrame = 300;       // a whole coarse pass (21 clones) advances >= 14 steps even on a slow machine
    int maxStepsPerFrame = 6000;      // hard cap (v0.4.x fixed cap 4000)
    double defaultStepCostUs = 3.0;   // before anything was measured (in-game 2026-09-30: 2.5-3.1 us)
    double stepCostEmaAlpha = 0.1;    // per rendered frame with at least one clone step
    // the guard that pauses NEW measurements (kept from v0.4.x / FPC): 1 s EMA of clone sim ms per frame
    double throttleOnMs = 4.0;
    double throttleOffMs = 2.0;
    double slowFrameDt = 1.0 / 30.0;  // rendered dt above this skips the input (the game itself hitched)
    // lagging clones
    int catchUpLagSteps = 8;          // a clone this far behind the timeline is "lagging"
    int catchUpStepsPerStep = 8;      // and advances at most this many steps per real step
    int extensionStepsPerFrame = 8;   // synthetic ticks per clone per rendered frame in the death pause
};

constexpr BudgetConfig kBudget{};

/// EMA of the cost of one clone step (microseconds) from a rendered frame's clone sim time.
/// Frames without clone steps leave the estimate alone.
inline double updateStepCostUs(double emaUs, double frameSimMs, int cloneSteps, BudgetConfig const& cfg = kBudget) {
    if (cloneSteps <= 0 || !(frameSimMs >= 0.0)) return emaUs;
    double cost = frameSimMs * 1000.0 / static_cast<double>(cloneSteps);
    if (!(emaUs > 0.0)) return cost;
    return emaUs + cfg.stepCostEmaAlpha * (cost - emaUs);
}

/// Clone steps allowed in one rendered frame for the measured cost: target ms / cost, clamped.
inline int stepBudgetForFrame(double stepCostUs, BudgetConfig const& cfg = kBudget) {
    double cost = stepCostUs > 0.0 && std::isfinite(stepCostUs) ? stepCostUs : cfg.defaultStepCostUs;
    double steps = cfg.targetSimMsPerFrame * 1000.0 / cost;
    if (!std::isfinite(steps)) steps = static_cast<double>(cfg.minStepsPerFrame);
    return std::clamp(static_cast<int>(steps), cfg.minStepsPerFrame, cfg.maxStepsPerFrame);
}

/// Whether a clone may be created now: the warm pool is free, later creations are rationed per
/// rendered frame; the control clone of a pass is never rationed (its lockstep proof needs it at once).
inline bool mayCreateClone(int poolSize, int createdThisFrame, bool forControl, BudgetConfig const& cfg = kBudget) {
    if (poolSize >= cfg.maxClones) return false;
    if (forControl) return true;
    return createdThisFrame < cfg.cloneCreatesPerFrame;
}

/// Catch-up budget of a clone that is `lag` steps behind the real timeline.
inline int catchUpBudget(int lag, BudgetConfig const& cfg = kBudget) {
    return lag > cfg.catchUpLagSteps ? cfg.catchUpStepsPerStep : 0x7fffffff;
}

/// Coverage = windows emitted / inputs that could have had one. Inputs the solver never had a
/// chance with are left out of the denominator: skipped while the player was dead or the level
/// not running, misses whose death no shift within the range could avoid (the death was not
/// caused by that input), and jobs cut by a restart / level end before their horizon.
struct CoverageInput {
    int inputs = 0;
    int emitted = 0;
    int skippedDead = 0;
    int notWindowable = 0;   // no_pass + reset + level_end
};
inline double coveragePercent(CoverageInput const& c) {
    int denom = c.inputs - c.skippedDead - c.notWindowable;
    if (denom <= 0) return c.emitted > 0 ? 100.0 : 0.0;
    return 100.0 * static_cast<double>(std::min(c.emitted, denom)) / static_cast<double>(denom);
}

// ---- budget/3: sequence-adjusted jobs (docs/TIMING_SOLVER_V2.md §3.2, §3.3) ----

/// The SA job's share of the frame: the M4 SequenceConfig budget fields, reused and re-tuned (the
/// load guard `loadAllows` and `stepAllowance` of core/solver/sequence.hpp are unchanged
/// functions, so the M4 proof that local jobs keep >= 65 % of the frame's steps still holds).
/// DEV DEFAULTS until an in-game log says otherwise.
struct SABudget {
    double frameShare = 0.35;         // SA clone steps per rendered frame <= this share of the step budget
    double targetMsPerFrame = 0.75;   // ... and <= this much clone sim per rendered frame (M4: 0.5)
    double idleLoadShare = 0.6;       // only while local + SA expected steps stay under this share
    int parallelClones = 6;           // SA trials running at once (M4: 4)
    int stepsPerClonePerStep = 16;    // a lagging SA clone advances at most this many steps per real step
    int poolReserve = 24;             // idle clones always left for the local jobs
    int cloneCreatesPerFrame = 1;     // PlayerObject::create for SA trials per rendered frame
};
constexpr SABudget kSABudget{};

// ---- budget/3: replay circuit breaker (§3.6, V2-D13) ----

/// An attempt whose shadow keeps mismatching stops creating jobs (they could only be dropped):
/// more than `maxEarlyMismatches` shadow mismatches within the attempt's first `earlySteps` steps,
/// or more than `maxShare` of the steps of any full `shareWindowSteps` window. The shadow keeps
/// running (the evidence); measuring resumes at the restart.
struct ReplayBreakerConfig {
    int maxEarlyMismatches = 20;
    int earlySteps = 480;
    double maxShare = 0.25;
    int shareWindowSteps = 2400;
};
constexpr ReplayBreakerConfig kReplayBreaker{};

/// Pure state machine of the breaker (host-tested in tuning_tests). `step` is called once per
/// compared shadow step with whether it mismatched; it returns true exactly once, at the step it
/// trips. Memory is bounded (a ring of `shareWindowSteps` flags), each call is O(1).
class ReplayBreaker {
public:
    explicit ReplayBreaker(ReplayBreakerConfig cfg = kReplayBreaker) : m_cfg(cfg) {}

    void reset() {
        m_steps = 0;
        m_mismatches = 0;
        m_window.clear();
        m_windowCount = 0;
        m_head = 0;
        m_tripped = false;
        m_trippedEarly = false;
    }

    bool step(bool mismatch) {
        ++m_steps;
        if (mismatch) ++m_mismatches;
        int const n = m_cfg.shareWindowSteps > 0 ? m_cfg.shareWindowSteps : 1;
        if (static_cast<int>(m_window.size()) < n) {
            m_window.push_back(mismatch ? 1 : 0);
            if (mismatch) ++m_windowCount;
        }
        else {
            // ring of the last n steps: drop the oldest, add this one (O(1), bounded memory)
            if (m_window[static_cast<size_t>(m_head)]) --m_windowCount;
            m_window[static_cast<size_t>(m_head)] = mismatch ? 1 : 0;
            if (mismatch) ++m_windowCount;
            m_head = (m_head + 1) % n;
        }
        if (m_tripped) return false;
        bool early = m_steps <= m_cfg.earlySteps && m_mismatches > m_cfg.maxEarlyMismatches;
        bool share = static_cast<int>(m_window.size()) >= n && static_cast<double>(m_windowCount) > m_cfg.maxShare * static_cast<double>(n);
        if (early || share) {
            m_tripped = true;
            m_trippedEarly = early;
            return true;
        }
        return false;
    }

    bool tripped() const { return m_tripped; }
    bool trippedEarly() const { return m_trippedEarly; }
    int steps() const { return m_steps; }
    int mismatches() const { return m_mismatches; }
    int windowMismatches() const { return m_windowCount; }
    ReplayBreakerConfig const& config() const { return m_cfg; }

private:
    ReplayBreakerConfig m_cfg;
    int m_steps = 0;
    int m_mismatches = 0;
    std::vector<uint8_t> m_window;
    int m_windowCount = 0;
    int m_head = 0;
    bool m_tripped = false;
    bool m_trippedEarly = false;
};

// ---- budget/3: debug traces (§2.12) ----

struct TraceCaps {
    int maxTracesPerAttempt = 8;   // traced inputs per attempt at most
    int maxTraceSteps = 160;       // recorded steps per clone at most (append-only, stops at the cap)
    int maxTraceTicksSetting = 40; // `solver-trace-max-ticks` upper bound
    int maxLineBytes = 2048;       // a `GPRL trace:` log line never exceeds this
};
constexpr TraceCaps kTraceCaps{};

}  // namespace gprl::solver::budget
