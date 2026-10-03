// PassPlanner host tests (docs/SOLVER_DESIGN.md §9.1): equivalence with LocalWindowSolver over the
// synthetic oracle, miss windows, late limits arriving after pass 0, NotTested gaps, pass count /
// resolution per sub-tick setting, budget cuts, determinism.
#include "test_util.hpp"
#include "synthetic_oracle.hpp"

#include "../core/solver/local_window.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_status.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace gprl;
using namespace gprl::solver;
using gprl::test::SyntheticOracle;

namespace {

ShiftOutcome outcome(double shift, ShiftKind kind, double deathAfter = 0.0, int obj = -1) {
    ShiftOutcome o;
    o.nominalFrames = o.appliedFrames = shift;
    o.kind = kind;
    o.deathAfterFrames = deathAfter;
    o.objectId = obj;
    return o;
}

/// Outcomes of a pass from a set of pass intervals in FRAMES relative to the actual input.
std::vector<ShiftOutcome> fromIntervals(std::vector<double> const& shifts, std::vector<std::pair<double, double>> const& pass) {
    std::vector<ShiftOutcome> out;
    for (double s : shifts) {
        bool ok = false;
        for (auto const& [a, b] : pass) if (s >= a - 1e-9 && s <= b + 1e-9) ok = true;
        out.push_back(outcome(s, ok ? ShiftKind::Survived : ShiftKind::Died, 3.0, 8));
    }
    return out;
}

void testCoarsePassShape() {
    SECTION("pass 0 = +-1..10 ticks, closest first, later / earlier alternating");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    CHECK(shifts.size() == 20);
    CHECK(shifts.size() >= 4 && shifts[0] == 1.0 && shifts[1] == -1.0 && shifts[2] == 2.0 && shifts[3] == -2.0);
    CHECK(shifts.back() == -10.0);
    CHECK(!p.done());
    CHECK(p.nextPass().empty());   // waiting for the ingest
    // early limit 3.5 frames (history / neighbour), sub-tick placement: -1 -2 -3 and the limit
    // point -3.5 (BoundarySearch `Limit` phase)
    PlannerConfig sub;
    sub.subtick = true;
    PassPlanner q(sub, 3.5);
    auto s2 = q.nextPass();
    int early = 0;
    bool limitPoint = false;
    for (double s : s2) {
        if (s < 0) ++early;
        if (std::fabs(s + 3.5) < 1e-9) limitPoint = true;
    }
    CHECK(early == 4);
    CHECK(limitPoint);
    CHECK(!q.early().blocked);
    // T-FIX-3 (V2-D8, RC-minor 5): WITHOUT sub-tick placement the exact limit point is never
    // planned - the engine would apply it on the neighbour's own step (`-7.9976(-8)A`)
    PassPlanner qn(cfg, 3.5);
    int earlyN = 0;
    bool limitPointN = false;
    for (double s : qn.nextPass()) {
        if (s < 0) ++earlyN;
        if (std::fabs(s + 3.5) < 1e-9) limitPointN = true;
    }
    CHECK(earlyN == 3);
    CHECK(!limitPointN);
    // blocked early side: limit below one tick -> only the limit point with sub-tick placement
    // (BoundarySearch semantics), nothing at all without it
    PassPlanner b(sub, 0.4);
    auto s3 = b.nextPass();
    int early3 = 0;
    for (double s : s3) if (s < 0) ++early3;
    CHECK(early3 == 1);
    CHECK(b.early().blocked);
    PassPlanner bn(cfg, 0.4);
    int early3n = 0;
    for (double s : bn.nextPass()) if (s < 0) ++early3n;
    CHECK(early3n == 0);
    CHECK(bn.early().blocked);
    // no room at all
    PassPlanner z(cfg, 0.0);
    auto s4 = z.nextPass();
    for (double s : s4) CHECK(s > 0);
    // step-grid alignment: input at x.5 -> shifts +0.5, +1.5 ... and -0.5, -1.5 ... (plus the 10
    // point only with sub-tick placement)
    PlannerConfig off;
    off.lateOffsetFrames = 0.5;
    off.earlyOffsetFrames = 0.5;
    PassPlanner g(off, kNaN);
    auto s5 = g.nextPass();
    CHECK(s5.size() == 20);
    CHECK(s5[0] == 0.5 && s5[1] == -0.5 && s5[2] == 1.5);
    bool ten = false;
    for (double s : s5) if (s == 10.0) ten = true;
    CHECK(!ten);
    off.subtick = true;
    PassPlanner gs(off, kNaN);
    auto s6 = gs.nextPass();
    CHECK(s6.size() == 22);
    ten = false;
    for (double s : s6) if (s == 10.0) ten = true;
    CHECK(ten);
}

void testHitWindowCoarse() {
    SECTION("coarse hit window: last pass / first fail per side, resolution one tick");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    // passes for shifts in [-2, +3]
    p.ingest(fromIntervals(shifts, {{-2.0, 3.0}}), true, false);
    CHECK(p.done());
    CHECK(!p.miss());
    CHECK(!p.refined());
    CHECK_NEAR(p.early().lastPass, 2.0, 1e-12);
    CHECK_NEAR(p.early().firstFail, 3.0, 1e-12);
    CHECK_NEAR(p.late().lastPass, 3.0, 1e-12);
    CHECK_NEAR(p.late().firstFail, 4.0, 1e-12);
    auto w = p.result(1000.0);
    CHECK(w.valid);
    CHECK(w.boundedEarly && w.boundedLate);
    CHECK_NEAR(w.earliestMs, 1000.0 - 2.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestMs, 1000.0 + 3.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.earliestFailMs, 1000.0 - 3.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestFailMs, 1000.0 + 4.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.resolutionMs, kTickMs, 1e-9);
    CHECK(w.trials == 20);
    CHECK(!w.nonMonotonic);
    // trial phases: coarse up to the first fail, island scan beyond
    int island = 0, coarse = 0;
    for (auto const& t : w.late.trials) {
        if (t.phase == TrialPhase::IslandScan) ++island;
        if (t.phase == TrialPhase::Coarse) ++coarse;
    }
    CHECK(coarse == 4 && island == 6);
    CHECK(!p.describe().empty());
}

void testUnboundedAndIslands() {
    SECTION("unbounded side, islands beyond the first fail, non-monotonic flag");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    // late: passes everywhere (unbounded); early: pass -1, fail -2 -3, island -4..-5, fail -6..
    p.ingest(fromIntervals(shifts, {{0.0, 10.0}, {-1.0, -1.0}, {-5.0, -4.0}}), true, false);
    CHECK(p.done());
    CHECK(p.late().exhausted);
    CHECK(!p.late().bounded());
    CHECK(p.early().bounded());
    CHECK(p.early().nonMonotonic);
    CHECK(p.early().islands.size() == 1);
    if (!p.early().islands.empty()) {
        CHECK_NEAR(p.early().islands[0].fromShiftMs, -4.0, 1e-9);
        CHECK_NEAR(p.early().islands[0].toShiftMs, -5.0, 1e-9);
    }
    auto w = p.result(500.0);
    CHECK(w.valid);
    CHECK(!w.boundedLate && w.boundedEarly);
    CHECK_NEAR(w.latestMs, 500.0 + 10.0 * kTickMs, 1e-9);
    CHECK(w.nonMonotonic);
    CHECK(w.early.islands.size() == 1);
}

void testMissWindow() {
    SECTION("miss: control died with the real player -> nearest pass run, actual outside");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    // the actual input was too late: shifts -4..-2 pass, everything else dies
    p.ingest(fromIntervals(shifts, {{-4.0, -2.0}}), false, true);
    CHECK(p.done());
    CHECK(p.miss());
    CHECK(!p.controlFailed());
    auto w = p.result(2000.0);
    CHECK(w.valid);
    CHECK_NEAR(w.earliestMs, 2000.0 - 4.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestMs, 2000.0 - 2.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.earliestFailMs, 2000.0 - 5.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestFailMs, 2000.0 - 1.0 * kTickMs, 1e-9);
    CHECK(w.boundedEarly && w.boundedLate);
    CHECK(w.actualMs > w.latestMs);   // a miss: the actual time is outside
    // nearest run wins when there are two (islands logged)
    PassPlanner q(cfg, kNaN);
    auto s2 = q.nextPass();
    q.ingest(fromIntervals(s2, {{-8.0, -7.0}, {2.0, 3.0}}), false, true);
    auto w2 = q.result(0.0);
    CHECK(w2.valid);
    CHECK_NEAR(w2.earliestMs, 2.0 * kTickMs, 1e-9);
    CHECK_NEAR(w2.latestMs, 3.0 * kTickMs, 1e-9);
    CHECK(w2.nonMonotonic);
    // the run touching the edge of the range is unbounded on that side
    PassPlanner r(cfg, kNaN);
    auto s3 = r.nextPass();
    r.ingest(fromIntervals(s3, {{6.0, 10.0}}), false, true);
    auto w3 = r.result(0.0);
    CHECK(w3.valid && w3.boundedEarly && !w3.boundedLate);
    CHECK_NEAR(w3.earliestFailMs, 5.0 * kTickMs, 1e-9);
    // no passing shift at all -> invalid, never emitted
    PassPlanner n(cfg, kNaN);
    auto s4 = n.nextPass();
    n.ingest(fromIntervals(s4, {}), false, true);
    CHECK(!n.result(0.0).valid);
    // control neither alive nor died with the real player = mismatch -> invalid
    PassPlanner m(cfg, kNaN);
    auto s5 = m.nextPass();
    m.ingest(fromIntervals(s5, {{-10.0, 10.0}}), false, false);
    CHECK(m.done() && m.controlFailed() && !m.result(0.0).valid);
}

void testLateLimitAfterPass0() {
    SECTION("late limit arriving after pass 0: Limit shifts ignored, blocked / unbounded sides");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    // the next same-channel input is logged 2.5 frames later (limit 2.4976): +3.. are Limit
    p.setLateLimit(2.4976);
    CHECK(p.late().limitKnown && !p.late().blocked);
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        if (s >= 2.4976) outs.push_back(outcome(s, ShiftKind::NotTested));
        else outs.push_back(outcome(s, ShiftKind::Survived));
    }
    p.ingest(outs, true, false);
    CHECK(p.done());   // coarse only: nothing more to plan
    auto w = p.result(0.0);
    CHECK(w.valid);
    CHECK(!w.boundedLate);
    CHECK_NEAR(w.latestMs, 2.0 * kTickMs, 1e-9);   // largest tested pass, never the untested limit
    CHECK_NEAR(w.late.limitMs, 2.4976 * kTickMs, 1e-9);
    CHECK(!w.late.budgetExhausted);
    // a limit below one tick blocks the side
    PassPlanner b(cfg, kNaN);
    auto s2 = b.nextPass();
    b.setLateLimit(0.5);
    std::vector<ShiftOutcome> o2;
    for (double s : s2) o2.push_back(outcome(s, s > 0 ? ShiftKind::NotTested : ShiftKind::Survived));
    b.ingest(o2, true, false);
    auto w2 = b.result(0.0);
    CHECK(w2.valid && w2.blockedLate && !w2.boundedLate);
    CHECK_NEAR(w2.latestMs, 0.0, 1e-9);
    // with sub-tick placement the exact limit point is tested in the next pass
    PlannerConfig sub = cfg;
    sub.subtick = true;
    PassPlanner c(sub, kNaN);
    auto s3 = c.nextPass();
    c.setLateLimit(2.4976);
    std::vector<ShiftOutcome> o3;
    for (double s : s3) o3.push_back(outcome(s, s >= 2.4976 ? ShiftKind::NotTested : ShiftKind::Survived));
    c.ingest(o3, true, false);
    CHECK(!c.done());
    auto s4 = c.nextPass();
    bool hasLimit = false;
    for (double s : s4) if (std::fabs(s - 2.4976) < 1e-9) hasLimit = true;
    CHECK(hasLimit);
    c.ingest({outcome(2.4976, ShiftKind::Survived)}, true, false);
    CHECK(c.done());
    auto w3 = c.result(0.0);
    CHECK(w3.valid && !w3.boundedLate);
    CHECK_NEAR(w3.latestMs, 2.4976 * kTickMs, 1e-9);
    CHECK(w3.late.trials.back().phase == TrialPhase::Limit);
    // a limit-only pass subdivides no bracket: the window is not "refined" (stays gprl-clone/1)
    CHECK(!c.refined());
    CHECK(c.passesIngested() == 2);
}

void testNotTestedGaps() {
    SECTION("NotTested gaps widen the bracket (and mark it cut) instead of guessing");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        if (s == 2.0) outs.push_back(outcome(s, ShiftKind::NotTested, 0, -1));   // pool ran out here
        else outs.push_back(outcome(s, s >= -1.0 && s <= 1.0 ? ShiftKind::Survived : ShiftKind::Died, 2.0, 9));
    }
    p.ingest(outs, true, false, false);
    auto w = p.result(0.0);
    CHECK(w.valid);
    CHECK_NEAR(w.latestMs, 1.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestFailMs, 3.0 * kTickMs, 1e-9);   // the fail at +3: +2 untested
    CHECK_NEAR(w.late.bracketMs, 2.0 * kTickMs, 1e-9);
    CHECK(w.late.budgetExhausted);
    CHECK(w.budgetExhausted);
    CHECK_NEAR(w.early.bracketMs, kTickMs, 1e-9);
    CHECK_NEAR(w.resolutionMs, 2.0 * kTickMs, 1e-9);
}

void testRefinementPasses() {
    SECTION("sub-tick refinement: 1/8 tick = 1 pass of 7 points per side, 1/64 tick = 2 passes");
    PlannerConfig cfg;
    cfg.subtick = true;
    cfg.resolutionFrames = 0.125;
    cfg.maxRefinePasses = 1;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    // true window in frames: [-1.3, +2.6]
    std::vector<std::pair<double, double>> truth = {{-1.3, 2.6}};
    p.ingest(fromIntervals(shifts, truth), true, false);
    CHECK(!p.done());
    auto pass1 = p.nextPass();
    CHECK(pass1.size() == 14);
    for (double s : pass1) CHECK((s > 2.0 && s < 3.0) || (s < -1.0 && s > -2.0));
    p.ingest(fromIntervals(pass1, truth), true, false);
    CHECK(p.done());
    CHECK(p.refined());
    auto w = p.result(0.0);
    CHECK(w.valid);
    CHECK_NEAR(w.resolutionMs, 0.125 * kTickMs, 1e-9);
    CHECK(w.earliestMs >= -1.3 * kTickMs - 1e-9 && w.earliestMs > -1.3 * kTickMs - 0.125 * kTickMs);
    CHECK(w.latestMs <= 2.6 * kTickMs + 1e-9 && w.latestMs > 2.6 * kTickMs - 0.125 * kTickMs);
    CHECK(w.early.trials.back().phase == TrialPhase::Bisect);
    // 1/64 tick: a second pass inside the 1/8 bracket
    PlannerConfig fine = cfg;
    fine.resolutionFrames = 1.0 / 64.0;
    fine.maxRefinePasses = 2;
    PassPlanner q(fine, kNaN);
    auto s0 = q.nextPass();
    q.ingest(fromIntervals(s0, truth), true, false);
    auto s1 = q.nextPass();
    CHECK(s1.size() == 14);
    q.ingest(fromIntervals(s1, truth), true, false);
    CHECK(!q.done());
    auto s2 = q.nextPass();
    CHECK(s2.size() == 14);
    q.ingest(fromIntervals(s2, truth), true, false);
    CHECK(q.done());
    auto w2 = q.result(0.0);
    CHECK(w2.valid);
    CHECK_NEAR(w2.resolutionMs, kTickMs / 64.0, 1e-9);
    CHECK(q.passesIngested() == 3);
    // an unbounded side is never refined; a coarse-only planner never asks for a pass 1
    PassPlanner u(cfg, kNaN);
    auto us = u.nextPass();
    u.ingest(fromIntervals(us, {{-10.0, 10.0}}), true, false);
    CHECK(u.done());
    PlannerConfig coarse;
    PassPlanner c(coarse, kNaN);
    auto cs = c.nextPass();
    c.ingest(fromIntervals(cs, truth), true, false);
    CHECK(c.done() && !c.refined());
    // a refinement pass cut by the pool (NotTested points inside the bracket)
    PassPlanner k(cfg, kNaN);
    auto k0 = k.nextPass();
    k.ingest(fromIntervals(k0, truth), true, false);
    auto k1 = k.nextPass();
    std::vector<ShiftOutcome> cut;
    for (size_t i = 0; i < k1.size(); ++i) {
        if (i % 2 == 0) cut.push_back(outcome(k1[i], ShiftKind::NotTested));
        else {
            bool ok = k1[i] >= -1.3 && k1[i] <= 2.6;
            cut.push_back(outcome(k1[i], ok ? ShiftKind::Survived : ShiftKind::Died, 1.0, 8));
        }
    }
    k.ingest(cut, true, false, false);
    auto wk = k.result(0.0);
    CHECK(wk.valid);
    CHECK(wk.budgetExhausted);
    CHECK(wk.resolutionMs > 0.125 * kTickMs);
}

void testMissRefinement() {
    SECTION("a miss window is refined at its run edges too");
    PlannerConfig cfg;
    cfg.subtick = true;
    PassPlanner p(cfg, kNaN);
    auto s0 = p.nextPass();
    std::vector<std::pair<double, double>> truth = {{-4.4, -1.7}};
    p.ingest(fromIntervals(s0, truth), false, true);
    CHECK(!p.done());
    auto s1 = p.nextPass();
    CHECK(s1.size() == 14);
    for (double s : s1) CHECK((s > -5.0 && s < -4.0) || (s > -2.0 && s < -1.0));
    p.ingest(fromIntervals(s1, truth), false, true);
    CHECK(p.done());
    auto w = p.result(100.0);
    CHECK(w.valid && p.miss());
    CHECK(w.earliestMs >= 100.0 - 4.4 * kTickMs - 1e-9);
    CHECK(w.latestMs <= 100.0 - 1.7 * kTickMs + 1e-9);
    CHECK(w.actualMs > w.latestMs);
}

void testEquivalenceWithLocalWindowSolver() {
    SECTION("equivalence with LocalWindowSolver over 200 random synthetic pass-interval sets");
    std::mt19937 rng(20260929u);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    int agree = 0, cases = 0;
    for (int n = 0; n < 200; ++n) {
        double actual = 1000.0;
        // a main pass interval around the actual input (may be one-sided / unbounded), plus 0-2 islands
        double a = actual - u01(rng) * 12.0 * kTickMs;
        double b = actual + u01(rng) * 12.0 * kTickMs;
        if (n % 7 == 0) a = actual - 100.0 * kTickMs;   // unbounded early
        if (n % 11 == 0) b = actual + 100.0 * kTickMs;  // unbounded late
        std::vector<SyntheticOracle::Interval> iv = {{a, b}};
        // Islands on whole ticks (3..9 ticks away, 0..2 ticks wide): an island strictly inside a
        // coarse bracket is invisible to bisection but visible to the planner's refinement, so
        // the two searches are only comparable when islands sit on the coarse grid.
        int islands = static_cast<int>(u01(rng) * 3.0);
        for (int i = 0; i < islands; ++i) {
            int k = 3 + static_cast<int>(u01(rng) * 7.0);
            int wdt = static_cast<int>(u01(rng) * 3.0);
            double sign = u01(rng) < 0.5 ? -1.0 : 1.0;
            double from = actual + sign * k * kTickMs;
            double to = actual + sign * (k + wdt) * kTickMs;
            iv.push_back({std::min(from, to) - 1e-6, std::max(from, to) + 1e-6});
        }
        double history = n % 5 == 0 ? actual - u01(rng) * 4.0 * kTickMs : 0.0;   // sometimes a blocked / limited early side
        SyntheticOracle oracle(iv, 0, history);
        // reference: LocalWindowSolver (coarse step 1 tick, max 10 ticks, islands 2, bisect to 0.05 ms)
        LocalWindowConfig lc;
        lc.search.resolutionMs = 0.05;
        lc.search.maxTrials = 60;
        lc.search.islandScanSteps = 2;
        lc.search.maxShiftMs = 10.0 * kTickMs;
        LocalWindowSolver ref(oracle, lc);
        auto s = test::singleInput(actual);
        auto rw = ref.solve(0, s, 0);
        // planner with sub-tick refinement to 1/64 tick (0.065 ms)
        PlannerConfig pc;
        pc.subtick = true;
        pc.resolutionFrames = 1.0 / 64.0;
        pc.maxRefinePasses = 2;
        // BoundarySearch scans islandScanSteps beyond the first fail and stops there. Two planner
        // variants: the v0.6.x pruning (pruneNearShifts, kept for exactly this comparison) must
        // agree on everything; the v0.7.1 default (Fable D5: pruneBeyondHitFail, a side is pruned
        // only when its nearest shift passed) must agree on every edge, and on the islands inside
        // BoundarySearch's scan range (an unpruned side may see islands further out)
        double earlyLimit = std::isnan(history) || history <= 0.0 ? kNaN : (actual - history) / kTickMs;
        for (int variant = 0; variant < 2; ++variant) {
            PlannerConfig vc = pc;
            vc.pruneNearShifts = variant == 0;
            CHECK(vc.pruneBeyondHitFail);   // the default
            PassPlanner planner(vc, earlyLimit);
            runAgainstOracle(oracle, planner, 0, s, 0, 0.5);
            auto pw = planner.result(actual);
            ++cases;
            auto islandsInScan = [&](BoundaryResult const& b) {
                if (variant == 0 || !b.bounded) return b.islands.size();
                // BoundarySearch scans from the COARSE first fail (whole ticks here); a refined
                // fail edge lies up to one tick inside it
                double reach = (std::ceil(std::fabs(b.failShiftMs) / kTickMs - 1e-9) + vc.islandScanSteps) * kTickMs + 1e-6;
                size_t n2 = 0;
                for (auto const& is : b.islands) {
                    if (std::min(std::fabs(is.fromShiftMs), std::fabs(is.toShiftMs)) <= reach) ++n2;
                }
                return n2;
            };
            bool ok = rw.valid && pw.valid;
            ok = ok && rw.boundedEarly == pw.boundedEarly && rw.boundedLate == pw.boundedLate;
            ok = ok && rw.blockedEarly == pw.blockedEarly && rw.blockedLate == pw.blockedLate;
            ok = ok && rw.early.islands.size() == islandsInScan(pw.early) && rw.late.islands.size() == islandsInScan(pw.late);
            double tol = std::max(pw.resolutionMs, rw.resolutionMs) + 1e-9;
            if (pw.boundedEarly) ok = ok && std::fabs(rw.earliestMs - pw.earliestMs) <= tol;
            else ok = ok && std::fabs(rw.earliestMs - pw.earliestMs) <= 1e-6;
            if (pw.boundedLate) ok = ok && std::fabs(rw.latestMs - pw.latestMs) <= tol;
            else ok = ok && std::fabs(rw.latestMs - pw.latestMs) <= 1e-6;
            if (ok) ++agree;
            else {
                std::printf("  case %d variant %d: dE %.9f dL %.9f tol %.9f\n", n, variant, rw.earliestMs - pw.earliestMs, rw.latestMs - pw.latestMs, tol);
                std::printf("  case %d: ref [%.4f, %.4f] b(%d,%d) bl(%d,%d) isl(%zu,%zu) | planner [%.4f, %.4f] b(%d,%d) bl(%d,%d) isl(%zu,%zu) res %.4f valid %d/%d\n", n,
                            rw.earliestMs, rw.latestMs, rw.boundedEarly, rw.boundedLate, rw.blockedEarly, rw.blockedLate, rw.early.islands.size(), rw.late.islands.size(),
                            pw.earliestMs, pw.latestMs, pw.boundedEarly, pw.boundedLate, pw.blockedEarly, pw.blockedLate, pw.early.islands.size(), pw.late.islands.size(),
                            pw.resolutionMs, rw.valid, pw.valid);
            }
        }
    }
    CHECK_MSG(agree == cases, "planner disagrees with LocalWindowSolver in " + std::to_string(cases - agree) + " of " + std::to_string(cases) + " cases");
}

void testDeterminism() {
    SECTION("same inputs twice -> identical passes and results");
    auto run = [](std::vector<double>& passes, WindowResult& w) {
        PlannerConfig cfg;
        cfg.subtick = true;
        PassPlanner p(cfg, 6.3);
        std::vector<std::pair<double, double>> truth = {{-2.2, 1.9}, {4.0, 5.0}};
        for (int guard = 0; guard < 8 && !p.done(); ++guard) {
            auto s = p.nextPass();
            if (s.empty()) break;
            passes.insert(passes.end(), s.begin(), s.end());
            p.ingest(fromIntervals(s, truth), true, false);
        }
        w = p.result(250.0);
    };
    std::vector<double> p1, p2;
    WindowResult w1, w2;
    run(p1, w1);
    run(p2, w2);
    CHECK(p1 == p2);
    CHECK(w1.valid && w2.valid);
    CHECK(w1.earliestMs == w2.earliestMs && w1.latestMs == w2.latestMs && w1.resolutionMs == w2.resolutionMs);
    CHECK(w1.trials == w2.trials);
    CHECK(w1.late.islands.size() == 1);
}

// ---- v2 (docs/TIMING_SOLVER_V2.md §3.5, §5: T-FIX-1..3, T-ATT-1) ----

ShiftOutcome died(double shift, double deathAfter, int laterFixed, int obj = -1) {
    ShiftOutcome o = outcome(shift, ShiftKind::Died, deathAfter, obj);
    o.laterFixed = laterFixed;
    return o;
}

void testMissWithoutPruning() {
    SECTION("T-FIX-1: a miss window with alive near shifts keeps its true bracket (no cancellation)");
    // #595-like (RC3): control died with the real player, +1 died, +2 +3 alive, +4..+7 alive, +8 died.
    // v0.6.x cancelled +4..+7 (beyond the +1 fail + 2) and reported a 16.67 ms window over an
    // untested gap; with no pruning every clone runs and the run [+2, +7] is bracketed exactly.
    PlannerConfig cfg;
    CHECK(!cfg.pruneNearShifts);
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        if (s >= 2.0 && s <= 7.0) outs.push_back(outcome(s, ShiftKind::Survived));
        else outs.push_back(died(s, 97.0 - s, 0, -1));
    }
    p.ingest(outs, false, true);
    auto w = p.result(0.0);
    CHECK(w.valid && p.miss());
    CHECK_NEAR(w.earliestMs, 2.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestMs, 7.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.earliestFailMs, 1.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.latestFailMs, 8.0 * kTickMs, 1e-9);
    CHECK(!w.budgetExhausted);
    CHECK(w.early.edge.stop == EdgeStop::Fail && w.late.edge.stop == EdgeStop::Fail);
    // the same outcomes after the v0.6.x pruning: +4..+7 NotTested -> the bracket is untested (cut)
    PassPlanner q(cfg, kNaN);
    auto s2 = q.nextPass();
    std::vector<ShiftOutcome> pruned;
    for (double s : s2) {
        if (s >= 4.0 && s <= 7.0) pruned.push_back(outcome(s, ShiftKind::NotTested));
        else if (s >= 2.0 && s <= 3.0) pruned.push_back(outcome(s, ShiftKind::Survived));
        else pruned.push_back(died(s, 97.0 - s, 0, -1));
    }
    q.ingest(pruned, false, true);
    auto wq = q.result(0.0);
    CHECK(wq.valid);
    CHECK(wq.late.budgetExhausted);   // an untested gap inside the late bracket [+3, +8]
    CHECK_NEAR(wq.late.bracketMs, 5.0 * kTickMs, 1e-9);
    // runAgainstOracle mirrors the pruning only when asked to
    SyntheticOracle oracle({{-100.0, -1.0 * kTickMs}}, 0);   // actual 0 dies, every earlier shift passes
    auto sched = test::singleInput(0.0);
    PassPlanner a(cfg, kNaN);
    runAgainstOracle(oracle, a, 0, sched, 0, 0.5);
    int notTested = 0;
    for (auto const& o : a.outcomes()) if (o.kind == ShiftKind::NotTested) ++notTested;
    CHECK(notTested == 0);
    PlannerConfig legacy;
    legacy.pruneNearShifts = true;
    PassPlanner b(legacy, kNaN);
    SyntheticOracle oracle2({{-5.0 * kTickMs, 5.0 * kTickMs}}, 0);
    runAgainstOracle(oracle2, b, 0, sched, 0, 0.5);
    notTested = 0;
    for (auto const& o : b.outcomes()) if (o.kind == ShiftKind::NotTested) ++notTested;
    CHECK(notTested == 4);   // +9 +10 and -9 -10: beyond the first fail (+-6) + islandScanSteps (2)
}

void testNothingTested() {
    SECTION("T-FIX-2: no tested shift within the limits -> invalid no_shift_tested, never a 0 ms window");
    // #610 press t=0.0000: early side blocked by the attempt start, every late shift `l` (limit)
    PlannerConfig cfg;
    PassPlanner p(cfg, 0.0);
    p.setEarlyLimitKind(LimitKind::AttemptStart);
    auto shifts = p.nextPass();
    p.setLateLimit(0.0);
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        auto o = outcome(s, ShiftKind::NotTested);
        o.reason = "limit";
        outs.push_back(o);
    }
    p.ingest(outs, true, false);
    CHECK(!p.anyShiftTested());
    auto w = p.result(0.0);
    CHECK(!w.valid);
    CHECK(w.invalidReason == kNoShiftTested);
    // one tested shift is enough for a (one-sided) window
    PassPlanner q(cfg, 0.0);
    auto s2 = q.nextPass();
    std::vector<ShiftOutcome> o2;
    for (double s : s2) o2.push_back(s == 1.0 ? outcome(s, ShiftKind::Survived) : outcome(s, ShiftKind::NotTested));
    q.ingest(o2, true, false);
    CHECK(q.result(0.0).valid);
}

void testAttribution() {
    SECTION("T-ATT-1: edge stop / cause from laterFixed, limit kinds, describe format");
    PlannerConfig cfg;
    PassPlanner p(cfg, 6.0);   // previous input 6 frames back
    p.setEarlyLimitKind(LimitKind::Neighbour);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        if (s >= -2.0 && s <= 3.0) outs.push_back(outcome(s, ShiftKind::Survived));
        else if (s < 0) outs.push_back(died(s, 18.0, 0, 1717));   // self: before any later input
        else outs.push_back(died(s, 66.0, 3, -1));               // downstream: 3 later inputs applied
    }
    p.ingest(outs, true, false);
    auto w = p.result(1000.0);
    CHECK(w.valid);
    CHECK(w.early.edge.attributed && w.late.edge.attributed);
    CHECK(w.early.edge.stop == EdgeStop::Fail && w.early.edge.cause == EdgeCause::Self);
    CHECK(w.early.edge.laterInputs == 0 && w.early.edge.failObjectId == 1717);
    CHECK_NEAR(w.early.edge.failAfterMs, 18.0 * kTickMs, 1e-9);
    CHECK(w.late.edge.stop == EdgeStop::Fail && w.late.edge.cause == EdgeCause::Downstream);
    CHECK(w.late.edge.laterInputs == 3);
    // death times are measured from the UNSHIFTED input (the log's @N)
    bool found = false;
    for (auto const& t : w.late.trials) {
        if (std::fabs(t.shiftMs - 4.0 * kTickMs) < 1e-9) {
            found = true;
            CHECK_NEAR(t.outcome.tMs, 1000.0 + 66.0 * kTickMs, 1e-9);
        }
    }
    CHECK(found);
    std::string d = p.describe();
    CHECK_MSG(d.find("-3D@18.0/0Ls#1717") != std::string::npos, d);
    CHECK_MSG(d.find("+4D@66.0/3Ld") != std::string::npos, d);
    // an extension death is `extension`, whatever its later inputs
    PassPlanner e(cfg, kNaN);
    auto se = e.nextPass();
    std::vector<ShiftOutcome> oe;
    for (double s : se) {
        auto o = s >= -1.0 && s <= 1.0 ? outcome(s, ShiftKind::Survived) : died(s, 40.0, 1);
        if (s > 1.0) o.extension = true;
        oe.push_back(o);
    }
    e.ingest(oe, true, false);
    auto we = e.result(0.0);
    CHECK(we.late.edge.cause == EdgeCause::Extension);
    CHECK(we.early.edge.cause == EdgeCause::Downstream);
    // open sides: range, neighbour (late limit), the early limit kind, history, untested
    PassPlanner o(cfg, 4.0);
    o.setEarlyLimitKind(LimitKind::AttemptStart);
    auto so = o.nextPass();
    o.setLateLimit(5.5);
    std::vector<ShiftOutcome> oo;
    for (double s : so) oo.push_back(s > 5.5 ? outcome(s, ShiftKind::NotTested) : outcome(s, ShiftKind::Survived));
    o.ingest(oo, true, false);
    auto wo = o.result(0.0);
    CHECK(wo.early.edge.stop == EdgeStop::AttemptStart);
    CHECK(wo.late.edge.stop == EdgeStop::Neighbour);
    CHECK(wo.late.edge.cause == EdgeCause::None);
    PassPlanner r(cfg, kNaN);
    auto sr = r.nextPass();
    std::vector<ShiftOutcome> orr;
    for (double s : sr) orr.push_back(s == -10.0 ? outcome(s, ShiftKind::NotTested) : outcome(s, ShiftKind::Survived));
    r.ingest(orr, true, false);
    auto wr = r.result(0.0);
    CHECK(wr.late.edge.stop == EdgeStop::Range);
    CHECK(wr.early.edge.stop == EdgeStop::Untested);
    PassPlanner h(cfg, kNaN);
    auto sh = h.nextPass();
    std::vector<ShiftOutcome> oh;
    for (double s : sh) oh.push_back(s < -5.0 ? outcome(s, ShiftKind::NotTested) : outcome(s, ShiftKind::Survived));
    h.ingest(oh, true, false);
    h.markEarlyBlocked();
    CHECK(h.result(0.0).early.edge.stop == EdgeStop::History);
    // the offline rule: an input logged at F was applied before a death at D iff F < D
    InputSchedule sched;
    for (double t : {3435.0, 3474.0, 3500.0, 3518.0, 3535.0}) sched.inputs.push_back({t, 1, Button::Jump, true});
    CHECK(laterFixedBefore(sched, 0, {}, 3518.0) == 2);   // #668 -2 @83 -> 3474, 3500 (not 3518 itself)
    CHECK(laterFixedBefore(sched, 0, {}, 3519.0) == 3);
    CHECK(laterFixedBefore(sched, 0, {1}, 3519.0) == 2);  // a moved input is never "fixed"
    CHECK(laterFixedBefore(sched, 2, {}, 3518.0) == 0);   // #670 -2 @18 -> 3518 = one step BEFORE #671 (self)
}

// ---- v0.7.1: Fable review deltas D4, D5 (docs/TIMING_SOLVER_V2_FABLE.md §4) ----

void testMissRunUntestedNeighbour() {
    SECTION("Fable D4 missRunUntestedNeighbour: 0D +1D +2A +3A +4-(pool) +5- +6D -> late bracket [+3,+6], budgetCut, res 3 ticks, untested_gap_in_bracket");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        if (s == 2.0 || s == 3.0) outs.push_back(outcome(s, ShiftKind::Survived));
        else if (s == 4.0 || s == 5.0) {
            auto o = outcome(s, ShiftKind::NotTested);
            o.reason = "pool";   // the deferred-spawn deadline: still possible with pruning off
            outs.push_back(o);
        }
        else outs.push_back(died(s, 40.0 - s, 0, -1));
    }
    ShiftOutcome control = died(0.0, 40.0, 0, -1);
    p.setControlDeath(control);
    p.ingest(outs, false, true);   // the control died with the real player: a miss
    auto w = p.result(1000.0);
    CHECK(w.valid && p.miss());
    // the late side of the run [+2, +3] is bracketed at the nearest TESTED death beyond it (+6),
    // never at the untested +4 / +5, and the gap is flagged
    CHECK_NEAR(w.late.passShiftMs, 3.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.late.failShiftMs, 6.0 * kTickMs, 1e-9);
    CHECK(w.late.budgetExhausted);
    CHECK_NEAR(w.late.bracketMs, 3.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.resolutionMs, 3.0 * kTickMs, 1e-9);
    // the early side: the run starts at +2, bracketed by +1 (tested, died) without a gap
    CHECK_NEAR(w.early.passShiftMs, 2.0 * kTickMs, 1e-9);
    CHECK_NEAR(w.early.failShiftMs, 1.0 * kTickMs, 1e-9);
    CHECK(!w.early.budgetExhausted);
    // the payload keeps the true bracket (no clamp) and the status says so
    auto lp = localWindowPayload(w, 1000.0, false);
    CHECK_NEAR(lp.resolutionMs, 3.0 * kTickMs, 1e-9);
    LocalEvidence ev;
    ev.window = &w;
    ev.outcomes = &p.outcomes();
    ev.miss = true;
    ev.frame = 1000.0 / kTickMs;
    ev.horizonFrame = ev.frame + 130.0;
    auto lf = localFacts(ev);
    CHECK(lf.untestedGap);
    auto st = status::statusOf({}, lf, saFacts(nullptr), {});
    CHECK(st.status == status::TimingStatus::LowConfidence);
    CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::UntestedGapInBracket) != st.reasons.end());
    CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::ResolutionAboveMax) != st.reasons.end());   // 12.5 ms > 4.2 ms
}

void testMissWindowInvalidNamesTrial() {
    SECTION("v0.7.1 missWindowInvalidNamesTrial: an invalid trial in a miss window names its own reason (owner's 19.47.37 log, job 602: -10..+6 died, +7..+10 teleport-invalid)");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        if (s >= 7.0) {
            auto o = outcome(s, ShiftKind::Invalid);
            o.reason = "teleport portal";
            outs.push_back(o);
        }
        else outs.push_back(died(s, 60.0 - s, 0, 216));
    }
    p.setControlDeath(died(0.0, 60.0, 0, 216));
    p.ingest(outs, false, true);
    auto w = p.result(1000.0);
    CHECK(!w.valid);
    // the engine maps a drop by this text (teleport -> control_invalid_teleport, dual ->
    // control_invalid_dual_portal, history lost); before 0.7.1 it read only "invalid trial in the
    // miss window" and the result went out as unresolved (payload_invalid)
    CHECK(w.invalidReason == "late: invalid trial in the miss window (teleport portal)");
    // a trial without a reason still says so
    PassPlanner q(cfg, kNaN);
    auto sq = q.nextPass();
    std::vector<ShiftOutcome> oq;
    for (double s : sq) oq.push_back(s <= -9.0 ? outcome(s, ShiftKind::Invalid) : died(s, 60.0 - s, 0, 216));
    q.setControlDeath(died(0.0, 60.0, 0, 216));
    q.ingest(oq, false, true);
    auto wq = q.result(1000.0);
    CHECK(!wq.valid);
    CHECK(wq.invalidReason == "early: invalid trial in the miss window (invalid)");
}

void testPruneOnlyAfterPassRun() {
    SECTION("Fable D5 pruneOnlyAfterPassRun: a side is pruned beyond its first fail + the island scan only when its nearest shift passed");
    auto probes = [](std::vector<std::pair<double, ProbeState>> v) {
        std::vector<PruneProbe> out;
        for (auto const& [m, s] : v) out.push_back({m, s});
        return out;
    };
    // hit side `+1A +2D`: pruned beyond 2 + 2 = 4 (+5..+10 cancelled, as v0.6.x did)
    CHECK_NEAR(hitPruneLimit(probes({{1, ProbeState::Passed}, {2, ProbeState::Died}, {3, ProbeState::Running}, {7, ProbeState::Running}}), 2.0), 4.0, 1e-12);
    // miss side `+1D +2A +3A ...`: the nearest shift died -> never pruned
    CHECK(std::isnan(hitPruneLimit(probes({{1, ProbeState::Died}, {2, ProbeState::Passed}, {3, ProbeState::Passed}, {4, ProbeState::Died}}), 2.0)));
    // the nearest shift still running (its pass is not known yet): not now
    CHECK(std::isnan(hitPruneLimit(probes({{1, ProbeState::Running}, {2, ProbeState::Died}}), 2.0)));
    // nothing died, invalid nearest, no probes: never
    CHECK(std::isnan(hitPruneLimit(probes({{1, ProbeState::Passed}, {2, ProbeState::Passed}}), 2.0)));
    CHECK(std::isnan(hitPruneLimit(probes({{1, ProbeState::Other}, {3, ProbeState::Died}}), 2.0)));
    CHECK(std::isnan(hitPruneLimit({}, 2.0)));
    // an untested nearest shift is no probe: the nearest TESTED one decides (+2 passed, +4 died -> 6)
    CHECK_NEAR(hitPruneLimit(probes({{2, ProbeState::Passed}, {4, ProbeState::Died}}), 2.0), 6.0, 1e-12);

    // the pull-oracle mirror (runAgainstOracle, default config): a hit window [-3, +1]
    PlannerConfig cfg;
    CHECK(cfg.pruneBeyondHitFail && !cfg.pruneNearShifts);
    SyntheticOracle hit({{-3.0 * kTickMs, 1.0 * kTickMs}}, 0);
    auto sched = test::singleInput(0.0);
    PassPlanner a(cfg, kNaN);
    runAgainstOracle(hit, a, 0, sched, 0, 0.5);
    std::vector<double> cancelled;
    for (auto const& o : a.outcomes()) if (o.kind == ShiftKind::NotTested) cancelled.push_back(o.nominalFrames);
    std::sort(cancelled.begin(), cancelled.end());
    // late: +1 passed, +2 died -> +5..+10 cancelled; early: -1..-3 passed, -4 died -> -7..-10 cancelled
    std::vector<double> want = {-10, -9, -8, -7, 5, 6, 7, 8, 9, 10};
    CHECK_MSG(cancelled == want, "cancelled " + std::to_string(cancelled.size()));
    auto wa = a.result(0.0);
    CHECK(wa.valid && wa.boundedEarly && wa.boundedLate && !wa.budgetExhausted);
    CHECK_NEAR(wa.late.passShiftMs, 1.0 * kTickMs, 1e-9);
    CHECK_NEAR(wa.early.passShiftMs, -3.0 * kTickMs, 1e-9);

    // the #595 miss pattern (the control died, +1 died, the run lies at +2..+7): nothing is
    // cancelled on either side, the run keeps its true bracket [+7, +8]
    SyntheticOracle miss({{2.0 * kTickMs - 1e-6, 7.0 * kTickMs + 1e-6}}, 0);
    PassPlanner m(cfg, kNaN);
    runAgainstOracle(miss, m, 0, sched, 0, 0.5);
    int notTested = 0;
    for (auto const& o : m.outcomes()) if (o.kind == ShiftKind::NotTested) ++notTested;
    CHECK(notTested == 0);
    auto wm = m.result(0.0);
    CHECK(wm.valid && m.miss());
    CHECK_NEAR(wm.earliestMs, 2.0 * kTickMs, 1e-9);
    CHECK_NEAR(wm.latestMs, 7.0 * kTickMs, 1e-9);
    CHECK_NEAR(wm.latestFailMs, 8.0 * kTickMs, 1e-9);
    CHECK(!wm.budgetExhausted);
    // the v0.6.x rule on the same miss cancels the run's far part (the RC3.1 defect, comparison only)
    PlannerConfig legacy;
    legacy.pruneNearShifts = true;
    PassPlanner l(legacy, kNaN);
    runAgainstOracle(miss, l, 0, sched, 0, 0.5);
    int legacyCut = 0;
    for (auto const& o : l.outcomes()) if (o.kind == ShiftKind::NotTested && o.nominalFrames > 0) ++legacyCut;
    CHECK(legacyCut == 7);   // +4..+10 beyond the +1 fail + 2
}

}  // namespace

int main() {
    testCoarsePassShape();
    testHitWindowCoarse();
    testUnboundedAndIslands();
    testMissWindow();
    testLateLimitAfterPass0();
    testNotTestedGaps();
    testRefinementPasses();
    testMissRefinement();
    testEquivalenceWithLocalWindowSolver();
    testDeterminism();
    testMissWithoutPruning();
    testNothingTested();
    testAttribution();
    testMissRunUntestedNeighbour();
    testPruneOnlyAfterPassRun();
    testMissWindowInvalidNamesTrial();
    return gprl::test::finish("pass_planner_tests");
}
