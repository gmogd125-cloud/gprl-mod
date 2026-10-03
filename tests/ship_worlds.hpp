#pragma once
// Shared helpers of the synthetic SHIP tests (tests/ship_pack_tests.cpp, tests/ship_cases_tests.cpp;
// docs/SHIP_SOLVER.md §7, §11.6) on the kinematic DEV FIXTURE physics (tests/kinematic_oracle.hpp,
// NOT Geometry Dash): schedules, the local + compensation + phase solvers of one input, the brute
// force of a compensated shift, and the fixture worlds (corridors built around a recorded path).
#include "test_util.hpp"
#include "kinematic_oracle.hpp"
#include "comp_oracle.hpp"

#include "../core/solver/compensation.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_status.hpp"
#include "../core/solver/timing_units.hpp"
#include "../core/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace gprl::test::ship {

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::solver::comp;
using namespace gprl::test::kin;

constexpr double T = kTickMs;
constexpr double kH = 0.5 + 10.0 / 240.0;
constexpr double kMargin = 0.0024;

inline InputSchedule schedule(std::vector<std::pair<double, bool>> const& ticks) {
    InputSchedule s;
    for (auto const& [tick, down] : ticks) s.inputs.push_back({tick * T, 1, Button::Jump, down});
    return s;
}
inline double frameOf(InputSchedule const& s, size_t i) { return s.inputs[i].tMs / T; }
inline double earlyLimitOf(InputSchedule const& s, size_t i) {
    double t = frameOf(s, i);
    if (i == 0) return std::min(10.0, t);
    return std::min(10.0, t - frameOf(s, i - 1) - kMargin);
}
inline double lateLimitOf(InputSchedule const& s, size_t i) {
    if (i + 1 >= s.inputs.size()) return kNaN;
    return frameOf(s, i + 1) - frameOf(s, i) - kMargin;
}

inline PassPlanner solveLocal(KinematicOracle& o, InputSchedule const& s, size_t i, bool subtick = false) {
    PlannerConfig pc;
    pc.subtick = subtick;
    PassPlanner p(pc, earlyLimitOf(s, i));
    p.setEarlyLimitKind(i == 0 ? LimitKind::AttemptStart : LimitKind::Neighbour);
    double late = lateLimitOf(s, i);
    if (!std::isnan(late)) p.setLateLimit(late);
    o.setReference(s);
    runAgainstOracle(o, p, 0, s, i, kH);
    return p;
}

inline void oracleTrial(KinematicOracle& o, InputSchedule const& ref, InputSchedule const& sched, CompTrial const& t, CompOutcome& out, CompConfig const& cfg = fixtureComp(),
                        bool subtick = false) {
    compOracleTrial(o, ref, sched, t, out, cfg, subtick);
}

struct Member {
    size_t index = 0;
    PassPlanner local;
    CompPlanner comp;
    WindowResult window;
    SAResult result;
    int trials = 0;
};

inline std::vector<Member> solveAll(KinematicOracle& o, InputSchedule const& s, std::vector<size_t> const& members, bool subtick = false, CompConfig cfg = fixtureComp(),
                                    std::vector<size_t> breaks = {}) {
    std::vector<Member> out;
    for (size_t i : members) {
        Member m;
        m.index = i;
        m.local = solveLocal(o, s, i, subtick);
        m.trials = runCompAgainstOracle(s, i, m.local, cfg, breaks, [&](InputSchedule const& sched, CompTrial const& t, CompOutcome& r) { oracleTrial(o, s, sched, t, r, cfg, subtick); }, m.comp);
        m.window = m.local.result(s.inputs[i].tMs);
        m.result = m.comp.result();
        out.push_back(std::move(m));
    }
    return out;
}

/// The PHASE search of the control press `pressIndex` + its release (the next input): both moved
/// together, the inputs after the release free to compensate (CompMember::rigid).
struct Phase {
    CompPlanner planner;
    SAResult result;
    int trials = 0;
};
inline Phase solvePhase(KinematicOracle& o, InputSchedule const& s, size_t pressIndex, CompConfig cfg = fixtureComp()) {
    Phase p;
    p.trials = runCompAgainstOracle(s, pressIndex, nullptr, cfg, {}, [&](InputSchedule const& sched, CompTrial const& t, CompOutcome& r) { oracleTrial(o, s, sched, t, r, cfg, false); }, p.planner, 1);
    p.result = p.planner.result();
    return p;
}

inline double localWidthTicks(WindowResult const& w) { return (w.latestMs - w.earliestMs) / T; }
/// FRAMES AVAILABLE (edge to edge; the count of ticks the input could have been made in) of the
/// local and of the compensated window: the convention of the control card (control_card.hpp).
inline double localFrames(WindowResult const& w) { return localEdgeFrames(w.late) - localEdgeFrames(w.early); }
inline double compFrames(SAResult const& r) { return r.sequence.late.edgeFrames() - r.sequence.early.edgeFrames(); }
inline double widthTicks(SAWindow const& w) { return w.late.edgeFrames() - w.early.edgeFrames(); }
inline double localEdgeTicks(BoundaryResult const& b) { return localEdgeFrames(b); }
inline bool downstream(WindowResult const& w) {
    return (w.early.bounded && w.early.edge.cause == EdgeCause::Downstream) || (w.late.bounded && w.late.edge.cause == EdgeCause::Downstream);
}

inline void printMember(char const* label, InputSchedule const& s, Member const& m) {
    std::printf("  %s #%zu %s local [%+.2f,%+.2f] %.2f f%s | comp [%+.2f,%+.2f] %.2f f %s %s/%s trials %d | %s\n      local: %s\n", label, m.index,
                s.inputs[m.index].down ? "press  " : "release", localEdgeTicks(m.window.early), localEdgeTicks(m.window.late), localWidthTicks(m.window),
                downstream(m.window) ? " (downstream)" : "", m.result.sequence.early.edgeFrames(), m.result.sequence.late.edgeFrames(), widthTicks(m.result.sequence),
                m.result.decided ? "decided" : "UNDECIDED", name(m.result.sequence.early.proof), name(m.result.sequence.late.proof), m.trials,
                m.result.debug.empty() ? "" : m.result.debug[0].c_str(), m.local.describe().c_str());
}

/// One compensated schedule judged like a planner trial: the member at +shift, the first
/// offsets.size() later inputs at their recorded time + offset. false = not a legal schedule (the
/// input order would change); else `out` is the trial's outcome.
inline bool compScheduleRun(KinematicOracle& o, InputSchedule const& s, size_t i, double shift, std::vector<double> const& offsets, CompConfig const& cfg, size_t rigid, CompOutcome& out) {
    CompTrial t;
    t.shiftFrames = shift;
    t.followers = static_cast<int>(offsets.size());
    InputSchedule m = s;
    double last = frameOf(s, i) + shift;
    double prev = last;
    t.moved.push_back({static_cast<uint32_t>(i + 1), s.inputs[i].down, last});
    m.inputs[i].tMs = last * T;
    for (size_t r = 1; r <= rigid; ++r) {
        if (i + r >= s.inputs.size()) return false;
        prev = frameOf(s, i + r) + shift;
        t.moved.push_back({static_cast<uint32_t>(i + r + 1), s.inputs[i + r].down, prev});
        m.inputs[i + r].tMs = prev * T;
        last = std::max(last, prev);
    }
    if (i > 0 && frameOf(s, i) + shift <= frameOf(s, i - 1) + cfg.neighbourMarginFrames) return false;   // the past is fixed
    for (size_t j = 0; j < offsets.size(); ++j) {
        size_t idx = i + rigid + 1 + j;
        if (idx >= s.inputs.size()) return false;
        double f = frameOf(s, idx) + offsets[j];
        if (f <= prev + cfg.neighbourMarginFrames) return false;
        if (j + 1 == offsets.size() && idx + 1 < s.inputs.size() && f >= frameOf(s, idx + 1) - cfg.neighbourMarginFrames) return false;
        t.moved.push_back({static_cast<uint32_t>(idx + 1), s.inputs[idx].down, f});
        m.inputs[idx].tMs = f * T;
        prev = f;
        last = std::max(last, f);
    }
    if (offsets.empty()) {
        size_t next = i + rigid + 1;
        if (next < s.inputs.size() && prev >= frameOf(s, next) - cfg.neighbourMarginFrames) return false;
    }
    t.offsetsFrames = offsets;
    t.lastMovedFrame = last;
    t.lookAheadFrame = std::max(last + cfg.lookAheadFrames, last + cfg.convergeSteps + 1.0);
    t.attributeAfterFrame = frameOf(s, i);
    t.devFromFrame = std::min(frameOf(s, i) + shift, frameOf(s, i));
    compOracleTrial(o, s, m, t, out, cfg, false);
    return true;
}
/// true = alive AND re-joined (a pass of the compensated window).
inline bool compSchedulePasses(KinematicOracle& o, InputSchedule const& s, size_t i, double shift, std::vector<double> const& offsets, CompConfig const& cfg, size_t rigid = 0) {
    CompOutcome out;
    if (!compScheduleRun(o, s, i, shift, offsets, cfg, rigid, out)) return false;
    return out.kind == CompOutcome::Kind::Pass && rejoin::rejoined(out.rejoin);
}

/// BRUTE FORCE of one compensated shift: some offset vector of the first k later inputs
/// (k = 0..maxK, every lattice point of the planner's offset range, order preserved) passes.
/// Bounded by `maxEvals` simulations (returns false and sets *exhausted when it ran out).
inline bool bruteCompPass(KinematicOracle& o, InputSchedule const& s, size_t i, double shift, CompConfig const& cfg, int maxK, size_t rigid = 0, int maxEvals = 4000,
                          bool* exhausted = nullptr) {
    int evals = 0;
    double const R = static_cast<double>(cfg.offsetRangeTicks);
    double const lo = std::min(shift, 0.0) - R, hi = std::max(shift, 0.0) + R;
    for (int k = 0; k <= maxK; ++k) {
        if (i + rigid + static_cast<size_t>(k) >= s.inputs.size()) break;
        if (k > 0 && frameOf(s, i + rigid + static_cast<size_t>(k)) > frameOf(s, i + rigid) + cfg.horizonFrames) break;
        std::vector<double> off(static_cast<size_t>(k), lo);
        while (true) {
            if (++evals > maxEvals) {
                if (exhausted) *exhausted = true;
                return false;
            }
            if (compSchedulePasses(o, s, i, shift, off, cfg, rigid)) return true;
            size_t j = 0;
            while (j < off.size()) {
                off[j] += 1.0;
                if (off[j] <= hi + 1e-9) break;
                off[j] = lo;
                ++j;
            }
            if (j == off.size()) break;
        }
    }
    return false;
}

struct BruteWalk {
    double lastPass = 0.0;
    double firstFail = kNaN;
};
template <class Pred>
BruteWalk bruteWalk(Pred&& pass, int limit) {
    BruteWalk w;
    for (int k = 1; k <= limit; ++k) {
        if (pass(static_cast<double>(k))) w.lastPass = k;
        else { w.firstFail = k; break; }
    }
    return w;
}

// ---- worlds ----

inline World shipBase() {
    World w;
    w.startMode = Mode::Ship;
    w.startY = 0.0;
    w.half = 3.0;
    w.shipGravity = 0.03;
    w.floor.pts = {{-100.0, -3.0}, {5000.0, -3.0}};
    return w;
}

inline std::vector<State> pathOf(World w, InputSchedule const& s, int64_t ticks) {
    w.spikes.clear();
    KinematicOracle o(w);
    return o.run(s, ticks);
}

/// Lethal bands around the recorded path: one rect per `every` ticks over [fromTick, toTick],
/// `below(k)` units under and `above(k)` units over the path there (plus the hitbox half). The
/// ship's hitbox is 2 x half wide, so a rect over ticks [k, k + every] takes the path's min / max y
/// over the ticks whose x the hitbox can overlap while inside the rect (about +-3 ticks at speed 1).
template <class Below, class Above>
void bandsAround(World& w, std::vector<State> const& path, int fromTick, int toTick, Below&& below, Above&& above, int every = 2) {
    int const n = static_cast<int>(path.size());
    int const reach = static_cast<int>(std::ceil((w.half + 0.65) / 1.3)) + 1;
    for (int k = fromTick; k <= toTick && k < n; k += every) {
        auto const& a = path[static_cast<size_t>(k - 1)];
        auto const& b = path[static_cast<size_t>(std::min(k - 1 + every, n - 1))];
        double ymin = 1e9, ymax = -1e9;
        for (int j = std::max(1, k - reach); j <= std::min(n, k + every + reach); ++j) {
            ymin = std::min(ymin, path[static_cast<size_t>(j - 1)].y);
            ymax = std::max(ymax, path[static_cast<size_t>(j - 1)].y);
        }
        w.spikes.push_back({a.x - 0.65, ymin - w.half - below(k) - 400.0, b.x + 0.65, ymin - w.half - below(k), 30});
        w.spikes.push_back({a.x - 0.65, ymax + w.half + above(k), b.x + 0.65, ymax + w.half + above(k) + 400.0, 31});
    }
}

/// A gentle ship flight that HOVERS: 6-tick holds every 16 ticks (thrust 0.08 x 6 ~ gravity 0.03 x 16),
/// so the recorded path stays near one height and a changed hold moves it by a few units, not dozens.
inline InputSchedule gentleFlight() {
    return schedule({{100, true}, {106, false}, {116, true}, {122, false}, {132, true}, {138, false}, {148, true}, {154, false}, {164, true}, {170, false},
                     {180, true}, {186, false}, {196, true}, {202, false}, {212, true}, {218, false}});
}

/// PROMPT §14.11 (2026-10-02): a press whose timing only works when the release compensates. DEV
/// FIXTURE found by a parameter scan: gravity 0.06, thrust 0.08, the ship rests on a floor that
/// ENDS right after take-off (no landing: nothing re-joins by the floor clamp), presses at 100,
/// releases at 108, and flies a per-tick corridor of `clear` units around its recorded path over
/// ticks [112, 124]. Shifting the press alone changes the hold (dies at once); shifting press +
/// release together moves the whole arc in time; a DIFFERENT release offset restores the height.
inline World criticalWorld(InputSchedule const& s, double clear) {
    World w = shipBase();
    w.shipGravity = 0.06;
    w.shipThrust = 0.08;
    w.floor.pts = {{-100.0, -3.0}, {1.3 * 109.0, -3.0}, {1.3 * 109.5, -600.0}, {5000.0, -600.0}};
    auto path = pathOf(w, s, 520);
    for (int k = 112; k <= 124; ++k) {
        auto const& a = path[static_cast<size_t>(k - 1)];
        auto const& b = path[static_cast<size_t>(k)];
        double ymin = std::min(a.y, b.y), ymax = std::max(a.y, b.y);
        w.spikes.push_back({a.x - 0.65, ymin - w.half - clear - 400.0, b.x + 0.65, ymin - w.half - clear, 21});
        w.spikes.push_back({a.x - 0.65, ymax + w.half + clear, b.x + 0.65, ymax + w.half + clear + 400.0, 22});
    }
    return w;
}

inline bool recordedSurvives(World const& w, InputSchedule const& s, double seconds) {
    KinematicOracle o(w);
    o.setReference(s);
    return o.trial(0, s, seconds).passed();
}

}  // namespace gprl::test::ship
