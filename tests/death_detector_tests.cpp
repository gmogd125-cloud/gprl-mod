// Host tests for core/death_detector (noclip-death-detector/2, docs/NOCLIP_DEATH_DETECTOR.md): the
// owner's regression pack A-L ("Mega Hack noclip / false would-be-death fix", §12) plus the rules
// the disassembly added (locked player, out-of-bounds re-arm, hook order, external kills).
//
// `World` is a small model of what the game side sees: GD's physics step raises a kill for a live
// player on the ticks a lethal contact exists (the LIVE truth); a noclip menu may swallow the call;
// the solver, the analyzer and other mods raise their own candidates around it. Nothing in here
// reconstructs a collision for the detector: it only ever receives what a caller raised.
#include <string>
#include <vector>

#include "../core/classify.hpp"
#include "../core/death_detector.hpp"
#include "test_util.hpp"

using namespace gprl;
using namespace gprl::death;

namespace {

struct World {
    Detector det;
    uint32_t sessionGen = 0;
    uint32_t attemptGen = 0;
    bool noclip = true;          // a menu swallows PlayLayer::destroyPlayer for the real players
    bool hookBypassed = false;   // that menu's hook sits BEFORE GPRL's: only checkCollisions' return value is seen
    bool dead[3] = {false, false, false};
    std::vector<Verdict> log;

    void enterLevel() {
        det.beginSession(++sessionGen);
        attemptGen = 0;
    }
    void startAttempt() {
        det.beginAttempt(++attemptGen);
        dead[1] = dead[2] = false;
    }
    Candidate liveCandidate(int slot, int64_t tick, bool hasObject) const {
        Candidate c;
        c.sessionGen = sessionGen;
        c.attemptGen = attemptGen;
        c.tick = tick;
        c.origin = Origin::LivePlayLayer;
        c.playerSlot = slot;
        c.inLiveStep = true;
        c.hasObject = hasObject;
        c.wasDeadBefore = dead[slot];
        return c;
    }
    /// GD's own physics step found the real player `slot` in a lethal contact on `tick`.
    Verdict liveKill(int slot, int64_t tick, bool hasObject = true) {
        Candidate c = liveCandidate(slot, tick, hasObject);
        if (noclip) {
            c.gdDeathFired = false;   // swallowed before the game's own function
            c.deadAfter = false;
            if (hookBypassed) c.via = Via::CollisionReturn;
        }
        else {
            c.gdDeathFired = true;
            c.deadAfter = !c.wasDeadBefore;
            if (c.deadAfter) dead[1] = dead[2] = true;   // GD destroys both players in dual
        }
        Verdict v = det.onCandidate(c);
        log.push_back(v);
        return v;
    }
    Verdict raise(Candidate const& c) {
        Verdict v = det.onCandidate(c);
        log.push_back(v);
        return v;
    }
    int wouldBe() const { return det.counters().totalWouldBe(); }
    int wouldBe(int slot) const { return det.counters().wouldBeDeaths[static_cast<size_t>(slot)]; }
    int deaths() const { return det.counters().deaths; }
};

World freshAttempt() {
    World w;
    w.enterLevel();
    w.startAttempt();
    return w;
}

/// A hidden timing clone died (as the live hook would see it).
Candidate cloneKill(World const& w, int64_t tick, int slotTheGamePassed = 0) {
    Candidate c;
    c.sessionGen = w.sessionGen;
    c.attemptGen = w.attemptGen;
    c.tick = tick;
    c.origin = Origin::LivePlayLayer;
    c.playerSlot = slotTheGamePassed;
    c.solverClone = slotTheGamePassed == 0;
    c.solverStepping = true;
    c.hasObject = true;
    return c;
}

/// A collision somebody reconstructed (analyzer hitboxes, replay, practice copy).
Candidate simulated(World const& w, Origin origin, int64_t tick, int slot = 1) {
    Candidate c;
    c.sessionGen = w.sessionGen;
    c.attemptGen = w.attemptGen;
    c.tick = tick;
    c.origin = origin;
    c.playerSlot = slot;
    c.hasObject = true;
    return c;
}

// ---- A. open air ----
void testOpenAir() {
    SECTION("A. open air: nothing the live game did not raise can be a would-be death");
    World w = freshAttempt();
    // 2000 ticks of flying through empty space: GD raises nothing for the real player. Around it the
    // solver's clones die, the analyzer's reconstructed hitboxes "collide", a replay copy dies, and
    // another mod simulates on the real player's pointer outside the physics step.
    for (int64_t t = 0; t < 2000; ++t) {
        if (t % 3 == 0) CHECK(w.raise(cloneKill(w, t)).decision == Decision::Rejected);
        if (t % 5 == 0) CHECK(w.raise(cloneKill(w, t, 1)).decision == Decision::Rejected);   // the game passed player 1's pointer during a clone step
        if (t % 7 == 0) CHECK(w.raise(simulated(w, Origin::Analyzer, t)).decision == Decision::Rejected);
        if (t % 11 == 0) CHECK(w.raise(simulated(w, Origin::Replay, t)).decision == Decision::Rejected);
        if (t % 13 == 0) CHECK(w.raise(simulated(w, Origin::PracticeSim, t)).decision == Decision::Rejected);
        if (t % 17 == 0) {
            Candidate c = w.liveCandidate(1, t, true);
            c.inLiveStep = false;   // a trajectory mod stepped the REAL player object itself and swallowed the kill
            Verdict v = w.raise(c);
            CHECK(v.decision == Decision::Rejected);
            CHECK(v.reason == Reason::OutsideLiveStep);
            CHECK(v.source == Source::SimulatedCollision);
        }
    }
    CHECK(w.wouldBe() == 0);
    CHECK(w.deaths() == 0);
    CHECK(w.det.firstWouldBeTick() == -1);
    CHECK(w.det.attemptOpen());
    CHECK(w.det.counters().rejectedClone > 0);
    CHECK(w.det.counters().simulated > 0);
}

// ---- B. single spike ----
void testSingleSpike() {
    SECTION("B. one spike, 20 ticks inside it with noclip: exactly 1");
    World w = freshAttempt();
    for (int64_t t = 500; t < 520; ++t) {
        Verdict v = w.liveKill(1, t);
        CHECK(v.accepted());
        CHECK(v.source == Source::LiveGdDeath);
        CHECK((v.decision == Decision::WouldBeDeath) == (t == 500));
        CHECK(v.contactId == 1);
        CHECK(v.player == 1);
    }
    CHECK(w.wouldBe() == 1);
    CHECK(w.det.firstWouldBeTick() == 500);
    CHECK(w.det.counters().continued == 19);
    CHECK(w.deaths() == 0);
}

// ---- C. two separate spikes ----
void testTwoSpikes() {
    SECTION("C. two separate contacts: 2 (the prompt's tick 100-103 / 110 example)");
    World w = freshAttempt();
    CHECK(w.liveKill(1, 100).decision == Decision::WouldBeDeath);   // death #1 at tick 100
    CHECK(w.liveKill(1, 101).decision == Decision::ContinuesContact);
    CHECK(w.liveKill(1, 102).decision == Decision::ContinuesContact);
    // tick 103: left the hazard (GD raises nothing); tick 110: another hazard
    Verdict second = w.liveKill(1, 110);
    CHECK(second.decision == Decision::WouldBeDeath);
    CHECK(second.contactId == 2);
    CHECK(w.wouldBe() == 2);
    // one clean tick is enough to end a contact with an object
    CHECK(w.liveKill(1, 112).decision == Decision::WouldBeDeath);
    CHECK(w.wouldBe() == 3);
}

// ---- D. overlapping hazards ----
void testOverlappingHazards() {
    SECTION("D. overlapping hazards / several kills per tick / changing object: 1");
    World w = freshAttempt();
    for (int64_t t = 40; t <= 60; ++t) {
        w.liveKill(1, t, true);            // spike A
        w.liveKill(1, t, true);            // saw B on the same tick
        if (t % 2) w.liveKill(1, t, false);   // plus a block crush on some ticks (no object)
    }
    CHECK(w.wouldBe() == 1);
    // the contact moves from one hazard into the next without a clean tick: still the same episode
    for (int64_t t = 61; t <= 70; ++t) w.liveKill(1, t, t % 2 == 0);
    CHECK(w.wouldBe() == 1);
    for (auto const& v : w.log) CHECK(v.contactId == 1);
}

// ---- E. hidden solver clone dies ----
void testSolverClone() {
    SECTION("E. the real player is safe, timing clones die: 0");
    World w = freshAttempt();
    for (int64_t t = 0; t < 600; ++t) {
        Verdict a = w.raise(cloneKill(w, t));       // the clone's own pointer
        CHECK(a.decision == Decision::Rejected);
        CHECK(a.source == Source::RejectedClone);
        CHECK(a.player == 0);
        Verdict b = w.raise(cloneKill(w, t, 1));    // player 1's pointer, raised inside a clone step
        CHECK(b.decision == Decision::Rejected);
        CHECK(b.reason == Reason::SolverStep);
        Candidate c = simulated(w, Origin::TimingSolver, t);   // a result reported by the solver itself
        Verdict d = w.raise(c);
        CHECK(d.decision == Decision::Rejected);
        CHECK(d.source == Source::RejectedClone);
    }
    CHECK(w.wouldBe() == 0);
    CHECK(w.deaths() == 0);
    CHECK(w.det.attemptOpen());
    CHECK(w.det.counters().rejectedClone == 1800);
    // a clone that DIES (dead afterwards) never ends the live attempt either
    Candidate dying = cloneKill(w, 700);
    dying.deadAfter = true;
    CHECK(w.raise(dying).decision == Decision::Rejected);
    CHECK(w.det.attemptOpen());
    // an unknown PlayerObject (another mod's fake player) is not a clone of ours but still not live
    Candidate stranger = w.liveCandidate(0, 710, true);
    Verdict s = w.raise(stranger);
    CHECK(s.decision == Decision::Rejected);
    CHECK(s.source == Source::RejectedUnknownPlayer);
    stranger.deadAfter = true;
    CHECK(w.raise(stranger).decision == Decision::Rejected);
    CHECK(w.det.attemptOpen());
}

// ---- F. dual ----
void testDual() {
    SECTION("F. dual: the contact belongs to the player GD tried to kill");
    World w = freshAttempt();
    for (int64_t t = 200; t < 210; ++t) CHECK(w.liveKill(1, t).player == 1);
    CHECK(w.wouldBe(1) == 1);
    CHECK(w.wouldBe(2) == 0);
    // reversed
    w.startAttempt();
    for (int64_t t = 200; t < 210; ++t) CHECK(w.liveKill(2, t).player == 2);
    CHECK(w.wouldBe(1) == 0);
    CHECK(w.wouldBe(2) == 1);
    // both, overlapping in time: one each, never merged and never doubled
    w.startAttempt();
    for (int64_t t = 300; t < 320; ++t) {
        w.liveKill(1, t);
        if (t >= 305 && t < 312) w.liveKill(2, t);
    }
    CHECK(w.wouldBe(1) == 1);
    CHECK(w.wouldBe(2) == 1);
    // player 2 touching a hazard one tick after player 1 left its own is player 2's own death
    w.startAttempt();
    w.liveKill(1, 400);
    Verdict p2 = w.liveKill(2, 401);
    CHECK(p2.decision == Decision::WouldBeDeath);
    CHECK(p2.player == 2);
    CHECK(w.wouldBe() == 2);
}

// ---- G. reset race ----
void testResetRace() {
    SECTION("G. a kill of attempt N can never touch attempt N+1");
    World w = freshAttempt();
    Candidate queued = w.liveCandidate(1, 900, true);   // raised (and captured) in attempt N
    w.det.endAttempt();
    w.startAttempt();                                   // attempt N+1 starts before the callback is processed
    Verdict v = w.raise(queued);
    CHECK(v.decision == Decision::Rejected);
    CHECK(v.reason == Reason::StaleAttempt);
    CHECK(v.source == Source::RejectedStaleAttempt);
    CHECK(w.wouldBe() == 0);
    CHECK(w.det.counters().rejectedStale == 1);
    // even a kill that left the old player dead does not end the new attempt
    queued.deadAfter = true;
    CHECK(w.raise(queued).decision == Decision::Rejected);
    CHECK(w.det.attemptOpen());
    CHECK(w.deaths() == 0);
    // raised while the level resets
    Candidate resetting = w.liveCandidate(1, 0, true);
    resetting.duringReset = true;
    CHECK(w.raise(resetting).reason == Reason::DuringReset);
    // after the attempt ended (restart / completion / exit) and before the next one
    w.det.endAttempt();
    CHECK(w.raise(w.liveCandidate(1, 950, true)).reason == Reason::NoOpenAttempt);
    // another level session entirely
    Candidate oldSession = w.liveCandidate(1, 10, true);
    w.enterLevel();
    w.startAttempt();
    CHECK(w.raise(oldSession).reason == Reason::StaleSession);
    w.det.endSession();
    CHECK(w.raise(w.liveCandidate(1, 11, true)).reason == Reason::StaleSession);
    CHECK(w.wouldBe() == 0);
    // contacts never carry over: the first kill of a new attempt on the tick after the old one's is new
    World x = freshAttempt();
    x.liveKill(1, 50);
    x.startAttempt();
    CHECK(x.liveKill(1, 51).decision == Decision::WouldBeDeath);
    CHECK(x.liveKill(1, 51).contactId == 1);
}

// ---- H. moving hazard ----
void testMovingHazard() {
    SECTION("H. moving hazard: only the live position's kill counts");
    World w = freshAttempt();
    // A saw stored at x = 300 is moved to x = 900 by a move trigger before the player arrives.
    // The player travels 1 unit per tick. GD's live collision check (the truth) sees the saw at 900.
    auto liveContact = [](int64_t tick) { return tick >= 895 && tick <= 905; };
    auto startPositionContact = [](int64_t tick) { return tick >= 295 && tick <= 305; };
    for (int64_t t = 0; t < 1200; ++t) {
        if (startPositionContact(t)) {
            // a reconstruction from the level string (the analyzer's start-position hitbox)
            Verdict v = w.raise(simulated(w, Origin::Analyzer, t));
            CHECK(v.decision == Decision::Rejected);
            CHECK(v.source == Source::SimulatedCollision);
        }
        if (liveContact(t)) w.liveKill(1, t);
    }
    CHECK(w.wouldBe() == 1);
    CHECK(w.det.firstWouldBeTick() == 895);
}

// ---- I. rotated / scaled saw ----
void testRotatedSaw() {
    SECTION("I. rotated / scaled saw: no death unless GD itself recognises the contact");
    World w = freshAttempt();
    // The saw's bounding box covers ticks 50-70; GD's own test (circle / oriented box) only 58-62.
    for (int64_t t = 50; t <= 70; ++t) {
        CHECK(w.raise(simulated(w, Origin::Analyzer, t)).decision == Decision::Rejected);
        if (t >= 58 && t <= 62) w.liveKill(1, t);
    }
    CHECK(w.wouldBe() == 1);
    CHECK(w.det.firstWouldBeTick() == 58);
    // the player only grazes the bounding box: GD never raises a kill
    w.startAttempt();
    for (int64_t t = 50; t <= 70; ++t) w.raise(simulated(w, Origin::Analyzer, t));
    CHECK(w.wouldBe() == 0);
}

// ---- J. Mega Hack-like suppressed death ----
void testSuppressedDeath() {
    SECTION("J. a live lethal callback that leaves the player alive: 1");
    World w = freshAttempt();
    Verdict v = w.liveKill(1, 1234);
    CHECK(v.decision == Decision::WouldBeDeath);
    CHECK(v.reason == Reason::NewLethalContact);
    CHECK(v.source == Source::LiveGdDeath);
    CHECK(w.wouldBe() == 1);
    CHECK(w.det.attemptOpen());   // the run continues
    // a byte-patch style noclip: the game's own function runs but nobody dies
    w.startAttempt();
    Candidate patched = w.liveCandidate(1, 10, true);
    patched.gdDeathFired = true;
    CHECK(w.raise(patched).decision == Decision::WouldBeDeath);
    // hook order: the menu's hook runs BEFORE GPRL's, so only checkCollisions' return value is seen
    w.startAttempt();
    w.hookBypassed = true;
    for (int64_t t = 20; t < 40; ++t) w.liveKill(1, t);
    CHECK(w.wouldBe() == 1);
    CHECK(w.log.back().accepted());
    // both signals for the same tick (the hook AND the return value): still one contact
    w.startAttempt();
    w.hookBypassed = false;
    for (int64_t t = 20; t < 40; ++t) {
        w.liveKill(1, t);
        Candidate ret = w.liveCandidate(1, t, false);
        ret.via = Via::CollisionReturn;
        w.raise(ret);
    }
    CHECK(w.wouldBe() == 1);
    // a collision check that ran with ignoreDamage raised no kill: its return value is never a death.
    // Click Between Frames probes the real player like this between its sub-steps, right before the
    // game's own check - a normal death under CBF must stay ONE death and no would-be death
    w.startAttempt();
    w.noclip = false;
    for (int64_t t = 5; t < 25; ++t) {
        Candidate probe = w.liveCandidate(1, t, false);
        probe.via = Via::CollisionReturn;
        probe.ignoreDamage = true;
        Verdict iv = w.raise(probe);
        CHECK(iv.decision == Decision::Rejected);
        CHECK(iv.reason == Reason::IgnoreDamage);
        CHECK(iv.source == Source::RejectedNotLethal);
    }
    CHECK(w.wouldBe() == 0);
    CHECK(w.det.firstWouldBeTick() == -1);
    Candidate probeAtDeath = w.liveCandidate(1, 25, false);
    probeAtDeath.via = Via::CollisionReturn;
    probeAtDeath.ignoreDamage = true;
    CHECK(w.raise(probeAtDeath).decision == Decision::Rejected);   // the sub-step probe overlaps the spike
    CHECK(w.liveKill(1, 25).decision == Decision::Death);          // then the game's own check kills
    CHECK(w.wouldBe() == 0);
    CHECK(w.deaths() == 1);
    w.noclip = true;
}

// ---- K. many ticks in one hazard ----
void testManyTicks() {
    SECTION("K. 470 consecutive lethal ticks: 1 (the v0.14.1 tick-unit fix stays)");
    World w = freshAttempt();
    for (int64_t t = 0; t < 470; ++t) w.liveKill(1, 3000 + t);
    CHECK(w.wouldBe() == 1);
    CHECK(w.det.counters().continued == 469);
    // fed from GD's progress counter (2 units per tick) through classify::tickFromProgress
    w.startAttempt();
    for (int64_t progress = 6000; progress < 6000 + 2 * 470; progress += 2) w.liveKill(1, classify::tickFromProgress(progress));
    CHECK(w.wouldBe() == 1);
    // the raw progress units would not be contiguous: this is the bug that must not come back
    w.startAttempt();
    for (int64_t progress = 6000; progress < 6000 + 2 * 470; progress += 2) w.liveKill(1, progress);
    CHECK(w.wouldBe() == 470);
}

// ---- L. noclip off ----
void testNoclipOff() {
    SECTION("L. noclip off: a normal death, never an extra would-be death");
    World w = freshAttempt();
    w.noclip = false;
    Verdict v = w.liveKill(1, 777);
    CHECK(v.decision == Decision::Death);
    CHECK(v.reason == Reason::RealDeath);
    CHECK(v.source == Source::LiveGdDeath);
    CHECK(v.contactId == 0);
    CHECK(w.deaths() == 1);
    CHECK(w.wouldBe() == 0);
    CHECK(!w.det.attemptOpen());
    // GD keeps calling on the following ticks / for player 2 while the death animation plays
    Verdict again = w.liveKill(1, 778);
    CHECK(again.decision == Decision::Rejected);
    Verdict p2 = w.liveKill(2, 777);
    CHECK(p2.decision == Decision::Rejected);
    CHECK(w.deaths() == 1);
    CHECK(w.wouldBe() == 0);
    // noclip attempt that ends in a real death (noclip switched off mid-run): both are kept apart
    w.startAttempt();
    w.noclip = true;
    for (int64_t t = 10; t < 15; ++t) w.liveKill(1, t);
    w.noclip = false;
    CHECK(w.liveKill(1, 200).decision == Decision::Death);
    CHECK(w.wouldBe() == 1);
    CHECK(w.deaths() == 1);
    // dual: player 2 dies -> GD destroys both, one death
    w.startAttempt();
    Verdict d2 = w.liveKill(2, 5);
    CHECK(d2.decision == Decision::Death);
    CHECK(d2.player == 2);
    CHECK(w.liveKill(1, 5).decision == Decision::Rejected);
    CHECK(w.deaths() == 1);
}

// ---- GD's own non-kills ----
void testGdNonKills() {
    SECTION("kills GD itself does not execute are not would-be deaths");
    World w = freshAttempt();
    // end animation of a level without 2.2 changes: collisions still run for the locked player and
    // PlayLayer::destroyPlayer returns at once (m_isLocked) - no noclip involved
    for (int64_t t = 5000; t < 5060; ++t) {
        Candidate c = w.liveCandidate(1, t, true);
        c.playerLocked = true;
        c.gdDeathFired = true;
        Verdict v = w.raise(c);
        CHECK(v.decision == Decision::Rejected);
        CHECK(v.reason == Reason::LockedPlayer);
        CHECK(v.source == Source::RejectedNotLethal);
    }
    // the anti-cheat spike GD keeps moving onto the player
    Candidate spike = w.liveCandidate(1, 1, true);
    spike.anticheatSpike = true;
    spike.gdDeathFired = true;
    CHECK(w.raise(spike).reason == Reason::AnticheatSpike);
    // m_playerDied already set
    Candidate flag = w.liveCandidate(1, 2, true);
    flag.layerPlayerDied = true;
    flag.gdDeathFired = true;
    CHECK(w.raise(flag).reason == Reason::PlayerDiedFlag);
    // a player that was dead before the call
    Candidate deadBefore = w.liveCandidate(2, 3, true);
    deadBefore.wasDeadBefore = true;
    deadBefore.deadAfter = true;
    CHECK(w.raise(deadBefore).reason == Reason::AlreadyDead);
    CHECK(w.wouldBe() == 0);
    CHECK(w.deaths() == 0);
    CHECK(w.det.attemptOpen());
    CHECK(w.det.counters().rejectedNotLethal == 63);
}

// ---- out of bounds ----
void testOutOfBounds() {
    SECTION("out of bounds: GD's kill re-arms every second tick, still one contact");
    World w = freshAttempt();
    // m_isOutOfBounds must be set on two consecutive ticks and is cleared when the kill fires, so
    // a player that stays out of bounds under noclip is killed on every SECOND tick, without an object
    for (int64_t t = 100; t < 400; t += 2) w.liveKill(1, t, false);
    CHECK(w.wouldBe() == 1);
    // back in bounds for a few ticks, then out again: a second contact
    for (int64_t t = 410; t < 440; t += 2) w.liveKill(1, t, false);
    CHECK(w.wouldBe() == 2);
    // the allowance is for object-less kills only: a hazard contact must be on consecutive ticks
    w.startAttempt();
    for (int64_t t = 100; t < 120; t += 2) w.liveKill(1, t, true);
    CHECK(w.wouldBe() == 10);
    // an object-less kill two ticks after a HAZARD kill is a new contact as well
    w.startAttempt();
    w.liveKill(1, 100, true);
    CHECK(w.liveKill(1, 102, false).decision == Decision::WouldBeDeath);
    // three ticks apart never continues
    w.startAttempt();
    w.liveKill(1, 100, false);
    CHECK(w.liveKill(1, 103, false).decision == Decision::WouldBeDeath);
    // an older tick (clamped clocks) never starts a death
    CHECK(w.liveKill(1, 90, false).decision == Decision::ContinuesContact);
}

// ---- kills from outside GD's physics step ----
void testOutsideLiveStep() {
    SECTION("the real player's pointer outside GD's own physics step");
    World w = freshAttempt();
    // swallowed: somebody simulated on the real PlayerObject, or called destroyPlayer directly
    Candidate c = w.liveCandidate(1, 10, false);
    c.inLiveStep = false;
    Verdict v = w.raise(c);
    CHECK(v.decision == Decision::Rejected);
    CHECK(v.source == Source::SimulatedCollision);
    CHECK(w.wouldBe() == 0);
    // the player really died (a kill-at-percent hack, a death link): the attempt ends, marked external
    c.deadAfter = true;
    c.gdDeathFired = true;
    Verdict d = w.raise(c);
    CHECK(d.decision == Decision::Death);
    CHECK(d.reason == Reason::ExternalKillDeath);
    CHECK(d.source == Source::ExternalKill);
    CHECK(!w.det.attemptOpen());
    // scope hooks not observed (another build / a hook that failed to install): the rule is off,
    // the detector falls back to "alive after GD's death path"
    w.startAttempt();
    Candidate unknown = w.liveCandidate(1, 10, true);
    unknown.inLiveStep = false;
    unknown.liveStepKnown = false;
    CHECK(w.raise(unknown).decision == Decision::WouldBeDeath);
    unknown.tick = 500;
    unknown.deadAfter = true;
    Verdict real = w.raise(unknown);
    CHECK(real.decision == Decision::Death);
    CHECK(real.source == Source::LiveGdDeath);
}

// ---- debug records ----
void testDebugRecords() {
    SECTION("debug records: every §1 field in one line, bounded history");
    World w = freshAttempt();
    DebugRecord r;
    r.sessionId = "19a7c3";
    r.attemptId = "19a7c3-a4";
    r.tick = 1234;
    r.percent = 42.125;
    r.playerPtr = 0xabcdef12;
    r.playerX = 100.f;
    r.playerY = 50.f;
    r.playerBox = {true, 85.f, 35.f, 30.f, 30.f};
    r.hasObject = true;
    r.objectId = 8;
    r.objectUid = 4711;
    r.objectType = 2;
    r.objectX = 110.f;
    r.objectY = 45.f;
    r.objectRotation = 90.f;
    r.objectBox = {true, 107.f, 39.f, 6.f, 12.f};
    r.megahackLoaded = true;
    r.noclipObserved = true;
    r.candidate = w.liveCandidate(1, 1234, true);
    r.verdict = w.raise(r.candidate);
    std::string line = format(r);
    for (char const* field : {"ACCEPT would_be_death", "contact=1", "reason=new_lethal_contact", "source=live_gd_death", "via=destroy_hook", "origin=live_play_layer",
                              "session=19a7c3", "attempt=19a7c3-a4", "gen=1/1", "tick=1234", "pct=42.125", "player=P1", "ptr=0xabcdef12", "object id=8", "uid=4711",
                              "type=2", "rot=90.0", "scale=(1.00,1.00)", "box=(107.0,39.0 6.0x12.0)", "boxes_overlap=yes", "gd_death_fired=no", "dead_after=no",
                              "gprl_inferred=no", "in_live_step=yes", "megahack=yes", "noclip_observed=yes", "menu_noclip=no"}) {
        CHECK_MSG(line.find(field) != std::string::npos, std::string("missing '") + field + "' in: " + line);
    }
    CHECK(overlayLabel(r).find("WOULD-BE #1 LIVE_GD_DEATH P1 t1234 obj 8") != std::string::npos);
    float cx = 0.f, cy = 0.f;
    CHECK(overlapCentre(r.playerBox, r.objectBox, cx, cy));
    CHECK_NEAR(cx, 110.0, 1e-4);
    CHECK_NEAR(cy, 45.0, 1e-4);
    Box far{true, 500.f, 500.f, 10.f, 10.f};
    CHECK(!overlaps(r.playerBox, far));
    CHECK(!overlaps(r.playerBox, Box{}));

    // a 470-tick contact is two entries (the new contact + one folded continuation), not 470 lines
    DebugHistory history(8);
    CHECK(history.add(r));
    int stored = 1;
    for (int64_t t = 1235; t < 1234 + 470; ++t) {
        DebugRecord n = r;
        n.tick = t;
        n.candidate.tick = t;
        n.verdict = w.raise(n.candidate);
        stored += history.add(n) ? 1 : 0;
    }
    CHECK(stored == 2);
    CHECK(history.entries().size() == 2);
    CHECK(history.entries().back().repeats == 468);
    CHECK(history.entries().back().lastTick == 1234 + 469);
    CHECK(format(history.entries().back()).find("+468 more through tick 1703") != std::string::npos);
    // a dying clone pool is one entry however many trials die
    int cloneLines = 0;
    for (int64_t t = 2000; t < 4000; t += 9) {
        DebugRecord n;
        n.tick = t;
        n.candidate = cloneKill(w, t);
        n.verdict = w.raise(n.candidate);
        cloneLines += history.add(n) ? 1 : 0;
    }
    CHECK(cloneLines == 1);
    // a rejection of another kind in between is its own entry, and the history stays bounded
    for (int i = 0; i < 40; ++i) {
        DebugRecord n;
        n.tick = 5000 + i * 10;
        n.candidate = w.liveCandidate(1, n.tick, true);
        n.candidate.playerLocked = i % 2 == 0;
        n.candidate.anticheatSpike = i % 2 == 1;
        n.verdict = w.raise(n.candidate);
        history.add(n);
    }
    CHECK(history.entries().size() == 8);
    CHECK(history.dropped() > 0);
    history.clear();
    CHECK(history.entries().empty());
}

// ---- wire names ----
void testNames() {
    SECTION("wire vocabulary (docs/TELEMETRY.md §12)");
    CHECK(std::string(kDetectorVersion) == "noclip-death-detector/2");
    CHECK(std::string(name(Source::LiveGdDeath)) == "live_gd_death");
    CHECK(std::string(name(Source::ExternalKill)) == "external_kill");
    CHECK(std::string(name(Source::SimulatedCollision)) == "simulated_collision");
    CHECK(std::string(name(Source::RejectedClone)) == "rejected_clone");
    CHECK(std::string(name(Source::RejectedStaleAttempt)) == "rejected_stale_attempt");
    CHECK(std::string(name(Source::RejectedUnknownPlayer)) == "rejected_unknown_player");
    CHECK(std::string(name(Source::RejectedNotLethal)) == "rejected_not_lethal");
    // only an accepted death / would-be death ever becomes a death event
    Verdict v;
    CHECK(!v.emitsEvent());
    v.decision = Decision::ContinuesContact;
    CHECK(v.accepted() && !v.emitsEvent());
    v.decision = Decision::WouldBeDeath;
    CHECK(v.emitsEvent());
    v.decision = Decision::Death;
    CHECK(v.emitsEvent());
}

}  // namespace

int main() {
    testOpenAir();
    testSingleSpike();
    testTwoSpikes();
    testOverlappingHazards();
    testSolverClone();
    testDual();
    testResetRace();
    testMovingHazard();
    testRotatedSaw();
    testSuppressedDeath();
    testManyTicks();
    testNoclipOff();
    testGdNonKills();
    testOutOfBounds();
    testOutsideLiveStep();
    testDebugRecords();
    testNames();
    return gprl::test::finish("death_detector_tests");
}
