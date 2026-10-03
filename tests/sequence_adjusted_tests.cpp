// SEQUENCE-ADJUSTED window host tests (docs/TIMING_SOLVER_V2.md §2.5-§2.9, §3.7, §5) on the
// kinematic DEV FIXTURE physics (tests/kinematic_oracle.hpp, NOT Geometry Dash):
//
//   waveZigzagCorridor   local windows narrow + mirrored + downstream; SA (pair) windows wider,
//                        set by the vertex clearance, equal to a brute-force family scan, no mirror
//   localVsSequence      the AUDIT §5 shape (press 100, release 108, press 116): local ~ 4 f, SA >> 4 f;
//                        W_local ⊆ W_SA and SA == brute force over 500 random corridors
//   cubeSymmetric / cubeAsymmetric / pressOnly / releaseOnly / holdDuration
//   shipCorrections      a far tight gap: chain members tried, the result is sequence_dependent
//                        (sa_undecided), never a guessed number
//   cbfRefine            sub-tick oracle: edges within 1/8 tick of the analytic boundary, placement cbf
//   speedPortal / gravityPortal / miniPortal
//   T-PROG-1..5, T-BUDGET  termination and trial caps against adversarial outcome scripts (§3.7)
//
// "Brute force" = every shift scanned directly with the oracle (the ground truth the planners are
// compared with); nothing here is a real level.
#include "test_util.hpp"
#include "kinematic_oracle.hpp"

#include "../core/solver/pass_planner.hpp"
#include "../core/solver/sequence_adjusted.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_status.hpp"
#include "../core/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::test::kin;

namespace {

constexpr double T = kTickMs;
constexpr double kH = 0.5 + 10.0 / 240.0;   // the engine's look-ahead t + 0.5 s + max shift, from the first difference
constexpr double kMargin = 0.0024;

/// The config with the full PAIR walk of presses switched on (off by default since Fable D9): the
/// tests of the pair window itself (AUDIT §7 hold duration) and of the spawn loop's bound use it.
SAConfig pairWalkOn() {
    SAConfig c = kSA;
    c.pairWalkForPresses = true;
    return c;
}

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

PassPlanner solveLocal(KinematicOracle& o, InputSchedule const& s, size_t i, bool subtick = false, double resolution = 0.125, int passes = 1) {
    PlannerConfig pc;
    pc.subtick = subtick;
    pc.resolutionFrames = resolution;
    pc.maxRefinePasses = passes;
    PassPlanner p(pc, earlyLimitOf(s, i));
    p.setEarlyLimitKind(i == 0 ? LimitKind::AttemptStart : LimitKind::Neighbour);
    double late = lateLimitOf(s, i);
    if (!std::isnan(late)) p.setLateLimit(late);
    o.setReference(s);
    runAgainstOracle(o, p, 0, s, i, kH);
    return p;
}

struct Solved {
    std::vector<PassPlanner> locals;
    SAPlanner sa;
    int saTrials = 0;
};

Solved solveAll(KinematicOracle& o, InputSchedule const& s, std::vector<size_t> const& members, std::vector<size_t> const& breaks = {},
                bool subtick = false, SAConfig cfg = kSA) {
    Solved r;
    for (size_t i : members) r.locals.push_back(solveLocal(o, s, i, subtick));
    SARunInput in;
    in.config = cfg;
    in.members = members;
    for (auto const& p : r.locals) in.locals.push_back(&p);
    in.breaks = breaks;
    in.subtick = subtick;
    o.setReference(s);
    r.saTrials = runSAAgainstOracle(o, 0, s, in, r.sa);
    return r;
}

// ---- brute force (the ground truth) ----

bool localPassAt(KinematicOracle& o, InputSchedule const& s, size_t i, double shift) {
    auto m = s;
    m.inputs[i].tMs += shift * T;
    o.setReference(s);
    // the engine's local horizon is fixed at the unshifted input + 0.5 s + max shift (runAgainstOracle does the same)
    return o.trial(0, m, kH + (shift < 0 ? -shift / 240.0 : 0.0)).passed();
}

/// One family member exactly as SAPlanner builds it (the member + k followers, the same look-ahead:
/// one horizon after the LAST moved input, Fable review D2).
bool memberPassAt(KinematicOracle& o, InputSchedule const& s, size_t i, int k, double shift, SAConfig const& cfg = kSA) {
    auto m = s;
    for (int f = 0; f <= k; ++f) m.inputs[i + static_cast<size_t>(f)].tMs += shift * T;
    double t = frameOf(s, i);
    double last = frameOf(s, i + static_cast<size_t>(k)) + shift;
    double horizon = cfg.horizonSeconds * 240.0 + cfg.maxShiftTicks;
    double lookAhead = std::max(cfg.horizonFromLastMoved ? last + horizon : t + shift + horizon, last + cfg.convergeSteps + 1.0);
    double earliest = std::min(t + shift, t);
    o.setReference(s);
    return o.trial(0, m, (lookAhead - earliest) / 240.0).passed();
}

bool crossesAt(InputSchedule const& s, size_t lastMoved, double shift) {
    if (shift <= 0 || lastMoved + 1 >= s.inputs.size()) return false;
    return frameOf(s, lastMoved) + shift >= frameOf(s, lastMoved + 1) - kMargin - 1e-9;
}

/// SA(s) by brute force: local (when legal) or some legal member of F passes.
bool familyPassAt(KinematicOracle& o, InputSchedule const& s, size_t i, double shift, std::vector<size_t> const& breaks = {}, int maxChain = 3) {
    if (!crossesAt(s, i, shift) && localPassAt(o, s, i, shift)) return true;
    for (int k = 1; k <= maxChain; ++k) {
        if (i + static_cast<size_t>(k) >= s.inputs.size()) break;
        bool broken = false;
        for (size_t b : breaks) if (b > i && b <= i + static_cast<size_t>(k)) broken = true;
        if (broken) break;
        if (crossesAt(s, i + static_cast<size_t>(k), shift)) continue;
        if (memberPassAt(o, s, i, k, shift)) return true;
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
        else {
            w.firstFail = k;
            break;
        }
    }
    return w;
}

double widthTicks(SAWindow const& w) { return w.late.edgeFrames() - w.early.edgeFrames(); }
double localWidthTicks(WindowResult const& w) { return (w.latestMs - w.earliestMs) / T; }
double localEdgeTicks(BoundaryResult const& b) { return localEdgeFrames(b); }

/// The mirror relation of RC1 between consecutive inputs: early_i ~ -late_{i+1} and late_i ~ -early_{i+1}.
bool mirrored(double earlyA, double lateA, double earlyB, double lateB, double tolTicks = 0.12) {
    return std::fabs(earlyA + lateB) <= tolTicks && std::fabs(lateA + earlyB) <= tolTicks;
}

// ---- worlds ----

World waveWorld() {
    World w;
    w.startMode = Mode::Wave;
    w.startY = 0.0;
    w.half = 3.0;
    return w;
}

/// Tick of a wave x position at speed 1.
double tickAt(double x) { return x / 1.3; }

void testWaveZigzagCorridor() {
    SECTION("waveZigzagCorridor: narrow far gap -> local narrow, mirrored, downstream; SA (pair) wider, vertex clearance, no mirror");
    auto s = schedule({{100, true}, {118, false}, {136, true}, {154, false}, {172, true}, {190, false}});
    World w = waveWorld();
    auto path = freePath(w, s, 360);
    // per-segment clearances (below, above), different per vertex segment; a tight spot at ticks
    // 205..209, 2 below and 5 above the reference run
    struct Seg { double from, to, below, above; };
    std::vector<Seg> segs = {{0, 100, 20, 20}, {100, 118, 16, 12}, {118, 136, 13, 21}, {136, 154, 19, 15}, {154, 172, 14, 22},
                             {172, 190, 23, 17}, {190, 205, 20, 20}, {205, 210, 2, 5}, {210, 400, 20, 20}};
    auto clearance = [&](double x, bool above) {
        double k = tickAt(x);
        for (auto const& g : segs) if (k >= g.from && k < g.to) return above ? g.above : g.below;
        return 20.0;
    };
    corridorAround(w, path, [&](double x) { return clearance(x, false); }, [&](double x) { return clearance(x, true); });
    KinematicOracle o(w);
    std::vector<size_t> members = {0, 1, 2, 3, 4, 5};
    auto solved = solveAll(o, s, members);
    CHECK(solved.sa.done());
    std::vector<double> le, ll, se, sl;
    for (size_t m = 0; m < members.size(); ++m) {
        size_t i = members[m];
        auto w0 = solved.locals[m].result(s.inputs[i].tMs);
        auto r = solved.sa.result(static_cast<int>(m));
        CHECK(w0.valid && r.valid);
        le.push_back(localEdgeTicks(w0.early));
        ll.push_back(localEdgeTicks(w0.late));
        se.push_back(r.sequence.early.edgeFrames());
        sl.push_back(r.sequence.late.edgeFrames());
        std::printf("  #%zu %s local [%+.1f,%+.1f] (%s/%s) | SA [%+.1f,%+.1f] %s %s | %s\n", i, s.inputs[i].down ? "press  " : "release", le.back(), ll.back(),
                    name(w0.early.edge.cause), name(w0.late.edge.cause), se.back(), sl.back(), r.decided ? "decided" : "UNDECIDED",
                    r.adaptationUsed.empty() ? "-" : name(r.adaptationUsed.front()), r.debug.empty() ? "" : r.debug[0].c_str());
        // W_local ⊆ W_SA
        CHECK(se.back() <= le.back() + 1e-9 && sl.back() >= ll.back() - 1e-9);
        CHECK(r.decided);
        // SA == brute force family scan on both sides
        double lim[2] = {std::min(10.0, earlyLimitOf(s, i)), 10.0};
        for (int side = 0; side < 2; ++side) {
            double sign = side ? 1.0 : -1.0;
            auto bw = bruteWalk([&](double k) { return familyPassAt(o, s, i, sign * k); }, lim[side]);
            SAEdge const& e = side ? r.sequence.late : r.sequence.early;
            CHECK_MSG(std::fabs(std::fabs(e.passFrames) - bw.lastPass) < 1e-9,
                      "input " + std::to_string(i) + " side " + std::to_string(side) + ": SA pass " + std::to_string(e.passFrames) + " brute " + std::to_string(bw.lastPass));
            if (e.stop == EdgeStop::Fail) CHECK(!std::isnan(bw.firstFail) && std::fabs(std::fabs(e.failFrames) - bw.firstFail) < 1e-9);
        }
        if (i + 1 < s.inputs.size()) {
            // every input before the tight spot: local edges bounded by the far gap = downstream
            CHECK(w0.early.bounded && w0.late.bounded);
            CHECK(w0.early.edge.cause == EdgeCause::Downstream || w0.late.edge.cause == EdgeCause::Downstream);
            CHECK(localWidthTicks(w0) <= 2.0 + 1e-9);
            CHECK_MSG(widthTicks(r.sequence) >= localWidthTicks(w0) + 4.0 - 1e-9, "SA must be much wider than local for input " + std::to_string(i));
            CHECK(r.sequence.early.cause == EdgeCause::Self && r.sequence.late.cause == EdgeCause::Self);
            CHECK(!r.adaptationUsed.empty() && r.adaptationUsed.front() == SAAdaptation::Pair);
            CHECK(!r.isolated);
        }
        else {
            CHECK(r.isolated);   // the last input: no later input, W_SA = W_local
            CHECK(std::fabs(se.back() - le.back()) < 1e-9 && std::fabs(sl.back() - ll.back()) < 1e-9);
        }
    }
    // RC1's signature: consecutive local windows mirror each other (one clearance limit counted on every input)
    int localMirror = 0, saMirror = 0;
    for (size_t k = 0; k + 1 < members.size(); ++k) {
        if (mirrored(le[k], ll[k], le[k + 1], ll[k + 1])) ++localMirror;
        if (mirrored(se[k], sl[k], se[k + 1], sl[k + 1])) ++saMirror;
    }
    CHECK_MSG(localMirror >= 4, "local mirror pairs " + std::to_string(localMirror));
    CHECK_MSG(saMirror == 0, "SA mirror pairs " + std::to_string(saMirror));
    // statuses: ok (decided SA) with the downstream edge noted; the timing_result validates
    for (size_t m = 0; m < members.size(); ++m) {
        size_t i = members[m];
        auto w0 = solved.locals[m].result(s.inputs[i].tMs);
        auto r = solved.sa.result(static_cast<int>(m));
        LocalEvidence ev;
        ev.window = &w0;
        ev.outcomes = &solved.locals[m].outcomes();
        ev.frame = frameOf(s, i);
        ev.nextFrame = i + 1 < s.inputs.size() ? frameOf(s, i + 1) : kNaN;
        ev.nextFollows = true;
        ev.horizonFrame = ev.frame + 130.0;
        status::ContextFacts cx;
        cx.isolated = r.isolated;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), cx);
        CHECK_MSG(st.status == status::TimingStatus::Ok, std::string("status ") + status::name(st.status));
        TimingResultContext ctx;
        ctx.inputSeq = static_cast<int64_t>(10 + i);
        ctx.kind = s.inputs[i].down ? InputKind::Press : InputKind::Release;
        ctx.attemptInputIndex = static_cast<int>(i + 1);
        ctx.eventT = s.inputs[i].tMs / 1000.0;
        ctx.gamemode = Gamemode::Wave;
        ctx.cluster = {"a1:1", static_cast<int>(i + 1), i == 0 ? cluster::Tri::No : cluster::Tri::Yes, cluster::Tri::Yes};
        if (!s.inputs[i].down) {
            ctx.pressMs = s.inputs[i - 1].tMs;
            ctx.pressSeq = static_cast<int64_t>(10 + i - 1);
        }
        auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
        CHECK_MSG(built.ok, built.error);
        CHECK(built.payload.sequence.has_value() && built.payload.sequence->decided);
        if (!s.inputs[i].down) CHECK(built.payload.hold && built.payload.hold->basis == "sequence");
    }
}

void testLocalVsSequence() {
    SECTION("localVsSequence: the AUDIT §5 shape - press 100, release 108, press 116: local ~ 4 f, SA >> 4 f");
    auto s = schedule({{100, true}, {108, false}, {116, true}});
    World w = waveWorld();
    auto path = freePath(w, s, 300);
    // a tight spot 90 ticks after the last press (inside every look-ahead): 6 units below and above
    corridorAround(w, path, [](double x) { double k = tickAt(x); return (k >= 205 && k < 208) ? 6.0 : 30.0; },
                   [](double x) { double k = tickAt(x); return (k >= 205 && k < 208) ? 6.0 : 30.0; });
    KinematicOracle o(w);
    auto solved = solveAll(o, s, {0, 1, 2});
    auto w0 = solved.locals[0].result(s.inputs[0].tMs);
    auto r0 = solved.sa.result(0);
    double lw = localWidthTicks(w0);
    double sw = widthTicks(r0.sequence);
    std::printf("  press #0: local %.2f f [%+.1f,%+.1f] | sequence-adjusted %.2f f [%+.1f,%+.1f] %s\n", lw, localEdgeTicks(w0.early), localEdgeTicks(w0.late), sw,
                r0.sequence.early.edgeFrames(), r0.sequence.late.edgeFrames(), r0.decided ? "decided" : "undecided");
    CHECK(lw >= 3.0 && lw <= 6.0);    // "Local window: 4.0f"
    CHECK(sw >= 2.0 * lw);            // "Sequence-adjusted window: 10.7f" (>> local)
    CHECK(r0.decided);
    CHECK(w0.late.edge.cause == EdgeCause::Downstream);

    SECTION("W_local ⊆ W_SA and SA == brute force on decided sides, over 500 random wave corridors");
    std::mt19937 rng(20260930u);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int cases = 0, undecidedSides = 0, containFail = 0, bruteFail = 0, payloadFail = 0, gateFail = 0;
    for (int n = 0; n < 500; ++n) {
        int count = 3 + static_cast<int>(u(rng) * 3.0);
        std::vector<std::pair<double, bool>> ticks;
        double t = 60.0 + std::floor(u(rng) * 20.0);
        for (int k = 0; k < count; ++k) {
            ticks.push_back({t, k % 2 == 0});
            t += 6.0 + std::floor(u(rng) * 19.0);
        }
        auto sc = schedule(ticks);
        World wr = waveWorld();
        auto pr = freePath(wr, sc, static_cast<int64_t>(t) + 200);
        std::vector<double> segBelow, segAbove;
        for (int k = 0; k <= count + 1; ++k) {
            segBelow.push_back(6.0 + u(rng) * 24.0);
            segAbove.push_back(6.0 + u(rng) * 24.0);
        }
        double tight = t + 5.0 + std::floor(u(rng) * 40.0);
        double tightBelow = 2.0 + u(rng) * 8.0, tightAbove = 2.0 + u(rng) * 8.0;
        auto segOf = [&](double x) {
            double k = tickAt(x);
            int seg = 0;
            for (auto const& [tt, d] : ticks) if (k >= tt) ++seg;
            return seg;
        };
        corridorAround(wr, pr, [&](double x) { double k = tickAt(x); return (k >= tight && k < tight + 3) ? tightBelow : segBelow[static_cast<size_t>(segOf(x))]; },
                       [&](double x) { double k = tickAt(x); return (k >= tight && k < tight + 3) ? tightAbove : segAbove[static_cast<size_t>(segOf(x))]; });
        KinematicOracle orr(wr);
        // the reference run must survive its own corridor
        orr.setReference(sc);
        if (!orr.trial(0, sc, 1.5).passed()) continue;
        std::vector<size_t> members;
        for (size_t i = 0; i < sc.inputs.size(); ++i) members.push_back(i);
        auto sv = solveAll(orr, sc, members);
        ++cases;
        for (size_t m = 0; m < members.size(); ++m) {
            size_t i = members[m];
            auto lw0 = sv.locals[m].result(sc.inputs[i].tMs);
            if (!lw0.valid) continue;
            auto r = sv.sa.result(static_cast<int>(m));
            // containment: every local pass is an SA pass, and (Fable D1: an undecided side at the
            // local edge inherits the local bracket) the reported edges too, decided or not
            bool ok = std::fabs(r.sequence.early.passFrames) + 1e-9 >= std::fabs(lw0.early.passShiftMs / T)
                   && r.sequence.late.passFrames + 1e-9 >= lw0.late.passShiftMs / T;
            ok = ok && r.sequence.early.edgeFrames() <= localEdgeTicks(lw0.early) + 1e-9;
            ok = ok && r.sequence.late.edgeFrames() >= localEdgeTicks(lw0.late) - 1e-9;
            if (!ok) ++containFail;
            for (int side = 0; side < 2; ++side) {
                if (!r.sideDecided[side]) {
                    ++undecidedSides;
                    continue;
                }
                double sign = side ? 1.0 : -1.0;
                double lim = side ? 10.0 : std::min(10.0, earlyLimitOf(sc, i));
                auto bw = bruteWalk([&](double k) { return familyPassAt(orr, sc, i, sign * k); }, lim);
                SAEdge const& e = side ? r.sequence.late : r.sequence.early;
                bool same = std::fabs(std::fabs(e.passFrames) - bw.lastPass) < 1e-9;
                if (e.stop == EdgeStop::Fail) same = same && !std::isnan(bw.firstFail) && std::fabs(std::fabs(e.failFrames) - bw.firstFail) < 1e-9;
                if (!same) {
                    ++bruteFail;
                    if (bruteFail <= 3) {
                        std::printf("  case %d input %zu side %d: SA pass %.1f fail %.1f stop %s | brute pass %.1f fail %.1f | %s\n", n, i, side, e.passFrames, e.failFrames,
                                    name(e.stop), bw.lastPass, bw.firstFail, r.debug.empty() ? "" : r.debug[0].c_str());
                    }
                }
            }
            // the payload always validates
            LocalEvidence ev;
            ev.window = &lw0;
            ev.outcomes = &sv.locals[m].outcomes();
            ev.frame = frameOf(sc, i);
            ev.nextFrame = i + 1 < sc.inputs.size() ? frameOf(sc, i + 1) : kNaN;
            ev.nextFollows = true;
            ev.horizonFrame = ev.frame + 130.0;
            auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
            TimingResultContext ctx;
            ctx.inputSeq = static_cast<int64_t>(i + 2);
            ctx.kind = sc.inputs[i].down ? InputKind::Press : InputKind::Release;
            ctx.attemptInputIndex = static_cast<int>(i + 1);
            ctx.eventT = sc.inputs[i].tMs / 1000.0;
            ctx.cluster = {"a1:1", static_cast<int>(i + 1), cluster::Tri::Unknown, cluster::Tri::Unknown};
            auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
            if (!built.ok) {
                ++payloadFail;
                if (payloadFail <= 3) std::printf("  case %d input %zu payload: %s\n", n, i, built.error.c_str());
            }
            else {
                // MOD verifier: the evidence gate's mirror too (W_local ⊆ W_SA on pass AND reported
                // edges, Fable D1; proof consistency, D3b) - what the server stores as usable
                auto gate = checkTimingResultPayload(built.payload, ctx.eventT, 0.0);
                if (!gate.accepted) {
                    ++gateFail;
                    std::string why;
                    for (auto const& s : gate.reasons) why += s + " ";
                    if (gateFail <= 3) std::printf("  case %d input %zu gate mirror: %s\n", n, i, why.c_str());
                }
            }
        }
    }
    std::printf("  %d corridors, %d undecided sides, containment failures %d, brute-force disagreements %d, payload failures %d, gate-mirror refusals %d\n", cases,
                undecidedSides, containFail, bruteFail, payloadFail, gateFail);
    CHECK(cases >= 300);
    CHECK(containFail == 0);
    CHECK(bruteFail == 0);
    CHECK(payloadFail == 0);
    CHECK(gateFail == 0);
}

// ---- termination and caps against adversarial scripts (§3.7) ----

SAInput scriptedInput(uint32_t id, double frame, bool down, std::vector<ShiftOutcome> local, bool pairWalk = false) {
    SAInput in;
    in.id = id;
    in.frame = frame;
    in.down = down;
    in.earlyLimitFrames = 10.0;
    in.earlyLimitKind = LimitKind::Neighbour;
    in.local = std::move(local);
    in.pairWalk = pairWalk;
    return in;
}

std::vector<ShiftOutcome> diedEverywhereDownstream(double passTo) {
    std::vector<ShiftOutcome> out;
    for (int k = 1; k <= 10; ++k) {
        for (double sign : {-1.0, 1.0}) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = sign * k;
            if (k <= passTo) o.kind = ShiftKind::Survived;
            else {
                o.kind = ShiftKind::Died;
                o.deathAfterFrames = 90.0;
                o.laterFixed = 3;
            }
            out.push_back(o);
        }
    }
    return out;
}

SAContext zigzagContext(int n, double first, double gap) {
    SAContext c;
    for (int i = 0; i < n; ++i) c.inputs.push_back({static_cast<uint32_t>(i + 1), first + gap * i, i % 2 == 0, false});
    return c;
}

/// Drives the planner with scripted outcomes; returns the trials handed out.
int drive(SAPlanner& p, std::function<SAOutcome(SATrial const&)> const& script, int guardRounds = 10000) {
    int n = 0;
    for (int round = 0; round < guardRounds && !p.done(); ++round) {
        auto batch = p.nextBatch(1 << 20);
        if (batch.empty()) break;
        for (auto const& t : batch) {
            ++n;
            SAOutcome o = script(t);
            o.id = t.id;
            p.ingest(o);
        }
    }
    return n;
}

void testProgressAndBudget() {
    SECTION("T-PROG-2: nextBatch pops one queued trial per returned trial; the queue is finite without ingests");
    {
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, diedEverywhereDownstream(1), true)}, zigzagContext(5, 100.0, 18.0));
        CHECK(p.needsTrials());
        int handed = 0;
        for (int guard = 0; guard < 1000; ++guard) {
            auto b = p.nextBatch(1);
            if (b.empty()) break;
            CHECK(b.size() == 1);
            ++handed;
        }
        CHECK(handed > 0 && handed <= kSA.maxTrialsPerInput);
        CHECK(p.outstanding() == handed);
        CHECK(!p.done());
        p.finish(status::Reason::SaExpired);
        CHECK(p.done());
        auto r = p.result(0);
        CHECK(!r.decided);
        CHECK(r.failure == status::Reason::SaExpired);
    }
    SECTION("T-PROG-3: escalation stops at maxChain (oracle: every trial dies after a fixed input)");
    {
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, diedEverywhereDownstream(1))}, zigzagContext(8, 100.0, 18.0));
        int maxK = 0;
        int n = drive(p, [&](SATrial const& t) {
            maxK = std::max(maxK, static_cast<int>(t.adaptation));
            SAOutcome o;
            o.kind = SAOutcome::Kind::Died;
            o.deathFrame = t.moved.front().frame + 100.0;
            o.laterFixed = 2;
            return o;
        });
        CHECK(p.done());
        CHECK(maxK <= kSA.maxChain);
        CHECK(n <= 2 * kSA.maxChain);   // one escalation per side, then undecided (the walk stops)
        auto r = p.result(0);
        CHECK(!r.decided && r.undecidedShifts == 2);
        CHECK(r.sequence.early.stop == EdgeStop::Undecided && r.sequence.late.stop == EdgeStop::Undecided);
        // the conservative edge is the last pass (the local run), never inside the local window
        CHECK(std::fabs(r.sequence.early.passFrames + 1.0) < 1e-9 && std::fabs(r.sequence.late.passFrames - 1.0) < 1e-9);
    }
    SECTION("T-PROG-4 / T-BUDGET: trials per input <= maxTrialsPerInput (always invalid / always pass / alternating)");
    for (int mode = 0; mode < 3; ++mode) {
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, diedEverywhereDownstream(0), true), scriptedInput(2, 118.0, false, diedEverywhereDownstream(0))},
                    zigzagContext(8, 100.0, 18.0));
        int calls = 0;
        std::vector<int> perMember(2, 0);
        drive(p, [&](SATrial const& t) {
            ++perMember[static_cast<size_t>(t.member)];
            SAOutcome o;
            ++calls;
            if (mode == 0) o.kind = SAOutcome::Kind::Invalid;
            else if (mode == 1) o.kind = SAOutcome::Kind::Pass;
            else {
                o.kind = calls % 2 ? SAOutcome::Kind::Pass : SAOutcome::Kind::Died;
                o.deathFrame = t.moved.front().frame + 50.0;
                o.laterFixed = 1;
            }
            return o;
        });
        CHECK(p.done());
        for (int c : perMember) CHECK_MSG(c <= kSA.maxTrialsPerInput, "mode " + std::to_string(mode) + ": " + std::to_string(c) + " trials");
        if (mode == 1) {
            // every member passes: the walks run to the range (10 each side) and the pair walk completes
            auto r = p.result(0);
            CHECK(r.decided && r.pair.present);
            CHECK(std::fabs(r.sequence.late.passFrames - 10.0) < 1e-9);
        }
        if (mode == 0) CHECK(p.result(0).failure == status::Reason::SaInvalidTrials);
    }
    SECTION("T-BUDGET: a tiny trial cap ends sides undecided (sa_not_measured_budget), never guessed");
    {
        SAConfig tight = kSA;
        tight.maxTrialsPerInput = 3;
        SAPlanner p(tight, {scriptedInput(1, 100.0, true, diedEverywhereDownstream(0), true)}, zigzagContext(6, 100.0, 18.0));
        int n = drive(p, [](SATrial const&) {
            SAOutcome o;
            o.kind = SAOutcome::Kind::Pass;
            return o;
        });
        CHECK(n <= 3);
        auto r = p.result(0);
        CHECK(p.done() && !r.decided);
        CHECK(r.failure == status::Reason::SaNotMeasuredBudget);
    }
    SECTION("T-PROG-5: the side walk is bounded by the range (distance from 0 strictly increases)");
    {
        // no later input at all: every local fail is self, nothing to simulate, decided at once
        SAContext lone;
        lone.inputs.push_back({1, 100.0, true, false});
        auto selfDeaths = diedEverywhereDownstream(2);
        for (auto& o : selfDeaths) o.laterFixed = 0;   // nothing after the input: every death is its own
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, selfDeaths)}, lone);
        CHECK(!p.needsTrials());
        CHECK(p.done());
        auto r = p.result(0);
        CHECK(r.decided && r.isolated);
        CHECK(std::fabs(r.sequence.late.passFrames - 2.0) < 1e-9 && std::fabs(r.sequence.late.failFrames - 3.0) < 1e-9);
    }
    SECTION("self deaths decide with zero simulations when the next input could not act before them (late shifts)");
    {
        std::vector<ShiftOutcome> local;
        for (int k = 1; k <= 10; ++k) {
            for (double sign : {-1.0, 1.0}) {
                ShiftOutcome o;
                o.nominalFrames = o.appliedFrames = sign * k;
                if (k <= 2) o.kind = ShiftKind::Survived;
                else {
                    o.kind = ShiftKind::Died;
                    o.deathAfterFrames = sign > 0 ? 10.0 : 16.0;   // before the next input at +18
                    o.laterFixed = 0;
                }
                local.push_back(o);
            }
        }
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, local)}, zigzagContext(3, 100.0, 18.0));
        // late: the next input (118) + s >= the death (110): fail without simulation; early: the
        // next input shifted by -3 (115) acts before the death at 116 -> the pair must be tried
        auto batch = p.nextBatch(10);
        CHECK(batch.size() == 1);
        if (!batch.empty()) {
            CHECK(batch[0].shiftFrames < 0 && batch[0].adaptation == SAAdaptation::Pair);
            CHECK(batch[0].moved.size() == 2 && std::fabs(batch[0].moved[1].frame - 115.0) < 1e-9);
        }
    }
}

// ---- v2 planner rules added by builder A (review of the partial build) ----

std::vector<ShiftOutcome> scriptedLocal(int passTo, double deathAfter, int laterFixed, std::vector<int> untestedLate = {}) {
    std::vector<ShiftOutcome> out;
    for (int k = 1; k <= 10; ++k) {
        for (double sign : {-1.0, 1.0}) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = sign * k;
            bool untested = sign > 0 && std::find(untestedLate.begin(), untestedLate.end(), k) != untestedLate.end();
            if (untested) {
                o.kind = ShiftKind::NotTested;
                o.reason = "pool";
            }
            else if (k <= passTo) o.kind = ShiftKind::Survived;
            else {
                o.kind = ShiftKind::Died;
                o.deathAfterFrames = deathAfter;
                o.laterFixed = laterFixed;
            }
            out.push_back(o);
        }
    }
    return out;
}

void testPlannerRules() {
    SECTION("T-PROG-1: the engine's SA spawn loop (spawnSATrials) spawns one popped trial per iteration or breaks, <= parallel - running");
    {
        auto fresh = [] { return SAPlanner(pairWalkOn(), {scriptedInput(1, 100.0, true, diedEverywhereDownstream(1), true)}, zigzagContext(5, 100.0, 18.0)); };
        {
            // no clone ever: nothing is taken from the planner
            SAPlanner p = fresh();
            int allocs = 0;
            int n = spawnSATrials(p, 0, 6, [&] { ++allocs; return -1; }, [](int, SATrial const&) { return true; });
            CHECK(n == 0 && allocs == 1 && p.outstanding() == 0 && p.hasPending());
        }
        {
            // clones available: exactly parallel - running trials, each popped once
            SAPlanner p = fresh();
            int allocs = 0;
            std::vector<int> ids;
            int n = spawnSATrials(p, 2, 6, [&] { return ++allocs; }, [&](int, SATrial const& t) { ids.push_back(t.id); return true; });
            CHECK(n == 4 && allocs == 4 && p.outstanding() == 4);
            for (size_t a = 0; a < ids.size(); ++a)
                for (size_t b = a + 1; b < ids.size(); ++b) CHECK(ids[a] != ids[b]);
            CHECK(spawnSATrials(p, 6, 6, [&] { return ++allocs; }, [](int, SATrial const&) { return true; }) == 0);   // full: no iteration
            CHECK(allocs == 4);
        }
        {
            // more work queued than the pool may take (3 members with pair walks: 12 trials pending
            // at once): the cap is parallel - running, never the queue length; a full (or over-full)
            // pool allocates nothing and pops nothing. (The single-member planner above queues
            // exactly 4 trials, so it cannot tell the bound from an empty queue - verifier review.)
            SAPlanner p(pairWalkOn(),
                        {scriptedInput(1, 100.0, true, diedEverywhereDownstream(1), true), scriptedInput(3, 136.0, true, diedEverywhereDownstream(1), true),
                         scriptedInput(5, 172.0, true, diedEverywhereDownstream(1), true)},
                        zigzagContext(8, 100.0, 18.0));
            int allocs = 0;
            auto spawnOk = [](int, SATrial const&) { return true; };
            int n = spawnSATrials(p, 2, 6, [&] { return ++allocs; }, spawnOk);
            CHECK_MSG(n == 4 && allocs == 4 && p.outstanding() == 4, "spawned " + std::to_string(n) + ", allocs " + std::to_string(allocs));
            CHECK(p.hasPending());   // work is left: only the bound ended the loop
            CHECK(spawnSATrials(p, 6, 6, [&] { return ++allocs; }, spawnOk) == 0 && allocs == 4 && p.outstanding() == 4);
            CHECK(spawnSATrials(p, 9, 6, [&] { return ++allocs; }, spawnOk) == 0 && allocs == 4 && p.outstanding() == 4);
            CHECK(spawnSATrials(p, 5, 6, [&] { return ++allocs; }, spawnOk) == 1 && allocs == 5 && p.outstanding() == 5);
        }
        {
            // a failed spawn: the popped trial is ingested Invalid (never lost) and the loop stops
            SAPlanner p = fresh();
            int n = spawnSATrials(p, 0, 6, [] { return 7; }, [](int, SATrial const&) { return false; });
            CHECK(n == 0 && p.outstanding() == 0);
        }
        {
            // adversarial: alloc fails every 3rd call, spawn every 5th; every call ends after at
            // most `parallel` iterations and the planner still finishes (answers: always pass)
            SAPlanner p = fresh();
            int calls = 0, rounds = 0;
            std::vector<SATrial> running;
            for (; rounds < 400 && !p.done(); ++rounds) {
                int before = calls;
                int got = spawnSATrials(p, static_cast<int>(running.size()), 6, [&] { return (++calls % 3) ? calls : -1; },
                                        [&](int, SATrial const& t) {
                                            if (calls % 5 == 0) return false;
                                            running.push_back(t);
                                            return true;
                                        });
                CHECK(got <= 6 && calls - before <= 6);
                for (auto const& t : running) {
                    SAOutcome o;
                    o.id = t.id;
                    o.kind = SAOutcome::Kind::Pass;
                    p.ingest(o);
                }
                running.clear();
            }
            CHECK(p.done() && rounds < 400);
        }
    }
    SECTION("W_local ⊆ W_SA with an untested gap inside the local bracket: the gap is the local planner's, never simulated by SA");
    {
        // late: +1 pass, +2 NOT tested (pool), +3.. died downstream -> local bracket [+1, +3]
        auto local = scriptedLocal(1, 60.0, 1, {2});
        std::vector<double> shifts;
        {
            SAPlanner p(kSA, {scriptedInput(1, 100.0, true, local)}, zigzagContext(4, 100.0, 18.0));
            drive(p, [&](SATrial const& t) {
                shifts.push_back(t.shiftFrames);
                SAOutcome o;
                o.kind = SAOutcome::Kind::Pass;
                return o;
            });
            for (double s : shifts) CHECK_MSG(std::fabs(s - 2.0) > 1e-9, "a trial at +2 (inside the local bracket) was requested");
            auto r = p.result(0);
            CHECK(r.sideDecided[1] && r.sequence.late.passFrames >= 3.0 - 1e-9);
        }
        {
            // every family member dies inside its span right away: the SA side ends at the LOCAL fail (+3), edge == local edge
            SAPlanner p(kSA, {scriptedInput(1, 100.0, true, local)}, zigzagContext(4, 100.0, 18.0));
            drive(p, [&](SATrial const& t) {
                CHECK(std::fabs(t.shiftFrames - 2.0) > 1e-9);
                SAOutcome o;
                o.kind = SAOutcome::Kind::Died;
                o.deathFrame = t.moved.front().frame + 1.0;
                o.laterFixed = 0;
                return o;
            });
            auto r = p.result(0);
            CHECK(r.sideDecided[1] && r.sequence.late.stop == EdgeStop::Fail);
            CHECK(std::fabs(r.sequence.late.passFrames - 1.0) < 1e-9 && std::fabs(r.sequence.late.failFrames - 3.0) < 1e-9);
            CHECK(std::fabs(r.sequence.late.edgeFrames() - 2.0) < 1e-9);   // the local midpoint edge (1 + 3) / 2
        }
    }
    SECTION("the next input across a cluster break cannot follow: the SA late side stops at `neighbour` (decided, no trial)");
    {
        SAContext ctx;
        ctx.inputs.push_back({1, 100.0, true, false});
        ctx.inputs.push_back({2, 104.0, false, true});   // a gravity portal between #1 and #2
        ctx.inputs.push_back({3, 150.0, true, false});
        std::vector<ShiftOutcome> local;
        for (int k = 1; k <= 10; ++k) {
            ShiftOutcome e;
            e.nominalFrames = e.appliedFrames = -k;
            e.kind = ShiftKind::Survived;
            local.push_back(e);
            ShiftOutcome l;
            l.nominalFrames = l.appliedFrames = k;
            if (k <= 3) l.kind = ShiftKind::Survived;
            else {
                l.kind = ShiftKind::NotTested;
                l.reason = "limit";
            }
            local.push_back(l);
        }
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, local)}, ctx);
        CHECK(!p.needsTrials() && p.done());
        auto r = p.result(0);
        CHECK(r.decided && r.sequence.late.stop == EdgeStop::Neighbour && std::fabs(r.sequence.late.passFrames - 3.0) < 1e-9);
    }
    SECTION("F exhausted (chain3) with a death inside its adapted span -> fail; after a fixed input -> undecided (V2-D2)");
    for (int laterAtChain3 : {0, 1}) {
        // inputs every 5 ticks; the local run is +-1, every death at frame 121 (after 4 later inputs)
        auto local = scriptedLocal(1, 21.0, 4);
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, local)}, zigzagContext(7, 100.0, 5.0));
        int maxK = 0;
        drive(p, [&](SATrial const& t) {
            maxK = std::max(maxK, static_cast<int>(t.adaptation));
            SAOutcome o;
            o.kind = SAOutcome::Kind::Died;
            o.deathFrame = 121.0;
            o.laterFixed = t.adaptation == SAAdaptation::Chain3 ? laterAtChain3 : 2;
            return o;
        });
        auto r = p.result(0);
        CHECK(maxK == 3);
        // early -2: chain3 moved #1..#4 (the 4th follower 120 - 2 = 118 < 121 could act, but F ends at chain3)
        if (laterAtChain3 == 0) {
            CHECK_MSG(r.sideDecided[0] && r.sequence.early.stop == EdgeStop::Fail, std::string("early stop ") + name(r.sequence.early.stop));
            CHECK(std::fabs(r.sequence.early.failFrames + 2.0) < 1e-9 && r.sequence.early.cause == EdgeCause::Self);
        }
        else CHECK(!r.sideDecided[0] && r.sequence.early.stop == EdgeStop::Undecided);
    }
}

// ---- v0.7.1: Fable review deltas (docs/TIMING_SOLVER_V2_FABLE.md §4) ----

/// A real local window (PassPlanner): passes |s| <= passTo on both sides, every larger shift died
/// downstream (a later fixed input applied first) at the unshifted input + deathAfter.
PassPlanner scriptedLocalWindow(double frame, double nextFrame, int passTo, double deathAfter, int laterFixed) {
    PlannerConfig pc;
    PassPlanner p(pc, kNaN);
    auto shifts = p.nextPass();
    double const lateLimit = nextFrame - frame - pc.neighbourMarginFrames;
    p.setLateLimit(lateLimit);
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = s;
        if (s > lateLimit + 1e-9) {
            o.kind = ShiftKind::NotTested;   // the engine never spawns beyond the next input (Limit)
            o.reason = "limit";
        }
        else if (std::fabs(s) <= passTo + 1e-9) o.kind = ShiftKind::Survived;
        else {
            o.kind = ShiftKind::Died;
            o.deathAfterFrames = deathAfter;
            o.laterFixed = laterFixed;
        }
        outs.push_back(o);
    }
    p.ingest(outs, true, false);
    return p;
}

void testUndecidedSideInheritsLocalEdge() {
    SECTION("Fable D1 undecidedSideInheritsLocalEdge: local late bounded +2/+3, SA undecided at +3 -> the SA late edge IS the local edge (bit-exact), stop undecided");
    double const t = 100.0;
    auto local = scriptedLocalWindow(t, 118.0, 2, 60.0, 1);
    double const actualMs = t * kTickMs;
    auto w = local.result(actualMs);
    CHECK(w.valid && w.boundedEarly && w.boundedLate);
    SAInput in;
    in.id = 1;
    in.frame = t;
    in.down = true;
    in.local = local.outcomes();
    SAPlanner p(kSA, {in}, zigzagContext(5, t, 18.0));
    // late: every family member at +3 dies after a FIXED later input (downstream) -> undecided;
    // early: the pair re-joins everywhere -> decided at the range
    drive(p, [&](SATrial const& tr) {
        SAOutcome o;
        if (tr.shiftFrames < 0) {
            o.kind = SAOutcome::Kind::Pass;
            o.rejoined = true;
            return o;
        }
        o.kind = SAOutcome::Kind::Died;
        o.deathFrame = t + 60.0;
        o.laterFixed = 1;
        return o;
    });
    CHECK(p.done());
    auto r = p.result(0);
    CHECK(r.sequence.late.stop == EdgeStop::Undecided && !r.sideDecided[1] && !r.decided);
    CHECK(r.sequence.late.inherited && r.sequence.late.hasBracket() && !r.sequence.late.bounded());
    CHECK(r.sequence.late.passFrames == 2.0 && r.sequence.late.failFrames == 3.0);
    CHECK(r.sequence.early.stop == EdgeStop::Range && !r.sequence.early.inherited);
    // the payload: sequence.latestMs == local.latestMs bit for bit, the late side undecided with the
    // local failMs and no cause; the result validates (the 4-rule of D1) and is sequence_dependent
    LocalEvidence ev;
    ev.window = &w;
    ev.outcomes = &local.outcomes();
    ev.frame = t;
    ev.nextFrame = 118.0;
    ev.nextFollows = true;
    ev.horizonFrame = t + 130.0;
    auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
    CHECK(st.status == status::TimingStatus::SequenceDependent);
    TimingResultContext ctx;
    ctx.inputSeq = 4;
    ctx.kind = InputKind::Press;
    ctx.eventT = actualMs / 1000.0;
    ctx.gamemode = Gamemode::Wave;
    ctx.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
    auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
    CHECK_MSG(built.ok, built.error);
    auto const& sq = built.payload.sequence->window;
    auto const& lw = *built.payload.local;
    CHECK(sq.latestMs == lw.latestMs);
    CHECK(sq.late.stop == "undecided" && !sq.late.cause && sq.late.failMs && *sq.late.failMs == *lw.late.failMs && sq.late.passMs == lw.late.passMs);
    CHECK(sq.earliestMs < lw.earliestMs);
    CHECK(!built.payload.sequence->decided);
    // without the inheritance the side would report its last pass, INSIDE the local midpoint (W1)
    CHECK(actualMs + sq.late.passMs < lw.latestMs);
}

/// Scripted pull oracle (DEV FIXTURE, not GD) for the D2 look-ahead: the schedule press 100 /
/// release 130 / press 160 / release 190 / press 400. Moving input 0 by |s| <= 2 is harmless; a
/// larger shift dies at frame 195 unless ALL four inputs 100..190 follow it (chain3), which is
/// then clean up to a hazard 100 frames after the last follower (frame 290) that it only meets
/// when its look-ahead reaches that far.
class HazardOracle : public IPhysicsOracle {
public:
    explicit HazardOracle(InputSchedule ref) : m_ref(std::move(ref)) {}
    Outcome trial(SnapshotId, InputSchedule const& s, double horizonSeconds) override {
        ++m_trials;
        std::vector<size_t> moved;
        double first = 1e300;
        for (size_t i = 0; i < s.inputs.size() && i < m_ref.inputs.size(); ++i) {
            if (std::fabs(s.inputs[i].tMs - m_ref.inputs[i].tMs) <= 1e-9) continue;
            moved.push_back(i);
            first = std::min({first, s.inputs[i].tMs, m_ref.inputs[i].tMs});
        }
        if (moved.empty() || moved.front() != 0) return Outcome::survived();
        double shift = (s.inputs[0].tMs - m_ref.inputs[0].tMs) / T;
        if (std::fabs(shift) <= 2.0 + 1e-9) return Outcome::survived();
        if (moved.size() < 4) return Outcome::died(195.0 * T, 7);
        double end = first / T + horizonSeconds * 240.0;   // the trial runs to its look-ahead
        if (end >= kHazard - 1e-9) {
            ++m_hazardDeaths;
            return Outcome::died(kHazard * T, 9);
        }
        return Outcome::survived();
    }
    double historyStartMs(SnapshotId) const override { return 0.0; }
    std::string name() const override { return "hazard (DEV FIXTURE)"; }
    static constexpr double kHazard = 290.0;
    int m_trials = 0;
    int m_hazardDeaths = 0;

private:
    InputSchedule m_ref;
};

void testLookAheadFromLastFollower() {
    SECTION("Fable D2 lookAheadFromLastFollower: a hazard 100 frames after the chain3's last follower (+90 ticks) kills the trial (died, not survived)");
    auto s = schedule({{100, true}, {130, false}, {160, true}, {190, false}, {400, true}});
    for (int variant = 0; variant < 2; ++variant) {
        SAConfig cfg = kSA;
        cfg.horizonFromLastMoved = variant == 0;   // variant 1 = Opus's t + s + horizon (the defect)
        HazardOracle o(s);
        PlannerConfig pc;
        PassPlanner local(pc, kNaN);
        local.setLateLimit(30.0 - pc.neighbourMarginFrames);
        runAgainstOracle(o, local, 0, s, 0, kH);
        auto lw = local.result(s.inputs[0].tMs);
        CHECK(lw.valid && lw.boundedEarly && lw.boundedLate);
        CHECK(lw.late.edge.cause == EdgeCause::Downstream && lw.late.edge.laterInputs == 3);
        SARunInput in;
        in.config = cfg;
        in.members = {0};
        in.locals = {&local};
        in.pairWalk = {false};
        SAPlanner sa;
        runSAAgainstOracle(o, 0, s, in, sa);
        CHECK(sa.done());
        auto r = sa.result(0);
        std::printf("  %s: SA [%+.1f,%+.1f] %s, hazard deaths %d | %s\n", variant == 0 ? "D2 (last moved)" : "Opus (t + s)", r.sequence.early.edgeFrames(),
                    r.sequence.late.edgeFrames(), r.decided ? "decided" : "undecided", o.m_hazardDeaths, r.debug.empty() ? "" : r.debug[0].c_str());
        if (variant == 0) {
            // the chain3 trials at +-3 reach frame 290 and die there (inside their adapted span,
            // no fixed later input before it): the SA sides end at +-3, equal to the local edges
            CHECK(o.m_hazardDeaths == 2);
            CHECK(r.decided && r.sequence.early.stop == EdgeStop::Fail && r.sequence.late.stop == EdgeStop::Fail);
            CHECK(r.sequence.late.passFrames == 2.0 && r.sequence.late.failFrames == 3.0);
            CHECK(r.sequence.early.passFrames == -2.0 && r.sequence.early.failFrames == -3.0);
            CHECK(r.sequence.late.cause == EdgeCause::Self);
            // the job's look-ahead bound covers a chain3 trial one horizon after its last moved input
            CHECK(sa.lookAheadBound() >= 190.0 + 10.0 + 130.0 - 1e-9);
        }
        else {
            // Opus's look-ahead ends at t + s + 130 < 290: the chain3 passes as "survived" - the
            // widened window rests on ~40 frames of evidence after the last moved input
            CHECK(o.m_hazardDeaths == 0);
            CHECK(r.sequence.late.passFrames >= 3.0 && r.sequence.early.passFrames <= -3.0);
        }
    }
}

World shipWorld(double ceilingY, double floorBandTop, double floorBandFromTick);   // below (cube, ship, CBF, portals)

void testProofKinds() {
    SECTION("Fable D3b proofKinds: wave pair -> rejoined, ship chain -> survived, provenPassMs stops at the last rejoin");
    // 1. the kinematic wave zigzag: a pair shift re-joins the recorded path exactly (§1.3)
    {
        auto s = schedule({{100, true}, {118, false}, {136, true}});
        World w = waveWorld();
        auto path = freePath(w, s, 300);
        corridorAround(w, path, [](double x) { double k = tickAt(x); return (k >= 205 && k < 208) ? 6.0 : 30.0; },
                       [](double x) { double k = tickAt(x); return (k >= 205 && k < 208) ? 6.0 : 30.0; });
        KinematicOracle o(w);
        auto sv = solveAll(o, s, {0, 1, 2});
        auto r = sv.sa.result(0);
        CHECK(r.sequence.late.passFrames > localEdgeTicks(sv.locals[0].result(s.inputs[0].tMs).late));
        CHECK_MSG(r.sequence.early.proof == SAProof::Rejoined && r.sequence.late.proof == SAProof::Rejoined,
                  std::string("wave proofs ") + name(r.sequence.early.proof) + "/" + name(r.sequence.late.proof));
        CHECK(r.sequence.late.provenPassFrames == r.sequence.late.passFrames && !r.survivedOnly);
    }
    // 2. the kinematic ship (the holdDuration world below): it rests on a solid floor, holds 45
    // ticks under a ceiling band 4 units above its peak, then glides. Moving the press alone
    // changes the hold (the ceiling); moving the pair keeps it - the same climb, earlier or later.
    // With inertia and no landing inside the look-ahead the pair's path never meets the recorded
    // one again: it can only SURVIVE, never re-join (§1.3)
    {
        auto s = schedule({{100, true}, {145, false}});
        World base = shipWorld(1e6, -1e6, 1e6);
        base.shipGravity = 0.03;
        KinematicOracle freeRun(base);
        double peak = -1e9;
        for (auto const& st : freeRun.run(s, 420)) peak = std::max(peak, st.y);
        World w = shipWorld(peak + 3.0 + 4.0, 6.0, 150.0);
        w.shipGravity = 0.03;
        KinematicOracle o(w);
        auto sv = solveAll(o, s, {0, 1});
        auto r = sv.sa.result(0);
        auto lw = sv.locals[0].result(s.inputs[0].tMs);
        std::printf("  ship press: local [%+.1f,%+.1f] | SA [%+.1f,%+.1f] proofs %s/%s proven %+.1f/%+.1f | %s\n", localEdgeTicks(lw.early), localEdgeTicks(lw.late),
                    r.sequence.early.edgeFrames(), r.sequence.late.edgeFrames(), name(r.sequence.early.proof), name(r.sequence.late.proof),
                    r.sequence.early.provenPassFrames, r.sequence.late.provenPassFrames, r.debug.empty() ? "" : r.debug[0].c_str());
        CHECK(lw.boundedEarly);
        CHECK(r.sequence.early.passFrames < lw.early.passShiftMs / T - 1.0 + 1e-9);   // widened beyond the local edge
        CHECK_MSG(r.sequence.early.proof == SAProof::Survived, std::string("ship early proof ") + name(r.sequence.early.proof));
        // nothing beyond the local edge was proven by a re-join: the proven pass is the local one
        CHECK(r.sequence.early.provenPassFrames == lw.early.passShiftMs / T);
        CHECK(r.survivedOnly);
    }
    // 3. scripted: late +3 pair re-joins, +4 chain2 survives, +5 pair re-joins, +6 dies inside the
    // adapted span -> proof survived (the weakest widening), provenPassMs +3 (the last re-join
    // contiguous from the local edge +2), passMs +5
    {
        auto local = scriptedLocal(2, 60.0, 1);
        SAPlanner p(kSA, {scriptedInput(1, 100.0, true, local)}, zigzagContext(6, 100.0, 18.0));
        drive(p, [&](SATrial const& t) {
            SAOutcome o;
            double s = t.shiftFrames;
            if (s < 0) {   // early: every pair re-joins (range)
                o.kind = SAOutcome::Kind::Pass;
                o.rejoined = true;
                return o;
            }
            if (std::fabs(s - 3.0) < 1e-9 || std::fabs(s - 5.0) < 1e-9) {
                o.kind = SAOutcome::Kind::Pass;
                o.rejoined = true;
                return o;
            }
            if (std::fabs(s - 4.0) < 1e-9) {
                if (t.adaptation == SAAdaptation::Chain2) {
                    o.kind = SAOutcome::Kind::Pass;   // alive at its look-ahead, never re-joined
                    return o;
                }
                o.kind = SAOutcome::Kind::Died;       // the pair dies after a fixed later input: escalate
                o.deathFrame = 160.0;
                o.laterFixed = 1;
                return o;
            }
            o.kind = SAOutcome::Kind::Died;
            o.deathFrame = t.moved.front().frame + 1.0;   // inside the adapted span: decided
            o.laterFixed = 0;
            return o;
        });
        auto r = p.result(0);
        CHECK(r.sequence.late.passFrames == 5.0 && r.sequence.late.stop == EdgeStop::Fail);
        CHECK(r.sequence.late.proof == SAProof::Survived && r.sequence.late.provenPassFrames == 3.0);
        CHECK(r.sequence.early.proof == SAProof::Rejoined && r.sequence.early.provenPassFrames == r.sequence.early.passFrames);
        CHECK(r.survivedOnly);
        auto st = status::statusOf({}, status::LocalFacts{true}, saFacts(&r), {});
        CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::SaSurvivedOnly) != st.reasons.end());
        // the payload: proof + provenPassMs on the sequence edges, consistent for the gate mirror
        auto w = scriptedLocalWindow(100.0, 118.0, 2, 60.0, 1).result(100.0 * kTickMs);
        auto sq = saWindowPayload(r.sequence, 100.0 * kTickMs, false, true);
        CHECK(sq.late.proof == std::optional<std::string>("survived") && sq.late.provenPassMs && *sq.late.provenPassMs == 3.0 * kTickMs);
        CHECK(sq.early.proof == std::optional<std::string>("rejoined") && sq.early.provenPassMs == sq.early.passMs);
        telemetry::TimingResultPayload tp;
        tp.local = localWindowPayload(w, 100.0 * kTickMs, false);
        tp.sequence = telemetry::SequenceWindowV2Payload{sq, "gprl-clone-sa/4", true, {"pair", "chain2"}, 0, std::nullopt};
        CHECK(proofConsistent(tp));
        // a survived side without its proven pass, or a proven pass beyond the SA pass, is not
        tp.sequence->window.late.provenPassMs.reset();
        CHECK(!proofConsistent(tp));
        tp.sequence->window.late.provenPassMs = 6.0 * kTickMs;
        CHECK(!proofConsistent(tp));
    }
}

/// MOD verifier (v0.7.1): the SA walk runs the LOCAL member itself (k = 0) at a shift the local
/// planner left untested (here the late side's +6..+10 were cut by the pool, so the local side is
/// open at +5 with no fail). Such a pass lies BEYOND the local window and comes from an SA trial:
/// Fable §3.1's table gives SA trials the proofs `rejoined` / `survived` only (`local` = the lockstep
/// local job, inside the local window). Labelled `local` it made a side that widened carry `local`,
/// which the evidence gate (api proofConsistent + its C++ mirror) refuses as `proof_inconsistent`
/// ("a `local` side never widened") - a valid measurement stored unusable.
void testLocalTrialBeyondLocalRunProof() {
    SECTION("D3b: an SA-run local trial beyond the local run (pool-cut shifts) is proven by its own outcome (rejoined / survived), never `local`; the gate mirror accepts the payload");
    double const t = 100.0;
    double const next = 140.0;
    for (int variant = 0; variant < 2; ++variant) {
        bool const rejoin = variant == 1;
        PlannerConfig pc;
        PassPlanner local(pc, kNaN);
        auto shifts = local.nextPass();
        local.setLateLimit(next - t - pc.neighbourMarginFrames);
        std::vector<ShiftOutcome> outs;
        for (double s : shifts) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = s;
            if (s > 5.0 + 1e-9) {
                o.kind = ShiftKind::NotTested;   // the pool ran out before these clones could start
                o.reason = "pool";
            }
            else if (s >= -2.0 - 1e-9) o.kind = ShiftKind::Survived;
            else {
                o.kind = ShiftKind::Died;        // a self death before the next input could act
                o.deathAfterFrames = 5.0;
                o.laterFixed = 0;
            }
            outs.push_back(o);
        }
        local.ingest(outs, true, false);
        double const actualMs = t * kTickMs;
        auto w = local.result(actualMs);
        CHECK(w.valid && w.boundedEarly && !w.boundedLate);
        CHECK(std::fabs(w.late.passShiftMs / T - 5.0) < 1e-9);
        SAInput in;
        in.id = 1;
        in.frame = t;
        in.down = true;
        in.local = local.outcomes();
        SAPlanner p(kSA, {in}, zigzagContext(3, t, next - t));
        std::vector<int> ks;
        drive(p, [&](SATrial const& tr) {
            ks.push_back(static_cast<int>(tr.adaptation));
            SAOutcome o;
            o.kind = SAOutcome::Kind::Pass;
            o.rejoined = rejoin;
            return o;
        });
        CHECK(p.done() && !ks.empty());
        for (int k : ks) CHECK(k == 0);   // only the local member itself was run (at +6..+10)
        auto r = p.result(0);
        CHECK(std::fabs(r.sequence.late.passFrames - 10.0) < 1e-9 && r.sequence.late.stop == EdgeStop::Range);
        CHECK(r.adaptationUsed.empty());
        CHECK_MSG(r.sequence.late.proof == (rejoin ? SAProof::Rejoined : SAProof::Survived), std::string("late proof ") + name(r.sequence.late.proof));
        CHECK(std::fabs(r.sequence.late.provenPassFrames - (rejoin ? 10.0 : 5.0)) < 1e-9);
        CHECK(r.survivedOnly == !rejoin);
        CHECK(r.sequence.early.proof == SAProof::Local && r.sequence.early.bounded());
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &local.outcomes();
        ev.frame = t;
        ev.nextFrame = next;
        ev.nextFollows = true;
        ev.horizonFrame = t + 130.0;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
        // the pool-cut local side is a cut pass (untested_gap_in_bracket): low_confidence either way
        CHECK(st.status == status::TimingStatus::LowConfidence);
        CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::UntestedGapInBracket) != st.reasons.end());
        CHECK((std::find(st.reasons.begin(), st.reasons.end(), status::Reason::SaSurvivedOnly) != st.reasons.end()) == !rejoin);
        TimingResultContext ctx;
        ctx.inputSeq = 4;
        ctx.kind = InputKind::Press;
        ctx.eventT = actualMs / 1000.0;
        ctx.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
        auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
        CHECK_MSG(built.ok, built.error);
        auto const& sl = built.payload.sequence->window.late;
        CHECK(sl.proof == std::optional<std::string>(rejoin ? "rejoined" : "survived"));
        CHECK(proofConsistent(built.payload));
        auto gate = checkTimingResultPayload(built.payload, ctx.eventT, 0.0);
        std::string why;
        for (auto const& s : gate.reasons) why += s + " ";
        CHECK_MSG(gate.accepted, "gate mirror refused: " + why);
    }
}

/// MOD verifier (v0.7.1): Fable D1's inherited edge must be the LOCAL WINDOW's bracket. The engine
/// spawns pass 0 before the next input is logged; a late clone whose shifted input lies beyond the
/// neighbour limit that arrives later and that died BEFORE that input was logged stays `Died` (only
/// Running clones become Limit, CloneEngine::applyLateNeighbour). PassPlanner ignores it (beyond its
/// late limit: the local side ends at `neighbour`, unbounded); the SA planner read the local run up
/// to maxShift and saw a bracket [+4, +5]. With the next inputs too close for any family member to
/// follow, the SA side stops at `neighbour` at the local pass and used to INHERIT failMs +5 that the
/// local window does not have: the validator refused the result (fallback unresolved
/// payload_invalid) - a valid local window lost its timing_result.
void testInheritOnlyTheLocalWindowsBracket() {
    SECTION("D1: an SA side never inherits a bracket the local window does not have (a death beyond the late neighbour limit)");
    double const t = 100.0;
    for (int variant = 0; variant < 2; ++variant) {
        bool const crowded = variant == 0;   // 0: no family member can follow at +5 (neighbour); 1: the pair can
        PlannerConfig pc;
        PassPlanner local(pc, kNaN);
        auto shifts = local.nextPass();   // pass 0 planned before the next input is logged (late limit pending)
        std::vector<ShiftOutcome> outs;
        for (double s : shifts) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = s;
            if (s < 0 || s <= 4.0 + 1e-9) o.kind = ShiftKind::Survived;
            else {
                o.kind = ShiftKind::Died;   // died at the end of the step [t+4, t+5): before its own input, before the next one was logged
                o.deathAfterFrames = 5.0;
                o.laterFixed = 0;
            }
            outs.push_back(o);
        }
        local.setLateLimit(5.0 - pc.neighbourMarginFrames);   // the release at t+5 is logged
        local.ingest(outs, true, false);
        double const actualMs = t * kTickMs;
        auto w = local.result(actualMs);
        CHECK(w.valid && !w.boundedLate && w.late.edge.stop == EdgeStop::Neighbour);
        CHECK(std::fabs(w.late.passShiftMs / T - 4.0) < 1e-9);
        SAContext ctx;
        std::vector<double> frames = crowded ? std::vector<double>{100, 105, 106, 107, 108} : std::vector<double>{100, 105, 140};
        for (size_t i = 0; i < frames.size(); ++i) ctx.inputs.push_back({static_cast<uint32_t>(i + 1), frames[i], i % 2 == 0, false});
        SAInput in;
        in.id = 1;
        in.frame = t;
        in.down = true;
        in.earlyLimitFrames = kNaN;
        in.earlyLimitKind = LimitKind::History;
        in.lateLimitFrames = local.late().limit;   // what the engine / runSAAgainstOracle hand over
        in.local = local.outcomes();
        SAPlanner p(kSA, {in}, ctx);
        drive(p, [&](SATrial const&) {
            SAOutcome o;
            o.kind = SAOutcome::Kind::Pass;
            o.rejoined = true;
            return o;
        });
        CHECK(p.done());
        auto r = p.result(0);
        if (crowded) {
            CHECK_MSG(r.sequence.late.stop == EdgeStop::Neighbour, std::string("late stop ") + name(r.sequence.late.stop));
            CHECK(!r.sequence.late.inherited && std::isnan(r.sequence.late.failFrames));
            CHECK(std::fabs(r.sequence.late.edgeFrames() - 4.0) < 1e-9);   // == the local edge
        }
        else {
            // the pair can follow: the SA walk decides +5 itself (the death came before any input
            // a longer member could move: fail, no simulation) - its own bracket, never inherited
            CHECK(r.sequence.late.bounded() && !r.sequence.late.inherited);
            CHECK(std::fabs(r.sequence.late.passFrames - 4.0) < 1e-9 && std::fabs(r.sequence.late.failFrames - 5.0) < 1e-9);
        }
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &local.outcomes();
        ev.frame = t;
        ev.nextFrame = 105.0;
        ev.nextFollows = true;
        ev.horizonFrame = t + 130.0;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
        TimingResultContext tc;
        tc.inputSeq = 4;
        tc.kind = InputKind::Press;
        tc.eventT = actualMs / 1000.0;
        tc.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
        auto built = buildTimingResultEvent(tc, ev, &r, st, true);
        CHECK_MSG(built.ok, built.error);
        auto gate = checkTimingResultPayload(built.payload, tc.eventT, 0.0);
        std::string why;
        for (auto const& s : gate.reasons) why += s + " ";
        CHECK_MSG(gate.accepted, "gate mirror refused: " + why);
    }
}

/// MOD verifier (v0.7.1): randomized scripted local windows (pass runs, pool cuts, self /
/// downstream deaths, passing islands, whole ticks and CBF refinement, neighbour limits) and
/// scripted SA outcomes (re-joins, survivals, deaths, invalid trials). Every result must build a
/// payload the validator accepts AND the evidence gate's mirror accepts (W_local ⊆ W_SA on pass and
/// reported edges - Fable D1; proof consistency - D3b), and an undecided sequence window never
/// leaves the status `ok` (fail-safe statuses). Scripted DEV FIXTURE outcomes, not GD.
void testFuzzGateConsistency() {
    SECTION("fuzz (2000 scripted cases): every SA result validates and passes the gate mirror (D1 containment, D3b proof); undecided is never ok");
    std::mt19937 rng(0x5eed0731u);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int cases = 0, payloadFail = 0, gateFail = 0, okUndecided = 0, widened = 0, inherited = 0, openRange = 0, memberAlone = 0, beyondLimit = 0;
    for (int n = 0; n < 2000; ++n) {
        double const t = 100.0;
        double const gap = 4.0 + std::floor(u(rng) * 40.0);             // the next input 4..43 ticks later
        bool const subtick = u(rng) < 0.25;
        double const rE = u(rng) < 0.15 ? 10.5 : u(rng) * 11.0;         // pass radius per side (|s| <= r passes)
        double const rL = u(rng) < 0.15 ? 10.5 : u(rng) * 11.0;
        double const pPool = u(rng) < 0.5 ? 0.0 : 0.25;                 // NotTested (pool) rate
        double const pIsland = u(rng) < 0.7 ? 0.0 : 0.3;                // a pass beyond the radius
        double const earlyLimit = u(rng) < 0.5 ? kNaN : 2.0 + std::floor(u(rng) * 9.0) - kMargin;
        PlannerConfig pc;
        pc.subtick = subtick;
        PassPlanner local(pc, earlyLimit);
        local.setEarlyLimitKind(LimitKind::Neighbour);
        // the engine plans pass 0 when the input happens and learns the late neighbour limit when the
        // next input is logged (applyLateNeighbour): half the cases set it after pass 0 is planned
        bool const lateLimitAfterPass0 = u(rng) < 0.5;
        double const lateLimit = gap - pc.neighbourMarginFrames;
        // ... and a hazard that kills every input not applied by the next input's frame: the clones
        // beyond the limit died at t + gap, BEFORE that input was logged - they stay Died
        bool const hazardAtNext = lateLimitAfterPass0 && gap <= 10.0 && u(rng) < 0.5;
        if (!lateLimitAfterPass0) local.setLateLimit(lateLimit);
        auto outcomeAt = [&](double s) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = s;
            double const r = s < 0 ? rE : rL;
            if (u(rng) < pPool) {
                o.kind = ShiftKind::NotTested;
                o.reason = "pool";
            }
            else if (std::fabs(s) <= r || u(rng) < pIsland) o.kind = u(rng) < 0.5 ? ShiftKind::Resynced : ShiftKind::Survived;
            else {
                o.kind = ShiftKind::Died;
                o.deathAfterFrames = std::max(0.0, s) + 1.0 + std::floor(u(rng) * 60.0);
                o.laterFixed = static_cast<int>(u(rng) * 3.0);
                o.objectId = 7;
            }
            if (o.kind == ShiftKind::Resynced) o.deathAfterFrames = o.rejoinAfterFrames = std::max(0.0, s) + 20.0;
            return o;
        };
        for (int pass = 0; pass < 4 && !local.done(); ++pass) {
            auto shifts = local.nextPass();
            if (shifts.empty()) break;
            std::vector<ShiftOutcome> outs;
            for (double s : shifts) {
                auto o = outcomeAt(s);
                if (pass == 0 && hazardAtNext && s > lateLimit + 1e-9) {
                    o.kind = ShiftKind::Died;
                    o.reason.clear();
                    o.deathAfterFrames = gap;
                    o.laterFixed = 0;
                    o.objectId = 7;
                }
                if (pass == 0 && lateLimitAfterPass0 && s > lateLimit + 1e-9 && !(o.kind == ShiftKind::Died && o.deathAfterFrames <= gap)) {
                    // still running when the next input was logged: Limit (only a clone that died
                    // before that stays Died, beyond the limit)
                    o.kind = ShiftKind::NotTested;
                    o.reason = "limit";
                }
                outs.push_back(o);
            }
            if (pass == 0 && lateLimitAfterPass0) local.setLateLimit(lateLimit);
            local.ingest(outs, true, false);
        }
        double const actualMs = t * kTickMs;
        auto w = local.result(actualMs);
        if (!w.valid) continue;
        ++cases;
        SAInput in;
        in.id = 1;
        in.frame = t;
        in.down = true;
        in.subtick = subtick;
        in.earlyLimitFrames = earlyLimit;
        in.earlyLimitKind = LimitKind::Neighbour;
        in.lateLimitFrames = local.late().limit;   // as the engine hands it over (job.lateLimitFrames)
        in.local = local.outcomes();
        for (auto const& o : local.outcomes()) {
            if (o.appliedFrames > lateLimit + 1e-9 && o.kind == ShiftKind::Died) {
                ++beyondLimit;
                break;
            }
        }
        SAPlanner p(kSA, {in}, zigzagContext(5, t, gap));
        drive(p, [&](SATrial const& tr) {
            SAOutcome o;
            double const x = u(rng);
            if (tr.adaptation == SAAdaptation::Local) ++memberAlone;
            if (x < 0.45) {
                o.kind = SAOutcome::Kind::Pass;
                o.rejoined = u(rng) < 0.5;
            }
            else if (x < 0.92) {
                o.kind = SAOutcome::Kind::Died;
                o.deathFrame = tr.moved.front().frame + 1.0 + std::floor(u(rng) * 80.0);
                o.laterFixed = static_cast<int>(u(rng) * 3.0);
                o.objectId = 7;
            }
            else {
                o.kind = SAOutcome::Kind::Invalid;
                o.reason = "teleport portal";
            }
            return o;
        });
        CHECK(p.done());
        auto r = p.result(0);
        if (r.sequence.early.inherited || r.sequence.late.inherited) ++inherited;
        if (r.openRange) ++openRange;
        if (r.sequence.late.passFrames > w.late.passShiftMs / T + 1e-9 || r.sequence.early.passFrames < w.early.passShiftMs / T - 1e-9) ++widened;
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &local.outcomes();
        ev.frame = t;
        ev.nextFrame = t + gap;
        ev.nextFollows = true;
        ev.horizonFrame = t + 130.0;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
        if (st.status == status::TimingStatus::Ok && !r.decided) ++okUndecided;
        TimingResultContext ctx;
        ctx.inputSeq = 4;
        ctx.kind = InputKind::Press;
        ctx.eventT = actualMs / 1000.0;
        ctx.subtick = subtick;
        ctx.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
        auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
        if (!built.ok) {
            ++payloadFail;
            if (payloadFail <= 3) std::printf("  case %d payload: %s | %s\n", n, built.error.c_str(), r.debug.empty() ? "" : r.debug[0].c_str());
            continue;
        }
        auto gate = checkTimingResultPayload(built.payload, ctx.eventT, 0.0);
        if (!gate.accepted) {
            ++gateFail;
            std::string why;
            for (auto const& s : gate.reasons) why += s + " ";
            if (gateFail <= 3) std::printf("  case %d gate mirror: %s| %s | local %s\n", n, why.c_str(), r.debug.empty() ? "" : r.debug[0].c_str(), local.describe().c_str());
        }
    }
    std::printf("  %d cases (%d widened, %d inherited edges, %d open_range, %d SA trials of the member alone, %d with a death beyond the late limit): "
                "payload failures %d, gate-mirror refusals %d, ok while undecided %d\n",
                cases, widened, inherited, openRange, memberAlone, beyondLimit, payloadFail, gateFail, okUndecided);
    CHECK(cases >= 1000);
    CHECK(widened > 50 && inherited > 50 && openRange > 5 && memberAlone > 20 && beyondLimit > 20);   // the property is not vacuous
    CHECK(payloadFail == 0);
    CHECK(gateFail == 0);
    CHECK(okUndecided == 0);
}

void testOpenRange() {
    SECTION("Fable D3a: both local sides open to the range -> no SA trial (not even the pair walk), sequence = local copy, decided, ok (open_range)");
    double const t = 100.0;
    auto local = scriptedLocalWindow(t, 118.0, 10, 0.0, 0);   // every shift passes: [-10, +10] open
    auto w = local.result(t * kTickMs);
    CHECK(w.valid && !w.boundedEarly && !w.boundedLate);
    SAInput in;
    in.id = 1;
    in.frame = t;
    in.down = true;
    in.local = local.outcomes();
    in.pairWalk = true;                      // a press followed by an effective release
    SAConfig withPair = kSA;
    withPair.pairWalkForPresses = true;      // even with the pair walk switched on
    SAPlanner p(withPair, {in}, zigzagContext(4, t, 18.0));
    CHECK(!p.needsTrials() && p.done() && p.memberOpenRange(0));
    auto r = p.result(0);
    CHECK(r.openRange && r.decided && r.adaptationUsed.empty() && !r.pair.present && r.trials == 0);
    CHECK(r.sequence.early.stop == EdgeStop::Range && r.sequence.late.stop == EdgeStop::Range);
    CHECK(r.sequence.early.passFrames == -10.0 && r.sequence.late.passFrames == 10.0);
    LocalEvidence ev;
    ev.window = &w;
    ev.outcomes = &local.outcomes();
    ev.frame = t;
    ev.nextFrame = 118.0;
    ev.nextFollows = true;
    ev.horizonFrame = t + 130.0;
    auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
    CHECK(st.status == status::TimingStatus::Ok);
    CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::OpenRange) != st.reasons.end());
    TimingResultContext ctx;
    ctx.inputSeq = 4;
    ctx.kind = InputKind::Press;
    ctx.eventT = t * kTickMs / 1000.0;
    ctx.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
    auto built = buildTimingResultEvent(ctx, ev, &r, st, true);
    CHECK_MSG(built.ok, built.error);
    CHECK(built.payload.sequence && built.payload.sequence->decided && built.payload.sequence->window.earliestMs == built.payload.local->earliestMs
          && built.payload.sequence->window.latestMs == built.payload.local->latestMs);
    // a late side stopped by the NEXT input is not open: the next input can follow (SA work remains)
    auto limited = scriptedLocalWindow(t, 104.0, 10, 0.0, 0);   // late limit ~4 ticks, every tested shift passes
    SAInput li = in;
    li.local = limited.outcomes();
    SAPlanner q(kSA, {li}, zigzagContext(4, t, 4.0));
    CHECK(!q.memberOpenRange(0) && q.needsTrials());
}

// ---- cube, ship, CBF, portals ----

/// Local and SA windows of every member equal the brute-force scans (local: the moved input alone;
/// SA: the family) on decided sides; returns false (and prints) on any disagreement.
bool matchesBrute(KinematicOracle& o, InputSchedule const& s, std::vector<size_t> const& members, Solved const& sv, std::vector<size_t> const& breaks = {},
                  char const* label = "") {
    bool all = true;
    for (size_t m = 0; m < members.size(); ++m) {
        size_t i = members[m];
        auto w0 = sv.locals[m].result(s.inputs[i].tMs);
        auto r = sv.sa.result(static_cast<int>(m));
        if (!w0.valid) continue;
        double limE = std::min(10.0, earlyLimitOf(s, i));
        double limL = std::isnan(lateLimitOf(s, i)) ? 10.0 : std::min(10.0, lateLimitOf(s, i));
        auto le = bruteWalk([&](double k) { return localPassAt(o, s, i, -k); }, limE);
        auto ll = bruteWalk([&](double k) { return localPassAt(o, s, i, k); }, limL);
        bool ok = std::fabs(std::fabs(w0.early.passShiftMs / T) - le.lastPass) < 1e-9 && std::fabs(w0.late.passShiftMs / T - ll.lastPass) < 1e-9;
        for (int side = 0; side < 2; ++side) {
            if (!r.sideDecided[side]) continue;
            double sign = side ? 1.0 : -1.0;
            auto bw = bruteWalk([&](double k) { return familyPassAt(o, s, i, sign * k, breaks); }, side ? 10.0 : limE);
            SAEdge const& e = side ? r.sequence.late : r.sequence.early;
            ok = ok && std::fabs(std::fabs(e.passFrames) - bw.lastPass) < 1e-9;
            if (e.stop == EdgeStop::Fail) ok = ok && !std::isnan(bw.firstFail) && std::fabs(std::fabs(e.failFrames) - bw.firstFail) < 1e-9;
        }
        std::printf("  %s #%zu %s local [%+.1f,%+.1f] brute [%+.0f,%+.0f] | SA [%+.1f,%+.1f] %s%s | %s\n", label, i, s.inputs[i].down ? "press  " : "release",
                    localEdgeTicks(w0.early), localEdgeTicks(w0.late), -le.lastPass, ll.lastPass, r.sequence.early.edgeFrames(), r.sequence.late.edgeFrames(),
                    r.decided ? "decided" : "UNDECIDED", ok ? "" : "  <-- DISAGREES", r.debug.empty() ? "" : r.debug[0].c_str());
        all = all && ok;
    }
    return all;
}

World cubeWorld() {
    World w;
    w.startMode = Mode::Cube;
    w.startY = 0.0;
    w.groundY = 0.0;
    w.half = 3.0;
    return w;
}

void testCube() {
    SECTION("cubeSymmetric: an isolated jump over a spike centred on the arc -> local = SA = brute force, symmetric, status ok (isolated)");
    {
        World w = cubeWorld();
        double apex = 1.3 * (100.0 + 28.5);
        w.spikes.push_back({apex - 5.0, 0.0, apex + 5.0, 27.3, 8});
        KinematicOracle o(w);
        auto s = schedule({{100, true}});
        auto sv = solveAll(o, s, {0});
        CHECK(matchesBrute(o, s, {0}, sv, {}, "cube"));
        auto w0 = sv.locals[0].result(s.inputs[0].tMs);
        auto r = sv.sa.result(0);
        CHECK(w0.valid && w0.boundedEarly && w0.boundedLate);
        CHECK(std::fabs(std::fabs(localEdgeTicks(w0.early)) - localEdgeTicks(w0.late)) <= 1.0 + 1e-9);   // symmetric within one tick
        CHECK(r.isolated && r.decided && r.adaptationUsed.empty());
        CHECK(std::fabs(r.sequence.early.edgeFrames() - localEdgeTicks(w0.early)) < 1e-9 && std::fabs(r.sequence.late.edgeFrames() - localEdgeTicks(w0.late)) < 1e-9);
        CHECK(w0.early.edge.cause == EdgeCause::Self && w0.late.edge.cause == EdgeCause::Self);
        LocalEvidence ev;
        ev.window = &w0;
        ev.outcomes = &sv.locals[0].outcomes();
        ev.frame = 100.0;
        ev.horizonFrame = 230.0;
        status::ContextFacts cx;
        cx.isolated = r.isolated;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), cx);
        CHECK(st.status == status::TimingStatus::Ok);
        CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::Isolated) != st.reasons.end());
    }
    SECTION("cubeAsymmetric: the spike off the arc's centre -> asymmetric edges equal the brute-force bounds");
    {
        World w = cubeWorld();
        double apex = 1.3 * (100.0 + 28.5 + 3.0);
        w.spikes.push_back({apex - 5.0, 0.0, apex + 5.0, 27.3, 8});
        KinematicOracle o(w);
        auto s = schedule({{100, true}});
        auto sv = solveAll(o, s, {0});
        CHECK(matchesBrute(o, s, {0}, sv, {}, "cube asym"));
        auto w0 = sv.locals[0].result(s.inputs[0].tMs);
        CHECK(w0.valid && std::fabs(std::fabs(localEdgeTicks(w0.early)) - localEdgeTicks(w0.late)) >= 2.0 - 1e-9);
    }
    SECTION("pressOnly: a cube tap -> the release is no_effect (all_shifts_rejoined), the press window as without it");
    {
        World w = cubeWorld();
        double apex = 1.3 * (100.0 + 28.5);
        w.spikes.push_back({apex - 5.0, 0.0, apex + 5.0, 27.3, 8});
        KinematicOracle o(w);
        auto s = schedule({{100, true}, {106, false}});
        auto sv = solveAll(o, s, {0, 1});
        CHECK(matchesBrute(o, s, {0, 1}, sv, {}, "tap"));
        auto rel = sv.locals[1].result(s.inputs[1].tMs);
        CHECK(rel.valid && !rel.boundedEarly && !rel.boundedLate);
        LocalEvidence ev;
        ev.window = &rel;
        ev.outcomes = &sv.locals[1].outcomes();
        ev.frame = 106.0;
        ev.horizonFrame = 236.0;
        auto lf = localFacts(ev);
        CHECK(lf.noEffect);
        auto st = status::statusOf({}, lf, saFacts(nullptr), {});
        CHECK(st.status == status::TimingStatus::NoEffect);
        CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::AllShiftsRejoined) != st.reasons.end());
        // the press: its release follows in the SA window (pair), which reproduces the isolated press's bounds
        auto isolatedSv = solveAll(o, schedule({{100, true}}), {0});
        auto a = sv.sa.result(0), b = isolatedSv.sa.result(0);
        CHECK(a.decided && b.decided);
        CHECK(std::fabs(a.sequence.early.edgeFrames() - b.sequence.early.edgeFrames()) < 1e-9);
        CHECK(std::fabs(a.sequence.late.edgeFrames() - b.sequence.late.edgeFrames()) < 1e-9);
    }
}

World shipWorld(double ceilingY, double floorBandTop, double floorBandFromTick) {
    World w;
    w.startMode = Mode::Ship;
    w.startY = 0.0;
    w.half = 3.0;
    w.floor.pts = {{-100.0, -3.0}, {5000.0, -3.0}};   // solid: the ship rests on it
    w.spikes.push_back({-100.0, ceilingY, 5000.0, ceilingY + 50.0, 9});   // a ceiling spike band everywhere
    w.spikes.push_back({1.3 * floorBandFromTick, -100.0, 5000.0, floorBandTop, 10});   // a floor spike band after take-off
    return w;
}

void testShipHold() {
    SECTION("releaseOnly + holdDuration: only the hold length matters -> press local narrow, press PAIR window wide, release window = hold window");
    // DEV FIXTURE ship (slow fall): rests on a solid floor, holds 45 ticks, then glides down. The
    // hold decides the peak (a ceiling spike band 4 units above it) and how long the ship stays up
    // (a floor spike band after take-off it must not reach within the release's look-ahead)
    auto s = schedule({{100, true}, {145, false}});
    World base = shipWorld(1e6, -1e6, 1e6);
    base.shipGravity = 0.03;
    KinematicOracle freeRun(base);
    double peak = -1e9;
    for (auto const& st : freeRun.run(s, 420)) peak = std::max(peak, st.y);
    World w = shipWorld(peak + 3.0 + 4.0, 6.0, 150.0);
    w.shipGravity = 0.03;
    KinematicOracle o(w);
    // the PAIR window of the press is what this test is about: the full pair walk (off by
    // default since Fable D9) is switched on; the default run below needs no pair walk
    auto sv = solveAll(o, s, {0, 1}, {}, false, pairWalkOn());
    CHECK(matchesBrute(o, s, {0, 1}, sv, {}, "ship hold"));
    auto p = sv.locals[0].result(s.inputs[0].tMs);
    auto rl = sv.locals[1].result(s.inputs[1].tMs);
    auto pa = sv.sa.result(0);
    auto ra = sv.sa.result(1);
    CHECK(p.valid && rl.valid);
    CHECK(pa.pair.present);
    {
        // T-BUDGET (Fable D9): the default config runs no pair walk - fewer trials, the same SA
        // window, and `pair` only when the SA walk's own pair trials happened to cover it
        auto dv = solveAll(o, s, {0, 1});
        auto da = dv.sa.result(0);
        std::printf("  pair walk on: %d SA trials, off (default): %d\n", sv.saTrials, dv.saTrials);
        CHECK(dv.saTrials < sv.saTrials);
        CHECK(da.decided == pa.decided && da.sequence.early.edgeFrames() == pa.sequence.early.edgeFrames()
              && da.sequence.late.edgeFrames() == pa.sequence.late.edgeFrames());
        if (da.pair.present) CHECK(da.pair.late.edgeFrames() == pa.pair.late.edgeFrames() && da.pair.early.edgeFrames() == pa.pair.early.edgeFrames());
    }
    double pairWidth = pa.pair.late.edgeFrames() - pa.pair.early.edgeFrames();
    std::printf("  press local %.2f f, press pair %.2f f [%+.1f,%+.1f], press SA %.2f f, release local %.2f f, release SA %.2f f\n", localWidthTicks(p), pairWidth,
                pa.pair.early.edgeFrames(), pa.pair.late.edgeFrames(), widthTicks(pa.sequence), localWidthTicks(rl), widthTicks(ra.sequence));
    CHECK(pairWidth >= localWidthTicks(p) + 4.0 - 1e-9);         // no double narrow: the press carries the start, free duration
    CHECK(widthTicks(pa.sequence) >= pairWidth - 1e-9);          // the press's SA window contains its pair walk
    CHECK(ra.isolated && ra.decided);
    CHECK(rl.boundedEarly && rl.boundedLate);
    // the release's window as a hold duration (press fixed): [t_R + e - t_P, t_R + l - t_P], equal to
    // the brute-force range of hold lengths the ship survives (the release's own look-ahead)
    double holdMin = 45.0 + ra.sequence.early.passFrames, holdMax = 45.0 + ra.sequence.late.passFrames;
    int bruteMin = 99, bruteMax = -1;
    for (int h = 35; h <= 55; ++h) {
        if (!localPassAt(o, s, 1, static_cast<double>(h - 45))) continue;
        bruteMin = std::min(bruteMin, h);
        bruteMax = std::max(bruteMax, h);
    }
    std::printf("  hold window [%.0f, %.0f] ticks (conservative), brute force [%d, %d]\n", holdMin, holdMax, bruteMin, bruteMax);
    CHECK(std::fabs(holdMin - bruteMin) < 1e-9 && std::fabs(holdMax - bruteMax) < 1e-9);
    LocalEvidence ev;
    ev.window = &rl;
    ev.outcomes = &sv.locals[1].outcomes();
    ev.frame = 145.0;
    ev.horizonFrame = 275.0;
    TimingResultContext ctx;
    ctx.inputSeq = 5;
    ctx.kind = InputKind::Release;
    ctx.eventT = s.inputs[1].tMs / 1000.0;
    ctx.gamemode = Gamemode::Ship;
    ctx.pressMs = s.inputs[0].tMs;
    ctx.pressSeq = 4;
    ctx.cluster = {"a1:1", 2, cluster::Tri::Yes, cluster::Tri::No};
    auto st = status::statusOf({}, localFacts(ev), saFacts(&ra), {});
    auto built = buildTimingResultEvent(ctx, ev, &ra, st, true);
    CHECK_MSG(built.ok, built.error);
    CHECK(built.payload.hold.has_value());
    if (built.payload.hold) {
        CHECK(built.payload.hold->basis == "sequence");
        CHECK(built.payload.hold->minMs <= built.payload.hold->localMinMs + 1e-9 && built.payload.hold->maxMs >= built.payload.hold->localMaxMs - 1e-9);
    }
}

void testShipCorrections() {
    SECTION("shipCorrections: corrections every 12 ticks and a far tight slot -> chain members tried, sequence_dependent (sa_undecided), never guessed");
    // DEV FIXTURE: eight ship corrections 12 ticks apart in open air, then a narrow slot 16 ticks
    // after the last one (inside every input's look-ahead): chain3 cannot move every later input
    auto s = schedule({{100, true}, {112, false}, {124, true}, {136, false}, {148, true}, {160, false}, {172, true}, {184, false}});
    World w;
    w.startMode = Mode::Ship;
    w.startY = 40.0;
    w.half = 3.0;
    auto path = freePath(w, s, 360);
    double yAt = 0.0;
    for (auto const& st : path) if (st.tick == 200) yAt = st.y;
    w.spikes.push_back({1.3 * 199.0, yAt + 3.0 + 3.0, 1.3 * 202.0, yAt + 300.0, 11});
    w.spikes.push_back({1.3 * 199.0, yAt - 300.0, 1.3 * 202.0, yAt - 3.0 - 3.0, 12});
    KinematicOracle o(w);
    std::vector<size_t> members = {0, 1, 2, 3, 4, 5, 6, 7};
    auto sv = solveAll(o, s, members);
    CHECK(sv.sa.done());
    CHECK(matchesBrute(o, s, members, sv, {}, "ship"));
    int seqDependent = 0, undecidedSides = 0;
    bool chainTried = false;
    for (size_t m = 0; m < members.size(); ++m) {
        auto r = sv.sa.result(static_cast<int>(m));
        for (auto const& d : r.debug) if (d.find("undecided") != std::string::npos) ++undecidedSides;
        auto w0 = sv.locals[m].result(s.inputs[members[m]].tMs);
        LocalEvidence ev;
        ev.window = &w0;
        ev.outcomes = &sv.locals[m].outcomes();
        ev.frame = frameOf(s, members[m]);
        ev.nextFrame = members[m] + 1 < s.inputs.size() ? frameOf(s, members[m] + 1) : kNaN;
        ev.nextFollows = true;
        ev.horizonFrame = ev.frame + 130.0;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
        if (st.status == status::TimingStatus::SequenceDependent) {
            ++seqDependent;
            CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::SaUndecided) != st.reasons.end());
            CHECK(!r.decided);
        }
        if (r.trials > 2) chainTried = true;
    }
    std::printf("  %d of %zu inputs sequence_dependent, %d undecided sides\n", seqDependent, members.size(), undecidedSides);
    CHECK(seqDependent >= 1);
    CHECK(chainTried);
}

void testCbfRefine() {
    SECTION("cbfRefine: sub-tick oracle -> local and SA edges within 1/8 tick of the analytic boundary; placement cbf, gprl-clone/6-cbf");
    auto s = schedule({{100, true}, {118, false}, {136, true}});
    World w = waveWorld();
    auto path = freePath(w, s, 330, true);
    corridorAround(w, path, [](double x) { double k = tickAt(x); return (k >= 205 && k < 208) ? 3.3 : (k < 118 ? 9.7 : 12.2); },
                   [](double x) { double k = tickAt(x); return (k >= 205 && k < 208) ? 4.1 : (k < 118 ? 11.3 : 8.9); });
    KinematicOracle o(w, true);
    auto sv = solveAll(o, s, {0, 1, 2}, {}, true);
    // analytic boundary by bisection on the oracle itself (1e-4 tick)
    auto boundary = [&](std::function<bool(double)> const& pass, double inside, double outside) {
        for (int it = 0; it < 60; ++it) {
            double mid = 0.5 * (inside + outside);
            if (pass(mid)) inside = mid;
            else outside = mid;
        }
        return 0.5 * (inside + outside);
    };
    for (int m = 0; m < 3; ++m) {
        size_t i = static_cast<size_t>(m);
        auto w0 = sv.locals[i].result(s.inputs[i].tMs);
        CHECK(w0.valid && sv.locals[i].refined());
        double le = localEdgeTicks(w0.early), ll = localEdgeTicks(w0.late);
        double trueE = boundary([&](double k) { return localPassAt(o, s, i, k); }, 0.0, std::floor(le) - 1.0);
        double trueL = boundary([&](double k) { return localPassAt(o, s, i, k); }, 0.0, std::ceil(ll) + 1.0);
        auto r = sv.sa.result(m);
        double se = r.sequence.early.edgeFrames(), sl = r.sequence.late.edgeFrames();
        std::printf("  #%d local [%+.3f,%+.3f] true [%+.3f,%+.3f] | SA [%+.3f,%+.3f] %s\n", m, le, ll, trueE, trueL, se, sl, r.decided ? "decided" : "undecided");
        CHECK(std::fabs(le - trueE) <= 0.125 + 1e-6 && std::fabs(ll - trueL) <= 0.125 + 1e-6);
        if (r.sideDecided[0] && r.sequence.early.stop == EdgeStop::Fail) {
            double t0 = boundary([&](double k) { return familyPassAt(o, s, i, k); }, r.sequence.early.passFrames, r.sequence.early.failFrames);
            CHECK_MSG(std::fabs(se - t0) <= 0.125 + 1e-6, "SA early " + std::to_string(se) + " vs " + std::to_string(t0));
            // the bracket is at most 1/4 tick: the local refinement points, or the SA round of 3 points
            CHECK(std::fabs(r.sequence.early.failFrames - r.sequence.early.passFrames) <= 0.25 + 1e-9);
        }
        if (r.sideDecided[1] && r.sequence.late.stop == EdgeStop::Fail) {
            double t1 = boundary([&](double k) { return familyPassAt(o, s, i, k); }, r.sequence.late.passFrames, r.sequence.late.failFrames);
            CHECK_MSG(std::fabs(sl - t1) <= 0.125 + 1e-6, "SA late " + std::to_string(sl) + " vs " + std::to_string(t1));
        }
        LocalEvidence ev;
        ev.window = &w0;
        ev.outcomes = &sv.locals[i].outcomes();
        ev.frame = frameOf(s, i);
        ev.nextFrame = i + 1 < s.inputs.size() ? frameOf(s, i + 1) : kNaN;
        ev.nextFollows = true;
        ev.horizonFrame = ev.frame + 130.0;
        TimingResultContext ctx;
        ctx.inputSeq = static_cast<int64_t>(2 + i);
        ctx.kind = s.inputs[i].down ? InputKind::Press : InputKind::Release;
        ctx.eventT = s.inputs[i].tMs / 1000.0;
        ctx.subtick = true;
        ctx.refined = sv.locals[i].refined();
        ctx.cluster = {"a1:1", m + 1, cluster::Tri::Unknown, cluster::Tri::Unknown};
        auto built = buildTimingResultEvent(ctx, ev, &r, status::statusOf({}, localFacts(ev), saFacts(&r), {}), true);
        CHECK_MSG(built.ok, built.error);
        CHECK(built.payload.local && built.payload.local->early.placement == "cbf");
        CHECK(built.payload.solverVersion == "gprl-clone/6-cbf");
    }
    // without CBF: whole ticks, placement tick, the lattice count is exact
    KinematicOracle whole(w, false);
    auto sw = solveAll(whole, s, {0}, {}, false);
    auto w1 = sw.locals[0].result(s.inputs[0].tMs);
    CHECK(!sw.locals[0].refined());
    CHECK(std::fabs(w1.early.passShiftMs / T - std::round(w1.early.passShiftMs / T)) < 1e-9);
    CHECK(matchesBrute(whole, s, {0}, sw, {}, "no-cbf"));
}

void testPortals() {
    SECTION("speedPortal: a speed change inside the look-ahead -> local and SA equal brute force (ms windows)");
    {
        auto s = schedule({{100, true}, {118, false}, {136, true}});
        World w = waveWorld();
        w.portals.push_back({1.3 * 150.0, Portal::Kind::Speed, 1.5});
        auto path = freePath(w, s, 330);
        corridorAround(w, path, [](double) { return 7.0; }, [](double) { return 9.0; });
        KinematicOracle o(w);
        auto sv = solveAll(o, s, {0, 1, 2});
        CHECK(matchesBrute(o, s, {0, 1, 2}, sv, {}, "speed"));
    }
    SECTION("gravityPortal: a flip between #1 and #2 is a cluster break -> no follower is ever moved across it");
    {
        auto s = schedule({{100, true}, {118, false}, {136, true}, {154, false}});
        World w = waveWorld();
        w.portals.push_back({1.3 * 127.0, Portal::Kind::Gravity, 1.0, true});
        auto path = freePath(w, s, 330);
        corridorAround(w, path, [](double x) { double k = tickAt(x); return (k >= 190 && k < 193) ? 3.0 : 14.0; },
                       [](double x) { double k = tickAt(x); return (k >= 190 && k < 193) ? 4.0 : 14.0; });
        KinematicOracle o(w);
        std::vector<size_t> breaks = {2};
        auto sv = solveAll(o, s, {0, 1, 2, 3}, breaks);
        CHECK(matchesBrute(o, s, {0, 1, 2, 3}, sv, breaks, "gravity"));
        // input #1's family can never include #2 (across the flip): its SA is decided without it or undecided
        auto r1 = sv.sa.result(1);
        for (auto a : r1.adaptationUsed) CHECK(a == SAAdaptation::Local);
        CHECK(r1.adaptationUsed.empty());
    }
    SECTION("miniPortal: slope and hitbox change -> brute force; a portal within 2 ticks of the input is a transition_input");
    {
        auto s = schedule({{100, true}, {118, false}, {136, true}});
        World w = waveWorld();
        w.portals.push_back({1.3 * 119.0, Portal::Kind::Mini, 1.0, true});
        auto path = freePath(w, s, 330);
        corridorAround(w, path, [](double) { return 8.0; }, [](double) { return 10.0; });
        KinematicOracle o(w);
        auto sv = solveAll(o, s, {0, 1, 2});
        CHECK(matchesBrute(o, s, {0, 1, 2}, sv, {}, "mini"));
        auto w1 = sv.locals[1].result(s.inputs[1].tMs);
        auto r1 = sv.sa.result(1);
        LocalEvidence ev;
        ev.window = &w1;
        ev.outcomes = &sv.locals[1].outcomes();
        ev.frame = 118.0;
        ev.nextFrame = 136.0;
        ev.nextFollows = true;
        ev.horizonFrame = 248.0;
        status::ContextFacts cx;
        cx.transitionInput = true;   // the engine: a size portal within transitionTicks after the input
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r1), cx);
        CHECK(std::find(st.reasons.begin(), st.reasons.end(), status::Reason::TransitionInput) != st.reasons.end());
        CHECK(status::informational(status::Reason::TransitionInput));
    }
}

}  // namespace

int main() {
    testWaveZigzagCorridor();
    testLocalVsSequence();
    testCube();
    testShipHold();
    testShipCorrections();
    testCbfRefine();
    testPortals();
    testProgressAndBudget();
    testPlannerRules();
    testUndecidedSideInheritsLocalEdge();
    testOpenRange();
    testLookAheadFromLastFollower();
    testProofKinds();
    testLocalTrialBeyondLocalRunProof();
    testInheritOnlyTheLocalWindowsBracket();
    testFuzzGateConsistency();
    return gprl::test::finish("sequence_adjusted_tests");
}
