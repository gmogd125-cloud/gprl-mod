// The reference-run beam search (core/sim/search) on synthetic worlds with the real engine:
// an empty world completes with no input; a spike needs a jump; an impossible wall becomes an
// unsolved span and the search continues past it; determinism (same digest twice); unsupported
// spans are skipped honestly; orbs / portals do not break it; tick budgets and stop().
#include "test_util.hpp"

#include <cstdio>

#include "../core/sim/engine.hpp"
#include "../core/sim/search.hpp"
#include "sim_analysis_world.hpp"

using namespace gprl::sim;

namespace {

ReferenceRun runSearch(World const& w, SearchConfig cfg = {}, std::vector<std::string>* dbg = nullptr) {
    Search s(&w, cfg);
    bool done = s.run([] { return true; }, 0);
    CHECK(done);
    CHECK(s.done());
    if (dbg) *dbg = s.debug();
    return s.result();
}

void print(char const* what, ReferenceRun const& r) {
    std::printf("  %s: %s, solved %.1f %%, %zu inputs, %zu ticks, %d restarts, %d checkpoints, %llu engine ticks, digest %llu\n", what,
                r.completed ? "completed" : (r.reachedEnd ? "reached end with gaps" : "stopped"), r.solvedPercent, r.inputs.size(), r.ticks.size(), r.restarts,
                r.checkpoints, static_cast<unsigned long long>(r.ticksSimulated), static_cast<unsigned long long>(r.trajectoryDigest));
}

void testEmptyWorld() {
    SECTION("an empty world completes by walking to endX with no input");
    World w = simworld::emptyWorld(600.f);
    ReferenceRun r = runSearch(w);
    print("empty", r);
    CHECK(r.completed);
    CHECK(r.reachedEnd);
    CHECK(r.inputs.empty());
    CHECK(r.unsolved.empty());
    CHECK(r.skippedUnsupported.empty());
    CHECK_NEAR(r.solvedPercent, 100.0, 1e-9);
    CHECK(r.segments.size() == 1);
    CHECK(!r.ticks.empty());
    CHECK(r.ticks.back().x >= 600.f - 2.f);
    CHECK(r.trajectoryDigest != 0);
    // every recorded tick step is consecutive from 1
    bool consecutive = true;
    for (size_t i = 0; i < r.ticks.size(); ++i)
        if (r.ticks[i].step != static_cast<int>(i) + 1) consecutive = false;
    CHECK(consecutive);
}

void testSpike() {
    SECTION("a spike needs a jump: the run completes with >= 1 input (a press) and no gap");
    World w = simworld::spikeWorld(300.f, 900.f);
    std::vector<std::string> dbg;
    ReferenceRun r = runSearch(w, {}, &dbg);
    print("spike", r);
    for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
    CHECK(r.completed);
    CHECK(!r.inputs.empty());
    if (!r.inputs.empty()) {
        CHECK(r.inputs.front().down);
        CHECK(r.inputs.front().step >= 1);
        // the press happens before the spike
        CHECK(r.ticks[static_cast<size_t>(r.inputs.front().step - 1)].x < 300.f);
    }
    CHECK(r.unsolved.empty());
    CHECK_NEAR(r.solvedPercent, 100.0, 1e-9);
    // the trajectory is the replay of the inputs: the last tick reaches the end alive
    CHECK(!r.ticks.empty() && !r.ticks.back().dead);
    // the same inputs replayed stand-alone give the same trajectory
    auto replay = replayTrajectory(w, w.start, r.segments.front().inputs, r.segments.front().ticks);
    CHECK(replay.size() == r.ticks.size());
    CHECK(trajectoryDigest(replay, r.segments.front().inputs) == r.trajectoryDigest);
}

void testSolidWallPhysics() {
    SECTION("physics: a grounded cube walking into a solid floor-to-ceiling column must die (wall death)");
    // Demonstrates an engine defect found 2026-10-02: engine.cpp solid() returns at `if (!boolJ) return;`
    // (boolJ = the player moved up this step) BEFORE the inner-rect death check of the
    // collidedWithObjectInternal tail, so a cube on the ground (dy = 0) walks through any block side.
    World w = simworld::solidWallWorld(600.f, 1500.f);
    auto traj = replayTrajectory(w, w.start, {}, 5000);
    CHECK(!traj.empty());
    bool diedAtWall = !traj.empty() && traj.back().dead && traj.back().x > 560.f && traj.back().x < 600.f;
    CHECK_MSG(diedAtWall, std::string("no wall death: the cube ended at x ") + std::to_string(traj.empty() ? 0.f : traj.back().x) + (traj.empty() || !traj.back().dead ? " alive" : " dead"));
    if (diedAtWall) {
        // and the search then treats the column as impossible
        ReferenceRun r = runSearch(w, {});
        CHECK(!r.completed);
        CHECK(r.unsolved.size() >= 1);
    }
}

void testWall() {
    SECTION("an impossible wall (a hazard column): an unsolved span is recorded and the search continues past it");
    World w = simworld::wallWorld(600.f, 1500.f);
    std::vector<std::string> dbg;
    SearchConfig cfg;
    cfg.beamWidth = 16;
    cfg.beamWidthMax = 64;
    ReferenceRun r = runSearch(w, cfg, &dbg);
    print("wall", r);
    for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
    CHECK(!r.completed);
    CHECK(r.reachedEnd);
    CHECK(r.unsolved.size() >= 1);
    if (!r.unsolved.empty()) {
        CHECK(r.unsolved.front().mechanic == "search_exhausted");
        CHECK(r.unsolved.front().x0 < 600.f);
        CHECK(r.unsolved.front().x1 <= 600.f + 1.f);
        CHECK(r.unsolved.front().percentTo > r.unsolved.front().percentFrom);
    }
    CHECK(r.segments.size() >= 2);
    if (r.segments.size() >= 2) {
        CHECK(r.segments.back().start.x > 600.f);
        CHECK(r.segments.back().completed);
        CHECK(r.segments.front().endReason == "unsolved");
    }
    CHECK(r.solvedPercent < 100.0);
    CHECK(r.solvedPercent > 50.0);
    CHECK(r.restarts >= 1);
    // M2 (gprl-sim/2): the flattened steps are LEVEL ticks: consecutive inside a segment, and the
    // skipped stretch's speed-integrated travel time between segments
    bool increasing = true;
    for (size_t i = 1; i < r.ticks.size(); ++i)
        if (r.ticks[i].step <= r.ticks[i - 1].step) increasing = false;
    CHECK(increasing);
    for (size_t k = 1; k < r.segments.size(); ++k) {
        ReferenceSegment const& a = r.segments[k - 1];
        ReferenceSegment const& b = r.segments[k];
        double const gap = ticksAcross(w, a.x1, b.x0, speedAtX(w, a.x1));
        CHECK(b.gapTicksBefore == static_cast<int>(std::llround(gap)));
        CHECK(b.tickOffset == a.tickOffset + a.ticks + b.gapTicksBefore);
        CHECK(b.gapTicksBefore > 0);
    }
}

void testDeterminism() {
    SECTION("the same world twice gives the same digest, inputs and ticks");
    World w = simworld::tripleSpikeWorld(300.f, 1200.f);
    w.objects.push_back(simworld::spike(700.f));
    simworld::finalize(w);
    ReferenceRun a = runSearch(w);
    ReferenceRun b = runSearch(w);
    print("triple+1 (a)", a);
    CHECK(a.trajectoryDigest == b.trajectoryDigest);
    CHECK(a.inputs.size() == b.inputs.size());
    CHECK(a.ticks.size() == b.ticks.size());
    CHECK(a.ticksSimulated == b.ticksSimulated);
    bool same = a.inputs.size() == b.inputs.size();
    for (size_t i = 0; same && i < a.inputs.size(); ++i) same = a.inputs[i].step == b.inputs[i].step && a.inputs[i].down == b.inputs[i].down;
    CHECK(same);
    CHECK(a.completed);
}

void testUnsupported() {
    SECTION("an unsupported span ends coverage honestly and the search resumes past it");
    World w = simworld::base(1500.f);
    w.objects.push_back(simworld::spike(300.f));
    simworld::addUnsupported(w, 600.f, 900.f, "move_trigger");
    simworld::finalize(w);
    std::vector<std::string> dbg;
    ReferenceRun r = runSearch(w, {}, &dbg);
    print("unsupported", r);
    for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
    CHECK(!r.completed);
    CHECK(r.reachedEnd);
    CHECK(r.skippedUnsupported.size() == 1);
    if (!r.skippedUnsupported.empty()) {
        CHECK(r.skippedUnsupported.front().mechanic == "move_trigger");
        CHECK_NEAR(r.skippedUnsupported.front().x1, 900.f, 1e-3);
    }
    CHECK(r.unsolved.empty());
    CHECK(r.segments.size() == 2);
    if (r.segments.size() == 2) {
        CHECK(r.segments.front().endReason == "unsupported");
        CHECK(r.segments.front().x1 <= 600.f + 2.f);
        CHECK(r.segments.back().start.x >= 900.f);
        CHECK(r.segments.back().completed);
    }
    CHECK(r.solvedPercent < 100.0);
    CHECK(r.solvedPercent > 60.0);
}

void testOrbsAndPortals() {
    SECTION("a ship portal into open space completes; an orb level finishes honestly either way");
    World ship = simworld::base(1200.f);
    ship.objects.push_back(simworld::portal(400.f, Gamemode::Ship));
    simworld::finalize(ship);
    ReferenceRun rs = runSearch(ship);
    print("ship", rs);
    CHECK(rs.reachedEnd);
    CHECK(rs.completed);

    // 5 spikes = 150 units of hazards: one jump (134 units of air) cannot clear them, a jump + the
    // yellow orb above the row can (release + press while touching the ring: a real orb usage)
    World orbs = simworld::base(1500.f);
    for (int i = 0; i < 5; ++i) orbs.objects.push_back(simworld::spike(400.f + 30.f * i));
    orbs.objects.push_back(simworld::orb(430.f, 75.f));
    simworld::finalize(orbs);
    std::vector<std::string> dbg;
    ReferenceRun ro = runSearch(orbs, {}, &dbg);
    print("orbs", ro);
    for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
    CHECK(ro.reachedEnd);
    CHECK_MSG(ro.completed, "the solvable orb row was not solved (the search must find release + press on the orb)");
    if (ro.completed) {
        CHECK(ro.inputs.size() >= 3);
        int presses = 0;
        for (auto const& in : ro.inputs) presses += in.down ? 1 : 0;
        CHECK(presses >= 2);   // the ground jump and the orb
    }
    ReferenceRun ro2 = runSearch(orbs);
    CHECK(ro2.trajectoryDigest == ro.trajectoryDigest);

    // 7 spikes = 210 units: even the orb cannot carry the cube past the last spike (it can
    // launch from x <= 460 and reaches ~134 units: ~594 < the 630 needed) -> an honest unsolved span
    World impossible = simworld::base(1500.f);
    for (int i = 0; i < 7; ++i) impossible.objects.push_back(simworld::spike(400.f + 30.f * i));
    impossible.objects.push_back(simworld::orb(430.f, 75.f));
    simworld::finalize(impossible);
    ReferenceRun ri = runSearch(impossible);
    print("orbs impossible", ri);
    CHECK(ri.reachedEnd);
    CHECK(!ri.completed);
    CHECK(!ri.unsolved.empty());
    if (!ri.unsolved.empty()) CHECK(ri.unsolved.front().x0 < 400.f && ri.unsolved.front().x1 > 400.f);
    CHECK(runSearch(impossible).trajectoryDigest == ri.trajectoryDigest);
    // Engine defect found 2026-10-02 (engine.cpp save() line ~641 / restore() line ~652): restore(save(x)) is not
    // an identity around ring state - a restored state lets the next press ring-jump rings overlapped in the
    // previous step, a plain step() does not (off by one). The beam lives in restored states, so its committed
    // inputs must reproduce on a plain replay; the search counts every miss. Passes once the engine round-trips.
    CHECK_MSG(ro.replayMismatches == 0, "the committed orb-run inputs do not reproduce on a plain replay (engine save/restore identity)");
    if (ro.replayMismatches == 0) {
        size_t total = 0;
        for (auto const& seg : ro.segments) {
            CHECK(seg.trajectory.size() == static_cast<size_t>(seg.ticks));   // every committed segment replays to its full length
            total += seg.trajectory.size();
        }
        CHECK(ro.ticks.size() == total);
    }
}

void testBudgetsAndStop() {
    SECTION("a per-call tick budget returns early and resumes; stop() ends with an unsolved span");
    World w = simworld::tripleSpikeWorld(300.f, 1500.f);
    w.objects.push_back(simworld::spike(900.f));
    simworld::finalize(w);
    Search s(&w, {});
    bool done = s.run([] { return true; }, 300);
    CHECK(!done);
    CHECK(!s.done());
    CHECK(s.progress() < 1.0);
    CHECK(s.ticksSimulated() >= 300);
    int calls = 0;
    while (!s.done() && calls++ < 100000) s.run([] { return true; }, 5000);
    CHECK(s.done());
    CHECK(s.result().completed);
    ReferenceRun full = runSearch(w);
    CHECK(full.trajectoryDigest == s.result().trajectoryDigest);   // resumption changes nothing

    SECTION("mayContinue() = false pauses after the current beam tick");
    Search p(&w, {});
    int polls = 0;
    bool d = p.run([&] { return ++polls <= 3; }, 0);
    CHECK(!d);
    CHECK(polls == 4);
    d = p.run([] { return true; }, 0);
    CHECK(d);
    CHECK(p.result().trajectoryDigest == full.trajectoryDigest);

    SECTION("stop(budget) commits what exists and marks the rest unsolved");
    Search q(&w, {});
    q.run([] { return true; }, 2000);
    CHECK(!q.done());
    q.stop("budget");
    CHECK(q.done());
    ReferenceRun const& r = q.result();
    CHECK(!r.completed);
    CHECK(!r.reachedEnd);
    CHECK(r.unsolved.size() == 1);
    if (!r.unsolved.empty()) {
        CHECK(r.unsolved.front().mechanic == "budget");
        CHECK_NEAR(r.unsolved.front().x1, 1500.f, 1e-3);
    }
    CHECK(r.solvedPercent < 100.0);

    SECTION("maxTicks stops the search by itself");
    SearchConfig tiny;
    tiny.maxTicks = 500;
    Search t(&w, tiny);
    CHECK(t.run([] { return true; }, 0));
    CHECK(t.done());
    CHECK(!t.result().completed);
    CHECK(!t.result().unsolved.empty());
}

bool hasLine(std::vector<std::string> const& dbg, char const* text) {
    for (auto const& l : dbg)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

void testRestartStates() {
    SECTION("H1: a 3x speed portal inside a skipped span: the segment after it runs at 3x (state from the objects with x <= the restart)");
    {
        World w = simworld::base(3000.f);
        simworld::addUnsupported(w, 500.f, 1100.f, "toggle_trigger");
        w.objects.push_back(simworld::speedChange(700.f, Speed::Triple));
        simworld::finalize(w);
        std::vector<std::string> dbg;
        ReferenceRun r = runSearch(w, {}, &dbg);
        for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
        CHECK(r.segments.size() == 2);
        if (r.segments.size() == 2) {
            ReferenceSegment const& b = r.segments.back();
            CHECK(b.start.speed == Speed::Triple);
            CHECK(b.syntheticStart && b.startKind == "synthetic");
            CHECK(b.trajectory.size() >= 2);
            if (b.trajectory.size() >= 2) CHECK_NEAR(b.trajectory[1].x - b.trajectory[0].x, unitsPerTick(Speed::Triple), 1e-3);
            CHECK(b.completed);
            // M2: the gap is the speed-integrated time over [x1 of segment 1, x0 of segment 2]
            ReferenceSegment const& a = r.segments.front();
            double const gap = (700.0 - a.x1) / unitsPerTick(Speed::Normal) + (b.x0 - 700.0) / unitsPerTick(Speed::Triple);
            CHECK(std::abs(b.gapTicksBefore - static_cast<int>(std::llround(gap))) <= 0);
            CHECK(b.tickOffset == a.tickOffset + a.ticks + b.gapTicksBefore);
            CHECK(r.ticks.back().step == b.tickOffset + b.ticks);
        }
        CHECK(hasLine(dbg, "synthetic start x 1160: cube speed 3"));
    }

    SECTION("H1: a ship portal inside the span under a ceiling: the restart is a ship in the corridor's middle");
    {
        World w = simworld::base(3000.f);
        w.ceilingY = 390.f;
        simworld::addUnsupported(w, 500.f, 1100.f, "move_trigger");
        w.objects.push_back(simworld::portal(700.f, Gamemode::Ship));
        simworld::finalize(w);
        std::vector<std::string> dbg;
        ReferenceRun r = runSearch(w, {}, &dbg);
        for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
        CHECK(r.segments.size() == 2);
        if (r.segments.size() == 2) {
            ReferenceSegment const& b = r.segments.back();
            CHECK(b.start.mode == Gamemode::Ship);
            CHECK_NEAR(b.start.y, 240.0, 1.0);   // (90 + 390) / 2
            CHECK(b.completed);
        }
    }

    SECTION("H1: a StartPos at / after the span end wins over a synthetic start");
    {
        World w = simworld::base(3000.f);
        simworld::addUnsupported(w, 500.f, 1100.f, "dual_portal");
        StartState sp;
        sp.x = 1150.f;
        sp.y = 105.f;
        sp.mode = Gamemode::Ball;
        sp.speed = Speed::Double;
        w.startPositions.push_back(sp);
        StartState inside = sp;   // a StartPos inside the span is never used
        inside.x = 900.f;
        w.startPositions.insert(w.startPositions.begin(), inside);
        simworld::finalize(w);
        std::vector<std::string> dbg;
        ReferenceRun r = runSearch(w, {}, &dbg);
        CHECK(r.segments.size() == 2);
        if (r.segments.size() == 2) {
            ReferenceSegment const& b = r.segments.back();
            CHECK(b.startKind == "startpos" && !b.syntheticStart);
            CHECK(b.start.x == 1150.f && b.start.mode == Gamemode::Ball && b.start.speed == Speed::Double);
        }
        CHECK(hasLine(dbg, "startpos start x 1150"));
    }

    SECTION("H1: the floor at the restart is all spikes: the first candidate that survives the probe is the platform top");
    {
        World w = simworld::base(2400.f);
        simworld::addUnsupported(w, 500.f, 900.f, "rotate_trigger");
        for (float x = 990.f; x < 1500.f; x += 12.f) w.objects.push_back(simworld::spike(x));   // the floor ahead of x 960
        for (float x = 885.f; x < 1530.f; x += 30.f) w.objects.push_back(simworld::block(x, 60.f));   // tops at 180
        simworld::finalize(w);
        std::vector<std::string> dbg;
        ReferenceRun r = runSearch(w, {}, &dbg);
        for (auto const& l : dbg) std::printf("    %s\n", l.c_str());
        CHECK(r.segments.size() >= 2);
        if (r.segments.size() >= 2) {
            ReferenceSegment const& b = r.segments[1];
            CHECK_NEAR(b.start.x, 960.0, 1e-3);
            CHECK_NEAR(b.start.y, 195.0, 1e-3);   // 90 + 60 + 30 + 15
            CHECK(b.syntheticStart);
            CHECK(b.ticks > 100);                 // it runs along the platform instead of dying at once
        }
        CHECK(hasLine(dbg, "(candidate 2 of 2)"));
    }
}

}  // namespace

int main() {
    testEmptyWorld();
    testSpike();
    testWall();
    testSolidWallPhysics();
    testDeterminism();
    testUnsupported();
    testOrbsAndPortals();
    testBudgetsAndStop();
    testRestartStates();
    return gprl::test::finish("sim_search_tests");
}
