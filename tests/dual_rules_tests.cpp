// Dual-mode rule host tests (docs/LIVE_ISOLATION_DESIGN.md §2.7, ISO-D6): the pure mirrors of
// GD 2.2081's dual link read from the disassembly. The gravity link of GJBaseGameLayer::flipGravity
// (0x212b3f-0x212bf8) targets the PAIR PARTNER (never a real player); the legacy uid rule shows why
// a clone flipped the real player 1 up to v0.7.2 (D1); the gamemode-portal gravity rule of
// playerWillSwitchMode (0x213001-0x213117) and the input routing of handleButton (0x2338e0).
// portal_model.hpp carries the same portal rule under another name: the two must agree.
#include "test_util.hpp"

#include "../core/solver/dual_rules.hpp"
#include "../core/solver/portal_model.hpp"

#include <string>

using namespace gprl::solver;

namespace {

dual::ModeFlags flags(char mode) {
    dual::ModeFlags m;
    switch (mode) {
        case 'S': m.ship = true; break;
        case 'U': m.bird = true; break;
        case 'B': m.ball = true; break;
        case 'W': m.dart = true; break;
        case 'R': m.robot = true; break;
        case 'P': m.spider = true; break;
        case 'G': m.swing = true; break;
        default: break;   // cube
    }
    return m;
}

void testGravityLink() {
    SECTION("gravityLinkTarget: linked dual with equal modes -> the partner; unlink / solo / two-player / mode mismatch -> none; wave is not compared");
    dual::DualFacts f;
    f.dualMode = true;
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::Partner);
    f.unlink = true;
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::None);
    f.unlink = false;
    f.twoPlayerMode = true;
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::None);
    f.twoPlayerMode = false;
    f.dualMode = false;
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::None);
    f.dualMode = true;
    f.p1 = flags('S');
    f.p2 = flags('C');
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::None);
    f.p2 = flags('S');
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::Partner);
    // the dart flag is not part of the mode test: cube P1 + wave P2 still link
    f.p1 = flags('C');
    f.p2 = flags('W');
    CHECK(dual::linkModesEqual(f.p1, f.p2));
    CHECK(dual::gravityLinkTarget(f) == dual::LinkTarget::Partner);
    for (char a : {'S', 'U', 'B', 'R', 'P', 'G'}) {
        CHECK(!dual::linkModesEqual(flags(a), flags('C')));
        CHECK(dual::linkModesEqual(flags(a), flags(a)));
    }
    CHECK(dual::isCube(flags('C')) && !dual::isCube(flags('W')));
}

void testLegacyTargetExplainsD1() {
    SECTION("legacy uid rule: the real player 1 flips its partner, a clone (uid != real P1) flipped the REAL player 1 (design D1)");
    dual::DualFacts f;
    f.dualMode = true;
    constexpr int kRealP1 = 1, kClone = 20493;
    CHECK(dual::legacyGravityLinkTarget(f, kRealP1, kRealP1) == dual::LegacyTarget::RealPlayer2);
    CHECK(dual::legacyGravityLinkTarget(f, kClone, kRealP1) == dual::LegacyTarget::RealPlayer1);
    f.unlink = true;
    CHECK(dual::legacyGravityLinkTarget(f, kClone, kRealP1) == dual::LegacyTarget::None);
    // the owner's fingerprint: real vy -0.48 (flipped by the portal x0.5, flipped back by the clone's
    // link x0.5) vs the shadow's -0.96 (flipped once)
    double const v = -1.92;
    CHECK_NEAR(v * dual::kFlipVelocityFactor, -0.96, 1e-9);
    CHECK_NEAR(v * dual::kFlipVelocityFactor * dual::kFlipVelocityFactor, -0.48, 1e-9);
}

void testModeSwitchGravityAgreesWithPortalModel() {
    SECTION("modeSwitchGravity == portal::dualGravity for every portal type and partner mode (two transcriptions of 0x213001-0x213117)");
    for (int type : {0, 5, 6, 16, 19, 26, 27, 33, 41, 42}) {
        for (char pm : {'C', 'S', 'U', 'B', 'W', 'R', 'P', 'G'}) {
            for (bool up : {false, true}) {
                auto d = dual::modeSwitchGravity(type, true, false, false, flags(pm), up);
                portal::ModeFlags pf;
                auto m = flags(pm);
                pf.ship = m.ship; pf.bird = m.bird; pf.ball = m.ball; pf.dart = m.dart; pf.robot = m.robot; pf.spider = m.spider; pf.swing = m.swing;
                auto p = portal::dualGravity(true, false, false, type, pf, up);
                CHECK_MSG(d.flip == p.flip && d.upsideDown == p.upsideDown, "type " + std::to_string(type) + " partner " + pm);
            }
        }
    }
    // the two gates
    CHECK(dual::modeSwitchGravity(5, false, false, false, flags('S'), false) == dual::ModeSwitchGravity{});
    CHECK(dual::modeSwitchGravity(5, true, true, false, flags('S'), false) == dual::ModeSwitchGravity{});
    CHECK(dual::modeSwitchGravity(5, true, false, true, flags('S'), false) == dual::ModeSwitchGravity{});
    auto d = dual::modeSwitchGravity(5, true, false, false, flags('S'), false);
    CHECK(d.flip && d.upsideDown);
    // wave (26) never links through a portal; ball (16) links in dual_rules' reading of the table
    CHECK(!dual::partnerHasTargetMode(26, flags('W')));
}

void testInputTargets() {
    SECTION("inputTargets (handleButton 0x2338e0): one key drives both players of a single-control dual, P1 first");
    auto solo = dual::inputTargets(false, false, true);
    CHECK(solo.p1 && !solo.p2);
    auto both = dual::inputTargets(true, false, true);
    CHECK(both.p1 && both.p2);
    auto twoP1 = dual::inputTargets(true, true, true);
    CHECK(twoP1.p1 && !twoP1.p2);
    auto twoP2 = dual::inputTargets(true, true, false);
    CHECK(!twoP2.p1 && twoP2.p2);
    auto p2keySolo = dual::inputTargets(false, false, false);
    CHECK(p2keySolo.p1 && !p2keySolo.p2);
    auto entry = dual::p2EntryState(false, 3.5);
    CHECK(entry.upsideDown && entry.yVelocity == -3.5);
}

}  // namespace

int main() {
    testGravityLink();
    testLegacyTargetExplainsD1();
    testModeSwitchGravityAgreesWithPortalModel();
    testInputTargets();
    return gprl::test::finish("dual_rules_tests");
}
