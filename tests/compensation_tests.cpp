// Lockstep compensation planner host tests (docs/SHIP_SOLVER.md §4, §7) on the kinematic DEV
// FIXTURE physics (tests/kinematic_oracle.hpp, NOT Geometry Dash):
//
//   solveOffsets / devAt       the response model's linear algebra
//   pressNeedsReleaseComp      THE critical case (PROMPT §14.11): the press can shift only if the
//                              release compensates -> local press window tiny, compensated window
//                              wide, proof `compensated`, adaptation `comp1`; equal to a brute-force
//                              compensated scan; W_local ⊆ W_SA
//   isolatedShip               no follower -> sequence = local, decided, isolated
//   lateDiedSelf               a late shift that died with no later input acting -> fail with zero trials
//   determinism                two runs, identical trial sequence and result text
//   budget / finish            undecided (sa_not_measured_budget / sa_cut_by_restart), never guessed
//   ordering                   follower offsets keep the input order (the legality clamp)
//   payload                    the comp result validates, passes the gate mirror, carries comp1 /
//                              compensated / the compensation block
//
// Nothing here is a real level; no number is a target.
#include "test_util.hpp"
#include "kinematic_oracle.hpp"

#include "../core/solver/compensation.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_status.hpp"
#include "../core/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::solver::comp;
using namespace gprl::test::kin;

namespace {

constexpr double T = kTickMs;
constexpr double kH = 0.5 + 10.0 / 240.0;
constexpr double kMargin = 0.0024;

InputSchedule schedule(std::vector<std::pair<double, bool>> const& ticks) {
    InputSchedule s;
    for (auto const& [tick, down] : ticks) s.inputs.push_back({tick * T, 1, Button::Jump, down});
    return s;
}
double frameOf(InputSchedule const& s, size_t i) { return s.inputs[i].tMs / T; }
double earlyLimitOf(InputSchedule const& s, size_t i) {
    double t = frameOf(s, i);
    if (i == 0) return std::min(10.0, t);
    return std::min(10.0, t - frameOf(s, i - 1) - kMargin);
}
double lateLimitOf(InputSchedule const& s, size_t i) {
    if (i + 1 >= s.inputs.size()) return kNaN;
    return frameOf(s, i + 1) - frameOf(s, i) - kMargin;
}

PassPlanner solveLocal(KinematicOracle& o, InputSchedule const& s, size_t i) {
    PlannerConfig pc;
    PassPlanner p(pc, earlyLimitOf(s, i));
    p.setEarlyLimitKind(i == 0 ? LimitKind::AttemptStart : LimitKind::Neighbour);
    double late = lateLimitOf(s, i);
    if (!std::isnan(late)) p.setLateLimit(late);
    o.setReference(s);
    runAgainstOracle(o, p, 0, s, i, kH);
    return p;
}

/// One compensated trial on the oracle: the look-ahead from the trial's own fields, the deviation
/// samples from the member's shifted frame, laterFixed by the offline rule.
void oracleTrial(KinematicOracle& o, InputSchedule const& ref, InputSchedule const& sched, CompTrial const& t, CompOutcome& out) {
    o.setReference(ref);
    double first = t.moved.front().frame;
    for (auto const& mv : t.moved) first = std::min(first, mv.frame);
    double earliest = std::min(first, t.attributeAfterFrame);
    double horizonSeconds = (t.lookAheadFrame - earliest) / 240.0;
    std::vector<KinematicOracle::Dev> dev;
    Outcome r = o.trialTrace(sched, horizonSeconds, t.devFromFrame * T, dev);
    for (auto const& d : dev) out.dev.push_back({d.frame, d.dy, d.dvy});
    switch (r.kind) {
        case OutcomeKind::Survived: out.kind = CompOutcome::Kind::Pass; break;
        case OutcomeKind::Resynced: out.kind = CompOutcome::Kind::Pass; out.rejoined = true; break;
        case OutcomeKind::Died: {
            out.kind = CompOutcome::Kind::Died;
            out.deathFrame = r.tMs / T;
            out.objectId = r.objectId;
            std::vector<size_t> moved;
            for (size_t k = 1; k < t.moved.size(); ++k) moved.push_back(static_cast<size_t>(t.moved[k].id) - 1);
            out.laterFixed = laterFixedBefore(ref, static_cast<size_t>(t.moved.front().id) - 1, moved, r.tMs);
            break;
        }
        case OutcomeKind::Invalid: out.kind = CompOutcome::Kind::Invalid; out.reason = r.reason; break;
    }
}

struct CompSolved {
    PassPlanner local;
    CompPlanner comp;
    int trials = 0;
};

CompSolved solveComp(KinematicOracle& o, InputSchedule const& s, size_t i, CompConfig cfg = kComp, std::vector<size_t> breaks = {}) {
    CompSolved r;
    r.local = solveLocal(o, s, i);
    r.trials = runCompAgainstOracle(s, i, r.local, cfg, breaks, [&](InputSchedule const& sched, CompTrial const& t, CompOutcome& out) { oracleTrial(o, s, sched, t, out); }, r.comp);
    return r;
}

// ---- brute force (the ground truth of a compensated shift) ----

/// A shift passes by brute force when the member alone passes (local) or some follower offset
/// vector within the range (k = 1..maxFollowers, order preserved) passes with the planner's look-ahead.
bool bruteCompPass(KinematicOracle& o, InputSchedule const& s, size_t i, double shift, CompConfig const& cfg) {
    auto run = [&](std::vector<double> const& offsets) {
        auto m = s;
        m.inputs[i].tMs += shift * T;
        double last = frameOf(s, i) + shift;
        double prev = last;
        for (size_t j = 0; j < offsets.size(); ++j) {
            size_t idx = i + 1 + j;
            if (idx >= s.inputs.size()) return false;
            double f = frameOf(s, idx) + offsets[j];
            if (f <= prev + cfg.neighbourMarginFrames) return false;
            if (idx + 1 < s.inputs.size() && j + 1 == offsets.size() && f >= frameOf(s, idx + 1) - cfg.neighbourMarginFrames) return false;
            m.inputs[idx].tMs = f * T;
            prev = f;
            last = std::max(last, f);
        }
        double lookAhead = std::max(last + cfg.lookAheadFrames, last + cfg.convergeSteps + 1.0);
        double earliest = std::min(frameOf(s, i) + shift, frameOf(s, i));
        o.setReference(s);
        return o.trial(0, m, (lookAhead - earliest) / 240.0).passed();
    };
    // local: the engine's look-ahead rule of the local job
    {
        auto m = s;
        m.inputs[i].tMs += shift * T;
        o.setReference(s);
        bool crosses = i + 1 < s.inputs.size() && shift > 0 && frameOf(s, i) + shift >= frameOf(s, i + 1) - kMargin - 1e-9;
        if (!crosses && o.trial(0, m, kH + (shift < 0 ? -shift / 240.0 : 0.0)).passed()) return true;
    }
    int const R = cfg.offsetRangeTicks;
    for (int k = 1; k <= cfg.maxFollowers; ++k) {
        if (i + static_cast<size_t>(k) >= s.inputs.size()) break;
        if (frameOf(s, i + static_cast<size_t>(k)) > frameOf(s, i) + cfg.horizonFrames) break;
        std::vector<double> off(static_cast<size_t>(k), shift - R);
        // every lattice point of the k-cube (7^k <= 343)
        for (int guard = 0; guard < 400; ++guard) {
            if (run(off)) return true;
            size_t j = 0;
            while (j < off.size()) {
                off[j] += 1.0;
                if (off[j] <= shift + R + 1e-9) break;
                off[j] = shift - R;
                ++j;
            }
            if (j == off.size()) break;
        }
    }
    return false;
}

struct Walk {
    double lastPass = 0.0;
    double firstFail = kNaN;
};
template <class Pred>
Walk bruteWalk(Pred&& pass, double limit) {
    Walk w;
    for (int k = 1; k <= 10 && k <= limit + 1e-9; ++k) {
        if (pass(static_cast<double>(k))) w.lastPass = k;
        else { w.firstFail = k; break; }
    }
    return w;
}

double localEdgeTicks(BoundaryResult const& b) { return localEdgeFrames(b); }
double localWidthTicks(WindowResult const& w) { return (w.latestMs - w.earliestMs) / T; }
double widthTicks(SAWindow const& w) { return w.late.edgeFrames() - w.early.edgeFrames(); }

// ---- worlds ----

/// DEV FIXTURE ship: rests on a solid floor; `bands` = lethal rects (ceiling / floor bands).
World shipBase() {
    World w;
    w.startMode = Mode::Ship;
    w.startY = 0.0;
    w.half = 3.0;
    w.shipGravity = 0.03;
    w.floor.pts = {{-100.0, -3.0}, {5000.0, -3.0}};
    return w;
}

/// The reference path's y at a tick (free flight in `w`).
std::vector<State> pathOf(World w, InputSchedule const& s, int64_t ticks) {
    w.spikes.clear();
    KinematicOracle o(w);
    return o.run(s, ticks);
}

/// PROMPT §14.11: a press whose timing only works when the release compensates. DEV FIXTURE
/// found by a parameter scan (scratch explorer, 2026-10-02): gravity 0.06, thrust 0.08, the ship
/// rests on a floor that ENDS right after take-off (no landing: nothing re-joins by the floor
/// clamp), presses at 100, releases at 108, and flies a per-tick corridor of `clear` units around
/// its recorded path over ticks [112, 124]. Shifting the press alone changes the hold (dies at
/// once); shifting press + release together moves the whole arc in time (dies after ~2 ticks);
/// a DIFFERENT release offset restores the height (passes up to ~6 ticks early).
World criticalWorld(InputSchedule const& s, double clear) {
    World w = shipBase();
    w.shipGravity = 0.06;
    w.shipThrust = 0.08;
    w.floor.pts = {{-100.0, -3.0}, {1.3 * 109.0, -3.0}, {1.3 * 109.5, -600.0}, {5000.0, -600.0}};
    auto path = pathOf(w, s, 420);
    for (int k = 112; k <= 124; ++k) {
        auto const& a = path[static_cast<size_t>(k - 1)];
        auto const& b = path[static_cast<size_t>(k)];
        double ymin = std::min(a.y, b.y), ymax = std::max(a.y, b.y);
        w.spikes.push_back({a.x - 0.65, ymin - w.half - clear - 400.0, b.x + 0.65, ymin - w.half - clear, 21});
        w.spikes.push_back({a.x - 0.65, ymax + w.half + clear, b.x + 0.65, ymax + w.half + clear + 400.0, 22});
    }
    return w;
}

bool hasAdaptation(SAResult const& r, SAAdaptation a) { return std::find(r.adaptationUsed.begin(), r.adaptationUsed.end(), a) != r.adaptationUsed.end(); }

// =============================================================================================

void testLinearAlgebra() {
    SECTION("solveOffsets: one follower, exact; two followers, minimum norm; an unknown column stays at 0; devAt picks the latest sample at or before the frame");
    {
        DevSample base{200.0, 2.0, 0.0};
        std::vector<DevSample> cols = {{200.0, 1.0, 0.0}};   // moving follower 1 by +1 tick lowers y by 1
        auto d = CompPlanner::solveOffsets(base, cols, 1.0, 1e-9);
        CHECK(d.size() == 1);
        CHECK_NEAR(d[0], 2.0, 1e-6);
    }
    {
        DevSample base{200.0, 2.0, 0.0};
        std::vector<DevSample> cols = {{200.0, 1.0, 0.0}, {200.0, 1.0, 0.0}};   // identical responses: split evenly
        auto d = CompPlanner::solveOffsets(base, cols, 1.0, 1e-9);
        CHECK(d.size() == 2);
        CHECK_NEAR(d[0], 1.0, 1e-4);
        CHECK_NEAR(d[1], 1.0, 1e-4);
    }
    {
        DevSample base{200.0, 2.0, 0.5};
        std::vector<DevSample> cols = {{200.0, kNaN, kNaN}, {200.0, 1.0, 0.5}};   // follower 1 unknown
        auto d = CompPlanner::solveOffsets(base, cols, 1.0, 1e-9);
        CHECK(d.size() == 2 && d[0] == 0.0);
        CHECK_NEAR(d[1], 2.0, 1e-6);
    }
    {
        std::vector<DevSample> dev = {{101.0, 0.1, 0.0}, {102.0, 0.2, 0.0}, {103.0, 0.3, 0.0}};
        DevSample out;
        CHECK(CompPlanner::devAt(dev, 102.5, out) && out.frame == 102.0);
        CHECK(CompPlanner::devAt(dev, 103.0, out) && out.frame == 103.0);
        CHECK(!CompPlanner::devAt(dev, 100.0, out));
    }
}

void testCriticalCase() {
    SECTION("pressNeedsReleaseComp (PROMPT §14.11): local press window tiny, compensated window wide, proof compensated / adaptation comp1, W_local ⊆ W_SA, equal to brute force");
    auto s = schedule({{100, true}, {108, false}});
    bool found = false;
    // DEV FIXTURE search over band clearances: the test pins the RELATION (local tiny, comp wide),
    // not a geometry; several clearances are tried and the first that makes the local window tight
    // is used
    for (double clear : {2.4, 1.6, 3.0}) {
        {
            World w = criticalWorld(s, clear);
            KinematicOracle o(w);
            o.setReference(s);
            if (!o.trial(0, s, 2.0).passed()) continue;   // the recorded run must survive
            auto sv = solveComp(o, s, 0);
            auto lw = sv.local.result(s.inputs[0].tMs);
            auto r = sv.comp.result();
            std::printf("  clearance %.1f: local [%+.2f,%+.2f] (%.2f f) | comp [%+.2f,%+.2f] (%.2f f) %s | %s\n", clear, localEdgeTicks(lw.early),
                        localEdgeTicks(lw.late), localWidthTicks(lw), r.sequence.early.edgeFrames(), r.sequence.late.edgeFrames(), widthTicks(r.sequence),
                        r.decided ? "decided" : "UNDECIDED", r.debug.empty() ? "" : r.debug[0].c_str());
            if (!lw.valid) continue;
            // W_local ⊆ W_SA on the pass edges, always
            CHECK(r.sequence.early.passFrames <= lw.early.passShiftMs / T + 1e-9 && r.sequence.late.passFrames >= lw.late.passShiftMs / T - 1e-9);
            // brute force on every decided side
            for (int side = 0; side < 2; ++side) {
                if (!r.sideDecided[side]) continue;
                double sign = side ? 1.0 : -1.0;
                auto bw = bruteWalk([&](double k) { return bruteCompPass(o, s, 0, sign * k, kComp); }, 10.0);
                SAEdge const& e = side ? r.sequence.late : r.sequence.early;
                // the planner may stop short of brute force (its search is bounded: a verified pass
                // is a LOWER bound of the window), never beyond it
                CHECK_MSG(std::fabs(e.passFrames) <= bw.lastPass + 1e-9, std::string("side ") + (side ? "late" : "early") + " pass beyond brute force");
                std::printf("    %s: planner pass %.0f fail %s | brute pass %.0f fail %s\n", side ? "late" : "early", std::fabs(e.passFrames),
                            e.stop == EdgeStop::Fail ? std::to_string(std::fabs(e.failFrames)).c_str() : "-", bw.lastPass,
                            std::isnan(bw.firstFail) ? "-" : std::to_string(bw.firstFail).c_str());
            }
            bool const localTight = lw.boundedEarly && lw.boundedLate && localWidthTicks(lw) <= 3.0 + 1e-9;
            bool const compWide = widthTicks(r.sequence) >= localWidthTicks(lw) + 4.0 - 1e-9;
            if (localTight && compWide && (hasAdaptation(r, SAAdaptation::Comp1) || r.sequence.early.proof == SAProof::Compensated || r.sequence.late.proof == SAProof::Compensated)) {
                found = true;
                CHECK(r.decided);
                CHECK(r.sequence.early.proof == SAProof::Compensated || r.sequence.late.proof == SAProof::Compensated);
                // the offsets of the compensated pass differ from the member's shift on at least one side
                bool differs = false;
                for (auto const* e : {&r.sequence.early, &r.sequence.late}) {
                    for (double off : e->followerOffsetsFrames) if (std::fabs(off - e->passFrames) > 1e-9) differs = true;
                }
                CHECK(differs);
                // the status rules: a decided sequence window -> ok (no sequence_dependent)
                LocalEvidence ev;
                ev.window = &lw;
                ev.outcomes = &sv.local.outcomes();
                ev.frame = 100.0;
                ev.nextFrame = 108.0;
                ev.nextFollows = true;
                ev.horizonFrame = 230.0;
                auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
                CHECK_MSG(st.status == status::TimingStatus::Ok || st.status == status::TimingStatus::LowConfidence, std::string("status ") + status::name(st.status));
                // the payload: comp1, compensated, the compensation block with the release offset
                TimingResultContext ctx;
                ctx.inputSeq = 7;
                ctx.kind = InputKind::Press;
                ctx.eventT = s.inputs[0].tMs / 1000.0;
                ctx.gamemode = Gamemode::Ship;
                ctx.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
                auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
                CHECK_MSG(built.ok, built.error);
                CHECK(built.payload.sequence.has_value());
                if (built.payload.sequence) {
                    auto const& sq = *built.payload.sequence;
                    CHECK(sq.solverVersion == std::string(kSASolverVersion) && sq.solverVersion == "gprl-clone-sa/5");
                    CHECK(std::find(sq.adaptationUsed.begin(), sq.adaptationUsed.end(), "comp1") != sq.adaptationUsed.end());
                    CHECK(sq.compensation.has_value());
                    if (sq.compensation) CHECK(!sq.compensation->earlyOffsetsMs.empty() || !sq.compensation->lateOffsetsMs.empty());
                    auto gate = checkTimingResultPayload(built.payload, ctx.eventT, 0.0, nullptr);
                    std::string why;
                    for (auto const& x : gate.reasons) why += x + " ";
                    CHECK_MSG(gate.accepted, "gate: " + why);
                    std::string err;
                    CHECK_MSG(telemetry::validateTimingResult(built.payload, "", &err), err);
                }
                break;
            }
        }
        if (found) break;
    }
    CHECK_MSG(found, "no band clearance produced the critical case (local tight, comp wide, compensated)");
}

void testIsolated() {
    SECTION("isolatedShip: no follower within the horizon -> sequence = local (decided, isolated), zero trials");
    auto s = schedule({{100, true}, {108, false}, {400, true}});
    World w = criticalWorld(s, 2.4);
    KinematicOracle o(w);
    // the release (member 1) has no follower within 120 frames: whatever its local window, the
    // compensated window is the local one
    auto sv = solveComp(o, s, 1);
    auto lw = sv.local.result(s.inputs[1].tMs);
    auto r = sv.comp.result();
    std::printf("  release local [%+.2f,%+.2f] comp [%+.2f,%+.2f] %s trials %d | %s\n", localEdgeTicks(lw.early), localEdgeTicks(lw.late), r.sequence.early.edgeFrames(),
                r.sequence.late.edgeFrames(), r.decided ? "decided" : "UNDECIDED", sv.trials, r.debug.empty() ? "" : r.debug[0].c_str());
    CHECK(r.isolated);
    CHECK(r.decided);
    CHECK(sv.trials == 0);
    CHECK(std::fabs(r.sequence.early.edgeFrames() - localEdgeTicks(lw.early)) < 1e-9 && std::fabs(r.sequence.late.edgeFrames() - localEdgeTicks(lw.late)) < 1e-9);
    CHECK(r.adaptationUsed.empty());
}

void testLateDiedSelf() {
    SECTION("lateDiedSelf: a late shift whose local copy died before any later input acted -> fail at once, no compensation trial for it");
    CompMember m;
    m.id = 1;
    m.frame = 100.0;
    m.down = true;
    CompPlanner p(kComp, m, {{2, 130.0, false, false}});
    p.setNow(400.0);
    std::vector<ShiftOutcome> all;
    for (int k = 1; k <= 10; ++k) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = static_cast<double>(k);
        if (k <= 2) o.kind = ShiftKind::Resynced;
        else {
            o.kind = ShiftKind::Died;
            o.deathAfterFrames = 20.0;   // frame 120: before the follower at 130
            o.laterFixed = 0;
        }
        all.push_back(o);
        ShiftOutcome e = o;
        e.nominalFrames = e.appliedFrames = -static_cast<double>(k);
        e.kind = ShiftKind::Resynced;
        all.push_back(e);
    }
    p.finalizeLocal(all, kNaN);
    CHECK(p.done());
    auto r = p.result();
    CHECK(r.decided);
    CHECK(r.sequence.late.stop == EdgeStop::Fail && r.sequence.late.passFrames == 2.0 && r.sequence.late.failFrames == 3.0);
    CHECK(r.sequence.late.cause == EdgeCause::Self && r.sequence.late.laterInputs == 0);
    CHECK(r.sequence.early.stop == EdgeStop::Range && r.sequence.early.passFrames == -10.0);
    CHECK(p.trials() == 0);
    std::printf("  %s\n", r.debug.empty() ? "" : r.debug[0].c_str());
}

void testEarlyWaitsForFollower() {
    SECTION("an early shift that died before the next input, with a follower that could still act: the slot waits for the follower, then compensates; without one it fails once the horizon passed");
    CompMember m;
    m.id = 1;
    m.frame = 100.0;
    m.down = true;
    // local: -3 died at frame 110 (deathAfterFrames 10), laterFixed 0; a follower at 112 shifted by -3 acts at 109 < 110
    auto local = [&](bool withFollower) {
        CompPlanner p(kComp, m, withFollower ? std::vector<CompFollower>{{2, 112.0, false, false}} : std::vector<CompFollower>{});
        std::vector<ShiftOutcome> all;
        for (int k = 1; k <= 10; ++k) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = -static_cast<double>(k);
            if (k <= 2) o.kind = ShiftKind::Resynced;
            else { o.kind = ShiftKind::Died; o.deathAfterFrames = 10.0; o.laterFixed = 0; }
            all.push_back(o);
            ShiftOutcome l = o;
            l.nominalFrames = l.appliedFrames = static_cast<double>(k);
            l.kind = ShiftKind::Resynced;
            all.push_back(l);
        }
        p.setNow(105.0);   // before D + |s| = 113: a follower may still come
        p.finalizeLocal(all, kNaN);
        return p;
    };
    {
        auto p = local(false);
        CHECK(!p.done());   // waiting for a possible follower
        CHECK(p.trials() == 0);
        p.setNow(300.0);   // the horizon passed: nothing came
        CHECK(p.done());
        auto r = p.result();
        CHECK(r.sequence.early.stop == EdgeStop::Fail && r.sequence.early.passFrames == -2.0 && r.sequence.early.failFrames == -3.0);
        CHECK(r.isolated);
    }
    {
        auto p = local(true);
        CHECK(!p.done());
        auto batch = p.nextBatch(10);
        CHECK(!batch.empty());   // the uniform trial of -3 with the follower moved
        if (!batch.empty()) {
            CHECK(batch.front().role == Role::Uniform && batch.front().followers == 1 && batch.front().shiftFrames == -3.0);
            CHECK(batch.front().moved.size() == 2 && std::fabs(batch.front().moved[1].frame - 109.0) < 1e-9);
        }
    }
}

void testDeterminismAndBudget() {
    SECTION("determinism: two identical runs give the same trial sequence and the same result text");
    auto s = schedule({{100, true}, {108, false}});
    World w = criticalWorld(s, 2.4);
    std::string a, b;
    int ta = 0, tb = 0;
    {
        KinematicOracle o(w);
        auto sv = solveComp(o, s, 0);
        a = sv.comp.describe();
        ta = sv.trials;
    }
    {
        KinematicOracle o(w);
        auto sv = solveComp(o, s, 0);
        b = sv.comp.describe();
        tb = sv.trials;
    }
    CHECK(a == b && ta == tb);
    std::printf("  %d trials: %s\n", ta, a.c_str());

    SECTION("T-BUDGET: a tiny trial cap ends the open sides undecided (sa_not_measured_budget), never guessed; trials <= the cap");
    {
        KinematicOracle o(w);
        CompConfig cfg = kComp;
        cfg.maxTrialsPerInput = 2;
        auto sv = solveComp(o, s, 0, cfg);
        auto r = sv.comp.result();
        CHECK(sv.trials <= 2);
        CHECK(sv.comp.done());
        bool undecidedSide = !r.sideDecided[0] || !r.sideDecided[1];
        if (undecidedSide) CHECK(r.failure == status::Reason::SaNotMeasuredBudget);
        std::printf("  budget 2: %s\n", r.debug.empty() ? "" : r.debug[0].c_str());
    }

    SECTION("finish(sa_cut_by_restart): every open side ends undecided with that reason; nothing is emitted as decided");
    {
        KinematicOracle o(w);
        auto local = solveLocal(o, s, 0);
        CompMember m;
        m.id = 1;
        m.frame = 100.0;
        m.down = true;
        CompPlanner p(kComp, m, {{2, 108.0, false, false}});
        p.setNow(1e300);
        p.finalizeLocal(local.outcomes(), lateLimitOf(s, 0));
        auto batch = p.nextBatch(100);
        bool hadWork = !batch.empty();
        p.finish(status::Reason::SaCutByRestart);
        CHECK(p.done());
        auto r = p.result();
        if (hadWork) {
            CHECK(!r.decided);
            CHECK(r.failure == status::Reason::SaCutByRestart);
        }
        auto st = status::statusOf({}, status::LocalFacts{true, false, false, false, false, 4.17, false, false, false, false, false, false, false, {false, false}, {true, true}}, saFacts(&r), {});
        if (hadWork) CHECK(st.status == status::TimingStatus::SequenceDependent);
    }
}

void testOrdering() {
    SECTION("ordering: follower offsets never cross the member or each other (the legality clamp) and never cross the next fixed input");
    CompMember m;
    m.id = 1;
    m.frame = 100.0;
    m.down = true;
    // followers 1 tick apart, then a fixed input well after them
    CompPlanner p(kComp, m, {{2, 101.0, false, false}, {3, 102.0, true, false}, {4, 103.0, false, false}, {5, 130.0, true, false}});
    p.setNow(1e300);
    std::vector<ShiftOutcome> all;
    for (int k = 1; k <= 10; ++k) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = static_cast<double>(k);
        o.kind = ShiftKind::Died;
        o.deathAfterFrames = 60.0;   // after every follower acted
        o.laterFixed = 3;
        all.push_back(o);
        ShiftOutcome e = o;
        e.nominalFrames = e.appliedFrames = -static_cast<double>(k);
        e.kind = ShiftKind::Resynced;
        all.push_back(e);
    }
    p.finalizeLocal(all, 1.0 - kMargin);
    int checked = 0;
    for (int round = 0; round < 40 && !p.done(); ++round) {
        auto batch = p.nextBatch(100);
        if (batch.empty()) break;
        for (auto const& t : batch) {
            double prev = t.moved.front().frame;
            bool ordered = true;
            for (size_t j = 1; j < t.moved.size(); ++j) {
                if (t.moved[j].frame <= prev + kMargin - 1e-9) ordered = false;
                prev = t.moved[j].frame;
            }
            // the next fixed input after the moved followers
            if (t.followers < 3 && t.followers >= 1) {
                double nextFixed = 100.0 + static_cast<double>(t.followers + 1);
                if (prev >= nextFixed - kMargin + 1e-9) ordered = false;
            }
            else if (t.followers == 3 && prev >= 130.0 - kMargin + 1e-9) ordered = false;
            CHECK(ordered);
            ++checked;
            CompOutcome o;
            o.id = t.id;
            o.kind = CompOutcome::Kind::Died;
            o.deathFrame = prev + 30.0;
            o.laterFixed = 1;
            o.dev.push_back({prev + 1.0, 1.0, 0.1});
            o.dev.push_back({prev + 30.0, 2.0, 0.2});
            p.ingest(o);
        }
    }
    CHECK(checked > 0);
    CHECK(p.trials() <= kComp.maxTrialsPerInput);
    std::printf("  %d trials checked for ordering | %s\n", checked, p.describe().c_str());
}

}  // namespace

int main() {
    testLinearAlgebra();
    testCriticalCase();
    testIsolated();
    testLateDiedSelf();
    testEarlyWaitsForFollower();
    testDeterminismAndBudget();
    testOrdering();
    return gprl::test::finish("compensation_tests");
}
