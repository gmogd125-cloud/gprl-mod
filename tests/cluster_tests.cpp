// cluster host tests (docs/TIMING_SOLVER_V2.md §2.7, §3.3, §5): the connectivity rules (measured,
// not guessed), forced breaks, the look-ahead gap, cluster ids / indices from connectedNext links,
// the id shape, and the SA job chunking (narrowest seed, consecutive linked candidates, at most 4
// members, span at most 96 ticks). Every loop is bounded by its input size (§3.7).
#include "test_util.hpp"

#include "../core/solver/cluster.hpp"
#include "../core/solver/sequence_adjusted.hpp"

#include <string>
#include <vector>

using namespace gprl::solver;
using namespace gprl::solver::cluster;

namespace {

ShiftOutcome out(double s, ShiftKind k, double after = 0.0, int laterFixed = 0) {
    ShiftOutcome o;
    o.nominalFrames = o.appliedFrames = s;
    o.kind = k;
    o.deathAfterFrames = after;
    o.rejoinAfterFrames = k == ShiftKind::Resynced ? after : kNaN;
    o.laterFixed = laterFixed;
    return o;
}

void testConnectedNext() {
    SECTION("connectedNext: yes when a passing shift had not re-joined before t_{i+1} or a fail died downstream");
    std::vector<ShiftOutcome> wave = {out(-1, ShiftKind::Survived), out(1, ShiftKind::Survived), out(2, ShiftKind::Died, 80, 3)};
    ConnectInput in;
    in.haveOutcomes = true;
    in.frame = 100;
    in.horizonFrame = 230;
    in.nextFrame = 118;
    in.outcomes = &wave;
    auto c = connectedNext(in);
    CHECK(c.value == Tri::Yes && std::string(c.why) == "not_rejoined");
    // re-joined AFTER the next input: still connected
    std::vector<ShiftOutcome> late = {out(-1, ShiftKind::Resynced, 25), out(1, ShiftKind::Resynced, 30)};
    in.outcomes = &late;
    CHECK(connectedNext(in).value == Tri::Yes);
    // every shift re-joined BEFORE the next input and no downstream fail: its timing cannot reach i+1
    std::vector<ShiftOutcome> early = {out(-1, ShiftKind::Resynced, 5), out(1, ShiftKind::Resynced, 6), out(2, ShiftKind::Died, 3, 0)};
    in.outcomes = &early;
    auto n = connectedNext(in);
    CHECK(n.value == Tri::No && std::string(n.why) == "rejoined_before_next");
    // a downstream fail connects even when the passes re-joined early
    std::vector<ShiftOutcome> down = {out(-1, ShiftKind::Resynced, 5), out(2, ShiftKind::Died, 40, 1)};
    in.outcomes = &down;
    auto d = connectedNext(in);
    CHECK(d.value == Tri::Yes && std::string(d.why) == "downstream_fail");

    SECTION("forced breaks, the look-ahead gap, unknown outcomes");
    in.outcomes = &wave;
    in.forcedBreak = true;
    CHECK(connectedNext(in).value == Tri::No);
    in.forcedBreak = false;
    in.nextFrame = 260;   // beyond the look-ahead
    CHECK(std::string(connectedNext(in).why) == "gap_beyond_horizon");
    in.nextFrame = kNaN;
    CHECK(connectedNext(in).value == Tri::No);
    in.nextFrame = 118;
    in.haveOutcomes = false;
    CHECK(connectedNext(in).value == Tri::Unknown);
    in.haveOutcomes = true;
    std::vector<ShiftOutcome> none = {out(1, ShiftKind::NotTested)};
    in.outcomes = &none;
    CHECK(connectedNext(in).value == Tri::Unknown);
}

void testTracker() {
    SECTION("cluster ids: <attemptId>:<first member index>, 1-based index, prev / next from the links");
    ClusterTracker t;
    // inputs 1-3 connected, 4 alone, 5-6 connected, 7 unknown link to 6
    t.setConnectedNext(1, Tri::Yes);
    t.setConnectedNext(2, Tri::Yes);
    t.setConnectedNext(3, Tri::No);
    t.setConnectedNext(4, Tri::No);
    t.setConnectedNext(5, Tri::Yes);
    t.setConnectedNext(6, Tri::Unknown);
    auto r3 = t.ref("a0f35ce1b7-a8", 3);
    CHECK(r3.id == "a0f35ce1b7-a8:1" && r3.index == 3 && r3.connectedPrev == Tri::Yes && r3.connectedNext == Tri::No);
    auto r4 = t.ref("a0f35ce1b7-a8", 4);
    CHECK(r4.id == "a0f35ce1b7-a8:4" && r4.index == 1 && r4.connectedPrev == Tri::No);
    auto r6 = t.ref("a0f35ce1b7-a8", 6);
    CHECK(r6.id == "a0f35ce1b7-a8:5" && r6.index == 2 && r6.connectedNext == Tri::Unknown);
    auto r7 = t.ref("a0f35ce1b7-a8", 7);
    CHECK(r7.id == "a0f35ce1b7-a8:7" && r7.connectedPrev == Tri::Unknown);
    CHECK(t.ref("x", 1).connectedPrev == Tri::No);   // the first input has no predecessor
    CHECK(clusterIdOk(r3.id) && clusterIdOk("a:1") && !clusterIdOk("a b:1") && !clusterIdOk(":1") && !clusterIdOk("a:") && !clusterIdOk("a:1234567"));
    // a long connected wave: bounded walk (index strictly decreases), linear in the input count
    ClusterTracker longWave;
    for (int i = 1; i < 400; ++i) longWave.setConnectedNext(i, Tri::Yes);
    auto rl = longWave.ref("a", 400);
    CHECK(rl.id == "a:1" && rl.index == 400);
    t.reset();
    CHECK(t.connectedNext(1) == Tri::Unknown);
}

void testChunking() {
    SECTION("SA chunking: the narrowest seed, consecutive linked candidates, <= 4 members, span <= 96 ticks");
    std::vector<ChunkCandidate> ready = {
        {10, 3435, 5.0, Tri::Yes, 1}, {11, 3474, 4.0, Tri::Yes, 2}, {12, 3500, 4.0, Tri::Yes, 3}, {13, 3518, 6.0, Tri::Yes, 4},
        {14, 3535, 6.0, Tri::Yes, 5}, {15, 3548, 7.0, Tri::No, 6},  {16, 3566, 9.0, Tri::Yes, 7}, {20, 3700, 2.0, Tri::Yes, 8},
    };
    int seed = pickSeed(ready);
    CHECK(seed == 7);   // width 2, the narrowest
    auto lone = chunkFor(ready, static_cast<size_t>(seed), 4, 96.0);
    CHECK(lone.size() == 1);   // no neighbour index 19 / 21 among the candidates
    ready.pop_back();
    seed = pickSeed(ready);
    CHECK(seed == 1);   // width 4, older than #12
    auto c = chunkFor(ready, static_cast<size_t>(seed), 4, 96.0);
    CHECK(c.size() == 4);
    std::vector<int> idx;
    for (size_t k : c) idx.push_back(ready[k].index);
    CHECK((idx == std::vector<int>{10, 11, 12, 13}));
    // the span limit (96 ticks) and a measured break stop the growth
    auto span = chunkFor(ready, 0, 4, 60.0);
    CHECK(span.size() == 2);   // 3435 .. 3474 (39), 3500 would make 65 > 60
    auto brk = chunkFor(ready, 5, 4, 96.0);   // #15 is linked backward from #14, not forward (No)
    std::vector<int> b;
    for (size_t k : brk) b.push_back(ready[k].index);
    CHECK((b == std::vector<int>{12, 13, 14, 15}));
    CHECK(chunkFor(ready, 99, 4, 96.0).empty());
    CHECK(pickSeed({}) == -1);
}

void testSAQueueRank() {
    SECTION("Fable D10 saQueueRank: unmeasured positions first, then the narrowest local window, then the oldest; stable on full ties");
    // ordering
    CHECK(saQueueRank(false, 9.0, 50).before(saQueueRank(true, 1.0, 1)));    // an unmeasured place beats a narrower measured one
    CHECK(saQueueRank(false, 2.0, 50).before(saQueueRank(false, 3.0, 1)));   // inside a rank: narrowest first
    CHECK(saQueueRank(true, 2.0, 50).before(saQueueRank(true, 3.0, 1)));
    CHECK(saQueueRank(false, 4.0, 1).before(saQueueRank(false, 4.0, 2)));    // equal widths: the older first
    CHECK(!saQueueRank(false, 4.0, 2).before(saQueueRank(false, 4.0, 1)));
    // ties: neither is before the other (pickSeed keeps the earlier position in its list)
    CHECK(!saQueueRank(true, 4.0, 3).before(saQueueRank(true, 4.0, 3)));
    // pickSeed: the dense-wave case - the narrow vertices measured twice this level visit wait
    // behind a wider vertex nobody measured yet (it expired in the earlier attempts)
    std::vector<ChunkCandidate> ready = {
        {10, 3435, 2.0, Tri::Yes, 1, true}, {11, 3474, 3.0, Tri::Yes, 2, true}, {12, 3500, 6.0, Tri::Yes, 3, false}, {13, 3518, 5.0, Tri::Yes, 4, false},
    };
    CHECK(pickSeed(ready) == 3);   // unmeasured, narrower than #12
    ready[3].positionMeasured = true;
    CHECK(pickSeed(ready) == 2);   // the only unmeasured one
    ready[2].positionMeasured = true;
    CHECK(pickSeed(ready) == 0);   // all measured: narrowest first again (never skipped)
    // stability on a full tie: the earlier entry wins, whatever the order of evaluation
    std::vector<ChunkCandidate> tie = {{1, 100, 4.0, Tri::Yes, 7, false}, {2, 118, 4.0, Tri::Yes, 7, false}};
    CHECK(pickSeed(tie) == 0);
    CHECK(gprl::solver::kSA.saMaxPerPosition == 2);
}

void testPortals() {
    SECTION("portal kinds: forced breaks (gamemode / gravity / size / dual / teleport), transitions, gamemodeAfter");
    CHECK(portalKind(660) == PortalKind::Gamemode && breaksCluster(portalKind(660)) && transitionPortal(portalKind(660)));
    CHECK(portalKind(11) == PortalKind::Gravity && breaksCluster(portalKind(11)) && transitionPortal(portalKind(11)));
    CHECK(portalKind(99) == PortalKind::Size && transitionPortal(portalKind(101)));
    CHECK(breaksCluster(portalKind(286)) && !transitionPortal(portalKind(286)));
    CHECK(breaksCluster(portalKind(747)));
    CHECK(!breaksCluster(portalKind(201)) && !transitionPortal(portalKind(1334)));   // speed changes do not break a cluster
    CHECK(!breaksCluster(portalKind(45)) && portalKind(12345) == PortalKind::Other);
    gprl::Gamemode g = gprl::Gamemode::Cube;
    CHECK(portalGamemode(13, g) && g == gprl::Gamemode::Ship);
    CHECK(portalGamemode(1933, g) && g == gprl::Gamemode::Swing);
    CHECK(!portalGamemode(11, g));
}

}  // namespace

int main() {
    testConnectedNext();
    testTracker();
    testChunking();
    testSAQueueRank();
    testPortals();
    return gprl::test::finish("cluster_tests");
}
