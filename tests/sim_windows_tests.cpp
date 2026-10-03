// Timing windows in the simulator (core/sim/windows) with the real engine: a cube jump over a
// triple spike has a window > 0 ticks bounded on both sides with earliest < 0 < latest; a shift
// that would cross a neighbouring input leaves that side unbounded (sequence-dependent edge);
// sub-tick refinement narrows the resolution; measurability, resumption, window fields.
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

#include "../core/sim/engine.hpp"
#include "../core/sim/search.hpp"
#include "../core/sim/windows.hpp"
#include "sim_analysis_world.hpp"

using namespace gprl::sim;

namespace {

constexpr double kTickMs = 1000.0 / 240.0;

std::vector<SimWindow> measure(World const& w, std::vector<RecordedInput> const& inputs, WindowsConfig cfg, WindowsProgress* progressOut = nullptr) {
    std::vector<SimWindow> out;
    WindowsProgress p;
    bool done = measureWindows(w, inputs, w.start, cfg, [] { return true; }, 0, out, p);
    CHECK(done);
    CHECK(p.done);
    if (progressOut) *progressOut = p;
    return out;
}

void printWindow(char const* what, SimWindow const& w) {
    std::printf("  %s: tick %d %s x%% %.1f window %.2f ms [%.2f, %.2f] bounded %d/%d res %.3f trials %d\n", what, w.tick, w.down ? "press" : "release", w.percent,
                w.windowMs, w.earliestMs, w.latestMs, w.boundedEarly ? 1 : 0, w.boundedLate ? 1 : 0, w.resolutionMs, w.trials);
}

ReferenceRun reference(World const& w) {
    Search s(&w, {});
    CHECK(s.run([] { return true; }, 0));
    return s.result();
}

void testTripleSpikeJump() {
    SECTION("the reference press over a triple spike: window > 0 ticks, bounded both sides, earliest < 0 < latest");
    World w = simworld::tripleSpikeWorld(300.f, 900.f);
    ReferenceRun r = reference(w);
    CHECK(r.completed);
    CHECK(!r.inputs.empty());
    if (r.inputs.empty()) return;
    WindowsConfig cfg;
    cfg.maxShift = 100;   // wide enough to find both edges of a plain jump whatever the reference timing
    WindowsProgress p;
    auto ws = measure(w, r.segments.front().inputs, cfg, &p);
    for (auto const& l : p.debug) std::printf("    %s\n", l.c_str());
    CHECK(!ws.empty());
    if (ws.empty()) return;
    SimWindow const& press = ws.front();
    printWindow("press", press);
    CHECK(press.down);
    CHECK(press.source == WindowSource::Reference);
    CHECK(press.boundedEarly);
    CHECK(press.boundedLate);
    CHECK(press.earliestMs < 0.0);
    CHECK(press.latestMs > 0.0);
    CHECK(press.windowMs > kTickMs - 1e-9);   // at least one tick
    CHECK_NEAR(press.windowMs, press.latestMs - press.earliestMs, 1e-9);
    CHECK_NEAR(press.resolutionMs, kTickMs, 1e-9);   // whole-tick brackets without CBF
    // the edges are midpoints of one-tick brackets: (k + 0.5) ticks
    double e = -press.earliestMs / kTickMs, l = press.latestMs / kTickMs;
    CHECK_NEAR(e - std::floor(e), 0.5, 1e-6);
    CHECK_NEAR(l - std::floor(l), 0.5, 1e-6);
    CHECK(press.tick == r.inputs.front().step);
    CHECK_NEAR(press.frame, static_cast<double>(press.tick), 1e-9);
    CHECK_NEAR(press.tSeconds, press.tick / 240.0, 1e-12);
    CHECK(press.percent > 0.0 && press.percent < 40.0);
    CHECK(press.gamemode == Gamemode::Cube);
    CHECK(press.speed == Speed::Normal);
    CHECK(press.supported);
    CHECK(!press.verified);   // no recording: never verified
    CHECK(press.geometryHash != 0);
    CHECK(press.geometryHashHex.size() == 16);
    CHECK(press.trials >= 2);
    CHECK(p.trials >= press.trials);
    CHECK(p.ticks > 0);
    // the window is a real local edge: the spike row kills the shifted copies, nothing else
    CHECK(press.windowMs < 100.0 * kTickMs);
}

void testNeighbourAndSubTick() {
    SECTION("a 1-tick tap: the press's late side and the release's early side stop at the neighbour (unbounded)");
    World w = simworld::tripleSpikeWorld(300.f, 900.f);
    ReferenceRun r = reference(w);
    CHECK(!r.inputs.empty());
    if (r.inputs.empty()) return;
    int const pressStep = r.inputs.front().step;
    std::vector<RecordedInput> tap;
    RecordedInput a;
    a.step = pressStep;
    a.down = true;
    tap.push_back(a);
    RecordedInput b;
    b.step = pressStep + 1;
    b.down = false;
    tap.push_back(b);
    // the tap must still clear the spikes (GD: the jump impulse is the press; the hold only matters at landing)
    auto traj = replayTrajectory(w, w.start, tap, 5000);
    CHECK(!traj.empty() && !traj.back().dead && traj.back().x >= 900.f - 2.f);
    WindowsConfig cfg;
    cfg.maxShift = 100;
    WindowsProgress p;
    auto ws = measure(w, tap, cfg, &p);
    CHECK(ws.size() == 2);
    if (ws.size() != 2) return;
    printWindow("tap press", ws[0]);
    printWindow("tap release", ws[1]);
    CHECK(ws[0].down && !ws[1].down);
    CHECK(ws[0].boundedEarly);
    CHECK(!ws[0].boundedLate);         // the release sits one tick later: no late shift possible
    CHECK_NEAR(ws[0].latestMs, 0.0, 1e-9);
    CHECK(ws[0].earliestMs < 0.0);
    CHECK(!ws[1].boundedEarly);        // the press sits one tick earlier: no early shift possible
    CHECK_NEAR(ws[1].earliestMs, 0.0, 1e-9);
    CHECK(ws[1].latestMs >= 0.0);
    CHECK(p.skippedNeighbour == 0);

    SECTION("sub-tick refinement narrows the bounded side's bracket to 1/8 tick (and 1/64 with two passes)");
    WindowsConfig fine = cfg;
    fine.subTick = true;
    fine.subTickPasses = 1;
    auto wf = measure(w, tap, fine);
    CHECK(wf.size() == 2);
    if (wf.size() == 2) {
        printWindow("refined press", wf[0]);
        CHECK(wf[0].boundedEarly);
        CHECK(wf[0].resolutionMs < kTickMs / 2.0);
        CHECK_NEAR(wf[0].resolutionMs, kTickMs / 8.0, 1e-6);
        // the refined early edge lies within the coarse bracket
        CHECK(wf[0].earliestMs <= ws[0].earliestMs + kTickMs * 0.5 + 1e-9);
        CHECK(wf[0].earliestMs >= ws[0].earliestMs - kTickMs * 0.5 - 1e-9);
        CHECK(wf[0].trials > ws[0].trials);
    }
    WindowsConfig finer = fine;
    finer.subTickPasses = 2;
    auto wff = measure(w, tap, finer);
    CHECK(wff.size() == 2);
    if (wff.size() == 2) {
        printWindow("refined x2 press", wff[0]);
        CHECK_NEAR(wff[0].resolutionMs, kTickMs / 64.0, 1e-6);
    }
}

void testMeasurabilityAndResume() {
    SECTION("an input the control cannot settle after (it dies) is skipped and counted");
    World w = simworld::tripleSpikeWorld(300.f, 900.f);
    ReferenceRun r = reference(w);
    CHECK(!r.inputs.empty());
    if (r.inputs.empty()) return;
    std::vector<RecordedInput> late;
    RecordedInput a;
    a.step = r.inputs.front().step + 40;   // far too late: the cube dies on the spikes
    a.down = true;
    late.push_back(a);
    auto traj = replayTrajectory(w, w.start, late, 5000);
    CHECK(!traj.empty() && traj.back().dead);
    WindowsProgress p;
    auto ws = measure(w, late, {}, &p);
    CHECK(ws.empty());
    CHECK(p.skippedControl == 1);
    CHECK(p.measured == 0);

    SECTION("the control completing the level counts as settled for the last input");
    World e = simworld::emptyWorld(400.f);
    std::vector<RecordedInput> one;
    RecordedInput o;
    o.step = 200;
    o.down = true;
    one.push_back(o);
    auto we = measure(e, one, {});
    CHECK(we.size() == 1);
    if (!we.empty()) {
        CHECK(!we[0].boundedEarly && !we[0].boundedLate);   // nothing can fail in an empty world
        CHECK_NEAR(we[0].earliestMs, -10 * kTickMs, 1e-9);
        CHECK_NEAR(we[0].latestMs, 10 * kTickMs, 1e-9);
    }

    SECTION("resumption: mayContinue() = false after the first window, the next call continues at the second");
    std::vector<RecordedInput> two = r.segments.front().inputs;
    RecordedInput extra;
    extra.step = r.inputs.back().step + 300;
    extra.down = true;
    two.push_back(extra);
    RecordedInput extraUp = extra;
    extraUp.step += 1;
    extraUp.down = false;
    two.push_back(extraUp);
    std::vector<SimWindow> out;
    WindowsProgress p2;
    int polls = 0;
    // gprl-sim/2 (H7): mayContinue() is polled before every TRIAL; a stop mid-window keeps the
    // walk's position and the next call continues with the same trial sequence
    bool done = measureWindows(w, two, w.start, {}, [&] { return ++polls <= 3; }, 0, out, p2);
    CHECK(!done);
    CHECK(polls == 4);   // the window's opening poll, two trials, then the refusal
    CHECK(p2.next == 0);
    CHECK(out.empty());
    CHECK(p2.trials == 2);
    CHECK(p2.state != nullptr);
    int resumes = 0;
    while (!done && resumes++ < 1000) {
        int budget = 0;
        done = measureWindows(w, two, w.start, {}, [&] { return ++budget <= 2; }, 0, out, p2);   // one or two trials per call
    }
    CHECK(done);
    CHECK(resumes > 5);
    CHECK(p2.next == static_cast<int>(two.size()));
    CHECK(!p2.state);   // M4: the control run and its snapshots are freed once done
    WindowsProgress pd;
    std::vector<SimWindow> direct = measure(w, two, {}, &pd);
    CHECK(direct.size() == out.size());
    CHECK(pd.trials == p2.trials);   // the resumed walk ran exactly the uninterrupted trial sequence
    for (size_t i = 0; i < direct.size() && i < out.size(); ++i) {
        CHECK(direct[i].tick == out[i].tick);
        CHECK_NEAR(direct[i].earliestMs, out[i].earliestMs, 1e-9);
        CHECK_NEAR(direct[i].latestMs, out[i].latestMs, 1e-9);
    }

    SECTION("tickOffset and source are carried into the window rows");
    WindowsConfig off;
    off.tickOffset = 1000;
    off.source = WindowSource::Recorded;
    auto wo = measure(w, r.segments.front().inputs, off);
    CHECK(!wo.empty());
    if (!wo.empty()) {
        CHECK(wo[0].tick == r.inputs.front().step + 1000);
        CHECK(wo[0].source == WindowSource::Recorded);
    }
}

bool resolutionInvariant(std::vector<SimWindow> const& ws) {
    for (auto const& w : ws)
        if (w.boundedEarly && w.boundedLate && w.windowMs + 1e-9 < w.resolutionMs) return false;
    return true;
}

void testResolutionRule() {
    SECTION("M9: a both-bounded window narrower than its resolution demotes its coarser side; measured windows obey the rule");
    double const nan = std::nan("");
    // early bracket a whole tick [0, 1] (refinement not tested), late refined to 1/8 [0.25, 0.375]:
    // width 0.5 + 0.3125 = 0.8125 tick < resolution 1 tick -> the early side becomes unbounded
    CHECK(coarseSideToDemote(0.0, 1.0, 1.0, 0.25, 0.375, 0.125) == -1);
    CHECK(coarseSideToDemote(0.25, 0.375, 0.125, 0.0, 1.0, 1.0) == +1);
    CHECK(coarseSideToDemote(0.0, 1.0, 1.0, 0.0, 1.0, 1.0) == 0);        // 1 tick wide = the resolution: kept
    CHECK(coarseSideToDemote(2.0, 3.0, 1.0, 2.0, 3.0, 1.0) == 0);
    CHECK(coarseSideToDemote(0.0, nan, 0.0, 0.0, 0.125, 0.125) == 0);   // one side unbounded already
    World w = simworld::tripleSpikeWorld(300.f, 900.f);
    ReferenceRun r = reference(w);
    if (r.inputs.empty()) return;
    WindowsConfig cfg;
    cfg.maxShift = 100;
    for (int passes = 0; passes <= 2; ++passes) {
        WindowsConfig c = cfg;
        c.subTick = passes > 0;
        c.subTickPasses = std::max(1, passes);
        CHECK(resolutionInvariant(measure(w, r.segments.front().inputs, c)));
    }
}

void testCentreResumable() {
    SECTION("H7: centring resumes per round / phase / input: tiny poll budgets give the same inputs as one uninterrupted run");
    World w = simworld::tripleSpikeWorld(300.f, 1200.f);
    w.objects.push_back(simworld::spike(800.f));
    simworld::finalize(w);
    ReferenceRun r = reference(w);
    CHECK(!r.segments.empty());
    if (r.segments.empty()) return;
    ReferenceSegment a = r.segments.front();
    ReferenceSegment b = r.segments.front();
    uint64_t ticks = 0;
    std::vector<std::string> lines;
    int const moved = centreInputs(w, a, 48, 3, ticks, &lines);
    for (auto const& l : lines) std::printf("    %s\n", l.c_str());
    CentreProgress p;
    int calls = 0;
    bool done = false;
    while (!done && calls++ < 1000000) {
        int budget = 0;
        done = centreInputsResumable(w, b, 48, 3, [&] { return ++budget <= 3; }, p);
    }
    CHECK(done && p.done);
    CHECK(calls > 10);
    CHECK(p.moved == moved);
    CHECK(!p.state);   // freed once done (M4)
    bool same = a.inputs.size() == b.inputs.size();
    for (size_t i = 0; same && i < a.inputs.size(); ++i) same = a.inputs[i].step == b.inputs[i].step && a.inputs[i].down == b.inputs[i].down;
    CHECK(same);
    CHECK(a.trajectory.size() == b.trajectory.size());
    // the centred inputs still clear the segment (the round's final replay validated them)
    auto traj = replayTrajectory(w, b.start, b.inputs, std::max(1, b.ticks));
    CHECK(!traj.empty() && !traj.back().dead);
    std::printf("  centre: %d moved in %d calls (%llu ticks), %d rejected, %d rounds\n", p.moved, calls, static_cast<unsigned long long>(p.ticks), p.rejected, p.round);
}

}  // namespace

int main() {
    testTripleSpikeJump();
    testNeighbourAndSubTick();
    testMeasurabilityAndResume();
    testResolutionRule();
    testCentreResumable();
    return gprl::test::finish("sim_windows_tests");
}
