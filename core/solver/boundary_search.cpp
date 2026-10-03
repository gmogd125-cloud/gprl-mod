#include "boundary_search.hpp"

#include <cmath>
#include <cstdio>

namespace gprl::solver {

namespace {

constexpr double kEps = 1e-9;

void note(BoundaryResult& r, char const* fmt, double a = 0.0, double b = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    r.debug.emplace_back(buf);
}

}  // namespace

BoundarySearch::BoundarySearch(IPhysicsOracle& oracle, BoundarySearchConfig config)
    : m_oracle(oracle), m_config(std::move(config)) {}

Trial BoundarySearch::runTrial(BoundaryResult& r, SnapshotId base, InputSchedule const& schedule, size_t movingIndex,
                               double actualMs, double shiftMs, double horizonSeconds, TrialPhase phase) {
    Trial t;
    t.index = r.trialCount++;
    t.shiftMs = shiftMs;
    t.tMs = actualMs + shiftMs;
    t.phase = phase;
    t.outcome = m_oracle.trial(base, schedule.withMoved(movingIndex, t.tMs), horizonSeconds);
    t.pass = t.outcome.passed();
    r.trials.push_back(t);
    return t;
}

BoundaryResult BoundarySearch::search(SnapshotId base, InputSchedule const& schedule, size_t movingIndex, Side side,
                                      double sideLimitMs, double horizonSeconds) {
    BoundaryResult r;
    r.version = m_config.version;
    r.side = side;
    if (movingIndex >= schedule.inputs.size()) {
        r.valid = false;
        r.invalidReason = "movingIndex out of range";
        return r;
    }
    double const actualMs = schedule.inputs[movingIndex].tMs;
    double const sign = side == Side::Earlier ? -1.0 : 1.0;
    double const step = m_config.tickMs * static_cast<double>(m_config.coarseStepTicks);
    double limit = m_config.maxShiftMs;
    if (!std::isnan(sideLimitMs) && sideLimitMs < limit) limit = sideLimitMs;
    if (limit < 0.0) limit = 0.0;
    r.limitMs = limit;
    r.blocked = limit < step - kEps;
    note(r, "side limit %.4f ms, coarse step %.4f ms", limit, step);
    if (limit <= kEps) {
        note(r, "blocked: no room to move on this side");
        r.passShiftMs = 0.0;
        return r;
    }

    auto budgetLeft = [&] { return r.trialCount < m_config.maxTrials; };
    auto abortInvalid = [&](Trial const& t) {
        r.valid = false;
        r.invalidReason = t.outcome.reason.empty() ? "oracle returned invalid" : t.outcome.reason;
        note(r, "invalid trial at shift %.4f ms", t.shiftMs);
    };

    // ---- 1. coarse scan outward ----
    double lastPass = 0.0;        // |shift| of the last contiguous pass (0 = the actual input)
    double firstFail = kNaN;      // |shift| of the first fail
    int k = 1;
    int islandScanLeft = m_config.islandScanSteps;
    bool inIsland = false;
    Island island{};
    bool scannedLimit = false;
    while (budgetLeft()) {
        double mag = step * static_cast<double>(k);
        TrialPhase phase = std::isnan(firstFail) ? TrialPhase::Coarse : TrialPhase::IslandScan;
        if (mag > limit + kEps) {
            // Past the limit. If the limit is not a multiple of the step, the last relevant point
            // is the limit itself (only while still looking for the first fail).
            if (std::isnan(firstFail) && !scannedLimit && limit - lastPass > kEps) {
                mag = limit;
                phase = TrialPhase::Limit;
                scannedLimit = true;
            }
            else break;
        }
        Trial t = runTrial(r, base, schedule, movingIndex, actualMs, sign * mag, horizonSeconds, phase);
        if (t.outcome.invalid()) { abortInvalid(t); return r; }
        if (std::isnan(firstFail)) {
            if (t.pass) lastPass = mag;
            else {
                firstFail = mag;
                note(r, "first fail at |shift| %.4f ms (last pass %.4f ms)", mag, lastPass);
                if (islandScanLeft <= 0) break;
            }
        }
        else {
            // island scanning beyond the first fail
            if (t.pass) {
                r.nonMonotonic = true;
                if (!inIsland) { inIsland = true; island.fromShiftMs = sign * mag; }
                island.toShiftMs = sign * mag;
            }
            else if (inIsland) {
                r.islands.push_back(island);
                inIsland = false;
            }
            if (--islandScanLeft <= 0) break;
        }
        if (phase == TrialPhase::Limit) break;
        ++k;
    }
    if (inIsland) r.islands.push_back(island);
    if (r.nonMonotonic) note(r, "non-monotonic: %.0f island(s) recorded beyond the first fail", static_cast<double>(r.islands.size()));

    if (std::isnan(firstFail)) {
        // unbounded on this side (or budget ran out while still passing)
        r.bounded = false;
        r.passShiftMs = sign * lastPass;
        r.failShiftMs = kNaN;
        r.bracketMs = 0.0;
        r.budgetExhausted = !budgetLeft() && lastPass + kEps < limit;
        note(r, r.budgetExhausted ? "budget exhausted during the coarse scan at |shift| %.4f ms"
                                  : "unbounded: still passing at the limit (%.4f ms)", lastPass);
        return r;
    }

    // ---- 2. adaptive bisection of [lastPass, firstFail] ----
    double lo = lastPass;   // passes
    double hi = firstFail;  // fails
    while (hi - lo > m_config.resolutionMs && budgetLeft()) {
        double mid = 0.5 * (lo + hi);
        Trial t = runTrial(r, base, schedule, movingIndex, actualMs, sign * mid, horizonSeconds, TrialPhase::Bisect);
        if (t.outcome.invalid()) { abortInvalid(t); return r; }
        if (t.pass) lo = mid;
        else hi = mid;
    }
    r.bounded = true;
    r.passShiftMs = sign * lo;
    r.failShiftMs = sign * hi;
    r.bracketMs = hi - lo;
    r.budgetExhausted = hi - lo > m_config.resolutionMs;
    note(r, r.budgetExhausted ? "budget exhausted: bracket %.4f ms (wanted %.4f ms)"
                              : "bisected to bracket %.4f ms (resolution %.4f ms)", r.bracketMs, m_config.resolutionMs);
    return r;
}

}  // namespace gprl::solver
