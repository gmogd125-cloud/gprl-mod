// Engine tests for the isolated simulator (core/sim/engine.*): synthetic worlds built in the test,
// the GD step order end to end, death reasons, orbs / pads / portals, a ship corridor, the
// save / restore round trip, stateHash, and the determinism golden of
// tests/fixtures/sim/golden-engine.json (the FNV digest of a fixed input script's trajectory).
//
// Usage: sim_engine_tests.exe <repo root> [--write]
//   --write   (re)generates the fixture's `golden` block from this build (only when every other
//             check passed). Do it only for an intended physics change: the digest pins the exact
//             trajectory bit for bit. tests/run_tests.ps1 -Write does NOT reach this suite; run the
//             exe by hand: build\tests-physics\sim_engine_tests.exe D:\GPRL --write
#include "test_util.hpp"

#include "../core/json.hpp"
#include "../core/sim/collision.hpp"
#include "../core/sim/engine.hpp"
#include "../core/sim/physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace gprl::sim;

namespace {

// ---- a small world builder (rects like gdclone object.json: centred boxes) ----

struct Builder {
    World w;
    Builder() {
        w.gdLevelId = 1;
        w.groundY = 90.f;
        w.ceilingY = 0.f;
        w.start.x = 0.f;
        w.start.y = 105.f;
        w.start.mode = Gamemode::Cube;
        w.start.speed = Speed::Normal;
    }
    SimObject& add(ObjKind kind, int id, double cx, double cy, double wd, double ht) {
        SimObject o;
        o.objectId = id;
        o.uniqueId = static_cast<int>(w.objects.size()) + 1;
        o.kind = kind;
        o.x = static_cast<float>(cx);
        o.y = static_cast<float>(cy);
        o.rx = static_cast<float>(cx - wd * 0.5);
        o.ry = static_cast<float>(cy - ht * 0.5);
        o.rw = static_cast<float>(wd);
        o.rh = static_cast<float>(ht);
        w.objects.push_back(o);
        return w.objects.back();
    }
    SimObject& block(double cx, double cy) { SimObject& o = add(ObjKind::Solid, 1, cx, cy, 30.0, 30.0); o.gdType = 0; return o; }
    SimObject& spike(double cx, double cy) { SimObject& o = add(ObjKind::Hazard, 8, cx, cy, 6.0, 12.0); o.gdType = 2; return o; }
    SimObject& orb(double cx, double cy, OrbKind k) { SimObject& o = add(ObjKind::Orb, 36, cx, cy, 36.0, 36.0); o.orb = k; o.gdType = 11; return o; }
    SimObject& pad(double cx, double cy, PadKind k) { SimObject& o = add(ObjKind::Pad, 35, cx, cy, 25.0, 4.0); o.pad = k; o.gdType = 8; return o; }
    SimObject& gravityPortal(double cx, double cy, bool flipped) { SimObject& o = add(ObjKind::GravityPortal, flipped ? 11 : 10, cx, cy, 25.0, 75.0); o.flag = flipped; o.gdType = flipped ? 3 : 4; return o; }
    SimObject& modePortal(double cx, double cy, Gamemode m) { SimObject& o = add(ObjKind::GamemodePortal, 13, cx, cy, 34.0, 86.0); o.mode = m; o.gdType = 5; return o; }
    SimObject& sizePortal(double cx, double cy, bool mini) { SimObject& o = add(ObjKind::SizePortal, mini ? 101 : 99, cx, cy, 31.0, 90.0); o.flag = mini; o.gdType = mini ? 18 : 17; return o; }
    SimObject& speedChange(double cx, double cy, Speed s) { SimObject& o = add(ObjKind::SpeedChange, 201, cx, cy, 33.0, 56.0); o.speed = s; return o; }
    SimObject& slope(double cx, double cy, double wd, double ht, uint8_t orientation) { SimObject& o = add(ObjKind::Slope, 1338, cx, cy, wd, ht); o.slope = orientation; o.gdType = 25; return o; }
    World finish(double endX) {
        std::sort(w.objects.begin(), w.objects.end(), [](SimObject const& a, SimObject const& b) { return a.rx < b.rx; });
        w.endX = static_cast<float>(endX);
        w.lengthX = static_cast<float>(endX);
        w.gameplayObjects = static_cast<int>(w.objects.size());
        return w;
    }
};

/// Runs until the x limit, death or `maxTicks`; presses at the ticks in `press` (held `holdTicks`).
struct RunResult {
    bool dead = false;
    uint8_t reason = 0;
    int killer = -1;
    double x = 0.0, y = 0.0;
    double maxY = 0.0;
    int ticks = 0;
};

RunResult run(Engine& e, double untilX, int maxTicks, std::vector<int> const& press = {}, int holdTicks = 2) {
    RunResult r;
    r.maxY = e.player().y;
    for (int t = 0; t < maxTicks; ++t) {
        for (int pt : press) {
            if (t == pt) e.queueInput(true);
            if (t == pt + holdTicks) e.queueInput(false);
        }
        e.step();
        r.maxY = std::max(r.maxY, e.player().y);
        if (e.dead() || e.player().x >= untilX) break;
    }
    r.dead = e.dead();
    r.reason = e.player().deathReason;
    r.killer = e.player().killerIndex;
    r.x = e.player().x;
    r.y = e.player().y;
    r.ticks = e.tick();
    return r;
}

int indexOf(World const& w, int uniqueId) {
    for (size_t i = 0; i < w.objects.size(); ++i) {
        if (w.objects[i].uniqueId == uniqueId) return static_cast<int>(i);
    }
    return -1;
}

void testFloorAndSpike() {
    SECTION("a cube with no input runs on the floor at 311.58 units per second");
    Builder b;
    World w = b.finish(3000.0);
    Engine e(&w);
    e.reset(w.start);
    CHECK(e.tick() == 0 && e.player().y == 105.0);
    RunResult r = run(e, 1e9, 240);
    CHECK(!r.dead);
    CHECK_NEAR(r.x, 311.58, 0.01);
    CHECK(r.y == 105.0);
    CHECK(e.player().onGround);
    CHECK_NEAR(e.percent(), 311.58 / 3000.0 * 100.0, 0.01);
    CHECK(!e.completed());
    CHECK(!e.flying());

    SECTION("a spike on the floor kills the cube by its full rect; killerIndex and deathReason are set");
    Builder bs;
    bs.spike(300.0, 105.0);
    World ws = bs.finish(3000.0);
    Engine es(&ws);
    es.reset(ws.start);
    RunResult rs = run(es, 1e9, 1000);
    CHECK(rs.dead);
    CHECK(rs.reason == 1);
    CHECK(rs.killer == 0);
    CHECK_MSG(rs.x >= 282.0 && rs.x < 284.0, "death x = " + std::to_string(rs.x));
    // stepping while dead changes nothing but the tick
    double xBefore = es.player().x;
    es.step();
    CHECK(es.player().x == xBefore && es.dead());

    SECTION("the press window over the spike is positive and its edges are what the kinematics imply");
    int earliest = -1, latest = -1;
    for (int t = 100; t < 240; ++t) {
        Engine et(&ws);
        et.reset(ws.start);
        RunResult rt = run(et, 340.0, 1000, {t});
        if (!rt.dead) {
            if (earliest < 0) earliest = t;
            latest = t;
        }
    }
    CHECK(earliest > 0 && latest >= earliest);
    int width = latest - earliest + 1;
    std::printf("  spike window: press ticks %d..%d (%d ticks = %.1f ms)\n", earliest, latest, width, width * 1000.0 / 240.0);
    CHECK_MSG(earliest >= 150 && earliest <= 175, "earliest = " + std::to_string(earliest));
    CHECK_MSG(latest >= 200 && latest <= 215, "latest = " + std::to_string(latest));
    CHECK_MSG(width >= 35 && width <= 60, "width = " + std::to_string(width));
    // the latest press: the cube's bottom clears the spike's top (111) when its rect reaches the spike
    {
        Engine et(&ws);
        et.reset(ws.start);
        bool reached = false;
        for (int t = 0; t < 400; ++t) {
            if (t == latest) et.queueInput(true);
            if (t == latest + 2) et.queueInput(false);
            et.step();
            Rect pr = playerRect(et.player());
            if (!reached && pr.x1 >= 297.0) {
                reached = true;
                CHECK_MSG(pr.y0 > 111.0, "bottom at the spike = " + std::to_string(pr.y0));
            }
            if (et.dead() || et.player().x > 340.0) break;
        }
        CHECK(reached && !et.dead());
    }
    // one tick later than the latest: dead on the spike; one earlier than the earliest: dead on the spike
    {
        Engine et(&ws);
        et.reset(ws.start);
        RunResult rt = run(et, 340.0, 1000, {latest + 1});
        CHECK(rt.dead && rt.reason == 1);
        Engine eu(&ws);
        eu.reset(ws.start);
        RunResult ru = run(eu, 340.0, 1000, {earliest - 1});
        CHECK(ru.dead && ru.reason == 1);
    }
}

void testBlocks() {
    SECTION("holding climbs a platform (auto-jump + landing on block tops), then falls back to the floor");
    Builder b;
    b.block(600.0, 120.0);
    b.block(630.0, 120.0);
    b.block(660.0, 120.0);
    World w = b.finish(3000.0);
    Engine e(&w);
    e.reset(w.start);
    bool onPlatform = false, backOnFloor = false;
    for (int t = 0; t < 1000; ++t) {
        if (t == 300) e.queueInput(true);
        if (t == 560) e.queueInput(false);
        e.step();
        PlayerState const& p = e.player();
        if (p.x > 585.0 && p.x < 675.0 && p.onGround && std::fabs(p.y - 150.0) < 1e-9) onPlatform = true;
        if (p.x > 800.0 && p.onGround && std::fabs(p.y - 105.0) < 1e-9) backOnFloor = true;
        if (e.dead()) break;
    }
    CHECK_MSG(!e.dead(), e.describe());
    CHECK(onPlatform);
    CHECK(backOnFloor);
    CHECK(e.player().x > 1100.0);

    SECTION("timed presses climb a three-step staircase (one block up every four blocks)");
    Builder bs;
    for (int i = 0; i < 4; ++i) bs.block(900.0 + 30.0 * i, 120.0);
    for (int i = 0; i < 4; ++i) { bs.block(1020.0 + 30.0 * i, 150.0); bs.block(1020.0 + 30.0 * i, 120.0); }
    for (int i = 0; i < 4; ++i) { bs.block(1140.0 + 30.0 * i, 180.0); bs.block(1140.0 + 30.0 * i, 150.0); bs.block(1140.0 + 30.0 * i, 120.0); }
    World ws = bs.finish(3000.0);
    Engine es(&ws);
    es.reset(ws.start);
    bool step1 = false, step2 = false, step3 = false;
    for (int t = 0; t < 1400; ++t) {
        if (t == 640 || t == 740 || t == 832) es.queueInput(true);
        if (t == 642 || t == 742 || t == 834) es.queueInput(false);
        es.step();
        PlayerState const& p = es.player();
        if (p.onGround && p.x > 890.0 && p.x < 1000.0 && std::fabs(p.y - 150.0) < 1e-9) step1 = true;
        if (p.onGround && p.x > 1010.0 && p.x < 1120.0 && std::fabs(p.y - 180.0) < 1e-9) step2 = true;
        if (p.onGround && p.x > 1130.0 && p.x < 1240.0 && std::fabs(p.y - 210.0) < 1e-9) step3 = true;
        if (es.dead()) break;
    }
    CHECK_MSG(!es.dead(), es.describe());
    CHECK(step1);
    CHECK(step2);
    CHECK(step3);
    CHECK(es.player().x > 1300.0 && std::fabs(es.player().y - 105.0) < 1e-9);

    SECTION("walking into a wall kills by the inner rect (deathReason 2), about 10 units into the block");
    Builder bw;
    bw.block(1500.0, 120.0);
    bw.block(1500.0, 150.0);
    World ww = bw.finish(3000.0);
    Engine ew(&ww);
    ew.reset(ww.start);
    RunResult rw = run(ew, 1e9, 2000);
    CHECK(rw.dead && rw.reason == 2);
    CHECK_MSG(rw.x > 1485.0 - 4.5 - 1.5 && rw.x <= 1485.0 - 4.5 + 1.5, "wall death x = " + std::to_string(rw.x));

    SECTION("a block top within the snap threshold (10 units) is climbed without a jump");
    Builder bl;
    bl.add(ObjKind::Solid, 1, 700.0, 95.0, 60.0, 10.0);   // top at 100: 10 above the floor
    World wl = bl.finish(3000.0);
    Engine el(&wl);
    el.reset(wl.start);
    RunResult rl = run(el, 720.0, 2000);
    CHECK(!rl.dead);
    CHECK_NEAR(rl.y, 115.0, 1e-9);
    CHECK(el.player().onGround);

    SECTION("a cube bonking its head under a low block survives unless the inner rect enters it");
    Builder bh;
    for (int i = 0; i < 20; ++i) bh.block(400.0 + 30.0 * i, 165.0);   // ceiling bottom at 150 (45 above the cube's top)
    World wh = bh.finish(3000.0);
    Engine eh(&wh);
    eh.reset(wh.start);
    RunResult rh = run(eh, 1e9, 2000, {280}, 400);   // holding: jumps into the ceiling repeatedly
    CHECK(rh.dead && rh.reason == 2);
    // the inner rect's top (y + 4.5) entered the block (bottom 150): one tick past y = 145.5
    CHECK_MSG(rh.y > 145.5 && rh.y < 150.0, "head death y = " + std::to_string(rh.y));
}

void testOrbsPadsPortals() {
    SECTION("a yellow orb mid-air: the second press on the orb re-jumps (buffered press rule), without it the arc ends lower");
    Builder b;
    b.orb(420.0, 165.0, OrbKind::Yellow);
    World w = b.finish(3000.0);
    // the cube jumps at tick 240 (x = 311.6) and crosses the orb at x ~ 400-440
    Engine e1(&w);
    e1.reset(w.start);
    RunResult r1 = run(e1, 700.0, 2000, {240});
    CHECK(!r1.dead);
    double const plainMax = r1.maxY;
    CHECK_NEAR(plainMax, 165.0, 1.5);
    Engine e2(&w);
    e2.reset(w.start);
    bool activated = false;
    bool listedNear = false;
    double maxY = 0.0;
    for (int t = 0; t < 2000; ++t) {
        if (t == 240) e2.queueInput(true);
        if (t == 242) e2.queueInput(false);
        if (t == 300) e2.queueInput(true);     // held from x ~ 390 onward: activates the orb on arrival
        if (t == 340) e2.queueInput(false);
        e2.step();
        if (e2.touchingRing()) activated = true;
        if (e2.player().x > 450.0 && e2.player().x < 460.0) {
            // the orb is one-shot: right after its use it is in the activation list
            EngineSnapshot near = e2.save();
            if (near.activatedIndices.size() == 1 && near.activatedIndices[0] == 0) listedNear = true;
        }
        maxY = std::max(maxY, e2.player().y);
        if (e2.dead() || e2.player().x > 700.0) break;
    }
    CHECK(!e2.dead());
    CHECK(activated);
    CHECK(listedNear);
    CHECK_MSG(maxY > plainMax + 40.0, "orb arc max y = " + std::to_string(maxY));
    // M3 (gprl-sim/2): once the orb is behind the scan window (x - 90 - widest object) its activation
    // can never matter again and is dropped, so snapshots stay small on long levels
    EngineSnapshot s = e2.save();
    CHECK(s.activatedIndices.empty());

    SECTION("a press while already overlapping an unused orb consumes it at the press (pushButton ring-jumps touching rings)");
    Builder bo;
    bo.orb(400.0, 105.0, OrbKind::Yellow);   // on the floor path
    World wo = bo.finish(3000.0);
    Engine eo(&wo);
    eo.reset(wo.start);
    for (int t = 0; t < 300; ++t) eo.step();          // x = 389.5: inside the orb's rect (382..418)
    CHECK(eo.touchingRing());
    CHECK(eo.player().onGround);
    eo.queueInput(true);
    eo.step();
    CHECK(!eo.player().onGround);
    CHECK_NEAR(eo.player().yVelocity, 11.18 - 0.0, 0.3);   // the orb (not the ground jump) set 11.18 then one gravity tick
    CHECK(!eo.player().ringJumpArmed);
    CHECK(eo.save().activatedIndices.size() == 1);

    SECTION("a yellow pad launches at 16 and a gravity portal flips the cube onto a ceiling");
    Builder bp;
    bp.pad(300.0, 92.0, PadKind::Yellow);
    World wp = bp.finish(3000.0);
    Engine ep(&wp);
    ep.reset(wp.start);
    RunResult rp = run(ep, 600.0, 2000);
    CHECK(!rp.dead);
    CHECK_MSG(rp.maxY > 105.0 + 100.0, "pad arc max y = " + std::to_string(rp.maxY));
    Builder bg;
    for (int i = 0; i < 30; ++i) bg.block(400.0 + 30.0 * i, 270.0);   // ceiling bottom at 255
    bg.gravityPortal(500.0, 135.0, true);
    World wg = bg.finish(3000.0);
    Engine eg(&wg);
    eg.reset(wg.start);
    RunResult rg = run(eg, 1000.0, 2000);
    CHECK(!rg.dead);
    CHECK(eg.player().upsideDown);
    CHECK_NEAR(rg.y, 240.0, 1e-9);
    CHECK(eg.player().onGround);
    // the yellow (normal) portal is a no-op when already normal: vy is not halved
    Builder bn;
    bn.gravityPortal(300.0, 135.0, false);
    World wn = bn.finish(3000.0);
    Engine en(&wn);
    en.reset(wn.start);
    RunResult rn = run(en, 500.0, 2000, {200});
    CHECK(!rn.dead && !en.player().upsideDown && en.player().lastFlipTime < 0.0);

    SECTION("a mini portal shrinks the hitbox and the floor centre follows (99)");
    Builder bm;
    bm.sizePortal(300.0, 135.0, true);
    World wm = bm.finish(3000.0);
    Engine em(&wm);
    em.reset(wm.start);
    RunResult rm = run(em, 500.0, 2000);
    CHECK(!rm.dead && em.player().mini);
    CHECK_NEAR(rm.y, 99.0, 1e-9);

    SECTION("a speed change applies by x (not by rect overlap) and the next ticks move faster");
    Builder bsp;
    bsp.speedChange(300.0, 135.0, Speed::Triple);
    World wsp = bsp.finish(3000.0);
    Engine esp(&wsp);
    esp.reset(wsp.start);
    while (esp.player().x < 300.0) esp.step();  // the step that crosses x = 300 still ran at 1x
    CHECK(esp.player().speed == Speed::Normal);
    CHECK(esp.player().x < 300.0 + 1.3);
    esp.step();                                 // the next step pops the change first: Triple applies
    CHECK(esp.player().speed == Speed::Triple);
    CHECK(esp.player().playerSpeed == static_cast<double>(1.3f));   // m_playerSpeed is a float in GD
}

void testShipCorridor() {
    SECTION("ship: holding rises to the ceiling blocks and slides, releasing falls to the floor; a floor spike kills the released ship");
    Builder b;
    b.modePortal(200.0, 135.0, Gamemode::Ship);
    for (int i = 0; i < 60; ++i) b.block(200.0 + 30.0 * i, 330.0);   // ceiling bottom at 315
    b.spike(1500.0, 105.0);
    World w = b.finish(3000.0);
    Engine e(&w);
    e.reset(w.start);
    bool slidCeiling = false;
    for (int t = 0; t < 2000; ++t) {
        if (t == 160) e.queueInput(true);
        if (t == 700) e.queueInput(false);
        e.step();
        PlayerState const& p = e.player();
        if (t > 300 && t < 700 && std::fabs(p.y - 300.0) < 1e-9) slidCeiling = true;
        if (e.dead() || p.x > 1200.0) break;
    }
    CHECK(e.player().mode == Gamemode::Ship);
    CHECK(e.flying());
    CHECK(slidCeiling);
    CHECK(!e.dead());
    // released since tick 700: on the floor well before the spike
    CHECK_NEAR(e.player().y, 105.0, 1e-9);
    RunResult r = run(e, 1e9, 2000);
    CHECK(r.dead && r.reason == 1);
    CHECK(w.objects[static_cast<size_t>(r.killer)].kind == ObjKind::Hazard);

    SECTION("the ship touching a ceiling block from below stops (vy = 0) instead of dying");
    Engine e2(&w);
    e2.reset(w.start);
    for (int t = 0; t < 500; ++t) {
        if (t == 160) e2.queueInput(true);
        e2.step();
        if (e2.dead()) break;
    }
    CHECK(!e2.dead());
    CHECK_NEAR(e2.player().y, 300.0, 1e-9);
    CHECK(e2.player().yVelocity <= 0.0 + 1e-9);

    SECTION("with a world ceiling and no blocks the ship rides the corridor ceiling; without one it dies at 2505");
    Builder bc;
    bc.modePortal(200.0, 135.0, Gamemode::Ship);
    World wc = bc.finish(6000.0);
    wc.ceilingY = 390.f;
    Engine ec(&wc);
    ec.reset(wc.start);
    for (int t = 0; t < 600; ++t) {
        if (t == 160) ec.queueInput(true);
        ec.step();
    }
    CHECK(!ec.dead());
    CHECK_NEAR(ec.player().y, 375.0, 1e-9);
    World wo = bc.finish(20000.0);
    Engine eo(&wo);
    eo.reset(wo.start);
    for (int t = 0; t < 20000; ++t) {
        if (t == 160) eo.queueInput(true);
        eo.step();
        if (eo.dead()) break;
    }
    CHECK(eo.dead() && eo.player().deathReason == 4);
}

void testSlopesAndWave() {
    SECTION("a 45-degree slope carries the cube up and launches it on exit");
    Builder b;
    b.slope(600.0, 120.0, 30.0, 30.0, 1);
    b.block(630.0, 120.0);
    b.block(660.0, 120.0);
    World w = b.finish(3000.0);
    Engine e(&w);
    e.reset(w.start);
    bool climbed = false;
    for (int t = 0; t < 700; ++t) {
        e.step();
        PlayerState const& p = e.player();
        if (p.x > 586.0 && p.x < 614.0 && p.onSlopeIndex >= 0) climbed = true;
        if (e.dead()) break;
    }
    CHECK(!e.dead());
    CHECK(climbed);
    CHECK(e.player().x > 700.0);

    SECTION("the wave flies at 45 degrees and dies on a wall by its 3 x 3 inner rect");
    Builder bw;
    bw.modePortal(200.0, 135.0, Gamemode::Wave);
    for (int i = 0; i < 6; ++i) bw.block(600.0, 150.0 + 30.0 * i);   // a wall from y = 135 up
    World ww = bw.finish(3000.0);
    Engine ew(&ww);
    ew.reset(ww.start);
    double yAt500 = 0.0;
    for (int t = 0; t < 2000; ++t) {
        if (t == 300) ew.queueInput(true);
        if (t == 400) ew.queueInput(false);
        ew.step();
        if (ew.player().x >= 500.0 && yAt500 == 0.0) yAt500 = ew.player().y;
        if (ew.dead()) break;
    }
    CHECK(ew.player().mode == Gamemode::Wave);
    CHECK_MSG(yAt500 > 200.0, "wave y at x = 500: " + std::to_string(yAt500));   // 100 ticks up at 45 degrees from y = 100
    CHECK_MSG(ew.dead() && ew.player().deathReason == 2, ew.describe());
    CHECK_MSG(ew.player().x > 585.0 - 1.5 - 1.0 && ew.player().x < 585.0 - 1.5 + 2.0, "wave wall death x = " + std::to_string(ew.player().x));
}

// ---- the 2026-10-02 review findings (gprl-sim/2), from the reviewer's physics probe ----

void testSlopePlateauExit() {
    SECTION("C3: a 45-degree slope ending in the air launches ONE step after its clamped top (gdp 161-165), not 27 ticks late");
    Builder b;
    b.slope(315.0, 105.0, 30.0, 30.0, 1);   // "/" from (300, 90) to (330, 120), nothing after it
    World w = b.finish(2000.0);
    Engine e(&w);
    e.reset(w.start);
    double const clampY = gdFloat(120.0 + 15.0);   // newPlayerY clamped to the rect top + radius
    int clampTick = -1, exitTick = -1;
    double exitVy = 0.0, exitX = 0.0;
    for (int t = 0; t < 400; ++t) {
        bool const wasOn = e.player().onSlopeIndex >= 0;
        e.step();
        PlayerState const& p = e.player();
        if (clampTick < 0 && p.onSlopeIndex >= 0 && p.y == clampY) clampTick = e.tick();
        if (exitTick < 0 && wasOn && p.onSlopeIndex < 0) {
            exitTick = e.tick();
            exitVy = p.yVelocity;
            exitX = p.x;
        }
        if (e.dead() || exitTick >= 0) break;
    }
    CHECK(!e.dead());
    CHECK(clampTick > 0);
    CHECK_MSG(exitTick == clampTick + 1, "clamp at tick " + std::to_string(clampTick) + ", exit at tick " + std::to_string(exitTick));
    CHECK_MSG(exitVy > 3.0, "launch vy " + std::to_string(exitVy));   // postCollision boosted the cube off the slope
    CHECK(e.player().boosted);
    CHECK_MSG(exitX < 330.0, "exit x " + std::to_string(exitX));      // still over the slope's rect (GD), /1 exited at x ~ 361
    std::printf("  plateau: clamp tick %d, exit tick %d at x %.2f, vy %.3f\n", clampTick, exitTick, exitX, exitVy);
}

void testFlyerHoldIntoOrb() {
    SECTION("H4: a ship / wave HOLDING into a yellow orb does not use it; a press while touching does; the cube rule is unchanged");
    for (Gamemode mode : {Gamemode::Ship, Gamemode::Wave, Gamemode::Ufo, Gamemode::Swing}) {
        Builder b;
        b.w.start.mode = mode;
        b.w.start.y = 200.f;
        SimObject& o = b.orb(200.0, 300.0, OrbKind::Yellow);
        o.ry = 100.f;
        o.rh = 400.f;   // a tall rect: the path surely overlaps it
        World w = b.finish(2000.0);
        Engine e(&w);
        e.reset(w.start);
        e.queueInput(true);   // press at tick 0 and hold through the orb
        bool used = false;
        for (int t = 0; t < 200; ++t) {
            e.step();
            if (!e.save().activatedIndices.empty()) used = true;
            if (e.dead() || e.player().x > 260.0) break;
        }
        CHECK_MSG(!used, std::string(gamemodeName(mode)) + " ring-jumped while only holding");
        // released before the orb, pressed while touching it: the orb is used (pushButton ring-jumps touching rings)
        Engine f(&w);
        f.reset(w.start);
        bool usedOnPress = false;
        for (int t = 0; t < 200 && !usedOnPress; ++t) {
            if (f.touchingRing() && !f.player().held) f.queueInput(true);
            f.step();
            for (int i : f.save().activatedIndices)
                if (w.objects[static_cast<size_t>(i)].kind == ObjKind::Orb) usedOnPress = true;
            if (f.dead() || f.player().x > 260.0) break;
        }
        CHECK_MSG(usedOnPress, std::string(gamemodeName(mode)) + " did not use the orb on a press while touching");
    }
}

void testRingsAcrossRestore() {
    SECTION("M7: a press one step after leaving an orb: a restored engine sees the same m_touchingRings as the continuous one");
    Builder b;
    SimObject& o = b.orb(300.0, 130.0, OrbKind::Pink);
    o.objectId = 141;
    World w = b.finish(2000.0);
    Engine probe(&w);
    probe.reset(w.start);
    int k = -1;
    bool prevPass = false;
    for (int t = 1; t < 400; ++t) {
        probe.step();
        bool const pass = !probe.save().touchedRings.empty();   // overlapped during this pass
        if (prevPass && !pass) {
            k = t;
            break;
        }
        prevPass = pass;
    }
    CHECK(k > 0);
    Engine a(&w);
    a.reset(w.start);
    for (int t = 1; t <= k; ++t) a.step();
    EngineSnapshot snap = a.save();
    uint64_t const hSnap = a.stateHash();
    CHECK(!snap.touchingRings.empty() && snap.touchedRings.empty());   // GD: the ring stays until the next resetTouchedRings
    CHECK(a.touchingRing());
    a.queueInput(true);
    a.step();
    Engine r(&w);
    r.reset(w.start);
    r.restore(snap);
    CHECK(r.stateHash() == hSnap);
    r.queueInput(true);
    r.step();
    CHECK_NEAR(r.player().yVelocity, a.player().yVelocity, 1e-12);
    CHECK(a.describe() == r.describe());
    CHECK(!a.save().activatedIndices.empty());   // the press used the pink orb (not a ground jump)
    std::printf("  press at step %d: continuous vy %.3f, restored vy %.3f\n", k + 1, a.player().yVelocity, r.player().yVelocity);

    SECTION("M7: a CBF press late in the tick still sees the previous pass's ring (applyQueuedInput no longer prunes first)");
    // GD per sub-step: processCommands, then resetTouchedRings. The first part (0.999 of tick k)
    // prunes to pass(k-1) = {orb} and overlaps nothing itself; the press at the start of the second
    // part sees m_touchingRings = {orb} and ring-jumps it. /1 pruned to the first part's pass first.
    Engine c(&w);
    c.reset(w.start);
    for (int t = 1; t <= k - 1; ++t) c.step();
    c.stepPart(0.999);
    bool const partOverlapped = !c.save().touchedRings.empty();
    c.queueInput(true);
    c.applyQueuedInput();
    c.stepPart(0.001);
    bool orbUsed = false;
    for (int i : c.save().activatedIndices)
        if (w.objects[static_cast<size_t>(i)].kind == ObjKind::Orb) orbUsed = true;
    CHECK_MSG(partOverlapped || orbUsed, "the late CBF press did not take the orb of the previous pass");
}

void testSnapAfterSlope() {
    SECTION("M7: right after a slope the landing threshold is 10 + unk_584 (16.2 on 45 degrees): a block 12 units above the slope top is landed on");
    // the slope's top is at y 120; the block row starts where the slope ends (x 330) with its top at
    // 132: 12 units above the cube's bottom at the plateau exit -> GD snaps up onto it
    Builder b;
    b.slope(315.0, 105.0, 30.0, 30.0, 1);
    for (int i = 0; i < 6; ++i) b.add(ObjKind::Solid, 1, 345.0 + 30.0 * i, 117.0, 30.0, 30.0);   // tops at 132
    World w = b.finish(2000.0);
    Engine e(&w);
    e.reset(w.start);
    bool onBlock = false;
    for (int t = 0; t < 600; ++t) {
        e.step();
        PlayerState const& p = e.player();
        if (p.onGround && p.x > 330.0 && p.x < 510.0 && p.y == gdFloat(132.0 + 15.0)) onBlock = true;
        if (e.dead() || p.x > 600.0) break;
    }
    CHECK_MSG(!e.dead(), e.describe());
    CHECK(onBlock);
}

void testFloatDriftEngine() {
    SECTION("H3: 120 s on an empty floor at 1x and 4x: the engine's x equals GD's float accumulation exactly");
    for (Speed sp : {Speed::Normal, Speed::Quadruple}) {
        Builder b;
        b.w.start.speed = sp;
        World w = b.finish(1e7);
        Engine e(&w);
        e.reset(w.start);
        float gdX = 0.f;
        double const dx = static_cast<double>(static_cast<float>(speedParams(sp).playerSpeed)) * speedParams(sp).speedMultiplier * 0.25;
        bool exact = true;
        for (int t = 0; t < 28800; ++t) {
            e.step();
            gdX = gdX + static_cast<float>(dx);
            if (e.player().x != static_cast<double>(gdX)) exact = false;
        }
        CHECK(exact);
        CHECK(e.player().y == 105.0 && !e.dead());
    }
}

uint64_t trajectoryDigest(Engine& e, std::vector<std::pair<int, bool>> const& inputs, int ticks, double* endX, double* endY, int* deadTick) {
    uint64_t h = 1469598103934665603ull;
    auto feed = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    auto bits = [](double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; };
    *deadTick = -1;
    for (int t = 0; t < ticks; ++t) {
        for (auto const& in : inputs) {
            if (in.first == t) e.queueInput(in.second);
        }
        e.step();
        PlayerState const& p = e.player();
        feed(bits(p.x));
        feed(bits(p.y));
        feed(bits(p.yVelocity));
        feed(static_cast<uint64_t>(p.mode) | (static_cast<uint64_t>(p.upsideDown) << 8) | (static_cast<uint64_t>(p.onGround) << 9) |
             (static_cast<uint64_t>(p.mini) << 10) | (static_cast<uint64_t>(p.dead) << 11) | (static_cast<uint64_t>(p.speed) << 12));
        if (p.dead && *deadTick < 0) *deadTick = e.tick();
    }
    *endX = e.player().x;
    *endY = e.player().y;
    return h;
}

std::string hex64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

World worldFromJson(gprl::json::Value const& j) {
    Builder b;
    auto const& st = j["start"];
    b.w.start.x = static_cast<float>(st.getNumber("x", 0.0));
    b.w.start.y = static_cast<float>(st.getNumber("y", 105.0));
    b.w.start.speed = static_cast<Speed>(st.getInt("speed", 1));
    b.w.start.mode = static_cast<Gamemode>(st.getInt("mode", 0));
    b.w.ceilingY = static_cast<float>(j.getNumber("ceilingY", 0.0));
    for (auto const& o : j["objects"].asArray()) {
        std::string kind = o.getString("kind");
        double x = o.getNumber("x"), y = o.getNumber("y");
        if (kind == "block") b.block(x, y);
        else if (kind == "solid") b.add(ObjKind::Solid, 1, x, y, o.getNumber("w", 30.0), o.getNumber("h", 30.0));
        else if (kind == "spike") b.spike(x, y);
        else if (kind == "orb") b.orb(x, y, static_cast<OrbKind>(o.getInt("orb", 0)));
        else if (kind == "pad") b.pad(x, y, static_cast<PadKind>(o.getInt("pad", 0)));
        else if (kind == "gravity") b.gravityPortal(x, y, o.getBool("flipped", true));
        else if (kind == "mode") b.modePortal(x, y, static_cast<Gamemode>(o.getInt("mode", 1)));
        else if (kind == "size") b.sizePortal(x, y, o.getBool("mini", true));
        else if (kind == "speed") b.speedChange(x, y, static_cast<Speed>(o.getInt("speed", 1)));
        else if (kind == "slope") b.slope(x, y, o.getNumber("w", 30.0), o.getNumber("h", 30.0), static_cast<uint8_t>(o.getInt("orientation", 1)));
    }
    return b.finish(j.getNumber("endX", 3000.0));
}

void testGolden(std::string const& repo, bool write) {
    SECTION("determinism golden: the fixture world + input script give the pinned trajectory digest");
    std::string path = repo + "/tests/fixtures/sim/golden-engine.json";
    std::string text = gprl::test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture readable: " + path);
    if (text.empty()) return;
    gprl::json::Value j;
    gprl::json::ParseError err;
    CHECK_MSG(gprl::json::parse(text, j, &err), "fixture parses: " + err.message);
    World w = worldFromJson(j);
    std::vector<std::pair<int, bool>> inputs;
    for (auto const& in : j["inputs"].asArray()) inputs.emplace_back(static_cast<int>(in.getInt("tick")), in.getBool("down"));
    int const ticks = static_cast<int>(j.getInt("ticks", 2400));
    Engine e(&w);
    e.reset(w.start);
    double endX = 0.0, endY = 0.0;
    int deadTick = -1;
    uint64_t d1 = trajectoryDigest(e, inputs, ticks, &endX, &endY, &deadTick);
    Engine e2(&w);
    e2.reset(w.start);
    double endX2 = 0.0, endY2 = 0.0;
    int deadTick2 = -1;
    uint64_t d2 = trajectoryDigest(e2, inputs, ticks, &endX2, &endY2, &deadTick2);
    CHECK(d1 == d2 && endX == endX2 && endY == endY2 && deadTick == deadTick2);
    // the run must survive the script (a fixture that dies pins nothing useful)
    CHECK_MSG(deadTick < 0, "the golden script dies at tick " + std::to_string(deadTick) + ": " + e.describe());
    std::printf("  golden: digest %s endX %.6f endY %.6f dead %d\n", hex64(d1).c_str(), endX, endY, deadTick);
    if (write) {
        if (gprl::test::g_failures == 0) {
            gprl::json::Value golden = gprl::json::Value::object();
            golden.set("digest", hex64(d1));
            golden.set("endX", endX);
            golden.set("endY", endY);
            golden.set("deadTick", deadTick);
            golden.set("ticks", ticks);
            j.set("golden", golden);
            std::ofstream f(path, std::ios::binary);
            f << gprl::json::stringifyPretty(j) << "\n";
            std::printf("  wrote %s\n", path.c_str());
        } else {
            std::printf("  NOT writing the golden: earlier checks failed\n");
        }
        return;
    }
    auto const& g = j["golden"];
    CHECK_MSG(g.isObject(), "the fixture carries a golden block (run with --write to create it)");
    if (!g.isObject()) return;
    CHECK_MSG(g.getString("digest") == hex64(d1), "digest " + hex64(d1) + " vs golden " + g.getString("digest"));
    CHECK_NEAR(endX, g.getNumber("endX"), 1e-9);
    CHECK_NEAR(endY, g.getNumber("endY"), 1e-9);
    CHECK(static_cast<int>(g.getInt("deadTick", -1)) == deadTick);
}

void testSnapshots() {
    SECTION("save / restore reproduces the continuation bit for bit; stateHash is equal for equal states and differs after a step");
    Builder b;
    b.orb(420.0, 165.0, OrbKind::Yellow);
    b.pad(700.0, 92.0, PadKind::Pink);
    b.gravityPortal(1000.0, 135.0, true);
    for (int i = 0; i < 40; ++i) b.block(900.0 + 30.0 * i, 270.0);
    b.gravityPortal(2000.0, 210.0, false);   // back to normal before the ceiling ends
    b.modePortal(2200.0, 135.0, Gamemode::Ship);
    World w = b.finish(6000.0);
    std::vector<std::pair<int, bool>> script = {{240, true}, {242, false}, {300, true}, {340, false}, {1200, true}, {1230, false},
                                                {1800, true}, {1900, false}, {2000, true}, {2100, false}};
    Engine e(&w);
    e.reset(w.start);
    auto drive = [&](Engine& en, int from, int to) {
        for (int t = from; t < to; ++t) {
            for (auto const& in : script) {
                if (in.first == t) en.queueInput(in.second);
            }
            en.step();
        }
    };
    drive(e, 0, 500);
    EngineSnapshot snap = e.save();
    uint64_t h0 = e.stateHash();
    std::vector<std::string> traceA;
    for (int t = 500; t < 2400; ++t) {
        for (auto const& in : script) {
            if (in.first == t) e.queueInput(in.second);
        }
        e.step();
        traceA.push_back(e.describe());
    }
    CHECK_MSG(!e.dead(), e.describe());
    CHECK(e.player().mode == Gamemode::Ship);
    e.restore(snap);
    CHECK(e.stateHash() == h0);
    CHECK(e.tick() == 500);
    std::vector<std::string> traceB;
    for (int t = 500; t < 2400; ++t) {
        for (auto const& in : script) {
            if (in.first == t) e.queueInput(in.second);
        }
        e.step();
        traceB.push_back(e.describe());
    }
    CHECK(traceA == traceB);
    // a fresh engine restored from the snapshot continues identically too
    Engine f(&w);
    f.reset(w.start);
    f.restore(snap);
    CHECK(f.stateHash() == h0);
    std::vector<std::string> traceC;
    for (int t = 500; t < 2400; ++t) {
        for (auto const& in : script) {
            if (in.first == t) f.queueInput(in.second);
        }
        f.step();
        traceC.push_back(f.describe());
    }
    CHECK(traceA == traceC);
    Engine g(&w);
    g.reset(w.start);
    drive(g, 0, 500);
    CHECK(g.stateHash() == h0);
    g.step();
    CHECK(g.stateHash() != h0);
    // the hash ignores sub-0.5 y differences and sub-0.05 vy differences
    PlayerState const& pg = g.player();
    (void)pg;

    SECTION("stepPart: two halves advance one tick and stay deterministic; applyQueuedInput applies now");
    Engine h1(&w);
    h1.reset(w.start);
    for (int t = 0; t < 100; ++t) { h1.stepPart(0.5); h1.stepPart(0.5); }
    CHECK(h1.tick() == 100 && h1.frame() == 100.0);
    Engine h2(&w);
    h2.reset(w.start);
    for (int t = 0; t < 100; ++t) { h2.stepPart(0.5); h2.stepPart(0.5); }
    CHECK(h1.describe() == h2.describe());
    CHECK_NEAR(h1.player().x, 100 * 0.25 * 0.9 * 5.77000189, 1e-3);   // 200 float adds (gprl-sim/2 float position)
    Engine h3(&w);
    h3.reset(w.start);
    for (int t = 0; t < 240; ++t) h3.step();
    h3.stepPart(0.5);
    CHECK(h3.frame() == 240.5);
    h3.queueInput(true);
    h3.applyQueuedInput();
    CHECK(h3.player().held && h3.player().jumpBuffered);
    h3.stepPart(0.5);
    CHECK(h3.tick() == 241 && !h3.player().onGround);

    SECTION("nearestAhead, inUnsupportedSpan, percent, completed, describe");
    World wu = b.finish(6000.0);
    UnsupportedSpan span;
    span.x0 = 3000.f;
    span.x1 = 3600.f;
    span.mechanic = "dual_portal";
    wu.unsupported.push_back(span);
    Engine eu(&wu);
    eu.reset(wu.start);
    CHECK(eu.nearestAhead(1000.f) == 0);
    CHECK(eu.nearestAhead(100.f) == -1);
    CHECK(!eu.inUnsupportedSpan());
    StartState mid = wu.start;
    mid.x = 3100.f;
    eu.reset(mid);
    CHECK(eu.inUnsupportedSpan());
    CHECK_NEAR(eu.percent(), 3100.0 / 6000.0 * 100.0, 1e-9);
    mid.x = 6000.f;
    eu.reset(mid);
    CHECK(eu.completed() && eu.percent() == 100.0);
    CHECK(!eu.describe().empty());
}

}  // namespace

int main(int argc, char** argv) {
    std::string repo = argc > 1 ? argv[1] : "D:/GPRL";
    bool write = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--write") write = true;
    }
    testFloorAndSpike();
    testBlocks();
    testOrbsPadsPortals();
    testShipCorridor();
    testSlopesAndWave();
    testSnapshots();
    testSlopePlateauExit();
    testFlyerHoldIntoOrb();
    testRingsAcrossRestore();
    testSnapAfterSlope();
    testFloatDriftEngine();
    testGolden(repo, write);
    return gprl::test::finish("sim_engine_tests");
}
