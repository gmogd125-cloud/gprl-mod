#pragma once
// SYNTHETIC joint oracle for the sequence solver's host tests: an analytic model where a trial
// survives iff the SHIFTS of a group of inputs (each from its nominal time) satisfy a predicate.
// The predicate knows the joint feasible region exactly, so the expected `jointFeasibleShare` can
// be computed in closed form or by brute force. No physics; exercises the search only.
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../core/solver/oracle.hpp"

namespace gprl::test {

class SyntheticJointOracle : public solver::IPhysicsOracle {
public:
    /// `shiftsMs[i]` = schedule.inputs[group[i]].tMs - nominalMs[i].
    using Feasible = std::function<bool(std::vector<double> const& shiftsMs)>;

    SyntheticJointOracle(std::vector<size_t> group, std::vector<double> nominalMs, Feasible feasible)
        : m_group(std::move(group)), m_nominal(std::move(nominalMs)), m_feasible(std::move(feasible)) {}

    solver::Outcome trial(solver::SnapshotId base, solver::InputSchedule const& schedule, double horizonSeconds) override {
        (void)base;
        (void)horizonSeconds;
        ++m_trials;
        std::vector<double> shifts;
        for (size_t i = 0; i < m_group.size(); ++i) {
            if (m_group[i] >= schedule.inputs.size()) return solver::Outcome::invalid("no such input");
            shifts.push_back(schedule.inputs[m_group[i]].tMs - m_nominal[i]);
        }
        // two group inputs that swapped order are not a schedule the solver may ever ask for
        for (size_t i = 0; i + 1 < m_group.size(); ++i) {
            if (schedule.inputs[m_group[i + 1]].tMs <= schedule.inputs[m_group[i]].tMs) ++m_crossedTrials;
        }
        int moved = 0;
        for (double s : shifts) if (std::fabs(s) > 1e-9) ++moved;
        if (moved >= 2) ++m_jointTrials;
        m_shifts.push_back(shifts);
        if (m_invalidEvery > 0 && moved >= 2 && (m_jointTrials % m_invalidEvery) == 0) return solver::Outcome::invalid("synthetic invalid trial");
        double t = schedule.inputs[m_group.front()].tMs;
        return m_feasible(shifts) ? solver::Outcome::survived() : solver::Outcome::died(t + 10.0, 8);
    }

    /// The history reaches back to time 0: no input is blocked by a missing snapshot.
    double historyStartMs(solver::SnapshotId) const override { return 0.0; }
    std::string name() const override { return "synthetic-joint"; }

    int trials() const { return m_trials; }
    int jointTrials() const { return m_jointTrials; }        // trials with two or more inputs shifted
    int crossedTrials() const { return m_crossedTrials; }    // must stay 0
    std::vector<std::vector<double>> const& shifts() const { return m_shifts; }
    void setInvalidEvery(int n) { m_invalidEvery = n; }

private:
    std::vector<size_t> m_group;
    std::vector<double> m_nominal;
    Feasible m_feasible;
    int m_trials = 0;
    int m_jointTrials = 0;
    int m_crossedTrials = 0;
    int m_invalidEvery = 0;
    std::vector<std::vector<double>> m_shifts;
};

}  // namespace gprl::test
