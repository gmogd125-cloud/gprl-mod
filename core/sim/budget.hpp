#pragma once
// CPU-budget rules of the background analyzer (docs/BACKGROUND_ANALYZER_DESIGN.md §0 AN-D12, §5).
// PURE constexpr rules, no clock and no thread in here: the job (job.cpp) asks them with the
// numbers it measured; the mod (src/analyzer/Worker.cpp) owns the thread, the priority and the
// frame-pressure sampler. Host-tested through tests/sim_job_tests.cpp.
#include <algorithm>
#include <cstdint>

namespace gprl::sim {

struct SimBudgetRules {
    double wallBudgetMs = 10 * 60 * 1000.0;   // the owner: "results can take 10 minutes"
    int yieldEveryTicks = 2000;               // the worker re-checks mayRun() this often (engine ticks)
    double yieldEveryMs = 50.0;               // ... and at least this often by the clock (a slow tick must not hide a pause request)
    int beamWidth = 48;                       // search beam (§4.3)
    int beamWidthMax = 256;                   // cap when the search stalls
    double searchShare = 0.6;                 // share of the wall budget the search may use; the rest is for the windows
    uint64_t searchTickCap = 400'000'000;     // hard cap on search engine ticks (rollouts included), clock independent
    int maxWindows = 4000;                    // the server's cap on windows per result
};

/// The next beam width after a stall: doubled, capped.
constexpr int widenedBeam(int current, int max) {
    if (current <= 0) return max > 0 ? max : 1;
    return current >= max ? max : std::min(max, current * 2);
}
constexpr bool canWiden(int current, int max) { return current < max; }

/// Width after a successful advance (a new checkpoint): decays back towards the base width.
constexpr int relaxedBeam(int current, int base) { return current > base ? std::max(base, current / 2) : base; }

/// Yield rule: by ticks or by wall time since the last poll, whichever comes first.
constexpr bool shouldPoll(uint64_t ticksSincePoll, double msSincePoll, SimBudgetRules const& r) {
    if (r.yieldEveryTicks > 0 && ticksSincePoll >= static_cast<uint64_t>(r.yieldEveryTicks)) return true;
    if (r.yieldEveryMs > 0.0 && msSincePoll >= r.yieldEveryMs) return true;
    return false;
}

constexpr double remainingMs(double budgetMs, double activeMs) { return budgetMs - activeMs > 0.0 ? budgetMs - activeMs : 0.0; }
constexpr bool budgetSpent(double budgetMs, double activeMs) { return budgetMs > 0.0 && activeMs >= budgetMs; }

/// The search must leave room for the windows phase: it stops at this share of the budget.
constexpr double searchBudgetMs(SimBudgetRules const& r) { return r.wallBudgetMs * std::clamp(r.searchShare, 0.1, 1.0); }

/// Upper bound of shifted simulations one window costs: both sides up to maxShift, plus the
/// refinement passes (pointsPerPass inside each bounded bracket per pass), plus the control.
constexpr int maxTrialsPerWindow(int maxShift, int refinePasses, int pointsPerPass = 7) {
    return 1 + 2 * std::max(0, maxShift) + 2 * std::max(0, refinePasses) * std::max(0, pointsPerPass);
}

}  // namespace gprl::sim
