// Physics verification (core/sim/verify): an attempt produced by the engine itself verifies
// 100 %; one tick's y perturbed by 2 units diverges in that bin only; a perturbation beyond the
// stop distance makes the rest `after_divergence` (counted); bins inside an unsupported span are
// excluded; discrete mismatches name their field; mergeVerify over attempts.
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../core/sim/engine.hpp"
#include "../core/sim/search.hpp"
#include "../core/sim/verify.hpp"
#include "sim_analysis_world.hpp"

using namespace gprl::sim;

namespace {

RecordedAttempt attemptFromEngine(World const& w, std::vector<RecordedInput> const& inputs) {
    RecordedAttempt a;
    a.attemptIndex = 1;
    a.start = w.start;
    a.inputs = inputs;
    a.ticks = replayTrajectory(w, w.start, inputs, 100000);
    a.endStep = a.ticks.empty() ? 0 : a.ticks.back().step;
    a.died = !a.ticks.empty() && a.ticks.back().dead;
    a.completed = !a.ticks.empty() && !a.ticks.back().dead && a.ticks.back().x >= w.endX - 2.f;
    a.endPercent = a.ticks.empty() ? 0.f : a.ticks.back().x / w.endX * 100.f;
    return a;
}

int binsWithTicks(VerifyResult const& v) {
    int n = 0;
    for (auto const& b : v.bins)
        if (b.ticks > 0 && !b.unsupported) ++n;
    return n;
}

int verifiedBins(VerifyResult const& v) {
    int n = 0;
    for (auto const& b : v.bins)
        if (b.ticks > 0 && !b.unsupported && b.verified) ++n;
    return n;
}

World world() {
    World w = simworld::tripleSpikeWorld(300.f, 1200.f);
    w.objects.push_back(simworld::spike(800.f));
    simworld::finalize(w);
    return w;
}

std::vector<RecordedInput> solve(World const& w) {
    Search s(&w, {});
    CHECK(s.run([] { return true; }, 0));
    CHECK(s.result().completed);
    return s.result().segments.front().inputs;
}

void testExact() {
    SECTION("the engine's own attempt verifies 100 %");
    World w = world();
    auto inputs = solve(w);
    RecordedAttempt a = attemptFromEngine(w, inputs);
    CHECK(a.completed);
    VerifyResult v = verifyAttempt(w, a, 0.5);
    std::printf("  exact: %d ticks compared, %d bins with ticks, share %.3f\n", v.ticksCompared, binsWithTicks(v), v.verifiedShare);
    CHECK(v.attempts == 1);
    CHECK(v.ticksCompared == static_cast<int>(a.ticks.size()));
    CHECK(v.bins.size() == 50);
    CHECK(binsWithTicks(v) >= 40);
    CHECK(verifiedBins(v) == binsWithTicks(v));
    CHECK_NEAR(v.verifiedShare, 1.0, 1e-12);
    CHECK_NEAR(v.tolerancePosition, 0.5, 1e-12);
    for (auto const& b : v.bins) {
        if (b.ticks <= 0) continue;
        CHECK(b.reason.empty());
        CHECK(b.firstDivergenceStep == 0);
        CHECK(b.maxError <= 0.5f);
    }
    CHECK(verifyBinIndex(0.0) == 0 && verifyBinIndex(1.99) == 0 && verifyBinIndex(2.0) == 1 && verifyBinIndex(99.9) == 49 && verifyBinIndex(100.0) == 49);
}

void testPerturbed() {
    SECTION("one tick's y perturbed by 2 units: that bin diverges (position), every other bin stays verified");
    World w = world();
    auto inputs = solve(w);
    RecordedAttempt a = attemptFromEngine(w, inputs);
    size_t const k = a.ticks.size() / 2;
    a.ticks[k].y += 2.f;
    VerifyResult v = verifyAttempt(w, a, 0.5);
    int const expectBin = verifyBinIndex(a.ticks[k].x / w.endX * 100.0);
    std::printf("  perturbed: bin %d, share %.3f\n", expectBin, v.verifiedShare);
    CHECK(!v.bins[static_cast<size_t>(expectBin)].verified);
    CHECK(v.bins[static_cast<size_t>(expectBin)].reason == "position");
    CHECK(v.bins[static_cast<size_t>(expectBin)].firstDivergenceStep == a.ticks[k].step);
    CHECK(v.bins[static_cast<size_t>(expectBin)].maxError >= 1.9f);
    CHECK(verifiedBins(v) == binsWithTicks(v) - 1);
    CHECK_NEAR(v.verifiedShare, static_cast<double>(binsWithTicks(v) - 1) / binsWithTicks(v), 1e-12);
    CHECK(v.ticksCompared == static_cast<int>(a.ticks.size()));   // 2 units < the 5-unit stop: the replay continues

    SECTION("a 6-unit divergence stops the replay: later bins are after_divergence and count as diverged");
    RecordedAttempt b = attemptFromEngine(w, inputs);
    size_t const j = b.ticks.size() / 3;
    b.ticks[j].x += 6.f;
    VerifyResult vb = verifyAttempt(w, b, 0.5);
    int const stopBin = verifyBinIndex(b.ticks[j].x / w.endX * 100.0);
    CHECK(!vb.bins[static_cast<size_t>(stopBin)].verified);
    CHECK(vb.bins[static_cast<size_t>(stopBin)].reason == "position");
    CHECK(vb.ticksCompared < static_cast<int>(b.ticks.size()));
    bool laterAllAfter = true;
    int later = 0;
    for (size_t i = static_cast<size_t>(stopBin) + 1; i < vb.bins.size(); ++i) {
        if (vb.bins[i].ticks <= 0) continue;
        ++later;
        if (vb.bins[i].verified || vb.bins[i].reason != "after_divergence") laterAllAfter = false;
    }
    CHECK(later > 0);
    CHECK(laterAllAfter);
    CHECK(vb.verifiedShare < v.verifiedShare);
    // bins before the stop stay verified
    bool earlierOk = true;
    for (int i = 0; i < stopBin; ++i)
        if (vb.bins[static_cast<size_t>(i)].ticks > 0 && !vb.bins[static_cast<size_t>(i)].verified) earlierOk = false;
    CHECK(earlierOk);

    SECTION("a discrete mismatch names its field");
    RecordedAttempt c = attemptFromEngine(w, inputs);
    c.ticks[k].mode = Gamemode::Ship;
    VerifyResult vc = verifyAttempt(w, c, 0.5);
    CHECK(vc.bins[static_cast<size_t>(expectBin)].reason == "gamemode");
    RecordedAttempt d = attemptFromEngine(w, inputs);
    d.ticks[k].yVelocity += 1.f;
    VerifyResult vd = verifyAttempt(w, d, 0.5);
    CHECK(vd.bins[static_cast<size_t>(expectBin)].reason == "y_velocity");

    SECTION("a wider tolerance accepts the 2-unit perturbation");
    VerifyResult wide = verifyAttempt(w, a, 3.0);
    CHECK_NEAR(wide.verifiedShare, 1.0, 1e-12);
}

void testUnsupportedAndMerge() {
    SECTION("bins overlapping an unsupported span are excluded from the share");
    World w = world();
    auto inputs = solve(w);
    RecordedAttempt a = attemptFromEngine(w, inputs);
    World u = w;
    simworld::addUnsupported(u, 500.f, 620.f, "move_trigger");   // 41.7 % .. 51.7 %: bins 20..25
    size_t const k = a.ticks.size() / 2;                           // the middle tick lies near x = 600: inside the span
    RecordedAttempt p = a;
    p.ticks[k].y += 2.f;
    VerifyResult v = verifyAttempt(u, p, 0.5);
    int const bin = verifyBinIndex(p.ticks[k].x / w.endX * 100.0);
    std::printf("  unsupported: perturbed bin %d (x %.0f), share %.3f\n", bin, static_cast<double>(p.ticks[k].x), v.verifiedShare);
    int unsupportedBins = 0;
    for (auto const& b : v.bins)
        if (b.unsupported) ++unsupportedBins;
    CHECK(unsupportedBins >= 5 && unsupportedBins <= 8);
    if (p.ticks[k].x >= 500.f && p.ticks[k].x < 620.f) {
        CHECK(v.bins[static_cast<size_t>(bin)].unsupported);
        CHECK_NEAR(v.verifiedShare, 1.0, 1e-12);   // the divergence inside the span is expected and not counted
    }
    VerifyResult plain = verifyAttempt(w, p, 0.5);
    CHECK(binsWithTicks(v) < binsWithTicks(plain));

    SECTION("mergeVerify: a bin is verified only when every attempt verified it; ticks add up");
    RecordedAttempt a2 = attemptFromEngine(w, inputs);
    VerifyResult v1 = verifyAttempt(w, a, 0.5);
    VerifyResult v2 = verifyAttempt(w, a2, 0.5);
    VerifyResult m = mergeVerify({v1, v2});
    CHECK(m.attempts == 2);
    CHECK(m.ticksCompared == v1.ticksCompared + v2.ticksCompared);
    CHECK_NEAR(m.verifiedShare, 1.0, 1e-12);
    RecordedAttempt a3 = attemptFromEngine(w, inputs);
    a3.ticks[k].y += 2.f;
    VerifyResult v3 = verifyAttempt(w, a3, 0.5);
    VerifyResult m2 = mergeVerify({v1, v3});
    int const bin3 = verifyBinIndex(a3.ticks[k].x / w.endX * 100.0);
    CHECK(!m2.bins[static_cast<size_t>(bin3)].verified);
    CHECK(m2.bins[static_cast<size_t>(bin3)].reason == "position");
    CHECK(m2.bins[static_cast<size_t>(bin3)].ticks == v1.bins[static_cast<size_t>(bin3)].ticks + v3.bins[static_cast<size_t>(bin3)].ticks);
    CHECK(verifiedBins(m2) == binsWithTicks(m2) - 1);
    CHECK(mergeVerify({}).attempts == 0);

    SECTION("an attempt that died verifies up to its death");
    RecordedInput lateIn;
    lateIn.step = inputs.front().step + 40;
    lateIn.down = true;
    RecordedAttempt dead = attemptFromEngine(w, {lateIn});
    CHECK(dead.died);
    VerifyResult vdead = verifyAttempt(w, dead, 0.5);
    CHECK(vdead.ticksCompared == static_cast<int>(dead.ticks.size()));
    CHECK_NEAR(vdead.verifiedShare, 1.0, 1e-12);
    CHECK(binsWithTicks(vdead) < binsWithTicks(v1));
}

void testSpanReseed() {
    SECTION("H2: the real run does something unmodelled inside a span (a pad the simulator lacks): the replay re-seeds at the span exit and verifies the rest");
    // "real" world: a yellow pad inside [700, 1300) launches the player; the simulator's world has
    // no pad there but marks the stretch unsupported (the mechanic it does not model)
    World real = simworld::base(2400.f);
    real.objects.push_back(simworld::spike(400.f));
    real.objects.push_back(simworld::spike(1700.f));
    SimObject pad;
    pad.objectId = 35;
    pad.gdType = 8;
    pad.kind = ObjKind::Pad;
    pad.pad = PadKind::Yellow;
    pad.x = 760.f;
    pad.y = 92.f;
    pad.rx = 747.5f;
    pad.ry = 90.f;
    pad.rw = 25.f;
    pad.rh = 4.f;
    real.objects.push_back(pad);
    simworld::finalize(real);
    World sim = real;
    sim.objects.erase(std::remove_if(sim.objects.begin(), sim.objects.end(), [](SimObject const& o) { return o.kind == ObjKind::Pad; }), sim.objects.end());
    simworld::addUnsupported(sim, 700.f, 1300.f, "move_trigger");
    simworld::finalize(sim);
    // two taps (x moves 1.298 per tick at 1x): over the spike at 400 and the one at 1700; the cube
    // runs over the pad on the ground in between
    std::vector<RecordedInput> inputs;
    for (int step : {241, 1242}) {
        RecordedInput p;
        p.step = step;
        p.down = true;
        inputs.push_back(p);
        p.step = step + 2;
        p.down = false;
        inputs.push_back(p);
    }
    RecordedAttempt a = attemptFromEngine(real, inputs);
    CHECK_MSG(a.completed, a.ticks.empty() ? std::string("no ticks") : "ended at x " + std::to_string(a.ticks.back().x));
    // the pad really changed the run inside the span
    auto plain = replayTrajectory(sim, sim.start, inputs, 100000);
    double maxDiff = 0.0;
    for (size_t i = 0; i < a.ticks.size() && i < plain.size(); ++i)
        if (a.ticks[i].x >= 700.f && a.ticks[i].x < 1300.f) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(a.ticks[i].y - plain[i].y)));
    CHECK_MSG(maxDiff > 20.0, "the pad did not change the recorded run: " + std::to_string(maxDiff));
    VerifyResult v = verifyAttempt(sim, a, 0.5);
    std::printf("  span re-seed: %d re-seeds, %d counted bins, share %.3f, %d ticks compared\n", v.reseeds, v.countedBins, v.verifiedShare, v.ticksCompared);
    CHECK(v.reseeds == 1);
    CHECK(v.countedBins == binsWithTicks(v));
    CHECK_NEAR(v.verifiedShare, 1.0, 1e-12);
    // the bins past the span were compared (not after_divergence) and verified
    int const exitBin = verifyBinIndex(1300.0 / sim.endX * 100.0);
    int compared = 0;
    for (size_t i = static_cast<size_t>(exitBin) + 1; i < v.bins.size(); ++i) {
        if (v.bins[i].ticks <= 0) continue;
        ++compared;
        CHECK(v.bins[i].verified && v.bins[i].reason.empty());
    }
    CHECK(compared >= 10);
    // a divergence BEFORE the span still stops the comparison only until the span exit
    RecordedAttempt d = a;
    size_t const k = 60;   // x ~ 78: well before the span
    for (size_t i = k; i < d.ticks.size() && d.ticks[i].x < 700.f; ++i) d.ticks[i].y += 8.f;
    VerifyResult vd = verifyAttempt(sim, d, 0.5);
    int const divBin = verifyBinIndex(d.ticks[k].x / sim.endX * 100.0);
    CHECK(!vd.bins[static_cast<size_t>(divBin)].verified);
    bool afterBefore = false, verifiedAfter = true;
    for (size_t i = 0; i < vd.bins.size(); ++i) {
        VerifyBin const& b = vd.bins[i];
        if (b.ticks <= 0 || b.unsupported) continue;
        if (static_cast<int>(i) > divBin && b.from < 700.0 / sim.endX * 100.0 && b.reason == "after_divergence") afterBefore = true;
        if (static_cast<int>(i) > exitBin && !b.verified) verifiedAfter = false;
    }
    CHECK(afterBefore);
    CHECK(verifiedAfter);
    CHECK(vd.reseeds == 1);

    SECTION("H2: a span that ends while the player is mid-jump: the airborne re-seed (vy, boosted) keeps verifying");
    World simAir = real;
    simAir.objects.erase(std::remove_if(simAir.objects.begin(), simAir.objects.end(), [](SimObject const& o) { return o.kind == ObjKind::Pad; }),
                         simAir.objects.end());
    simworld::addUnsupported(simAir, 700.f, 1250.f, "move_trigger");
    simworld::finalize(simAir);
    std::vector<RecordedInput> airInputs = inputs;
    RecordedInput jp;
    jp.step = 940;   // x ~ 1220: airborne across the span's end at 1250
    jp.down = true;
    airInputs.push_back(jp);
    jp.step = 942;
    jp.down = false;
    airInputs.push_back(jp);
    std::stable_sort(airInputs.begin(), airInputs.end(), [](RecordedInput const& p, RecordedInput const& q) { return p.step < q.step; });
    RecordedAttempt air = attemptFromEngine(real, airInputs);
    CHECK(air.completed);
    bool airborneAtExit = false;
    for (auto const& t : air.ticks)
        if (t.x >= 1250.f) {
            airborneAtExit = !t.onGround && t.yVelocity > 0.f;
            break;
        }
    CHECK(airborneAtExit);
    VerifyResult va = verifyAttempt(simAir, air, 0.5);
    std::printf("  airborne re-seed: %d re-seeds, share %.3f\n", va.reseeds, va.verifiedShare);
    CHECK(va.reseeds == 1);
    CHECK_NEAR(va.verifiedShare, 1.0, 1e-12);

    SECTION("M5: an attempt whose every tick lies in unsupported bins counts no bin (0 / 0): countedBins 0");
    World early = simworld::base(2400.f);
    simworld::addUnsupported(early, 10.f, 700.f, "toggle_trigger");
    simworld::finalize(early);
    RecordedAttempt e;
    e.attemptIndex = 1;
    e.start = early.start;
    e.ticks = replayTrajectory(early, early.start, {}, 400);   // ends at x ~ 519: inside the span
    VerifyResult ve = verifyAttempt(early, e, 0.5);
    CHECK(ve.countedBins == 0);
    CHECK(ve.verifiedShare == 0.0);
    CHECK(ve.reseeds == 0);
    VerifyResult vm = mergeVerify({ve, ve});
    CHECK(vm.countedBins == 0 && vm.attempts == 2);
}

}  // namespace

int main() {
    testExact();
    testPerturbed();
    testUnsupportedAndMerge();
    testSpanReseed();
    return gprl::test::finish("sim_verify_tests");
}
