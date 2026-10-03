// Live-state snapshot + isolation guard host tests (docs/LIVE_ISOLATION_DESIGN.md §3; owner spec
// docs/LIVE_ISOLATION_SPEC.md): no difference -> no field listed; a changed camera Y / gravity /
// object byte -> the owner's exact block (field / before / after) with bitwise float compares;
// the list is bounded with the total kept; the guard checks every block, samples when a check is
// too expensive (never off), escalates to every clone step after a breach and clears the breaker
// per attempt only.
#include "test_util.hpp"

#include "../core/solver/isolation_guard.hpp"
#include "../core/solver/live_state.hpp"

#include <cmath>
#include <string>

using namespace gprl::solver;

namespace {

live::Snapshot base() {
    live::Snapshot s;
    s.p1.posX = 1234.5f; s.p1.posY = 105.f; s.p1.gamemode = 1; s.p1.playerSpeed = 0.9f;
    s.p2.posX = 1234.5f; s.p2.posY = 195.f; s.p2.upsideDown = true; s.p2.secondPlayer = true;
    s.camera.posY = 42.25f; s.camera.follow17Y = 300.f; s.camera.freeMode = false;
    s.layer.dualMode = true; s.layer.lastPortal1Id = 13; s.layer.lastPortal1Ptr = 0x1234;
    s.objectCount = 3;
    for (int i = 0; i < 3; ++i) {
        auto& o = s.objects[static_cast<size_t>(i)];
        o.id = 1329 + i; o.uid = 500 + i; o.x = 1300.f + 30.f * i;
    }
    s.objects[1].slot1 = 1; s.objects[1].activated = 1;
    s.objectsHash = live::objectsHash(s);
    return s;
}

void testNoDifference() {
    SECTION("identical snapshots: no field, total 0; NaN equals itself bitwise");
    auto a = base();
    auto b = a;
    int total = -1;
    auto d = live::compare(a, b, 12, &total);
    CHECK(d.empty());
    CHECK(total == 0);
    a.camera.easing = std::nanf("");
    b.camera.easing = a.camera.easing;
    CHECK(live::compare(a, b).empty());
}

void testOwnerBlock() {
    SECTION("camera Y, player 2 gravity, a gamemode and an object byte are listed in the owner's format");
    auto a = base();
    auto b = a;
    b.camera.posY = 42.75f;
    b.p2.upsideDown = false;
    b.p1.gamemode = 0;
    b.objects[1].slot1 = 0;
    int total = 0;
    auto d = live::compare(a, b, 12, &total);
    CHECK(total == 4);
    CHECK(d.size() == 4);
    // players first (1 then 2), then camera, layer, objects
    CHECK(d[0].field == "player1.gamemode" && d[0].before == "ship" && d[0].after == "cube");
    CHECK(d[1].field == "player2.gravity" && d[1].before == "inverted" && d[1].after == "normal");
    CHECK(d[2].field == "camera.position.y" && d[2].before == "42.25" && d[2].after == "42.75");
    CHECK(d[3].field == "objects.activation");
    CHECK(d[3].before.find("#1330 (uid 501)") == 0);
    CHECK(d[3].before.find("slot1 1") != std::string::npos && d[3].after.find("slot1 0") != std::string::npos);
    CHECK(live::ownerBlock(d[1]) == "LIVE_STATE_MUTATION_DETECTED\nfield: player2.gravity\nbefore: inverted\nafter: normal");
}

void testBitwiseAndBounds() {
    SECTION("floats compare bitwise (-0 vs +0 differs), pointer fields show as hex, the list is bounded and the total counts everything");
    auto a = base();
    auto b = a;
    b.p1.posY = -0.f; a.p1.posY = 0.f;
    CHECK(live::compare(a, b).size() == 1);
    b = a;
    b.layer.lastPortal1Ptr = 0x5678;
    auto d = live::compare(a, b);
    CHECK(d.size() == 1 && d[0].field == "layer.lastPortal1.ptr" && d[0].before == "0x1234" && d[0].after == "0x5678");
    // 20 differing fields, 5 listed
    b = a;
    b.p1.posX += 1; b.p1.posY += 1; b.p1.mposX += 1; b.p1.mposY += 1; b.p1.yVelocity += 1; b.p1.held = true; b.p1.dead = true; b.p1.onGround = true;
    b.p2.posX += 1; b.p2.posY += 1; b.camera.zoom += 1; b.camera.angle += 1; b.camera.flip = 1.f; b.layer.dualMode = false; b.layer.items = 7;
    b.layer.betweenSteps = true; b.layer.checkpoints = 2; b.camera.tweens = 3; b.layer.mirror = 1.f; b.layer.playerDied = true;
    int total = 0;
    d = live::compare(a, b, 5, &total);
    CHECK(d.size() == 5);
    CHECK(total == 20);
    // a changed object count is one field, not per object
    b = a;
    b.objectCount = 2;
    d = live::compare(a, b, 12, &total);
    CHECK(d.size() == 1 && d[0].field == "objects.count" && d[0].before == "3" && d[0].after == "2");
    // the hash follows the bytes
    b = a;
    b.objects[0].powered = 1;
    b.objectsHash = live::objectsHash(b);
    CHECK(b.objectsHash != a.objectsHash);
    CHECK(std::string(live::kVersion) == "gprl-live-state/1");
    CHECK(std::string(live::gamemodeName(7)) == "swing" && std::string(live::gamemodeName(9)) == "?");
}

void testGuard() {
    SECTION("guard: every block by default, sampled when too expensive (never off), escalation after a breach, breaker per attempt");
    isolation::IsolationConfig cfg;
    cfg.maxCheckUsPerStep = 5.0;
    cfg.sampleEveryNBlocks = 4;
    cfg.minChecksBeforeFallback = 10;
    cfg.costEmaAlpha = 1.0;   // the EMA follows the last check exactly
    isolation::IsolationGuard g(cfg);
    g.setupLevel(isolation::CheckMode::EveryBlock);
    CHECK(g.mode() == isolation::CheckMode::EveryBlock && !g.breached() && g.sampleEvery() == 1);
    for (int i = 0; i < 10; ++i) {
        CHECK(g.beginBlock());
        CHECK(!g.noteCheck(1.0));
    }
    CHECK(g.blocks() == 10 && g.checks() == 10 && !g.fellBack());
    // one expensive check after the settling window -> sampling every 4th block
    CHECK(g.beginBlock());
    CHECK(g.noteCheck(9.0));
    CHECK(g.fellBack() && g.sampleEvery() == 4);
    int checked = 0;
    for (int i = 0; i < 8; ++i) if (g.beginBlock()) ++checked;
    CHECK(checked == 2);
    CHECK(g.skipped() == 6);
    // a breach: breaker on, mode escalates, sampling off (every step is checked now)
    g.onBreach();
    CHECK(g.breached() && g.mode() == isolation::CheckMode::EveryCloneStep && g.checksEveryStep() && g.sampleEvery() == 1);
    CHECK(!g.beginBlock());   // block mode is off in per-step mode
    CHECK(g.breaches() == 1);
    // the restart clears the breaker, not the escalation
    g.resetAttempt();
    CHECK(!g.breached() && g.mode() == isolation::CheckMode::EveryCloneStep);
    // a new level starts from the configured mode
    g.setupLevel(isolation::CheckMode::EveryBlock);
    CHECK(g.mode() == isolation::CheckMode::EveryBlock && g.blocks() == 0 && g.breaches() == 0);
    // configured per-step mode never checks blocks
    g.setupLevel(isolation::CheckMode::EveryCloneStep);
    CHECK(!g.beginBlock() && g.checksEveryStep());
    CHECK(std::string(isolation::name(isolation::CheckMode::EveryBlock)) == "every block");
    CHECK(std::string(isolation::kIsolationConfig.version) == "gprl-isolation/2");
    CHECK(isolation::kIsolationConfig.sampleEveryNBlocks > 1);   // the fallback is never "off"
}

}  // namespace

int main() {
    testNoDifference();
    testOwnerBlock();
    testBitwiseAndBounds();
    testGuard();
    return gprl::test::finish("live_state_tests");
}
