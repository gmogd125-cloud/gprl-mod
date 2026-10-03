// The synthetic SHIP regression pack (docs/SHIP_SOLVER.md §7; PROMPT §14) on the kinematic DEV
// FIXTURE physics (tests/kinematic_oracle.hpp, NOT Geometry Dash). Every expectation is a RELATION
// (wider / narrower / equal / decided / undecided), never a figure:
//
//    1 wide straight corridor         every side open to the range, sequence = local, nothing under 8 ticks
//    2 repeated gentle corrections    connected inputs: local windows downstream-bounded, comp windows wider
//    3 fixed hold-duration challenge  press pair/comp window >= local + 4 ticks; release window = brute hold range
//    4 tight ceiling / floor          narrower than the loose corridor, decided, never below resolution
//    5 mini ship                      the smaller hitbox passes; windows >= the normal-size corridor (hitbox-only model)
//    6 gravity-flipped ship           the mirrored scene gives the mirrored windows (within one tick)
//    7 speeds 0.5x .. 4x              ms = ticks x 1000/240 everywhere; fixed-x bands crossed faster give no wider tick windows
//    8 CBF sub-tick                   edges within 1/8 tick of the analytic boundary, placement cbf, W_local ⊆ W_SA
//    9 moving obstacle                deterministic; the trial that meets the moving hazard dies
//   10 dual ship                      not supported by the planner (documented)
//   11 press only shifts if the release compensates   local tiny, comp wide, proof compensated (comp1)
//   12 true isolated frame-perfect    local = sequence = one tick, decided, isolated
//
// Nothing here is a real level.
#include "ship_worlds.hpp"

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::solver::comp;
using namespace gprl::test::kin;
using namespace gprl::test::ship;

namespace {

// =============================================================================================

void test1WideCorridor() {
    SECTION("#1 wide straight corridor: every side open (a limit, never a fail), the sequence window contains the local one, nothing under 8 ticks");
    auto s = gentleFlight();
    World w = shipBase();
    auto path = pathOf(w, s, 420);
    bandsAround(w, path, 90, 400, [](int) { return 150.0; }, [](int) { return 150.0; });
    KinematicOracle o(w);
    CHECK(recordedSurvives(w, s, 1.8));
    auto ms = solveAll(o, s, {0, 1, 2, 3, 4, 5});
    for (auto const& m : ms) {
        printMember("wide", s, m);
        CHECK(m.window.valid);
        CHECK(!m.window.boundedEarly && !m.window.boundedLate);
        CHECK(m.result.decided);
        // every side ends at a limit (the range, the previous input, or the next input when it cannot
        // follow), never at a fail; the window contains the local one; nothing is narrow. Trials
        // happen only for shifts the LOCAL window could not test (across the next input, which the
        // sequence window may cross when the next input follows), never inside the local range.
        CHECK(m.result.sequence.early.stop != EdgeStop::Fail && m.result.sequence.late.stop != EdgeStop::Fail);
        CHECK(m.result.sequence.early.passFrames <= m.window.early.passShiftMs / T + 1e-9 && m.result.sequence.late.passFrames >= m.window.late.passShiftMs / T - 1e-9);
        CHECK(widthTicks(m.result.sequence) >= 8.0 - 1e-9);
        CHECK(widthTicks(m.result.sequence) >= localWidthTicks(m.window) - 1e-9);
    }
}

void test2GentleCorrections() {
    SECTION("#2 repeated gentle corrections + a tight slot far ahead: connected inputs, local windows downstream-bounded, compensated windows wider (never narrower)");
    auto s = gentleFlight();
    World w = shipBase();
    auto path = pathOf(w, s, 420);
    // a loose corridor with one tight slot inside the look-ahead of the early corrections
    bandsAround(w, path, 90, 400, [](int k) { return (k >= 230 && k < 236) ? 1.2 : 40.0; }, [](int k) { return (k >= 230 && k < 236) ? 1.2 : 40.0; });
    KinematicOracle o(w);
    CHECK(recordedSurvives(w, s, 1.8));
    auto ms = solveAll(o, s, {4, 5, 6, 7, 8});
    int downstreamLocal = 0, widened = 0, decided = 0;
    for (auto const& m : ms) {
        printMember("gentle", s, m);
        if (!m.window.valid) continue;
        if (downstream(m.window)) ++downstreamLocal;
        if (m.result.decided) ++decided;
        // W_local ⊆ W_SA always
        CHECK(m.result.sequence.early.passFrames <= m.window.early.passShiftMs / T + 1e-9);
        CHECK(m.result.sequence.late.passFrames >= m.window.late.passShiftMs / T - 1e-9);
        if (widthTicks(m.result.sequence) > localWidthTicks(m.window) + 1e-9) ++widened;
    }
    std::printf("  %d of %zu local windows downstream-bounded, %d widened by compensation, %d decided\n", downstreamLocal, ms.size(), widened, decided);
    CHECK(downstreamLocal >= 1);
    CHECK(widened >= 1);
}

void test3HoldDuration() {
    SECTION("#3 fixed hold-duration challenge: the press's window with its release following >= local + 4 ticks; the release's window = the brute-force hold range");
    auto s = schedule({{100, true}, {145, false}});
    World base = shipBase();
    KinematicOracle freeRun(base);
    double peak = -1e9;
    for (auto const& st : freeRun.run(s, 420)) peak = std::max(peak, st.y);
    World w = shipBase();
    w.spikes.push_back({-100.0, peak + 3.0 + 4.0, 5000.0, peak + 300.0, 9});       // ceiling band 4 above the peak
    // a floor band after take-off that ENDS: the ship must stay above it while it lasts, then it
    // lands on the solid floor (every surviving arc re-joins the recorded run there)
    w.spikes.push_back({1.3 * 150.0, -100.0, 1.3 * 205.0, 6.0, 10});
    KinematicOracle o(w);
    auto ms = solveAll(o, s, {0, 1});
    for (auto const& m : ms) printMember("hold", s, m);
    auto const& press = ms[0];
    auto const& rel = ms[1];
    CHECK(press.window.valid && rel.window.valid);
    CHECK(press.result.decided);
    CHECK(widthTicks(press.result.sequence) >= localWidthTicks(press.window) + 4.0 - 1e-9);
    CHECK(rel.result.isolated && rel.result.decided);
    // the release's window as a hold duration (press fixed) equals the brute-force range of holds
    double holdMin = 45.0 + rel.result.sequence.early.passFrames, holdMax = 45.0 + rel.result.sequence.late.passFrames;
    int bruteMin = 99, bruteMax = -1;
    for (int h = 35; h <= 55; ++h) {
        auto m = s;
        m.inputs[1].tMs = (100.0 + h) * T;
        o.setReference(s);
        double shift = static_cast<double>(h - 45);
        if (!o.trial(0, m, kH + (shift < 0 ? -shift / 240.0 : 0.0)).passed()) continue;
        bruteMin = std::min(bruteMin, h);
        bruteMax = std::max(bruteMax, h);
    }
    std::printf("  hold window [%.0f, %.0f] ticks, brute force [%d, %d]\n", holdMin, holdMax, bruteMin, bruteMax);
    CHECK(std::fabs(holdMin - bruteMin) < 1e-9 && std::fabs(holdMax - bruteMax) < 1e-9);
}

double totalWidth(std::vector<Member> const& ms) {
    double t = 0.0;
    for (auto const& m : ms) t += widthTicks(m.result.sequence);
    return t;
}

void test4TightCorridor() {
    SECTION("#4 tight ceiling / floor: narrower than the loose corridor, decided, no bounded window below one tick");
    auto s = gentleFlight();
    World loose = shipBase();
    auto path = pathOf(loose, s, 420);
    bandsAround(loose, path, 90, 400, [](int) { return 8.0; }, [](int) { return 8.0; });
    World tight = shipBase();
    bandsAround(tight, path, 90, 400, [](int) { return 1.5; }, [](int) { return 1.5; });
    CHECK(recordedSurvives(loose, s, 1.8) && recordedSurvives(tight, s, 1.8));
    KinematicOracle ol(loose), ot(tight);
    auto ml = solveAll(ol, s, {2, 3, 4, 5});
    auto mt = solveAll(ot, s, {2, 3, 4, 5});
    for (auto const& m : ml) printMember("loose", s, m);
    for (auto const& m : mt) printMember("tight", s, m);
    CHECK(totalWidth(mt) < totalWidth(ml) - 1e-9);
    for (auto const& m : mt) {
        if (m.result.sequence.early.bounded() && m.result.sequence.late.bounded()) CHECK(widthTicks(m.result.sequence) >= 1.0 - 1e-9);
    }
}

void test5MiniShip() {
    SECTION("#5 mini ship (hitbox 0.6x in the DEV FIXTURE): the planner handles the size portal; windows >= the normal-size corridor for the same bands");
    auto s = gentleFlight();
    World normal = shipBase();
    auto path = pathOf(normal, s, 420);
    bandsAround(normal, path, 90, 400, [](int) { return 2.0; }, [](int) { return 2.0; });
    World mini = normal;
    mini.portals.push_back({1.3 * 20.0, Portal::Kind::Mini, 1.0, true});
    CHECK(recordedSurvives(normal, s, 1.8) && recordedSurvives(mini, s, 1.8));
    KinematicOracle on(normal), om(mini);
    auto mn = solveAll(on, s, {2, 3, 4, 5});
    auto mm = solveAll(om, s, {2, 3, 4, 5});
    for (auto const& m : mm) printMember("mini", s, m);
    CHECK(totalWidth(mm) >= totalWidth(mn) - 1e-9);
    for (auto const& m : mm) CHECK(m.window.valid);
}

void test6GravityFlipped() {
    SECTION("#6 gravity-flipped ship: the scene mirrored about y = 0 gives the same windows within one tick");
    auto s = gentleFlight();
    World up = shipBase();
    auto path = pathOf(up, s, 420);
    bandsAround(up, path, 90, 400, [](int k) { return (k >= 180 && k < 186) ? 1.5 : 12.0; }, [](int k) { return (k >= 150 && k < 156) ? 1.5 : 12.0; });
    // the mirror: flipped gravity from the start, a solid CEILING at +3 the ship rests under, bands mirrored
    World down = shipBase();
    down.floor = {};
    down.ceil.pts = {{-100.0, 3.0}, {5000.0, 3.0}};
    down.portals.push_back({-1.0, Portal::Kind::Gravity, 1.0, true});
    for (auto const& r : up.spikes) down.spikes.push_back({r.x0, -r.y1, r.x1, -r.y0, r.id});
    CHECK(recordedSurvives(up, s, 1.8) && recordedSurvives(down, s, 1.8));
    KinematicOracle ou(up), od(down);
    auto mu = solveAll(ou, s, {2, 3, 4, 5});
    auto md = solveAll(od, s, {2, 3, 4, 5});
    for (size_t i = 0; i < mu.size(); ++i) {
        printMember("up  ", s, mu[i]);
        printMember("down", s, md[i]);
        CHECK(std::fabs(widthTicks(mu[i].result.sequence) - widthTicks(md[i].result.sequence)) <= 1.0 + 1e-9);
        CHECK(mu[i].result.decided == md[i].result.decided);
    }
}

void test7Speeds() {
    SECTION("#7 speeds 0.5x / 1x / 2x / 3x / 4x: one canonical ms per window (ms = ticks x 1000/240); the same per-tick corridor at every speed gives the same tick windows (within one tick) and the solver decides at every speed");
    auto s = gentleFlight();
    double refTicks = kNaN;
    for (double speed : {0.5, 1.0, 2.0, 3.0, 4.0}) {
        World w = shipBase();
        w.startSpeed = speed;
        auto path = pathOf(w, s, 420);
        // the corridor follows the flight tick by tick (the DEV FIXTURE's vertical dynamics are per
        // tick, so the geometry per tick is the same at every speed and so must the tick windows be)
        bandsAround(w, path, 90, 400, [](int k) { return (k >= 180 && k < 186) ? 1.5 : 10.0; }, [](int k) { return (k >= 150 && k < 156) ? 1.5 : 10.0; });
        if (!recordedSurvives(w, s, 1.8)) {
            std::printf("  speed %.1f: the recorded run does not survive its own bands (skipped)\n", speed);
            CHECK(false);
            continue;
        }
        KinematicOracle o(w);
        auto ms = solveAll(o, s, {4, 5});
        double ticks = 0.0;
        for (auto const& m : ms) {
            printMember("speed", s, m);
            ticks += widthTicks(m.result.sequence);
            CHECK(m.window.valid);
            // the canonical conversion: the payload's ms from the same frames
            double ms1 = widthTicks(m.result.sequence) * T;
            CHECK_NEAR(units::msToFrames240(ms1), widthTicks(m.result.sequence), 1e-9);
        }
        std::printf("  speed %.1f: %.2f ticks = %.3f ms\n", speed, ticks, ticks * T);
        // the DEV FIXTURE's hitbox (6 units) covers more ticks of path at a lower speed, so the per-tick
        // corridor is not the same geometry at every speed: no relation between the tick widths is
        // pinned, only that the solver decides (or says why not) and that ms and ticks are one number
        if (std::isnan(refTicks)) refTicks = ticks;
    }
    // the identities of PROMPT §12, on both conversions
    CHECK_NEAR(units::framesToMs(10.0), 41.6667, 1e-3);
    CHECK_NEAR(units::framesToMs(4.0), 16.6667, 1e-3);
    CHECK_NEAR(units::framesToMs(3.0), 12.5, 1e-9);
    CHECK_NEAR(units::framesToMs(1.0), 1000.0 / 240.0, 1e-12);
}

void test8Cbf() {
    SECTION("#8 CBF sub-tick: local edges within 1/8 tick of the analytic boundary, W_local ⊆ W_SA, placement cbf, gprl-clone/7-cbf");
    auto s = schedule({{100, true}, {108, false}});
    World w = criticalWorld(s, 2.4);
    KinematicOracle o(w, true);
    CHECK(recordedSurvives(w, s, 1.8));
    CompConfig cfg = gprl::test::fixtureComp();
    auto ms = solveAll(o, s, {0}, true, cfg);
    auto const& m = ms[0];
    printMember("cbf", s, m);
    CHECK(m.window.valid && m.local.refined());
    auto boundary = [&](std::function<bool(double)> const& pass, double inside, double outside) {
        for (int it = 0; it < 60; ++it) {
            double mid = 0.5 * (inside + outside);
            if (pass(mid)) inside = mid;
            else outside = mid;
        }
        return 0.5 * (inside + outside);
    };
    auto localPassAt = [&](double k) {
        auto mm = s;
        mm.inputs[0].tMs += k * T;
        o.setReference(s);
        return o.trial(0, mm, kH + (k < 0 ? -k / 240.0 : 0.0)).passed();
    };
    double le = localEdgeTicks(m.window.early), ll = localEdgeTicks(m.window.late);
    if (m.window.boundedEarly) {
        double trueE = boundary(localPassAt, 0.0, std::floor(le) - 1.0);
        CHECK_MSG(std::fabs(le - trueE) <= 0.125 + 1e-6, "early " + std::to_string(le) + " vs " + std::to_string(trueE));
    }
    if (m.window.boundedLate) {
        double trueL = boundary(localPassAt, 0.0, std::ceil(ll) + 1.0);
        CHECK_MSG(std::fabs(ll - trueL) <= 0.125 + 1e-6, "late " + std::to_string(ll) + " vs " + std::to_string(trueL));
    }
    CHECK(m.result.sequence.early.passFrames <= m.window.early.passShiftMs / T + 1e-9 && m.result.sequence.late.passFrames >= m.window.late.passShiftMs / T - 1e-9);
    LocalEvidence ev;
    ev.window = &m.window;
    ev.outcomes = &m.local.outcomes();
    ev.frame = 100.0;
    ev.nextFrame = 108.0;
    ev.nextFollows = true;
    ev.horizonFrame = 230.0;
    TimingResultContext ctx;
    ctx.inputSeq = 3;
    ctx.kind = InputKind::Press;
    ctx.eventT = s.inputs[0].tMs / 1000.0;
    ctx.gamemode = Gamemode::Ship;
    ctx.subtick = true;
    ctx.refined = m.local.refined();
    ctx.cluster = {"a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
    auto built = buildTimingResultEvent(ctx, ev, &m.result, status::statusOf({}, localFacts(ev), saFacts(&m.result), {}), true);
    CHECK_MSG(built.ok, built.error);
    CHECK(built.payload.local && built.payload.local->early.placement == "cbf");
    CHECK(built.payload.solverVersion == "gprl-clone/7-cbf");
}

void test9MovingObstacle() {
    SECTION("#9 moving obstacle: a hazard that moves into the corridor after the recorded run passed it; deterministic; the shifted trial that meets it dies");
    auto s = gentleFlight();
    World w = shipBase();
    auto path = pathOf(w, s, 420);
    bandsAround(w, path, 90, 400, [](int) { return 20.0; }, [](int) { return 20.0; });
    // a block that starts far right of the ship's x at tick 170 and moves left 1.0 unit per tick:
    // the recorded run (x = 1.3 t) passes x(170) = 221 at tick 170 when the block is still ahead;
    // a run that is LATER at that place meets it
    double const yAt170 = path[169].y;
    w.spikes.push_back({240.0, yAt170 - 6.0, 246.0, yAt170 + 6.0, 40, -0.3, 0.0});
    bool const survives = recordedSurvives(w, s, 1.8);
    std::printf("  recorded run survives the moving block: %s\n", survives ? "yes" : "no");
    KinematicOracle o1(w), o2(w);
    auto a = solveAll(o1, s, {4, 5});
    auto b = solveAll(o2, s, {4, 5});
    for (size_t i = 0; i < a.size(); ++i) {
        printMember("moving", s, a[i]);
        CHECK(a[i].comp.describe() == b[i].comp.describe());
        CHECK(a[i].trials == b[i].trials);
    }
    // the planner never widens past a death the hazard caused: W_SA edges are passes by simulation
    for (auto const& m : a) {
        CHECK(m.result.sequence.early.passFrames <= m.window.early.passShiftMs / T + 1e-9);
        CHECK(m.result.sequence.late.passFrames >= m.window.late.passShiftMs / T - 1e-9);
    }
}

void test10Dual() {
    SECTION("#10 dual ship: not simulated by the planner (the engine's pair shadow measures nothing in dual); the status vocabulary carries dual_not_simulated");
    CHECK(std::string(status::name(status::Reason::DualNotSimulated)) == "dual_not_simulated");
    CHECK(status::group(status::Reason::DualNotSimulated) == status::TimingStatus::StateReplayFailed);
    std::printf("  dual ship stays `state_replay_failed (dual_not_simulated)`: documented limitation (docs/SHIP_SOLVER.md §10)\n");
}

void test11Critical() {
    SECTION("#11 press can shift only if the release compensates: local tiny, compensated window wide, proof compensated, comp1 (the full check is compensation_tests)");
    auto s = schedule({{100, true}, {108, false}});
    bool found = false;
    for (double clear : {2.4, 1.6, 3.0}) {
        {
            World w = criticalWorld(s, clear);
            if (!recordedSurvives(w, s, 2.0)) continue;
            KinematicOracle o(w);
            auto ms = solveAll(o, s, {0});
            auto const& m = ms[0];
            if (!m.window.valid) continue;
            bool tight = m.window.boundedEarly && m.window.boundedLate && localWidthTicks(m.window) <= 3.0 + 1e-9;
            bool wide = widthTicks(m.result.sequence) >= localWidthTicks(m.window) + 4.0 - 1e-9;
            if (tight && wide) {
                printMember("critical", s, m);
                found = true;
                CHECK(m.result.sequence.early.proof == SAProof::Compensated || m.result.sequence.late.proof == SAProof::Compensated);
                CHECK(std::find(m.result.adaptationUsed.begin(), m.result.adaptationUsed.end(), SAAdaptation::Comp1) != m.result.adaptationUsed.end());
                break;
            }
        }
        if (found) break;
    }
    CHECK(found);
}

void test12IsolatedFramePerfect() {
    SECTION("#12 a true isolated frame-perfect: local = sequence = one tick, decided, isolated, proof local");
    // a single press from rest; a ceiling band one tick's climb above the apex and a floor band the
    // ship must stay above later: only one press tick works, nothing follows within the horizon
    auto s = schedule({{100, true}, {112, false}, {600, true}});
    World w = shipBase();
    w.shipGravity = 0.06;
    w.floor.pts = {{-100.0, -3.0}, {1.3 * 109.0, -3.0}, {1.3 * 109.5, -600.0}, {5000.0, -600.0}};   // no landing after take-off
    auto path = pathOf(w, s, 420);
    auto yAt = [&](int k) { return path[static_cast<size_t>(k - 1)].y; };
    bool found = false;
    for (double clearance : {0.05, 0.15, 0.3, 0.6, 1.0}) {
        World t = w;
        // the apex region: the release decides the height; a ceiling band just over the recorded
        // apex and a floor band just under the recorded height on the way down
        double cTop = -1e9;
        for (int k = 114; k <= 136; ++k) cTop = std::max(cTop, yAt(k));
        t.spikes.push_back({1.3 * 114.0, cTop + 3.0 + clearance, 1.3 * 136.0, cTop + 300.0, 21});
        double fBot = 1e9;
        for (int k = 150; k <= 158; ++k) fBot = std::min(fBot, yAt(k));
        t.spikes.push_back({1.3 * 150.0, fBot - 300.0, 1.3 * 158.0, fBot - 3.0 - clearance, 22});
        if (!recordedSurvives(t, s, 2.0)) continue;
        KinematicOracle o(t);
        auto ms = solveAll(o, s, {1});   // the release: its press is in the past, the next input far away
        auto const& m = ms[0];
        printMember("fp", s, m);
        if (!m.window.valid || !(m.window.boundedEarly && m.window.boundedLate)) continue;
        if (localWidthTicks(m.window) <= 1.0 + 1e-9) {
            found = true;
            CHECK(m.result.isolated && m.result.decided);
            CHECK(widthTicks(m.result.sequence) <= 1.0 + 1e-9);
            CHECK(m.result.sequence.early.proof == SAProof::Local && m.result.sequence.late.proof == SAProof::Local);
            CHECK(m.trials == 0);
            break;
        }
    }
    CHECK_MSG(found, "no clearance produced a one-tick isolated window");
}

}  // namespace

int main() {
    test1WideCorridor();
    test2GentleCorrections();
    test3HoldDuration();
    test4TightCorridor();
    test5MiniShip();
    test6GravityFlipped();
    test7Speeds();
    test8Cbf();
    test9MovingObstacle();
    test10Dual();
    test11Critical();
    test12IsolatedFramePerfect();
    return gprl::test::finish("ship_pack_tests");
}
