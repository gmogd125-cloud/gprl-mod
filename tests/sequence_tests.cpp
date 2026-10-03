// core/solver/sequence host tests (MASTER §5 sequence difficulty, §28 "timing-window solver";
// docs/SOLVER_DESIGN.md §13) on SYNTHETIC oracles with a known joint feasible region:
//   - local window -> axis (whole-tick atoms, sub-tick atoms, Voronoi weights, negative control)
//   - coarse nodes, the planner's grid-then-refine sampling, exactness at full refinement,
//     points that are never simulated (local trials, order-crossing combinations)
//   - pairs and triples, independent / diagonal band / ellipse / slab regions
//   - invalid trials, the sample cap, batches, finish(), determinism
//   - the reference SequenceSolver on the pull oracle (local windows + joint lattice)
//   - the `sequence_window` payload and the mirror of the server's evidence gate
//   - the engine's pure arithmetic: grouping, idle step allowance, clone grant, position key
#include "test_util.hpp"
#include "synthetic_joint_oracle.hpp"
#include "synthetic_oracle.hpp"

#include "../core/solver/budget.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/sequence.hpp"
#include "../core/solver/window_event.hpp"
#include "../core/telemetry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;
using gprl::test::SyntheticJointOracle;
using gprl::test::SyntheticOracle;

namespace {

constexpr double T = kTickMs;
using Shifts = std::array<double, kSequenceMaxInputs>;
using Pred = std::function<bool(Shifts const& shiftFrames)>;

/// A whole-tick local window: ticks kmin..kmax pass (kmin <= 0 <= kmax), the next tick on a
/// bounded side dies. Edges are the emitted midpoints (k -+ 0.5 ticks) on bounded sides.
LocalWindowInfo tickWindow(int kmin, int kmax, bool boundedEarly = true, bool boundedLate = true) {
    LocalWindowInfo i;
    i.valid = true;
    i.hit = true;
    i.earlyMs = (boundedEarly ? static_cast<double>(kmin) - 0.5 : static_cast<double>(kmin)) * T;
    i.lateMs = (boundedLate ? static_cast<double>(kmax) + 0.5 : static_cast<double>(kmax)) * T;
    i.widthMs = i.lateMs - i.earlyMs;
    i.resolutionMs = T;
    for (int k = kmin; k <= kmax; ++k) {
        if (k != 0) i.passShiftsMs.push_back(static_cast<double>(k) * T);
    }
    if (boundedLate) {
        i.hasFail = true;
        i.failShiftMs = static_cast<double>(kmax + 1) * T;
        i.failObjectId = 8;
    }
    return i;
}

/// A sub-tick local window [early, late] frames refined to `resolutionFrames`.
LocalWindowInfo subtickWindow(double earlyFrames, double lateFrames, double resolutionFrames = 0.125) {
    LocalWindowInfo i;
    i.valid = true;
    i.hit = true;
    i.earlyMs = earlyFrames * T;
    i.lateMs = lateFrames * T;
    i.widthMs = i.lateMs - i.earlyMs;
    i.resolutionMs = resolutionFrames * T;
    return i;
}

struct Run {
    int trials = 0;
    int batches = 0;
    std::vector<int> ids;
    std::vector<Shifts> shifts;
};

/// Drives a planner like the engine does: batches of trials, outcomes from the predicate.
Run drive(SequencePlanner& p, Pred const& pred, int batchSize = 8, std::function<bool(int)> const& invalid = nullptr) {
    Run r;
    for (int guard = 0; guard < 100000; ++guard) {
        auto batch = p.nextBatch(batchSize);
        if (batch.empty()) break;
        ++r.batches;
        CHECK(static_cast<int>(batch.size()) <= batchSize);
        for (auto const& t : batch) {
            r.ids.push_back(t.id);
            r.shifts.push_back(t.shiftFrames);
            ++r.trials;
            bool inv = invalid && invalid(r.trials);
            p.ingest({t.id, pred(t.shiftFrames), inv});
        }
    }
    return r;
}

/// The exact share of a lattice by brute force: every atom combination, weighted; a combination
/// that swaps the order of two inputs is infeasible.
double exactShare(std::vector<SequenceAxis> const& axes, std::vector<double> const& times, Pred const& pred,
                  double margin = kSequence.neighbourMarginFrames) {
    int const n = static_cast<int>(axes.size());
    double num = 0.0, den = 0.0;
    std::array<int, 3> dim{1, 1, 1};
    for (int i = 0; i < n; ++i) dim[static_cast<size_t>(i)] = axes[static_cast<size_t>(i)].size();
    for (int c = 0; c < dim[2]; ++c) {
        for (int b = 0; b < dim[1]; ++b) {
            for (int a = 0; a < dim[0]; ++a) {
                std::array<int, 3> idx{a, b, c};
                Shifts s{};
                double w = 1.0;
                for (int i = 0; i < n; ++i) {
                    s[static_cast<size_t>(i)] = axes[static_cast<size_t>(i)].positions[static_cast<size_t>(idx[static_cast<size_t>(i)])];
                    w *= axes[static_cast<size_t>(i)].weights[static_cast<size_t>(idx[static_cast<size_t>(i)])];
                }
                bool crossed = false;
                for (int i = 0; i + 1 < n; ++i) {
                    if ((times[static_cast<size_t>(i + 1)] + s[static_cast<size_t>(i + 1)]) - (times[static_cast<size_t>(i)] + s[static_cast<size_t>(i)]) < margin - 1e-9) crossed = true;
                }
                den += w;
                if (!crossed && pred(s)) num += w;
            }
        }
    }
    return den > 0.0 ? num / den : 0.0;
}

int movedCount(Shifts const& s) {
    int m = 0;
    for (double v : s) if (std::fabs(v) > 1e-9) ++m;
    return m;
}

std::vector<SequenceAxis> tickAxes(std::vector<std::pair<int, int>> const& windows) {
    std::vector<SequenceAxis> axes;
    for (auto const& [a, b] : windows) axes.push_back(makeAxis(tickWindow(a, b), false, 21));
    return axes;
}

// ---------------------------------------------------------------------------------------------

void testConfig() {
    SECTION("SequenceConfig: one object with a version; the shares leave the local jobs their budget");
    CHECK(std::string(kSequence.version) == "gprl-clone-seq/2");   // = kSequenceSolverVersion
    CHECK(std::string(kSequenceSolverVersion) == "gprl-clone-seq/2");   // v0.8.x: the isolated engine
    CHECK(kSequenceMinInputs == 2 && kSequenceMaxInputs == 3);
    CHECK(static_cast<int>(telemetry::kSequenceMinInputs) == kSequenceMinInputs && static_cast<int>(telemetry::kSequenceMaxInputs) == kSequenceMaxInputs);
    CHECK(kSequence.frameShare > 0.0 && kSequence.frameShare < kSequence.idleLoadShare && kSequence.idleLoadShare < 1.0);
    CHECK(kSequence.targetMsPerFrame > 0.0 && kSequence.targetMsPerFrame <= 1.0);   // well under the local jobs' 2 ms target
    CHECK(kSequence.maxGroupSize >= 2 && kSequence.maxGroupSize <= kSequenceMaxInputs);
    CHECK(kSequence.maxGapTicks > 0.0 && kSequence.maxTripleSpanTicks >= kSequence.maxGapTicks);
    CHECK(kSequence.nodesPerAxisPair >= 3 && kSequence.nodesPerAxisTriple >= 3);
    // the coarse grid alone always fits the sample cap: nodes^n minus the points on the axes
    CHECK(kSequence.nodesPerAxisPair * kSequence.nodesPerAxisPair <= kSequence.maxSamplesPair + 2 * kSequence.nodesPerAxisPair - 1);
    CHECK(kSequence.nodesPerAxisTriple * kSequence.nodesPerAxisTriple * kSequence.nodesPerAxisTriple <= kSequence.maxSamplesTriple + 3 * kSequence.nodesPerAxisTriple - 2);
    CHECK(kSequence.poolReserve >= 22);   // one whole coarse pass (20 shifts + control) stays free for a local job
    CHECK(kSequence.maxInvalidShare > 0.0 && kSequence.maxInvalidShare < 1.0);
}

void testLocalWindowInfo() {
    SECTION("local window -> info: the emitted midpoint edges, the passing shifts, the nearest known death");
    // input at 1000 ms; ticks -2..+3 pass, -3 and +4 die (1-D synthetic oracle, whole-tick planner)
    InputSchedule s = test::singleInput(1000.0);
    SyntheticOracle oracle({{1000.0 - 2.0 * T - 1.0, 1000.0 + 3.0 * T + 1.0}}, 0);
    PassPlanner planner(PlannerConfig{}, kNaN);
    planner.setLateLimit(10.0);
    runAgainstOracle(oracle, planner, 0, s, 0, 0.5);
    WindowResult w = planner.result(1000.0);
    CHECK_MSG(w.valid, w.invalidReason);
    LocalWindowInfo info = localWindowInfo(w);
    CHECK_MSG(info.valid, info.invalidReason);
    CHECK(info.hit);
    CHECK_NEAR(info.earlyMs, -2.5 * T, 1e-9);
    CHECK_NEAR(info.lateMs, 3.5 * T, 1e-9);
    CHECK_NEAR(info.resolutionMs, T, 1e-9);
    CHECK(info.passShiftsMs.size() == 5);
    if (info.passShiftsMs.size() == 5) {
        CHECK_NEAR(info.passShiftsMs.front(), -2.0 * T, 1e-9);
        CHECK_NEAR(info.passShiftsMs.back(), 3.0 * T, 1e-9);
    }
    // the nearest death is the early one (3 ticks away, the late one 4): the negative control
    CHECK(info.hasFail);
    CHECK_NEAR(info.failShiftMs, -3.0 * T, 1e-9);
    CHECK(info.failObjectId == 8);

    // the width is bit for bit what the timing_window event of the same result carries
    TimingFingerprint fp;
    WindowEventContext ctx;
    ctx.inputSeq = 7;
    ctx.actualMs = 1000.0;
    auto built = buildWindowEvent(w, ctx, fp);
    CHECK_MSG(built.ok, built.error);
    CHECK(info.widthMs == built.payload.latestMs - built.payload.earliestMs);
    CHECK(info.widthMs == built.widthMs);
    CHECK(info.earlyMs == built.payload.earliestMs - 1000.0 || std::fabs(info.earlyMs - (built.payload.earliestMs - 1000.0)) < 1e-9);

    // sub-tick refinement (Click Between Frames): refined edges, more passing shifts, same rule
    PlannerConfig pc;
    pc.subtick = true;
    PassPlanner refined(pc, kNaN);
    refined.setLateLimit(10.0);
    SyntheticOracle oracle2({{1000.0 - 2.0 * T - 1.0, 1000.0 + 3.0 * T + 1.0}}, 0);
    runAgainstOracle(oracle2, refined, 0, s, 0, 0.5);
    WindowResult w2 = refined.result(1000.0);
    CHECK_MSG(w2.valid, w2.invalidReason);
    CHECK(refined.refined());
    LocalWindowInfo info2 = localWindowInfo(w2);
    CHECK(info2.valid && info2.hit);
    CHECK(info2.resolutionMs < T && info2.resolutionMs > 0.0);
    CHECK(info2.earlyMs < -2.0 * T && info2.earlyMs > -3.0 * T);
    CHECK(info2.lateMs > 3.0 * T && info2.lateMs < 4.0 * T);
    CHECK(info2.passShiftsMs.size() > 5);
    ctx.refined = true;
    auto built2 = buildWindowEvent(w2, ctx, fp);
    CHECK_MSG(built2.ok, built2.error);
    CHECK(info2.widthMs == built2.payload.latestMs - built2.payload.earliestMs);
    CHECK(info2.hasFail && std::fabs(info2.failShiftMs) > 2.0 * T);

    // an unbounded window (nothing dies within the limit) has no negative control
    SyntheticOracle open({{0.0, 5000.0}}, 0);
    PassPlanner p3(PlannerConfig{}, kNaN);
    p3.setLateLimit(10.0);
    runAgainstOracle(open, p3, 0, s, 0, 0.5);
    LocalWindowInfo info3 = localWindowInfo(p3.result(1000.0));
    CHECK(info3.valid && info3.hit && !info3.hasFail);
    CHECK_NEAR(info3.earlyMs, -10.0 * T, 1e-9);
    CHECK_NEAR(info3.lateMs, 10.0 * T, 1e-9);
    CHECK(info3.passShiftsMs.size() == 20);

    // an invalid window result is refused
    WindowResult bad;
    bad.valid = false;
    bad.invalidReason = "control clone did not reproduce the real player";
    CHECK(!localWindowInfo(bad).valid);

    // a miss (the actual input outside its window) is recognised
    SyntheticOracle missOracle({{1000.0 + 2.0 * T - 1.0, 1000.0 + 4.0 * T + 1.0}}, 0);
    PassPlanner p4(PlannerConfig{}, kNaN);
    p4.setLateLimit(10.0);
    // drive it by hand: the control dies with the real player
    auto shifts = p4.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double sh : shifts) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = sh;
        auto r = missOracle.trial(0, s.withMoved(0, 1000.0 + sh * T), 0.5);
        o.kind = r.passed() ? ShiftKind::Survived : ShiftKind::Died;
        o.objectId = 8;
        outs.push_back(o);
    }
    p4.ingest(outs, false, true);
    WindowResult wm = p4.result(1000.0);
    CHECK_MSG(wm.valid, wm.invalidReason);
    LocalWindowInfo im = localWindowInfo(wm);
    CHECK(im.valid && !im.hit);
    CHECK(!makeAxis(im, false, 21).valid);
}

void testAxis() {
    SECTION("axis: whole-tick atoms with Voronoi weights that add up to the local width");
    SequenceAxis a = makeAxis(tickWindow(-2, 3), false, 21);
    CHECK_MSG(a.valid, a.invalidReason);
    CHECK(a.size() == 6 && a.origin == 2 && !a.continuous);
    CHECK_NEAR(a.positions.front(), -2.0, 1e-9);
    CHECK_NEAR(a.positions.back(), 3.0, 1e-9);
    double sum = 0.0;
    for (double w : a.weights) {
        CHECK_NEAR(w, 1.0, 1e-9);   // bounded on both sides: every passing tick stands for one tick
        sum += w;
    }
    CHECK_NEAR(sum, 6.0, 1e-9);
    CHECK_NEAR(sum * T, a.localWidthMs, 1e-9);
    CHECK_NEAR(a.earlyFrames, -2.5, 1e-9);
    CHECK_NEAR(a.lateFrames, 3.5, 1e-9);

    // an unbounded side ends at its last passing tick: that atom stands for half a tick
    SequenceAxis open = makeAxis(tickWindow(-2, 3, true, false), false, 21);
    CHECK(open.valid && open.size() == 6);
    CHECK_NEAR(open.weights.back(), 0.5, 1e-9);
    CHECK_NEAR(open.weights.front(), 1.0, 1e-9);
    sum = 0.0;
    for (double w : open.weights) sum += w;
    CHECK_NEAR(sum * T, open.localWidthMs, 1e-9);

    // a gap (an untested tick inside the window) widens its neighbours' cells, never the total
    LocalWindowInfo gap = tickWindow(0, 3);
    gap.passShiftsMs = {1.0 * T, 3.0 * T};   // +2 was not tested
    SequenceAxis g = makeAxis(gap, false, 21);
    CHECK(g.valid && g.size() == 3 && g.origin == 0);
    CHECK_NEAR(g.weights[0], 1.0, 1e-9);   // [-0.5, 0.5]
    CHECK_NEAR(g.weights[1], 1.5, 1e-9);   // [0.5, 2]
    CHECK_NEAR(g.weights[2], 1.5, 1e-9);   // [2, 3.5]

    // a one-sided window (blocked early side): the performed timing is the first atom
    SequenceAxis one = makeAxis(tickWindow(0, 4, false, true), false, 21);
    CHECK(one.valid && one.origin == 0 && one.size() == 5);
    CHECK_NEAR(one.weights[0], 0.5, 1e-9);

    // a frame-perfect input: a single atom of one tick
    SequenceAxis fp = makeAxis(tickWindow(0, 0), false, 21);
    CHECK(fp.valid && fp.size() == 1 && fp.origin == 0);
    CHECK_NEAR(fp.weights[0], 1.0, 1e-9);

    SECTION("axis: sub-tick atoms subdivide the window evenly, 0 is always an atom");
    SequenceAxis c = makeAxis(subtickWindow(-0.5, 0.5), true, 21);   // a frame perfect with CBF, 1/8 tick
    CHECK_MSG(c.valid, c.invalidReason);
    CHECK(c.continuous);
    CHECK(c.size() == 9 && c.origin == 4);
    CHECK_NEAR(c.positions[static_cast<size_t>(c.origin)], 0.0, 1e-12);
    sum = 0.0;
    for (double w : c.weights) {
        CHECK(w > 0.0);
        sum += w;
    }
    CHECK_NEAR(sum, 1.0, 1e-9);
    CHECK_NEAR(c.positions.front(), -c.positions.back(), 1e-9);
    CHECK(c.positions.front() > -0.5 && c.positions.back() < 0.5);   // atoms sit inside the window
    // never more atoms than the cap, never finer than the local resolution
    SequenceAxis capped = makeAxis(subtickWindow(-3.0, 3.0), true, 5);
    CHECK(capped.valid && capped.size() <= 5 && capped.size() >= 3);
    sum = 0.0;
    for (double w : capped.weights) sum += w;
    CHECK_NEAR(sum, 6.0, 1e-9);
    SequenceAxis coarse = makeAxis(subtickWindow(-3.0, 3.0, 1.0), true, 21);   // unrefined window: one tick resolution
    CHECK(coarse.valid && coarse.size() == 7);
    SequenceAxis wide = makeAxis(subtickWindow(-3.0, 3.0), true, 21);
    CHECK(wide.valid && wide.size() == 21 && wide.origin == 10);
    // asymmetric: each side gets its own even step
    SequenceAxis asym = makeAxis(subtickWindow(-0.4, 2.6), true, 21);
    CHECK(asym.valid);
    CHECK_NEAR(asym.positions[static_cast<size_t>(asym.origin)], 0.0, 1e-12);
    sum = 0.0;
    for (double w : asym.weights) sum += w;
    CHECK_NEAR(sum, 3.0, 1e-9);

    SECTION("axis: unusable windows are refused with the reason");
    LocalWindowInfo invalid;
    invalid.invalidReason = "no pass ingested";
    CHECK(!makeAxis(invalid, false, 21).valid);
    LocalWindowInfo miss = tickWindow(0, 2);
    miss.hit = false;
    CHECK(makeAxis(miss, false, 21).invalidReason.find("missed") != std::string::npos);
    LocalWindowInfo zero = tickWindow(0, 0, false, false);
    CHECK(!makeAxis(zero, false, 21).valid);
}

void testNodes() {
    SECTION("coarse nodes: first, performed timing and last always among them; widest gap filled first");
    CHECK((sequenceNodes(9, 4, 5) == std::vector<int>{0, 2, 4, 6, 8}));
    CHECK((sequenceNodes(21, 10, 5) == std::vector<int>{0, 5, 10, 15, 20}));
    CHECK((sequenceNodes(6, 1, 5) == std::vector<int>{0, 1, 2, 3, 5}));
    CHECK((sequenceNodes(1, 0, 5) == std::vector<int>{0}));
    CHECK((sequenceNodes(2, 0, 5) == std::vector<int>{0, 1}));
    CHECK((sequenceNodes(3, 2, 3) == std::vector<int>{0, 1, 2}));
    CHECK((sequenceNodes(21, 0, 3) == std::vector<int>{0, 10, 20}));
    CHECK((sequenceNodes(5, 4, 3) == std::vector<int>{0, 2, 4}));
    CHECK((sequenceNodes(4, 1, 9) == std::vector<int>{0, 1, 2, 3}));
    for (int atoms = 1; atoms <= 33; ++atoms) {
        for (int origin = 0; origin < atoms; ++origin) {
            for (int want : {2, 3, 5, 9}) {
                auto n = sequenceNodes(atoms, origin, want);
                CHECK(!n.empty() && n.front() == 0 && n.back() == atoms - 1);
                CHECK(std::find(n.begin(), n.end(), origin) != n.end());
                CHECK(std::is_sorted(n.begin(), n.end()) && std::adjacent_find(n.begin(), n.end()) == n.end());
                CHECK(static_cast<int>(n.size()) <= std::max(3, std::min(want, atoms)));
            }
        }
    }
}

void testIndependentPair() {
    SECTION("independent pair: share exactly 1, only the off-axis grid corners are simulated");
    auto axes = tickAxes({{-4, 4}, {-4, 4}});
    std::vector<double> times{0.0, 100.0};
    SequencePlanner p(kSequence, axes, times);
    CHECK_MSG(p.valid(), p.invalidReason());
    CHECK(!p.trivial());
    Pred always = [](Shifts const&) { return true; };
    Run run = drive(p, always);
    CHECK(p.done() && p.outstanding() == 0);
    SequenceResult r = p.result();
    CHECK_MSG(r.valid, r.invalidReason);
    CHECK(r.inputs == 2 && r.localWidthsMs.size() == 2);
    CHECK_NEAR(r.localWidthsMs[0], 9.0 * T, 1e-9);
    CHECK(r.jointFeasibleShare == 1.0);
    CHECK(run.trials == 16 && r.samples == 16 && r.passed == 16 && r.failed == 0);   // 5 x 5 nodes minus the 9 on the axes
    CHECK(r.knownLocal == 17);        // every on-axis atom: 9 + 9 - 1
    CHECK(r.crossed == 0 && r.invalidTrials == 0 && r.latticePoints == 81);
    CHECK(r.mixedBoxes == 0 && r.refineRounds == 0 && r.complete && !r.budgetExhausted);
    CHECK_NEAR(r.resolutionMs, 2.0 * T, 1e-9);   // nothing disagreed: the coarse grid spacing
    CHECK_NEAR(r.productMeasure, 81.0 * T * T, 1e-6);
    CHECK_NEAR(r.jointMeasure, r.productMeasure, 1e-9);
    CHECK(r.solverVersion == "gprl-clone-seq/2");
    // no trial ever had fewer than two inputs shifted (those are local trials, already known)
    for (auto const& s : run.shifts) CHECK(movedCount(s) == 2);
    // the extremes come first: a job cut short still has the four corners of the box
    CHECK(run.shifts.size() >= 4);
    for (size_t i = 0; i < 4 && i < run.shifts.size(); ++i) CHECK(std::fabs(run.shifts[i][0]) == 4.0 && std::fabs(run.shifts[i][1]) == 4.0);
}

void testDiagonalBand() {
    SECTION("diagonal band (hold length matters): exact lattice share at whole ticks");
    // feasible iff |sB - sA| <= 2 ticks -> each local window is -2..+2, the box 5 x 5, 19 of 25 pass
    Pred band = [](Shifts const& s) { return std::fabs(s[1] - s[0]) <= 2.0 + 1e-9; };
    auto axes = tickAxes({{-2, 2}, {-2, 2}});
    std::vector<double> times{0.0, 100.0};
    SequencePlanner p(kSequence, axes, times);
    Run run = drive(p, band);
    SequenceResult r = p.result();
    CHECK_MSG(r.valid, r.invalidReason);
    CHECK_NEAR(r.jointFeasibleShare, 19.0 / 25.0, 1e-12);
    CHECK_NEAR(r.jointFeasibleShare, exactShare(axes, times, band), 1e-12);
    CHECK(r.samples == 16 && r.passed == 10 && r.failed == 6);
    CHECK(r.complete && !r.budgetExhausted);
    CHECK_NEAR(r.resolutionMs, T, 1e-9);   // the boundary is located to one tick
    CHECK(r.mixedBoxes > 0);
    auto rows = p.map();
    CHECK(rows.size() == 5);
    if (rows.size() == 5) {
        CHECK_MSG(rows[0] == "..o##", rows[0]);   // sB = +2: sA -2, -1 fail, 0 known, +1, +2 pass
        CHECK_MSG(rows[2] == "ooooo", rows[2]);   // sB = 0: the local window of input 1
        CHECK_MSG(rows[4] == "##o..", rows[4]);
    }

    SECTION("asymmetric band on 9 x 9 ticks: refinement reaches the exact lattice share");
    // feasible iff -3 <= sB - sA <= 5 -> A alone: sA in -5..3, B alone: sB in -3..5
    Pred band2 = [](Shifts const& s) { double d = s[1] - s[0]; return d >= -3.0 - 1e-9 && d <= 5.0 + 1e-9; };
    auto axes2 = tickAxes({{-5, 3}, {-3, 5}});
    SequenceConfig big = kSequence;
    big.maxSamplesPair = 400;
    SequencePlanner full(big, axes2, times);
    Run fullRun = drive(full, band2);
    SequenceResult rf = full.result();
    double exact = exactShare(axes2, times, band2);
    CHECK_MSG(rf.valid, rf.invalidReason);
    CHECK_NEAR(rf.jointFeasibleShare, exact, 1e-12);
    CHECK(rf.complete && !rf.budgetExhausted && rf.refineRounds >= 1);
    CHECK(fullRun.trials < 81 - 17);   // adaptive: the uniform interior is never simulated
    CHECK_NEAR(rf.resolutionMs, T, 1e-9);
    for (auto const& s : fullRun.shifts) CHECK(movedCount(s) == 2);

    // the default cap: fewer trials, an honest flag, still close
    SequencePlanner capped(kSequence, axes2, times);
    Run cappedRun = drive(capped, band2);
    SequenceResult rc = capped.result();
    CHECK_MSG(rc.valid, rc.invalidReason);
    CHECK(cappedRun.trials <= kSequence.maxSamplesPair && rc.samples == cappedRun.trials);
    CHECK_MSG(std::fabs(rc.jointFeasibleShare - exact) <= 0.05, "share " + std::to_string(rc.jointFeasibleShare) + " exact " + std::to_string(exact));
    CHECK(rc.complete || rc.budgetExhausted);
}

void testOrderCrossing() {
    SECTION("order crossing: a release can never come before its press; never simulated, never feasible");
    // press at frame 0, release at frame 3, both windows -2..+2, physics passes everything
    auto axes = tickAxes({{-2, 2}, {-2, 2}});
    std::vector<double> times{0.0, 3.0};
    Pred always = [](Shifts const&) { return true; };
    SequencePlanner p(kSequence, axes, times);
    CHECK_MSG(p.valid(), p.invalidReason());
    Run run = drive(p, always);
    SequenceResult r = p.result();
    CHECK_MSG(r.valid, r.invalidReason);
    CHECK(r.crossed == 3);   // (sA, sB) = (+1,-2), (+2,-1), (+2,-2): the release would land on or before the press
    CHECK(r.samples == 25 - 9 - 3);
    CHECK_NEAR(r.jointFeasibleShare, 22.0 / 25.0, 1e-12);
    CHECK_NEAR(r.jointFeasibleShare, exactShare(axes, times, always), 1e-12);
    for (auto const& s : run.shifts) {
        CHECK((3.0 + s[1]) - (0.0 + s[0]) >= kSequence.neighbourMarginFrames);
        CHECK(movedCount(s) == 2);
    }
    auto rows = p.map();
    CHECK(rows.size() == 5);
    if (rows.size() == 5) CHECK_MSG(rows[4] == "##oxx", rows[4]);   // sB = -2: sA +1, +2 crossed

    // two inputs at the same instant are not a sequence at all
    SequencePlanner same(kSequence, axes, {5.0, 5.0});
    CHECK(!same.valid() && same.invalidReason().find("same instant") != std::string::npos);
    CHECK(same.nextBatch(8).empty() && same.done());
    CHECK(!same.result().valid);
}

void testContinuous() {
    SECTION("sub-tick lattice: a diagonal band against the analytic area");
    // windows +-3 ticks refined to 1/8 tick; feasible iff |sB - sA| <= 3.5 -> share 1 - 2.5^2 / 36
    Pred band = [](Shifts const& s) { return std::fabs(s[1] - s[0]) <= 3.5; };
    std::vector<SequenceAxis> axes{makeAxis(subtickWindow(-3.0, 3.0), true, kSequence.maxAtomsPerAxisPair),
                                   makeAxis(subtickWindow(-3.0, 3.0), true, kSequence.maxAtomsPerAxisPair)};
    std::vector<double> times{0.0, 100.0};
    double const analytic = 1.0 - (2.5 * 2.5) / 36.0;
    double const lattice = exactShare(axes, times, band);
    CHECK_MSG(std::fabs(lattice - analytic) < 0.02, "lattice " + std::to_string(lattice));

    SequenceConfig big = kSequence;
    big.maxSamplesPair = 1000;
    SequencePlanner full(big, axes, times);
    Run fullRun = drive(full, band);
    SequenceResult rf = full.result();
    CHECK_MSG(rf.valid, rf.invalidReason);
    CHECK_NEAR(rf.jointFeasibleShare, lattice, 1e-12);   // full refinement = the lattice's own share
    CHECK(rf.complete && !rf.budgetExhausted);
    CHECK(fullRun.trials < 21 * 21 - 41);
    CHECK(rf.resolutionMs < 0.5 * T);                    // one atom step (6 / 21 of a tick)

    SequencePlanner capped(kSequence, axes, times);
    Run run = drive(capped, band);
    SequenceResult r = capped.result();
    CHECK_MSG(r.valid, r.invalidReason);
    CHECK(run.trials <= kSequence.maxSamplesPair);
    CHECK_MSG(std::fabs(r.jointFeasibleShare - analytic) <= 0.04, "share " + std::to_string(r.jointFeasibleShare) + " analytic " + std::to_string(analytic));
    CHECK(r.resolutionMs > 0.0 && std::isfinite(r.resolutionMs));

    SECTION("sub-tick lattice: an ellipse (curved boundary) against brute force");
    Pred ellipse = [](Shifts const& s) { return (s[0] * s[0]) / 9.0 + (s[1] * s[1]) / 4.0 <= 1.3; };
    std::vector<SequenceAxis> eaxes{makeAxis(subtickWindow(-3.0, 3.0), true, 21), makeAxis(subtickWindow(-2.0, 2.0), true, 21)};
    // truth on a very fine grid of the continuous box
    int inside = 0, all = 0;
    for (int i = 0; i < 1200; ++i) {
        for (int j = 0; j < 800; ++j) {
            Shifts s{-3.0 + (i + 0.5) * 6.0 / 1200.0, -2.0 + (j + 0.5) * 4.0 / 800.0, 0.0};
            ++all;
            if (ellipse(s)) ++inside;
        }
    }
    double const truth = static_cast<double>(inside) / static_cast<double>(all);
    SequencePlanner pe(kSequence, eaxes, times);
    drive(pe, ellipse);
    SequenceResult re = pe.result();
    CHECK_MSG(re.valid, re.invalidReason);
    CHECK_MSG(std::fabs(re.jointFeasibleShare - truth) <= 0.05, "share " + std::to_string(re.jointFeasibleShare) + " truth " + std::to_string(truth));
    SequencePlanner peFull(big, eaxes, times);
    drive(peFull, ellipse);
    SequenceResult ref = peFull.result();
    CHECK_MSG(std::fabs(ref.jointFeasibleShare - truth) <= 0.02, "full share " + std::to_string(ref.jointFeasibleShare) + " truth " + std::to_string(truth));
    CHECK_NEAR(ref.jointFeasibleShare, exactShare(eaxes, times, ellipse), 1e-12);
    // more samples never make the estimate worse than the capped one by more than the tolerance
    CHECK(std::fabs(ref.jointFeasibleShare - truth) <= std::fabs(re.jointFeasibleShare - truth) + 0.02);
}

void testTriple() {
    SECTION("triple: independent inputs -> 1; a slab |sA + sB + sC| <= 3 -> the exact lattice share");
    auto axes = tickAxes({{-2, 2}, {-2, 2}, {-2, 2}});
    std::vector<double> times{0.0, 50.0, 100.0};
    Pred always = [](Shifts const&) { return true; };
    SequencePlanner ind(kSequence, axes, times);
    CHECK_MSG(ind.valid(), ind.invalidReason());
    Run indRun = drive(ind, always);
    SequenceResult ri = ind.result();
    CHECK_MSG(ri.valid, ri.invalidReason);
    CHECK(ri.inputs == 3 && ri.localWidthsMs.size() == 3);
    CHECK(ri.jointFeasibleShare == 1.0);
    CHECK(indRun.trials == 20);          // 3 x 3 x 3 nodes minus the 7 with at most one input shifted
    CHECK(ri.knownLocal == 13 && ri.latticePoints == 125);
    for (auto const& s : indRun.shifts) CHECK(movedCount(s) >= 2);
    CHECK(ind.map().empty());            // the text map is for pairs
    CHECK_NEAR(ri.productMeasure, 125.0 * T * T * T, 1e-6);

    Pred slab = [](Shifts const& s) { return std::fabs(s[0] + s[1] + s[2]) <= 3.0 + 1e-9; };
    double const exact = exactShare(axes, times, slab);
    CHECK(exact > 0.5 && exact < 1.0);
    SequenceConfig big = kSequence;
    big.maxSamplesTriple = 500;
    SequencePlanner full(big, axes, times);
    Run fullRun = drive(full, slab);
    SequenceResult rf = full.result();
    CHECK_MSG(rf.valid, rf.invalidReason);
    CHECK_NEAR(rf.jointFeasibleShare, exact, 1e-12);
    CHECK(rf.complete && !rf.budgetExhausted);
    CHECK(fullRun.trials <= 125 - 13);
    for (auto const& s : fullRun.shifts) CHECK(movedCount(s) >= 2);

    SequencePlanner capped(kSequence, axes, times);
    Run run = drive(capped, slab);
    SequenceResult r = capped.result();
    CHECK_MSG(r.valid, r.invalidReason);
    CHECK(run.trials <= kSequence.maxSamplesTriple);
    CHECK_MSG(std::fabs(r.jointFeasibleShare - exact) <= 0.08, "share " + std::to_string(r.jointFeasibleShare) + " exact " + std::to_string(exact));

    SECTION("triple: order crossing between neighbours, and the pair-only configuration");
    std::vector<double> close{0.0, 3.0, 6.0};
    SequencePlanner cross(big, axes, close);
    Run crossRun = drive(cross, always);
    SequenceResult rx = cross.result();
    CHECK_MSG(rx.valid, rx.invalidReason);
    CHECK(rx.crossed > 0);
    CHECK_NEAR(rx.jointFeasibleShare, exactShare(axes, close, always), 1e-12);
    for (auto const& s : crossRun.shifts) {
        CHECK((3.0 + s[1]) - s[0] >= kSequence.neighbourMarginFrames);
        CHECK((6.0 + s[2]) - (3.0 + s[1]) >= kSequence.neighbourMarginFrames);
    }
    SequenceConfig pairsOnly = kSequence;
    pairsOnly.maxGroupSize = 2;
    CHECK(!SequencePlanner(pairsOnly, axes, times).valid());
}

void testDegenerate() {
    SECTION("degenerate: a single-atom axis or an all-crossed box is trivial (nothing to simulate, nothing emitted)");
    std::vector<double> times{0.0, 100.0};
    // a frame-perfect press at whole ticks: every lattice point has the press unshifted = a local trial
    std::vector<SequenceAxis> axes{makeAxis(tickWindow(0, 0), false, 21), makeAxis(tickWindow(-3, 3), false, 21)};
    SequencePlanner p(kSequence, axes, times);
    CHECK_MSG(p.valid(), p.invalidReason());
    CHECK(p.trivial() && p.requested() == 0);
    CHECK(p.nextBatch(8).empty());
    CHECK(p.done());
    SequenceResult r = p.result();
    CHECK(r.trivial && !r.valid && r.samples == 0);
    CHECK(r.invalidReason.find("nothing to simulate") != std::string::npos);
    CHECK(r.jointFeasibleShare == 1.0);   // by definition of the local windows; not a measurement
    CHECK(!buildSequenceEvent(r, {4, 6}).ok);

    // the only off-axis combination swaps the order: trivial too, the share is below 1 by order alone
    std::vector<SequenceAxis> tap{makeAxis(tickWindow(0, 1, false, false), false, 21), makeAxis(tickWindow(-1, 0, false, false), false, 21)};
    SequencePlanner t(kSequence, tap, {0.0, 2.0});
    CHECK_MSG(t.valid(), t.invalidReason());
    CHECK(t.trivial());
    SequenceResult rt = t.result();
    CHECK(rt.trivial && !rt.valid && rt.crossed == 1 && rt.knownLocal == 3);
    CHECK(rt.jointFeasibleShare < 1.0);

    SECTION("degenerate: bad groups are refused with a reason");
    auto a3 = tickAxes({{-1, 1}});
    CHECK(!SequencePlanner(kSequence, a3, {0.0}).valid());
    auto a4 = tickAxes({{-1, 1}, {-1, 1}, {-1, 1}, {-1, 1}});
    CHECK(!SequencePlanner(kSequence, a4, {0.0, 10.0, 20.0, 30.0}).valid());
    auto a2 = tickAxes({{-1, 1}, {-1, 1}});
    CHECK(!SequencePlanner(kSequence, a2, {0.0}).valid());
    CHECK(!SequencePlanner(kSequence, a2, {10.0, 0.0}).valid());   // not ascending
    std::vector<SequenceAxis> withBad = a2;
    withBad[1] = makeAxis(LocalWindowInfo{}, false, 21);
    SequencePlanner bad(kSequence, withBad, {0.0, 10.0});
    CHECK(!bad.valid() && bad.invalidReason().find("input 2") != std::string::npos);
    CHECK(SequencePlanner().done());
    CHECK(!SequencePlanner().result().valid);
}

void testInvalidOutcomes() {
    SECTION("invalid trials: a few are left out, too many drop the job");
    Pred band = [](Shifts const& s) { return std::fabs(s[1] - s[0]) <= 2.0 + 1e-9; };
    auto axes = tickAxes({{-2, 2}, {-2, 2}});
    std::vector<double> times{0.0, 100.0};
    SequencePlanner some(kSequence, axes, times);
    Run run = drive(some, band, 8, [](int nth) { return nth == 3 || nth == 9; });
    SequenceResult r = some.result();
    CHECK_MSG(r.valid, r.invalidReason);
    CHECK(r.invalidTrials == 2 && r.samples == run.trials - 2);
    CHECK_MSG(std::fabs(r.jointFeasibleShare - 19.0 / 25.0) <= 0.08, std::to_string(r.jointFeasibleShare));
    auto rows = some.map();
    int bangs = 0;
    for (auto const& row : rows) bangs += static_cast<int>(std::count(row.begin(), row.end(), '!'));
    CHECK(bangs == 2);

    SequencePlanner many(kSequence, axes, times);
    drive(many, band, 8, [](int nth) { return nth % 2 == 0; });
    SequenceResult rm = many.result();
    CHECK(!rm.valid && rm.invalidReason.find("could not be simulated") != std::string::npos);
    CHECK(!buildSequenceEvent(rm, {1, 2}).ok);

    SequencePlanner all(kSequence, axes, times);
    drive(all, band, 8, [](int) { return true; });
    CHECK(!all.result().valid);
}

void testBudgetBatchesFinish() {
    SECTION("sample cap, batch sizes and finish(): never more trials than allowed, always a defined result");
    Pred band = [](Shifts const& s) { double d = s[1] - s[0]; return d >= -3.0 - 1e-9 && d <= 5.0 + 1e-9; };
    auto axes = tickAxes({{-5, 3}, {-3, 5}});
    std::vector<double> times{0.0, 100.0};
    double const exact = exactShare(axes, times, band);
    SequenceConfig unlimited = kSequence;
    unlimited.maxSamplesPair = 400;
    SequencePlanner whole(unlimited, axes, times);
    int const needed = drive(whole, band).trials;   // what the full refinement costs
    CHECK(needed > 28 && needed < 81 - 17);
    for (int cap : {16, 20, 28, needed - 1}) {
        SequenceConfig c = kSequence;
        c.maxSamplesPair = cap;
        SequencePlanner p(c, axes, times);
        Run run = drive(p, band, 3);
        SequenceResult r = p.result();
        CHECK_MSG(r.valid, r.invalidReason);
        CHECK(run.trials <= cap && p.requested() <= cap);
        CHECK_MSG(r.budgetExhausted && !r.complete, "cap " + std::to_string(cap) + " of " + std::to_string(needed));
        CHECK(r.resolutionMs >= T - 1e-9);
        CHECK_MSG(std::fabs(r.jointFeasibleShare - exact) <= 0.12, "cap " + std::to_string(cap) + " share " + std::to_string(r.jointFeasibleShare));
    }
    // a cap below the coarse grid (never the case with kSequence, testConfig): the grid is cut, the
    // flag says so, and the result is NOT valid - 6 of 16 grid points say nothing about the box
    SequenceConfig tiny = kSequence;
    tiny.maxSamplesPair = 6;
    SequencePlanner cut(tiny, axes, times);
    Run cutRun = drive(cut, band);
    CHECK(cutRun.trials == 6);
    SequenceResult rc = cut.result();
    CHECK(!rc.valid && rc.budgetExhausted && rc.samples == 6 && !rc.complete);
    CHECK_MSG(rc.invalidReason.find("coarse grid") != std::string::npos, rc.invalidReason);
    CHECK(rc.jointFeasibleShare >= 0.0 && rc.jointFeasibleShare <= 1.0);
    CHECK(!buildSequenceEvent(rc, {1, 2}).ok);

    // finish() in the middle of the coarse grid (history ran out): no further trials, outcomes
    // still accepted, and nothing to report - the grid is incomplete
    SequencePlanner p(kSequence, axes, times);
    auto first = p.nextBatch(5);
    CHECK(first.size() == 5 && p.outstanding() == 5);
    CHECK(p.nextBatch(0).empty());
    for (size_t i = 0; i < 3; ++i) p.ingest({first[i].id, band(first[i].shiftFrames), false});
    CHECK(!p.done());
    p.finish();
    CHECK(p.done());
    CHECK(p.nextBatch(8).empty());
    for (size_t i = 3; i < 5; ++i) p.ingest({first[i].id, band(first[i].shiftFrames), false});
    CHECK(p.outstanding() == 0);
    SequenceResult rf = p.result();
    CHECK(!rf.valid && rf.samples == 5 && !rf.complete);
    CHECK_MSG(rf.invalidReason.find("coarse grid") != std::string::npos, rf.invalidReason);
    CHECK(rf.jointFeasibleShare >= 0.0 && rf.jointFeasibleShare <= 1.0);
    CHECK(!buildSequenceEvent(rf, {1, 2}).ok);
    // an outcome for a point that was never asked, or asked twice, changes nothing
    p.ingest({first[0].id, false, false});
    p.ingest({-1, true, false});
    p.ingest({1 << 20, true, false});
    SequenceResult again = p.result();
    CHECK(again.samples == 5 && again.jointFeasibleShare == rf.jointFeasibleShare);
}

/// A job cut short by finish() (the engine: the history ring moved past the job's first
/// snapshot). What it may report: nothing while the coarse grid is incomplete; afterwards the
/// last state in which every corner was decided - a refinement round that was planned but not
/// simulated is undone, and `resolutionMs` is the size of the boxes it would have cut.
void testCutShort() {
    SECTION("cut short: an incomplete coarse grid is never a result (the known local axes would pull the share to 1)");
    auto axes = tickAxes({{-8, 8}, {-8, 8}});
    std::vector<double> times{0.0, 100.0};
    // every joint deviation fails: the feasible region is the two local axes only
    Pred axesOnly = [](Shifts const& s) { return std::fabs(s[0]) < 1e-9 || std::fabs(s[1]) < 1e-9; };
    Pred band = [](Shifts const& s) { return std::fabs(s[1] - s[0]) <= 9.0 + 1e-9; };
    // what finishing after `stop` ingested outcomes reports (one trial at a time, like a starved job)
    auto cutAfter = [&](Pred const& pred, int stop, SequenceConfig const& cfg = kSequence) {
        SequencePlanner p(cfg, axes, times);
        int ingested = 0;
        while (ingested < stop) {
            auto b = p.nextBatch(1);
            if (b.empty()) break;
            p.ingest({b.front().id, pred(b.front().shiftFrames), false});
            ++ingested;
        }
        p.finish();
        return p.result();
    };
    int const coarse = 16;   // 5 x 5 nodes minus the 9 on the axes
    for (int stop = 0; stop < coarse; ++stop) {
        for (Pred const* pred : {&axesOnly, &band}) {
            SequenceResult r = cutAfter(*pred, stop);
            CHECK_MSG(!r.valid && !r.complete, "valid after " + std::to_string(stop) + " of the coarse grid");
            // (stop 0: nothing was simulated at all, which is its own reason)
            if (stop > 0) CHECK_MSG(r.invalidReason.find("coarse grid") != std::string::npos, r.invalidReason);
            CHECK(r.samples == stop);
            CHECK(!buildSequenceEvent(r, {1, 2}).ok);
        }
    }
    // (before this rule 4 samples reported a share of 0.72 for this region)
    CHECK(exactShare(axes, times, axesOnly) < 0.12);

    SECTION("cut short: from the complete coarse grid on, every cut is a valid result that says how coarse it is");
    double const truthBand = exactShare(axes, times, band);
    SequencePlanner wholeBand(kSequence, axes, times);
    int const all = drive(wholeBand, band).trials;
    SequenceResult fullBand = wholeBand.result();
    CHECK_MSG(fullBand.valid, fullBand.invalidReason);
    CHECK(all > coarse);
    double previous = 1e9;
    for (int stop = coarse; stop <= all; ++stop) {
        SequenceResult r = cutAfter(band, stop);
        CHECK_MSG(r.valid, "cut after " + std::to_string(stop) + ": " + r.invalidReason);
        CHECK(r.samples == stop);
        CHECK(r.jointFeasibleShare >= 0.0 && r.jointFeasibleShare <= 1.0);
        // never a finer claim than the finished job's, and never coarser than an earlier cut's
        CHECK_MSG(r.resolutionMs >= fullBand.resolutionMs - 1e-9, "resolution " + std::to_string(r.resolutionMs) + " after " + std::to_string(stop));
        CHECK_MSG(r.resolutionMs <= previous + 1e-9, "resolution grew from " + std::to_string(previous) + " to " + std::to_string(r.resolutionMs) + " after " + std::to_string(stop));
        previous = r.resolutionMs;
        CHECK_MSG(std::fabs(r.jointFeasibleShare - truthBand) <= 0.06, "cut after " + std::to_string(stop) + ": share " + std::to_string(r.jointFeasibleShare) + " truth " + std::to_string(truthBand));
        CHECK(buildSequenceEvent(r, {1, 2}).ok);
    }
    SequenceResult last = cutAfter(band, all);
    CHECK(last.jointFeasibleShare == fullBand.jointFeasibleShare && last.resolutionMs == fullBand.resolutionMs);

    SECTION("cut short: a refinement round that was planned but not simulated is undone exactly");
    // cut right after round k was planned (its first trial handed out, nothing ingested): the
    // result must be the one of a planner that stopped after round k - 1
    int undone = 0;
    for (int round = 1; round <= 4; ++round) {
        SequencePlanner a(kSequence, axes, times);
        bool reached = false;
        for (int guard = 0; guard < 10000; ++guard) {
            auto b = a.nextBatch(1);
            if (b.empty()) break;
            if (a.result().refineRounds == round) {
                reached = true;   // this trial is the first of round `round`: leave it outstanding
                break;
            }
            a.ingest({b.front().id, band(b.front().shiftFrames), false});
        }
        if (!reached) break;   // the job has fewer rounds
        a.finish();
        SequenceResult ra = a.result();
        SequenceConfig stopEarly = kSequence;
        stopEarly.maxRefineRounds = round - 1;
        SequencePlanner b(stopEarly, axes, times);
        drive(b, band, 1);
        SequenceResult rb = b.result();
        CHECK_MSG(ra.valid && rb.valid, ra.invalidReason + rb.invalidReason);
        CHECK(ra.samples == rb.samples);
        CHECK_MSG(ra.jointFeasibleShare == rb.jointFeasibleShare, "round " + std::to_string(round) + ": " + std::to_string(ra.jointFeasibleShare) + " vs " + std::to_string(rb.jointFeasibleShare));
        CHECK_MSG(ra.resolutionMs == rb.resolutionMs, "round " + std::to_string(round) + ": " + std::to_string(ra.resolutionMs) + " vs " + std::to_string(rb.resolutionMs));
        CHECK(!ra.complete);
        ++undone;
    }
    CHECK(undone >= 1);

    SECTION("cut short: triples too (20 grid points), and an invalid trial is still a decided corner");
    auto axes3 = tickAxes({{-3, 3}, {-3, 3}, {-3, 3}});
    std::vector<double> times3{0.0, 50.0, 100.0};
    Pred slab = [](Shifts const& s) { return std::fabs(s[0] + s[1] + s[2]) <= 4.0 + 1e-9; };
    for (int stop : {0, 1, 8, 19, 20, 21, 30}) {
        SequencePlanner p(kSequence, axes3, times3);
        int ingested = 0;
        while (ingested < stop) {
            auto b = p.nextBatch(1);
            if (b.empty()) break;
            p.ingest({b.front().id, slab(b.front().shiftFrames), false});
            ++ingested;
        }
        p.finish();
        SequenceResult r = p.result();
        CHECK_MSG(r.valid == (stop >= 20), "triple cut after " + std::to_string(stop) + ": " + r.invalidReason);
    }
    // one invalid trial in a complete coarse grid: the corner is decided (as invalid), the result stands
    SequencePlanner inv(kSequence, axes, times);
    int nth = 0;
    for (int guard = 0; guard < coarse; ++guard) {
        auto b = inv.nextBatch(1);
        if (b.empty()) break;
        ++nth;
        inv.ingest({b.front().id, band(b.front().shiftFrames), nth == 7});
    }
    inv.finish();
    SequenceResult ri = inv.result();
    CHECK_MSG(ri.valid && ri.invalidTrials == 1 && ri.samples == coarse - 1, ri.invalidReason);
}

void testDeterminism() {
    SECTION("determinism: the same job gives the same trials and the same result, whatever the batch size");
    Pred ellipse = [](Shifts const& s) { return (s[0] * s[0]) / 9.0 + (s[1] * s[1]) / 4.0 <= 1.3; };
    std::vector<SequenceAxis> axes{makeAxis(subtickWindow(-3.0, 3.0), true, 21), makeAxis(subtickWindow(-2.0, 2.0), true, 21)};
    std::vector<double> times{0.0, 100.0};
    SequencePlanner a(kSequence, axes, times), b(kSequence, axes, times), c(kSequence, axes, times);
    Run ra = drive(a, ellipse, 8), rb = drive(b, ellipse, 8), rc = drive(c, ellipse, 1);
    CHECK(ra.ids == rb.ids);
    CHECK(ra.ids == rc.ids);
    SequenceResult x = a.result(), y = b.result(), z = c.result();
    CHECK(x.jointFeasibleShare == y.jointFeasibleShare && x.jointFeasibleShare == z.jointFeasibleShare);
    CHECK(x.resolutionMs == y.resolutionMs && x.samples == y.samples && x.samples == z.samples);
    CHECK(a.describe() == b.describe() && a.map() == c.map());
    std::set<int> unique(ra.ids.begin(), ra.ids.end());
    CHECK(unique.size() == ra.ids.size());   // no combination is simulated twice
    CHECK(a.describe().find("sim ") == 0);
}

void testReferenceSolver() {
    SECTION("reference SequenceSolver on the pull oracle: local windows first, then the joint lattice");
    // press at 1000 ms, release at 1050 ms. Feasible iff |a| <= 8, |b| <= 6 and the hold length is
    // within 5 ms of the performed one (|b - a| <= 5): each local window is +-5 ms, and inside that
    // 10 x 10 ms box the feasible region is the band -> share 1 - 2 * (5^2 / 2) / 100 = 0.75.
    InputSchedule s;
    s.inputs.push_back({1000.0, 1, Button::Jump, true});
    s.inputs.push_back({1050.0, 1, Button::Jump, false});
    auto feasible = [](std::vector<double> const& d) { return std::fabs(d[0]) <= 8.0 && std::fabs(d[1]) <= 6.0 && std::fabs(d[1] - d[0]) <= 5.0; };
    SyntheticJointOracle oracle({0, 1}, {1000.0, 1050.0}, feasible);
    SequenceSolver solver(oracle);
    auto out = solver.solve(0, s, {0, 1});
    CHECK_MSG(out.result.valid, out.result.invalidReason);
    CHECK(out.localWindows.size() == 2);
    CHECK_NEAR(out.result.localWidthsMs[0], 10.0, 0.1);
    CHECK_NEAR(out.result.localWidthsMs[1], 10.0, 0.1);
    CHECK_MSG(std::fabs(out.result.jointFeasibleShare - 0.75) <= 0.04, "share " + std::to_string(out.result.jointFeasibleShare));
    CHECK(out.result.samples >= 16 && out.result.samples <= kSequence.maxSamplesPair);
    CHECK(out.trials == out.result.samples + 1);           // + the control
    CHECK(oracle.jointTrials() == out.result.samples);     // every joint trial had both inputs shifted
    CHECK(oracle.crossedTrials() == 0);
    CHECK_NEAR(out.result.productMeasure, 100.0, 2.0);
    CHECK_NEAR(out.result.jointMeasure, 75.0, 5.0);
    CHECK(out.result.resolutionMs > 0.0 && out.result.resolutionMs < 5.0);
    auto event = buildSequenceEvent(out.result, {11, 14});
    CHECK_MSG(event.ok, event.error);
    CHECK(checkSequencePayload(event.payload).accepted);

    SECTION("reference solver: independent inputs, a triple, and groups it must refuse");
    auto box = [](std::vector<double> const& d) { return std::fabs(d[0]) <= 5.0 && std::fabs(d[1]) <= 7.0; };
    SyntheticJointOracle indep({0, 1}, {1000.0, 1050.0}, box);
    auto ind = SequenceSolver(indep).solve(0, s, {0, 1});
    CHECK_MSG(ind.result.valid, ind.result.invalidReason);
    CHECK(ind.result.jointFeasibleShare == 1.0);

    InputSchedule s3 = s;
    s3.inputs.push_back({1100.0, 1, Button::Jump, true});
    auto slab = [](std::vector<double> const& d) {
        return std::fabs(d[0]) <= 6.0 && std::fabs(d[1]) <= 6.0 && std::fabs(d[2]) <= 6.0 && std::fabs(d[0] + d[1] + d[2]) <= 9.0;
    };
    SyntheticJointOracle o3({0, 1, 2}, {1000.0, 1050.0, 1100.0}, slab);
    auto tri = SequenceSolver(o3).solve(0, s3, {0, 1, 2});
    CHECK_MSG(tri.result.valid, tri.result.invalidReason);
    CHECK(tri.result.inputs == 3 && tri.result.localWidthsMs.size() == 3);
    // the truth by brute force over the 12 x 12 x 12 ms box
    int in = 0, all = 0;
    for (int i = 0; i < 60; ++i) for (int j = 0; j < 60; ++j) for (int k = 0; k < 60; ++k) {
        std::vector<double> d{-6.0 + (i + 0.5) * 0.2, -6.0 + (j + 0.5) * 0.2, -6.0 + (k + 0.5) * 0.2};
        ++all;
        if (slab(d)) ++in;
    }
    double const truth = static_cast<double>(in) / static_cast<double>(all);
    CHECK_MSG(std::fabs(tri.result.jointFeasibleShare - truth) <= 0.08, "share " + std::to_string(tri.result.jointFeasibleShare) + " truth " + std::to_string(truth));
    CHECK(tri.result.samples <= kSequence.maxSamplesTriple);
    CHECK(o3.crossedTrials() == 0);

    // a miss: the performed press lies outside the passing region. The pull solvers take the
    // performed timing as passing, so the unshifted control is what catches it.
    auto missRegion = [](std::vector<double> const& d) { return d[0] >= 6.0 && d[0] <= 20.0; };
    SyntheticJointOracle missOracle({0, 1}, {1000.0, 1050.0}, missRegion);
    auto miss = SequenceSolver(missOracle).solve(0, s, {0, 1});
    CHECK(!miss.result.valid);
    CHECK_MSG(miss.result.invalidReason.find("unshifted schedule does not pass") != std::string::npos, miss.result.invalidReason);
    // bad groups
    CHECK(!solver.solve(0, s, {0}).result.valid);
    CHECK(!solver.solve(0, s, {1, 0}).result.valid);
    CHECK(!solver.solve(0, s, {0, 5}).result.valid);
    CHECK(!solver.solve(0, s3, {0, 1, 2, 2}).result.valid);

    // a few invalid trials from the oracle are tolerated, reported and left out
    SyntheticJointOracle flaky({0, 1}, {1000.0, 1050.0}, feasible);
    flaky.setInvalidEvery(9);
    auto fl = SequenceSolver(flaky).solve(0, s, {0, 1});
    CHECK_MSG(fl.result.valid, fl.result.invalidReason);
    CHECK(fl.result.invalidTrials >= 1);
    CHECK_MSG(std::fabs(fl.result.jointFeasibleShare - 0.75) <= 0.08, std::to_string(fl.result.jointFeasibleShare));
}

void testEvent() {
    SECTION("sequence_window payload: built only from a valid result; passes the validator and the server gate mirror");
    Pred band = [](Shifts const& s) { return std::fabs(s[1] - s[0]) <= 2.0 + 1e-9; };
    auto axes = tickAxes({{-2, 2}, {-2, 2}});
    SequencePlanner p(kSequence, axes, {0.0, 100.0});
    drive(p, band);
    SequenceResult r = p.result();
    auto built = buildSequenceEvent(r, {3, 5});
    CHECK_MSG(built.ok, built.error);
    auto const& pl = built.payload;
    CHECK((pl.inputSeqs == std::vector<int64_t>{3, 5}));
    CHECK(pl.localWidthsMs.size() == 2 && pl.localWidthsMs[0] == r.localWidthsMs[0]);
    CHECK(pl.jointFeasibleShare == r.jointFeasibleShare && pl.samples == 16);
    CHECK(pl.resolutionMs == r.resolutionMs && pl.solverVersion == "gprl-clone-seq/2");
    CHECK(checkSequencePayload(pl).accepted);

    telemetry::Batch batch;
    batch.sessionId = "s";
    batch.nonce = "n";
    batch.clientBuild = "gprl-geode 0.6.1+win";
    auto add = [&](double t, int64_t tick, int64_t seq, telemetry::Payload payload) {
        telemetry::Event e;
        e.t = t;
        e.tick = tick;
        e.seq = seq;
        e.attemptId = "a1";
        e.payload = std::move(payload);
        batch.events.push_back(std::move(e));
    };
    add(0.0, 0, 1, telemetry::AttemptStartPayload{});
    add(1.0, 240, 3, telemetry::InputPayload{1, Button::Jump, true, 0.0});
    add(1.05, 252, 5, telemetry::InputPayload{1, Button::Jump, false, 0.0});
    telemetry::AttemptEndPayload end;
    add(4.0, 960, 6, end);
    add(1.0, 240, 7, pl);   // deferred: the first input's t / tick, after attempt_end
    std::string err;
    CHECK_MSG(telemetry::validateBatch(batch, &err), err);
    CHECK_MSG(telemetry::checkBatchInvariants(batch, -1, {}, &err), err);
    std::string text = json::stringify(telemetry::toJson(batch.events.back()));
    CHECK_MSG(text.find("{\"kind\":\"sequence_window\",\"t\":1,\"tick\":240,\"seq\":7,\"attemptId\":\"a1\",\"inputSeqs\":[3,5],\"localWidthsMs\":[") == 0, text);
    CHECK(text.find("\"samples\":16,") != std::string::npos && text.find("\"solverVersion\":\"gprl-clone-seq/2\"}") != std::string::npos);
    telemetry::Batch back;
    CHECK_MSG(telemetry::parseBatch(telemetry::serializeBatch(batch), back, &err), err);
    CHECK(telemetry::canonicalBody(back) == telemetry::canonicalBody(batch));

    SECTION("the golden fixture's numbers are what the solver reports for such windows (batch-sequence.json)");
    {
        // pair: two 3-tick windows (12.5 ms each); both inputs off in the same direction fails
        // -> 4 simulated corners, 2 pass, 7 of the 9 lattice points feasible, 1 tick resolution
        auto pairAxes = tickAxes({{-1, 1}, {-1, 1}});
        SequencePlanner pp(kSequence, pairAxes, {360.0, 372.0});
        Pred opposite = [](Shifts const& s) { return s[0] * s[1] <= 0.0; };
        Run pr = drive(pp, opposite);
        SequenceResult fr = pp.result();
        CHECK_MSG(fr.valid, fr.invalidReason);
        CHECK(pr.trials == 4 && fr.samples == 4 && fr.knownLocal == 5 && fr.latticePoints == 9);
        CHECK_NEAR(fr.jointFeasibleShare, 0.7777777777777778, 1e-12);
        CHECK_NEAR(fr.resolutionMs, 4.166666666666667, 1e-12);
        CHECK_NEAR(fr.localWidthsMs[0], 12.5, 1e-9);
        // triple: windows of 3, 3 and 2 ticks (the third is 8.33 ms, one-sided around the input)
        // -> 18 lattice points, 6 known from the local windows, 12 simulated, 7 of them pass
        std::vector<SequenceAxis> tripleAxes = pairAxes;
        tripleAxes.push_back(makeAxis(tickWindow(0, 1), false, 21));
        SequencePlanner tp(kSequence, tripleAxes, {360.0, 372.0, 384.0});
        Pred rule = [](Shifts const& s) { return s[0] * s[1] <= 0.0 && !(s[0] == 1.0 && s[1] == 0.0 && s[2] == 1.0); };
        Run tr = drive(tp, rule);
        SequenceResult ft = tp.result();
        CHECK_MSG(ft.valid, ft.invalidReason);
        CHECK(tr.trials == 12 && ft.samples == 12 && ft.passed == 7 && ft.knownLocal == 6 && ft.latticePoints == 18);
        CHECK_NEAR(ft.jointFeasibleShare, 0.7222222222222222, 1e-12);
        CHECK_NEAR(ft.resolutionMs, 4.166666666666667, 1e-12);
        CHECK_NEAR(ft.localWidthsMs[2], 8.333333333333334, 1e-9);
        auto ev = buildSequenceEvent(ft, {2, 3, 4});
        CHECK_MSG(ev.ok, ev.error);
        CHECK(ev.payload.samples == 12 && ev.payload.inputSeqs.size() == 3);
    }

    SECTION("sequence_window payload: the builder and the gate refuse what the server would not use");
    CHECK(!buildSequenceEvent(r, {5, 3}).ok);      // not increasing
    CHECK(!buildSequenceEvent(r, {3, 3}).ok);
    CHECK(!buildSequenceEvent(r, {3}).ok);         // one seq for two inputs
    CHECK(!buildSequenceEvent(r, {3, 5, 9}).ok);
    CHECK(!buildSequenceEvent(r, {-1, 5}).ok);
    SequenceResult broken = r;
    broken.localWidthsMs[1] = 0.0;
    CHECK(!buildSequenceEvent(broken, {3, 5}).ok);
    broken = r;
    broken.jointFeasibleShare = 1.5;
    CHECK(!buildSequenceEvent(broken, {3, 5}).ok);
    broken = r;
    broken.resolutionMs = 0.0;
    CHECK(!buildSequenceEvent(broken, {3, 5}).ok);
    broken = r;
    broken.samples = 0;
    CHECK(!buildSequenceEvent(broken, {3, 5}).ok);
    broken = r;
    broken.valid = false;
    CHECK(!buildSequenceEvent(broken, {3, 5}).ok);

    auto rejects = [&](std::function<void(telemetry::SequenceWindowPayload&)> const& change, char const* reason) {
        telemetry::SequenceWindowPayload q = pl;
        change(q);
        auto c = checkSequencePayload(q);
        bool has = std::find(c.reasons.begin(), c.reasons.end(), std::string(reason)) != c.reasons.end();
        CHECK_MSG(!c.accepted && has, reason);
    };
    rejects([](auto& q) { q.jointFeasibleShare = std::nan(""); }, "non_finite_value");
    rejects([](auto& q) { q.inputSeqs = {3}; q.localWidthsMs = {4.0}; }, "input_count_out_of_range");
    rejects([](auto& q) { q.inputSeqs = {3, 5, 7, 9}; q.localWidthsMs = {4.0, 4.0, 4.0, 4.0}; }, "input_count_out_of_range");
    rejects([](auto& q) { q.localWidthsMs = {4.0}; }, "length_mismatch");
    rejects([](auto& q) { q.inputSeqs = {5, 3}; }, "input_seqs_not_increasing");
    rejects([](auto& q) { q.localWidthsMs[0] = 0.0; }, "local_width_out_of_range");
    rejects([](auto& q) { q.localWidthsMs[0] = 5000.0; }, "local_width_out_of_range");
    rejects([](auto& q) { q.jointFeasibleShare = 1.01; }, "share_out_of_range");
    rejects([](auto& q) { q.samples = 0; }, "samples_out_of_range");
    rejects([](auto& q) { q.samples = 100000; }, "samples_out_of_range");
    rejects([](auto& q) { q.resolutionMs = 0.0; }, "resolution_out_of_range");
    rejects([](auto& q) { q.resolutionMs = 500.0; }, "resolution_out_of_range");
}

void testEngineArithmetic() {
    SECTION("grouping: neighbours within the gap form a pair / triple ending at the newest input");
    std::vector<double> frames{10.0, 20.0, 30.0, 80.0, 80.0, 90.0, 110.0, 130.0};
    CHECK((sequence::groupEndingAt(frames, 1, 2) == std::vector<int>{0, 1}));
    CHECK((sequence::groupEndingAt(frames, 2, 2) == std::vector<int>{1, 2}));
    CHECK((sequence::groupEndingAt(frames, 2, 3) == std::vector<int>{0, 1, 2}));
    CHECK(sequence::groupEndingAt(frames, 3, 2).empty());        // 50 ticks apart
    CHECK(sequence::groupEndingAt(frames, 3, 3).empty());
    CHECK(sequence::groupEndingAt(frames, 4, 2).empty());        // the same instant: not two separable inputs
    CHECK((sequence::groupEndingAt(frames, 5, 2) == std::vector<int>{4, 5}));
    CHECK(sequence::groupEndingAt(frames, 5, 3).empty());        // 80, 80, 90: the first two coincide
    CHECK((sequence::groupEndingAt(frames, 7, 2) == std::vector<int>{6, 7}));
    CHECK(sequence::groupEndingAt(frames, 7, 3).empty());        // 90..130 spans 40 > 36 ticks
    CHECK((sequence::groupEndingAt(frames, 6, 3) == std::vector<int>{4, 5, 6}));   // 80..110 spans 30
    CHECK(sequence::groupEndingAt(frames, 0, 2).empty());
    CHECK(sequence::groupEndingAt(frames, 8, 2).empty());        // no such input
    CHECK(sequence::groupEndingAt(frames, 2, 1).empty() && sequence::groupEndingAt(frames, 2, 4).empty());
    SequenceConfig pairsOnly = kSequence;
    pairsOnly.maxGroupSize = 2;
    CHECK(sequence::groupEndingAt(frames, 2, 3, pairsOnly).empty());
    CHECK((sequence::groupEndingAt(frames, 2, 2, pairsOnly) == std::vector<int>{1, 2}));
    SequenceConfig tight = kSequence;
    tight.maxGapTicks = 9.0;
    CHECK(sequence::groupEndingAt(frames, 1, 2, tight).empty());

    SECTION("idle budget: the sequence job only gets what the local jobs leave, and never more than its share");
    // budget 666 steps / frame, cost not measured yet: own share 0.35 -> 233, idle line 0.6 -> 399
    CHECK(sequence::stepAllowance(666, 0, 0, 0, 0.0) == 233);
    CHECK(sequence::stepAllowance(666, 100, 0, 0, 0.0) == 233);
    CHECK(sequence::stepAllowance(666, 300, 0, 0, 0.0) == 99);      // the local jobs already used 300: only up to the idle line
    CHECK(sequence::stepAllowance(666, 399, 0, 0, 0.0) == 0);
    CHECK(sequence::stepAllowance(666, 650, 0, 0, 0.0) == 0);       // busy solver: no sequence work at all
    CHECK(sequence::stepAllowance(666, 0, 0, 233, 0.0) == 0);       // its own share is used up
    CHECK(sequence::stepAllowance(666, 0, 0, 200, 0.0) == 33);
    CHECK(sequence::stepAllowance(0, 0, 0, 0, 0.0) == 0);
    // the first physics step of a frame: the local jobs have used little SO FAR, but the previous
    // frame says they will need 350 -> the sequence job leaves them that (399 - 350 = 49)
    CHECK(sequence::stepAllowance(666, 20, 350, 0, 0.0) == 49);
    CHECK(sequence::stepAllowance(666, 20, 400, 0, 0.0) == 0);
    CHECK(sequence::stepAllowance(666, 380, 100, 0, 0.0) == 19);    // and what they already used counts when it is more
    CHECK(sequence::stepAllowance(666, 100, 100, 150, 0.0) == 83);  // own 233 - 150
    CHECK(sequence::stepAllowance(666, 200, 200, 150, 0.0) == 49);  // idle 399 - 200 - 150
    // time cap: at 3 us per clone step 0.5 ms is 166 steps; a slow machine (10 us) gets 50
    CHECK(sequence::stepAllowance(666, 0, 0, 0, 3.0) == 166);
    CHECK(sequence::stepAllowance(666, 0, 0, 0, 10.0) == 50);
    CHECK(sequence::stepAllowance(666, 0, 0, 100, 3.0) == 66);
    CHECK(sequence::stepAllowance(6000, 0, 0, 0, 0.3) == 1666);     // fast machine: 0.5 ms still rules (the share would be 2100)
    CHECK(sequence::stepAllowance(300, 0, 0, 0, 1.0) == 105);       // small budget: the share rules (0.5 ms would be 500)
    for (int budget : {300, 666, 2000, 6000}) {
        for (int local = 0; local <= budget; local += 37) {
            for (int last : {0, local / 2, local, budget}) {
                for (int seq : {0, 40, 5000}) {
                    int a = sequence::stepAllowance(budget, local, last, seq, 3.0);
                    CHECK(a >= 0);
                    // never pushes the frame over the step budget, never past its own share, never when the local jobs are busy
                    if (a > 0) CHECK(local + seq + a <= budget);
                    CHECK(static_cast<double>(seq + a) <= std::max(static_cast<double>(seq), kSequence.frameShare * budget) + 1e-9);
                    if (std::max(local, last) >= static_cast<int>(kSequence.idleLoadShare * budget)) CHECK(a == 0);
                }
            }
        }
    }

    SECTION("load guard: no sequence work within its own share of the throttle that pauses local measurements");
    // throttle 4 ms (budget.hpp throttleOnMs), sequence share 0.5 ms per frame
    double const throttle = budget::kBudget.throttleOnMs;
    CHECK(throttle == 4.0 && kSequence.targetMsPerFrame == 0.5);
    CHECK(sequence::loadAllows(0.0, throttle));
    CHECK(sequence::loadAllows(1.2, throttle));      // the usual idle case: local + sequence under 60 % of 2 ms
    CHECK(sequence::loadAllows(3.49, throttle));
    CHECK(!sequence::loadAllows(3.5, throttle));     // 3.5 + 0.5 would reach the throttle
    CHECK(!sequence::loadAllows(3.9, throttle));
    CHECK(!sequence::loadAllows(4.5, throttle));
    CHECK(!sequence::loadAllows(0.0, 0.5) && !sequence::loadAllows(0.0, 0.0));
    // the slow-machine case the step allowance alone lets through: the step budget at its minimum
    // (300) and 25 us per clone step - the local jobs at 140 steps are 3.5 ms, under 60 % of the
    // budget, and the allowance would add 0.5 ms right up to the 4 ms throttle
    CHECK(budget::stepBudgetForFrame(25.0) == 300);
    CHECK(sequence::stepAllowance(300, 140, 140, 0, 25.0) == 20);
    CHECK(!sequence::loadAllows(140 * 25.0 / 1000.0, throttle));
    // the 1 s average is EMA' = 0.6 EMA + 0.4 (local + sequence) (CloneEngine::frameEnd): with the
    // guard, a second in which a sequence job ran ends above the throttle only if the local jobs
    // alone were above it in that second
    for (int e = 0; e < 80; ++e) {
        for (int l = 0; l <= 160; ++l) {
            double const ema = e * 0.05, local = l * 0.05;
            if (!sequence::loadAllows(ema, throttle)) continue;
            double const next = 0.6 * ema + 0.4 * (local + kSequence.targetMsPerFrame);
            if (next > throttle) CHECK(local > throttle);
        }
    }

    SECTION("clone grant: the reserve for the local jobs is never touched");
    using sequence::CloneGrant;
    CHECK(sequence::cloneGrant(30, 48, 320, 0, 0, 3) == CloneGrant::Idle);
    CHECK(sequence::cloneGrant(25, 48, 320, 0, 0, 3) == CloneGrant::Idle);
    CHECK(sequence::cloneGrant(24, 48, 320, 0, 0, 3) == CloneGrant::Create);   // at the reserve: only a new clone
    CHECK(sequence::cloneGrant(0, 100, 320, 0, 0, 3) == CloneGrant::Create);
    CHECK(sequence::cloneGrant(0, 100, 320, 0, 1, 3) == CloneGrant::None);     // one creation per frame for sequences
    CHECK(sequence::cloneGrant(0, 100, 320, 3, 0, 3) == CloneGrant::None);     // the local jobs used the frame's ration
    CHECK(sequence::cloneGrant(10, 296, 320, 0, 0, 3) == CloneGrant::None);    // the pool's last 24 belong to the local jobs
    CHECK(sequence::cloneGrant(10, 295, 320, 0, 0, 3) == CloneGrant::Create);

    SECTION("position key: one block buckets, the group size is part of the key");
    CHECK(sequence::positionKey(0.0, 2) == sequence::positionKey(29.9, 2));
    CHECK(sequence::positionKey(29.9, 2) != sequence::positionKey(30.1, 2));
    CHECK(sequence::positionKey(100.0, 2) != sequence::positionKey(100.0, 3));
    CHECK(sequence::positionKey(-5.0, 2) != sequence::positionKey(5.0, 2));
    CHECK(sequence::positionKey(123456.0, 3) == sequence::positionKey(123450.0, 3));
}

/// 600 random jobs (whole-tick and sub-tick axes of every size, pairs and triples, slab and
/// ellipsoid regions that contain the local windows, random gaps so some combinations cross):
/// the planner's invariants hold for every one, and the estimate stays near the lattice's truth.
void testRandomized() {
    SECTION("randomised jobs: invariants for every lattice shape and region, estimate near the lattice's own share");
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    auto uni = [&](int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint64_t>(hi - lo + 1)); };
    auto real = [&] { return static_cast<double>(next() % 1000000ull) / 1000000.0; };
    int jobs = 0, trivial = 0, exactCount = 0, capped = 0;
    double worstCapped = 0.0, worstFull = 0.0;
    // error statistics by group size (index n - 2) and by whether the sample cap stopped the job
    struct Stat {
        int count = 0;
        double sum = 0.0, worst = 0.0;
        int samples = 0;
        void add(double e, int s) { ++count; sum += e; worst = std::max(worst, e); samples += s; }
        double mean() const { return count ? sum / count : 0.0; }
        double meanSamples() const { return count ? static_cast<double>(samples) / count : 0.0; }
    };
    Stat full[2], cut[2];
    for (int it = 0; it < 600; ++it) {
        int const n = uni(2, 3);
        bool const continuous = uni(0, 3) == 0;
        std::vector<SequenceAxis> axes;
        std::vector<double> times;
        std::array<double, 3> reach{1.0, 1.0, 1.0};
        double t = 100.0;
        for (int i = 0; i < n; ++i) {
            if (continuous) {
                double e = -real() * 4.0, l = real() * 4.0;
                if (l - e < 0.3) l = e + 0.6;
                axes.push_back(makeAxis(subtickWindow(std::min(e, 0.0), std::max(l, 0.0)), true, n == 2 ? kSequence.maxAtomsPerAxisPair : kSequence.maxAtomsPerAxisTriple));
            }
            else {
                int kmin = -uni(0, 6), kmax = uni(0, 6);
                bool be = uni(0, 1) == 1 || (kmin == 0 && kmax == 0);
                bool bl = uni(0, 1) == 1 || (kmin == 0 && kmax == 0);
                axes.push_back(makeAxis(tickWindow(kmin, kmax, be, bl), false, 21));
            }
            CHECK_MSG(axes.back().valid, axes.back().invalidReason);
            reach[static_cast<size_t>(i)] = std::max({1e-3, -axes.back().positions.front(), axes.back().positions.back()});
            times.push_back(t);
            t += continuous ? 0.5 + real() * 20.0 : static_cast<double>(uni(1, 20));
        }
        // a region that contains every single-input shift of the lattice (the local windows hold)
        Pred region;
        if (uni(0, 1) == 0) {
            std::array<double, 3> a{uni(0, 1) ? 1.0 : -1.0, uni(0, 1) ? 1.0 : -0.5, uni(0, 1) ? 0.75 : -1.0};
            double c = 0.0;
            for (int i = 0; i < n; ++i) c = std::max(c, std::fabs(a[static_cast<size_t>(i)]) * reach[static_cast<size_t>(i)]);
            c *= 1.0 + real() * 0.8;
            region = [a, c, n](Shifts const& s) {
                double v = 0.0;
                for (int i = 0; i < n; ++i) v += a[static_cast<size_t>(i)] * s[static_cast<size_t>(i)];
                return std::fabs(v) <= c + 1e-9;
            };
        }
        else {
            std::array<double, 3> r{reach[0] * (1.0 + real()), reach[1] * (1.0 + real()), reach[2] * (1.0 + real())};
            region = [r, n](Shifts const& s) {
                double v = 0.0;
                for (int i = 0; i < n; ++i) v += (s[static_cast<size_t>(i)] * s[static_cast<size_t>(i)]) / (r[static_cast<size_t>(i)] * r[static_cast<size_t>(i)]);
                return v <= 1.0 + 1e-9;
            };
        }
        SequencePlanner p(kSequence, axes, times);
        CHECK_MSG(p.valid(), p.invalidReason());
        if (!p.valid()) continue;
        ++jobs;
        double const truth = exactShare(axes, times, region);
        int const cap = n == 2 ? kSequence.maxSamplesPair : kSequence.maxSamplesTriple;
        Run run = drive(p, region, uni(1, 6));
        SequenceResult r = p.result();
        CHECK(p.done() && p.outstanding() == 0);
        CHECK(run.trials <= cap && r.samples == run.trials && r.invalidTrials == 0);
        CHECK(r.jointFeasibleShare >= 0.0 && r.jointFeasibleShare <= 1.0);
        CHECK(r.samples + r.knownLocal + r.crossed <= r.latticePoints);
        std::set<int> unique(run.ids.begin(), run.ids.end());
        CHECK(unique.size() == run.ids.size());
        for (auto const& s : run.shifts) {
            CHECK(movedCount(s) >= 2);
            for (int i = 0; i + 1 < n; ++i) {
                CHECK((times[static_cast<size_t>(i + 1)] + s[static_cast<size_t>(i + 1)]) - (times[static_cast<size_t>(i)] + s[static_cast<size_t>(i)]) >= kSequence.neighbourMarginFrames - 1e-9);
            }
        }
        if (r.trivial) {
            ++trivial;
            CHECK(!r.valid && run.trials == 0);
            // nothing off the axes to simulate: the share is decided by the order rule alone
            CHECK_NEAR(r.jointFeasibleShare, truth, 1e-9);
            continue;
        }
        CHECK_MSG(r.valid, r.invalidReason);
        CHECK(r.resolutionMs > 0.0 && std::isfinite(r.resolutionMs));
        CHECK(buildSequenceEvent(r, n == 2 ? std::vector<int64_t>{1, 2} : std::vector<int64_t>{1, 2, 3}).ok);
        double const err = std::fabs(r.jointFeasibleShare - truth);
        if (r.budgetExhausted) {
            ++capped;
            worstCapped = std::max(worstCapped, err);
            cut[n - 2].add(err, r.samples);
        }
        else {
            // refinement ran to the end: every box whose corners disagree is one atom wide
            CHECK(r.complete);
            worstFull = std::max(worstFull, err);
            if (err < 1e-12) ++exactCount;
            full[n - 2].add(err, r.samples);
        }
        // the same job again gives the same trials and the same number
        SequencePlanner again(kSequence, axes, times);
        Run run2 = drive(again, region, 4);
        CHECK(run2.ids == run.ids && again.result().jointFeasibleShare == r.jointFeasibleShare);
    }
    std::printf("  randomised: %d jobs (%d trivial), %d finished refinement (%d exact, worst error %.4f), %d stopped by the sample cap (worst error %.4f)\n",
                jobs, trivial, jobs - trivial - capped, exactCount, worstFull, capped, worstCapped);
    for (int k = 0; k < 2; ++k) {
        std::printf("  randomised %s: finished %d (mean error %.4f, worst %.4f, %.1f samples), capped %d (mean error %.4f, worst %.4f, %.1f samples)\n",
                    k == 0 ? "pairs  " : "triples", full[k].count, full[k].mean(), full[k].worst, full[k].meanSamples(), cut[k].count, cut[k].mean(), cut[k].worst,
                    cut[k].meanSamples());
    }
    CHECK(jobs == 600);
    // Regression guards on this fixed set (the generator is deterministic). A finished refinement
    // is the lattice's exact share except where a bulge of the region lies inside a coarse box
    // whose corners agree (the documented blind spot: 1 job of 360 here, 0.024). Under the sample
    // cap the estimate is an interpolation: pairs stay within 0.02, large triples (three windows
    // of 10+ ticks with a dependent region) can be off by 0.16 and say so through `resolutionMs`
    // and `budgetExhausted`.
    CHECK(full[0].count + full[1].count >= 350 && exactCount >= full[0].count + full[1].count - 2);
    CHECK_MSG(full[0].worst <= 1e-12, "pairs after full refinement " + std::to_string(full[0].worst));
    CHECK_MSG(worstFull <= 0.03, "worst error after full refinement " + std::to_string(worstFull));
    CHECK_MSG(cut[0].worst <= 0.02 && cut[0].mean() <= 0.006, "capped pairs: worst " + std::to_string(cut[0].worst) + " mean " + std::to_string(cut[0].mean()));
    CHECK_MSG(cut[1].worst <= 0.17 && cut[1].mean() <= 0.025, "capped triples: worst " + std::to_string(cut[1].worst) + " mean " + std::to_string(cut[1].mean()));
    CHECK(worstCapped <= 0.17);
}

}  // namespace

int main() {
    testConfig();
    testLocalWindowInfo();
    testAxis();
    testNodes();
    testIndependentPair();
    testDiagonalBand();
    testOrderCrossing();
    testContinuous();
    testTriple();
    testDegenerate();
    testInvalidOutcomes();
    testBudgetBatchesFinish();
    testCutShort();
    testDeterminism();
    testReferenceSolver();
    testEvent();
    testEngineArithmetic();
    testRandomized();
    return gprl::test::finish("sequence_tests");
}
