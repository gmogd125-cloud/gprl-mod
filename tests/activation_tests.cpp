// Activation-slot isolation host tests (docs/SOLVER_DESIGN.md §12): the game's slot rules as
// pure asm mirrors, the logical-world pack / present / repack round trip, and a synthetic
// reproduction of the in-game 2026-09-30 mismatch ("step 339: touching rings 1 vs 0" on every
// release job near an orb) with the v0.4.x rule failing and the v0.5.0 rule passing.
#include "test_util.hpp"

#include "../core/solver/activation.hpp"

#include <vector>

using namespace gprl;
using namespace gprl::solver::activation;

namespace {

constexpr int kRealUid = 1;        // GD gives player 1 unique id 1
constexpr int kCloneUid = 20481;   // a hidden clone is a much later GameObject

// A minimal model of one orb and one player over the collision + ring rules that matter here
// (GJBaseGameLayer::collisionCheckObjects gate -> playerTouchedRing -> ringJump), with the
// physics left out: the orb is "overlapped" on every step of the test.
struct RingModel {
    Flags ring;
    int touchingRings = 0;   // PlayerObject::m_touchingRings->count() after the step
    bool holding = false;    // m_stateJumpBuffered && m_stateRingJump2 at collision time

    /// One collision step of `playerUid` against the orb (overlapping).
    void step(int playerUid) {
        if (gameHasBeenActivatedByPlayer(ring, playerUid, false)) return;   // skipped before any handling
        touchingRings = 1;                                                    // addToTouchedRings
        if (holding) {                                                        // ringJump fires
            ring = gameActivatedByPlayer(ring, playerUid, false);
            touchingRings = 0;                                                // m_touchingRings->removeObject(ring)
        }
    }
};

void testGameRules() {
    SECTION("asm mirrors: slot by unique id, multi-activate bypass, triggerActivated sets both");
    Flags f;
    CHECK(!gameHasBeenActivatedByPlayer(f, kRealUid, false));
    CHECK(!gameHasBeenActivatedByPlayer(f, kCloneUid, false));
    auto byReal = gameActivatedByPlayer(f, kRealUid, false);
    CHECK(byReal.slot1 && !byReal.slot2 && byReal.activated);
    auto byClone = gameActivatedByPlayer(f, kCloneUid, false);
    CHECK(!byClone.slot1 && byClone.slot2 && byClone.activated);
    CHECK(gameHasBeenActivatedByPlayer(byReal, kRealUid, false));
    CHECK(!gameHasBeenActivatedByPlayer(byReal, kCloneUid, false));   // THE asymmetry
    CHECK(gameHasBeenActivatedByPlayer(byClone, kCloneUid, false));
    CHECK(!gameHasBeenActivatedByPlayer(byClone, kRealUid, false));
    CHECK(!gameHasBeenActivatedByPlayer(byReal, kRealUid, true));     // multi-activate: never gated
    auto multi = gameActivatedByPlayer(f, kRealUid, true);
    CHECK(multi.activated && !multi.slot1 && !multi.slot2);
    auto trig = gameTriggerActivated(f);
    CHECK(trig.slot1 && trig.slot2);
    CHECK(usesSlot2(kCloneUid) && !usesSlot2(kRealUid) && usesSlot2(2));
}

void testPackRoundTrip() {
    SECTION("logical world: pack from the real player, present to a clone, repack after its step");
    Flags real;
    real.slot1 = true;   // the real player 1 used it
    uint8_t logical = packReal(real, false);
    CHECK(logical == kSelf);
    auto seen = presentToClone(logical);
    CHECK(seen.slot1 && seen.slot2 && !seen.isActivated && !seen.activated);
    CHECK(gameHasBeenActivatedByPlayer(seen, kCloneUid, false));   // the clone reads slot 2 = self
    CHECK(gameHasBeenActivatedByPlayer(seen, kRealUid, false));
    // untouched by the clone: the logical value survives
    CHECK(packAfterClone(seen, logical) == logical);
    // a clone activation lands in slot 2 and folds back into bit0
    Flags fresh = presentToClone(0);
    CHECK(fresh == Flags{});
    auto afterClone = gameActivatedByPlayer(fresh, kCloneUid, false);
    uint8_t after = packAfterClone(afterClone, 0);
    CHECK((after & kSelf) != 0);
    CHECK((after & kActivated) != 0);
    CHECK((after & kOther) == 0);
    // presenting that again keeps the clone consistent with itself on the next step
    CHECK(gameHasBeenActivatedByPlayer(presentToClone(after), kCloneUid, false));
    // a portal's triggerActivated (both slots) is one self activation
    CHECK((packAfterClone(gameTriggerActivated(Flags{}), 0) & kSelf) != 0);
    // the isActivated / activated bits ride along
    Flags both;
    both.isActivated = true;
    both.activated = true;
    CHECK(packReal(both, false) == (kIsActivated | kActivated));
    CHECK(presentToClone(kIsActivated | kActivated) == both);
}

void testDualAndOddRealSlot() {
    SECTION("player 2's slot is carried but never shown to or written by a clone; real player with id != 1");
    Flags real;
    real.slot2 = true;   // player 2 (dual) used it, player 1 did not
    uint8_t logical = packReal(real, false);
    CHECK(logical == kOther);
    auto seen = presentToClone(logical);
    CHECK(!seen.slot1 && !seen.slot2);                          // the simulated player 1 may still use it
    CHECK(!gameHasBeenActivatedByPlayer(seen, kCloneUid, false));
    auto used = gameActivatedByPlayer(seen, kCloneUid, false);
    uint8_t after = packAfterClone(used, logical);
    CHECK((after & kSelf) != 0 && (after & kOther) != 0);      // player 2's bit kept from before
    // a real player 1 whose unique id is not 1 would live in slot 2: packReal follows the flag
    Flags odd;
    odd.slot2 = true;
    CHECK(packReal(odd, true) == kSelf);
    Flags odd2;
    odd2.slot1 = true;
    CHECK(packReal(odd2, true) == kOther);
}

void testReleaseRegression() {
    SECTION("2026-09-30 log: release job control after the real player used the orb (rings 1 vs 0)");
    // The real player holds through the orb: at the entry step ringJump fires, the orb is
    // recorded in slot 1 and leaves m_touchingRings; on the next steps (still overlapping) the
    // gate skips it, so the real player reports 0 touching rings.
    RingModel real;
    real.holding = true;
    real.step(kRealUid);
    CHECK(real.ring.slot1 && real.touchingRings == 0);
    real.holding = false;   // the release
    real.step(kRealUid);
    CHECK(real.touchingRings == 0);

    // A control clone spawned from the snapshot of the release step (after the activation).
    // v0.4.x: the physical bits are copied as they are -> the clone reads slot 2 -> re-touches.
    {
        RingModel control;
        control.ring = unpackLegacy(packLegacy(real.ring));
        control.holding = false;
        control.step(kCloneUid);
        CHECK_MSG(control.touchingRings == 1, "the v0.4.x rule must reproduce 'touching rings 1 vs 0'");
    }
    // v0.5.0: the logical world mirrors the real player's slot into the clone's slot -> skipped.
    {
        RingModel control;
        control.ring = presentToClone(packReal(real.ring, false));
        control.holding = false;
        control.step(kCloneUid);
        CHECK_MSG(control.touchingRings == 0, "the mirrored slot must match the real player");
        CHECK(control.touchingRings == real.touchingRings);
    }
}

void testPressJobControl() {
    SECTION("a control spawned BEFORE the activation activates the orb itself and stays consistent (why presses never mismatched)");
    RingModel real;
    real.holding = true;
    // snapshot before the entry step: nothing activated
    uint8_t logical = packReal(real.ring, false);
    CHECK(logical == 0);
    RingModel control;
    control.ring = presentToClone(logical);
    control.holding = true;
    // entry step for both
    real.step(kRealUid);
    control.step(kCloneUid);
    CHECK(real.touchingRings == control.touchingRings);
    uint8_t after = packAfterClone(control.ring, logical);
    CHECK((after & kSelf) != 0);
    // next step (still overlapping, released): both skip the used orb
    real.holding = false;
    control.holding = false;
    real.step(kRealUid);
    control.ring = presentToClone(after);
    control.step(kCloneUid);
    CHECK(real.touchingRings == 0 && control.touchingRings == 0);
    // and with the legacy rule the press-job control was consistent too (its own slot 2)
    RingModel legacy;
    legacy.ring = unpackLegacy(0);
    legacy.holding = true;
    legacy.step(kCloneUid);
    legacy.holding = false;
    legacy.ring = unpackLegacy(packLegacy(legacy.ring));
    legacy.step(kCloneUid);
    CHECK(legacy.touchingRings == 0);
}

void testPadAndPortalSameRule() {
    SECTION("pads / portals: canBeActivatedByPlayer = !hasBeenActivatedByPlayer, same slot rule");
    Flags pad;
    pad = gameActivatedByPlayer(pad, kRealUid, false);   // the real player bounced on it
    // a control spawned afterwards must not bounce again
    CHECK(gameHasBeenActivatedByPlayer(presentToClone(packReal(pad, false)), kCloneUid, false));
    // v0.4.x would have bounced (slot 2 clear)
    CHECK(!gameHasBeenActivatedByPlayer(unpackLegacy(packLegacy(pad)), kCloneUid, false));
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    testGameRules();
    testPackRoundTrip();
    testDualAndOddRealSlot();
    testReleaseRegression();
    testPressJobControl();
    testPadAndPortalSameRule();
    return gprl::test::finish("activation_tests");
}
