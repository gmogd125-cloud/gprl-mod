// Live-state isolation host tests (docs/LIVE_ISOLATION_DESIGN.md §2.3-§2.4, owner spec
// docs/LIVE_ISOLATION_SPEC.md): the object-type treatment table covers every GameObjectType and
// classifies the known layer writers as never-run; the activation overlay reproduces the game's
// own gate rules (canBeActivatedByPlayer 0x2178c0, hasBeenActivatedByPlayer 0x1a4af0,
// activatedByPlayer 0x1a4a90) on a per-trial value, so a clone never reads or writes a live
// object's slots; and the dual gravity bug (a clone's flip reaching the real player 1) cannot
// recur because the clone-side rule has no "other player".
#include "test_util.hpp"

#include "../core/solver/activation.hpp"
#include "../core/solver/isolation.hpp"

#include <string>

using namespace gprl::solver;

namespace {

void testTreatmentTable() {
    SECTION("treatmentOf: every GameObjectType 0..47 classified; gamemode / dual / teleport portals never PASS");
    int pass = 0, block = 0, filter = 0, replace = 0;
    for (int t = 0; t < isolation::kObjectTypeCount; ++t) {
        auto tr = isolation::treatmentOf(t);
        switch (tr) {
            case isolation::Treatment::Pass: ++pass; break;
            case isolation::Treatment::BlockModel:
            case isolation::Treatment::BlockIgnore:
            case isolation::Treatment::BlockInvalid: ++block; break;
            case isolation::Treatment::Filter:
            case isolation::Treatment::FilterInvalid: ++filter; break;
            case isolation::Treatment::Replace: ++replace; break;
        }
    }
    CHECK(pass + block + filter + replace == isolation::kObjectTypeCount);
    // gamemode portals: ship, cube, ball, ufo, wave, robot, spider, swing
    for (int t : {5, 6, 16, 19, 26, 27, 33, 41}) CHECK_MSG(isolation::treatmentOf(t) == isolation::Treatment::BlockModel, "gamemode portal type " + std::to_string(t));
    // dual / solo / teleport portals rewrite the layer: the clone is invalid
    for (int t : {23, 24, 28}) CHECK(isolation::treatmentOf(t) == isolation::Treatment::BlockInvalid);
    // mirror portals: visual only
    CHECK(isolation::treatmentOf(14) == isolation::Treatment::BlockIgnore);
    CHECK(isolation::treatmentOf(15) == isolation::Treatment::BlockIgnore);
    // collectibles and touch triggers are never in a clone's list
    for (int t : {20, 22, 30, 31, 45}) CHECK(isolation::treatmentOf(t) == isolation::Treatment::Filter);
    // orbs are transcribed clone-local
    for (int t : {11, 12, 13, 29, 32, 35, 36, 37, 38, 43, 46}) CHECK(isolation::treatmentOf(t) == isolation::Treatment::Replace);
    // what GD runs for the clone with clone-only effects
    for (int t : {0, 2, 3, 4, 8, 9, 10, 17, 18, 21, 25, 34, 40, 42, 44, 47}) CHECK(isolation::treatmentOf(t) == isolation::Treatment::Pass);
    // not a type
    CHECK(isolation::treatmentOf(1) == isolation::Treatment::FilterInvalid);
    CHECK(isolation::treatmentOf(-1) == isolation::Treatment::FilterInvalid);
    CHECK(isolation::treatmentOf(48) == isolation::Treatment::FilterInvalid);
    CHECK(isolation::treatmentOf(9999) == isolation::Treatment::FilterInvalid);
    CHECK(std::string(isolation::kVersion) == "gprl-isolation/2");
}

void testOverlayGate() {
    SECTION("overlay canActivate mirrors canBeActivatedByPlayer 0x2178c0: one-shot objects activate once per trial, multi-activate once per (object, player) entry");
    using namespace activation;
    // one-shot orb, fresh for this trial
    auto d1 = canActivate(0, kSelf, false);
    CHECK(d1.result);
    CHECK((d1.logical & kMapSelf) != 0);
    CHECK((d1.logical & kSelf) == 0);   // the gate records the map entry; activation itself is activatedByPlayer
    uint8_t after = activated(d1.logical, kSelf, false);
    CHECK((after & kSelf) != 0 && (after & kActivated) != 0);
    CHECK(hasBeen(after, kSelf, false));
    auto d2 = canActivate(after, kSelf, false);
    CHECK(!d2.result);                   // already activated by this trial's player: skipped like GD skips the real player
    // the OTHER slot (player 2 of a dual pair) is independent
    CHECK(!hasBeen(after, kOther, false));
    auto d3 = canActivate(after, kOther, false);
    CHECK(d3.result);
    CHECK((d3.logical & kMapOther) != 0);
    // multi-activate: gated by the map entry only, never by the slot
    auto m1 = canActivate(0, kSelf, true);
    CHECK(m1.result && (m1.logical & kMapSelf) != 0);
    auto m2 = canActivate(m1.logical, kSelf, true);
    CHECK(!m2.result);
    CHECK(!hasBeen(m1.logical, kSelf, true));
    uint8_t mAct = activated(m1.logical, kSelf, true);
    CHECK((mAct & kActivated) != 0 && (mAct & kSelf) == 0);
    // a pooled clone reused on a later job starts from the SNAPSHOT value, never from a stale map
    // entry of its own earlier life: the overlay is per trial, so a fresh trial sees 0 again
    auto fresh = canActivate(0, kSelf, true);
    CHECK(fresh.result);
}

void testOverlayMatchesGameRules() {
    SECTION("overlay rules equal the asm mirrors applied to a player that owns slot 1 (no unique-id asymmetry left)");
    using namespace activation;
    constexpr int kRealUid = 1;
    for (int multi = 0; multi < 2; ++multi) {
        Flags f;                         // the real object, untouched
        uint8_t logical = 0;             // the trial's view
        bool gameHas = gameHasBeenActivatedByPlayer(f, kRealUid, multi != 0);
        CHECK(hasBeen(logical, kSelf, multi != 0) == gameHas);
        Flags fAct = gameActivatedByPlayer(f, kRealUid, multi != 0);
        uint8_t lAct = activated(logical, kSelf, multi != 0);
        CHECK(((lAct & kSelf) != 0) == fAct.slot1);
        CHECK(((lAct & kActivated) != 0) == fAct.activated);
        CHECK(hasBeen(lAct, kSelf, multi != 0) == gameHasBeenActivatedByPlayer(fAct, kRealUid, multi != 0));
        // and the live object's bits were never needed: Flags f is still default
        CHECK(f == Flags{});
    }
}

void testRingPowerBit() {
    SECTION("ring power lives in the overlay (kRingPowered), not in RingObject::powerOnObject");
    using namespace activation;
    uint8_t v = 0;
    CHECK((v & kRingPowered) == 0);
    v = static_cast<uint8_t>(v | kRingPowered);
    CHECK((v & kRingPowered) != 0);
    CHECK((v & (kSelf | kOther | kMapSelf | kMapOther)) == 0);   // independent of the gate bits
}

void testTripwireList() {
    SECTION("tripwire census names every layer writer the design lists");
    std::string all;
    for (auto n : isolation::kTripwires) all += std::string(n) + ",";
    for (auto need : {"playerWillSwitchMode", "updateDualGround", "animateInDualGroundNew", "updateStaticCameraPos", "destroyObject",
                      "storeTriggeredID", "triggerActivated", "removeCheckpoint", "toggleDualMode", "teleportPlayer"}) {
        CHECK_MSG(all.find(std::string(need) + ",") != std::string::npos, std::string("tripwire ") + need);
    }
}

}  // namespace

int main() {
    testTreatmentTable();
    testOverlayGate();
    testOverlayMatchesGameRules();
    testRingPowerBit();
    testTripwireList();
    return gprl::test::finish("isolation_tests");
}
