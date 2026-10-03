#pragma once
// SYNTHETIC physics oracle for host tests: 1-D analytic model where the moved input survives iff
// its time lies in one of the pass intervals. No physics; exercises the search algorithms only.
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "../core/solver/oracle.hpp"

namespace gprl::test {

class SyntheticOracle : public solver::IPhysicsOracle {
public:
    struct Interval {
        double fromMs;
        double toMs;
    };

    SyntheticOracle(std::vector<Interval> passIntervals, size_t movingIndex, double historyStartMs = solver::kNaN)
        : m_pass(std::move(passIntervals)), m_moving(movingIndex), m_historyStart(historyStartMs) {}

    solver::Outcome trial(solver::SnapshotId base, solver::InputSchedule const& schedule, double horizonSeconds) override {
        (void)base;
        (void)horizonSeconds;
        ++m_trials;
        if (m_moving >= schedule.inputs.size()) return solver::Outcome::invalid("no moving input");
        double t = schedule.inputs[m_moving].tMs;
        m_trialTimes.push_back(t);
        if (!std::isnan(m_historyStart) && t < m_historyStart) return solver::Outcome::invalid("before history start");
        if (m_invalidAt >= 0.0 && std::fabs(t - m_invalidAt) < 1e-6) return solver::Outcome::invalid("synthetic invalid point");
        for (auto const& iv : m_pass) {
            if (t >= iv.fromMs && t <= iv.toMs) return m_resync ? solver::Outcome::resynced(t + 50.0) : solver::Outcome::survived();
        }
        return solver::Outcome::died(t + 10.0, 8);
    }

    double historyStartMs(solver::SnapshotId) const override { return m_historyStart; }
    std::string name() const override { return "synthetic-1d"; }

    int trials() const { return m_trials; }
    std::vector<double> const& trialTimes() const { return m_trialTimes; }
    void setResync(bool r) { m_resync = r; }
    void setInvalidAt(double t) { m_invalidAt = t; }
    void reset() { m_trials = 0; m_trialTimes.clear(); }

private:
    std::vector<Interval> m_pass;
    size_t m_moving;
    double m_historyStart;
    int m_trials = 0;
    bool m_resync = false;
    double m_invalidAt = -1.0;
    std::vector<double> m_trialTimes;
};

inline solver::InputSchedule singleInput(double tMs, bool down = true) {
    solver::InputSchedule s;
    s.inputs.push_back({tMs, 1, Button::Jump, down});
    return s;
}

}  // namespace gprl::test
