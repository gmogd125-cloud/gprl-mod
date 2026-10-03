// Gamemode-portal clone model host tests (docs/LIVE_ISOLATION_DESIGN.md §2.5, step 2): the plan of
// every portal type equals the player part of GD's branch read from the 2.2081 disassembly
// (collisionCheckObjects 0x214e86-0x215544), the enabling toggle receives the requested noEffects
// and nothing else does, the plan set equals the isolation table's BLOCK-MODEL set, and the
// linked-dual gravity rule (playerWillSwitchMode 0x213001-0x213117) is a pure function of the pair.
#include "test_util.hpp"

#include "../core/solver/isolation.hpp"
#include "../core/solver/portal_model.hpp"

#include <string>
#include <vector>

using namespace gprl::solver;
using portal::Op;
using portal::Step;

namespace {

std::vector<Step> stepsOf(portal::Plan const& p) {
    return std::vector<Step>(p.steps, p.steps + p.count);
}

Step on(Op op, bool ne) { return Step{op, true, ne}; }
Step off(Op op) { return Step{op, false, false}; }
Step S(Op op) { return Step{op, false, false}; }

char const* opName(Op op) {
    switch (op) {
        case Op::SwitchedToMode: return "switchedToMode";
        case Op::ToggleFly: return "toggleFlyMode";
        case Op::ToggleBird: return "toggleBirdMode";
        case Op::ToggleRoll: return "toggleRollMode";
        case Op::ToggleDart: return "toggleDartMode";
        case Op::ToggleRobot: return "toggleRobotMode";
        case Op::ToggleSpider: return "toggleSpiderMode";
        case Op::ToggleSwing: return "toggleSwingMode";
        case Op::LastPortal: return "lastPortal";
        case Op::UpdatePlayerArt: return "updatePlayerArt";
        case Op::UpdateDashArt: return "updateDashArt";
    }
    return "?";
}

std::string render(std::vector<Step> const& v) {
    std::string s;
    for (auto const& st : v) {
        s += opName(st.op);
        if (st.op != Op::LastPortal && st.op != Op::SwitchedToMode && st.op != Op::UpdatePlayerArt && st.op != Op::UpdateDashArt) {
            s += st.enable ? "(1," : "(0,";
            s += st.noEffects ? "1)" : "0)";
        }
        s += " ";
    }
    return s;
}

void expectPlan(int type, bool ne, std::vector<Step> const& want, char const* what) {
    auto got = stepsOf(portal::planFor(type, ne));
    CHECK_MSG(got == want, std::string(what) + ": got [" + render(got) + "] want [" + render(want) + "]");
}

void testPlansMatchAsm() {
    SECTION("planFor: each of the eight branches, in GD's order (cube 0x214e86, ship 0x214f61, ufo 0x215019, swing 0x2150d1, ball 0x215196, wave 0x215290, robot 0x215355, spider 0x21544f)");
    for (bool ne : {false, true}) {
        // cube: position, then every mode off with (false, false), then the art refresh
        expectPlan(6, ne, {S(Op::LastPortal), off(Op::ToggleFly), off(Op::ToggleBird), off(Op::ToggleRoll), off(Op::ToggleDart), off(Op::ToggleRobot),
                           off(Op::ToggleSpider), off(Op::ToggleSwing), S(Op::UpdatePlayerArt), S(Op::UpdateDashArt)},
                   "cube");
        // ship / ufo / swing / wave: switchedToMode(type), position, toggle<Mode>(true, noEffects)
        expectPlan(5, ne, {S(Op::SwitchedToMode), S(Op::LastPortal), on(Op::ToggleFly, ne)}, "ship");
        expectPlan(19, ne, {S(Op::SwitchedToMode), S(Op::LastPortal), on(Op::ToggleBird, ne)}, "ufo");
        expectPlan(41, ne, {S(Op::SwitchedToMode), S(Op::LastPortal), on(Op::ToggleSwing, ne)}, "swing");
        expectPlan(26, ne, {S(Op::SwitchedToMode), S(Op::LastPortal), on(Op::ToggleDart, ne)}, "wave");
        // ball / robot / spider: the six OTHER modes off, position, the mode on
        expectPlan(16, ne, {off(Op::ToggleFly), off(Op::ToggleBird), off(Op::ToggleDart), off(Op::ToggleRobot), off(Op::ToggleSpider), off(Op::ToggleSwing),
                            S(Op::LastPortal), on(Op::ToggleRoll, ne)},
                   "ball");
        expectPlan(27, ne, {off(Op::ToggleFly), off(Op::ToggleBird), off(Op::ToggleRoll), off(Op::ToggleDart), off(Op::ToggleSpider), off(Op::ToggleSwing),
                            S(Op::LastPortal), on(Op::ToggleRobot, ne)},
                   "robot");
        expectPlan(33, ne, {off(Op::ToggleFly), off(Op::ToggleBird), off(Op::ToggleRoll), off(Op::ToggleDart), off(Op::ToggleRobot), off(Op::ToggleSwing),
                            S(Op::LastPortal), on(Op::ToggleSpider, ne)},
                   "spider");
    }
}

void testPlanInvariants() {
    SECTION("plan invariants: exactly one LastPortal, at most one enabling toggle (with the requested noEffects), disabling toggles never carry noEffects, non-portals are empty");
    for (int type = -1; type <= 60; ++type) {
        auto p = portal::planFor(type, true);
        if (!portal::isGamemodePortal(type)) {
            CHECK_MSG(p.count == 0, "non-portal type " + std::to_string(type) + " has an empty plan");
            continue;
        }
        int lastPortal = 0, enabling = 0, badOff = 0;
        for (int i = 0; i < p.count; ++i) {
            auto const& s = p.steps[i];
            if (s.op == Op::LastPortal) ++lastPortal;
            bool toggle = s.op >= Op::ToggleFly && s.op <= Op::ToggleSwing;
            if (toggle && s.enable) { ++enabling; CHECK(s.noEffects); }
            if (toggle && !s.enable && s.noEffects) ++badOff;
        }
        CHECK_MSG(lastPortal == 1, "type " + std::to_string(type) + " sets the portal fields once");
        CHECK_MSG(enabling == (type == 6 ? 0 : 1), "type " + std::to_string(type) + " enabling toggles");
        CHECK(badOff == 0);
        CHECK(p.count <= portal::kMaxSteps);
    }
    // the plan with noEffects=false differs from the plan with true only in the enabling toggle
    for (int type : {5, 16, 19, 26, 27, 33, 41}) {
        auto a = stepsOf(portal::planFor(type, false));
        auto b = stepsOf(portal::planFor(type, true));
        CHECK(a.size() == b.size());
        int diffs = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            if (!(a[i] == b[i])) { ++diffs; CHECK(a[i].enable && b[i].enable && a[i].op == b[i].op); }
        }
        CHECK_MSG(diffs == 1, "type " + std::to_string(type) + ": noEffects reaches one call");
    }
}

void testSetEqualsIsolationTable() {
    SECTION("isGamemodePortal == (treatmentOf == BlockModel) for every type, and the model is switched on");
    for (int type = -2; type < isolation::kObjectTypeCount + 2; ++type) {
        bool block = isolation::treatmentOf(type) == isolation::Treatment::BlockModel;
        CHECK_MSG(portal::isGamemodePortal(type) == block, "type " + std::to_string(type));
    }
    CHECK(isolation::kModelGamemodePortals);
    CHECK(std::string(portal::kVersion) == "gprl-portal-model/1");
}

void testGlitterAndNoEffects() {
    SECTION("keepsGlitter = the 0x20004080020 mask (ship, ufo, wave, swing); gdNoEffects reads portal 2's flag only in dual with a portal 2");
    for (int type = 0; type < 48; ++type) {
        bool inMask = type == 5 || type == 19 || type == 26 || type == 41;
        CHECK(portal::keepsGlitter(type) == inMask);
    }
    CHECK(portal::gdNoEffects(false, false, true, false) == false);
    CHECK(portal::gdNoEffects(false, false, false, true) == true);
    CHECK(portal::gdNoEffects(false, true, true, false) == false);   // not dual: portal 2 ignored
    CHECK(portal::gdNoEffects(true, false, true, false) == false);   // dual without portal 2: the portal's own
    CHECK(portal::gdNoEffects(true, true, true, false) == true);     // dual with portal 2: its flag
    CHECK(portal::gdNoEffects(true, true, false, true) == false);
}

void testDualGravityRule() {
    SECTION("dualGravity mirrors 0x213001-0x213117: only in linked dual, only when the partner already has the mode, gravity = opposite of the partner's; wave has no case");
    portal::ModeFlags cube{};
    portal::ModeFlags ship{}; ship.ship = true;
    portal::ModeFlags ball{}; ball.ball = true;
    portal::ModeFlags spider{}; spider.spider = true;
    // partnerInMode
    CHECK(portal::partnerInMode(6, cube));
    CHECK(!portal::partnerInMode(6, ship));
    CHECK(!portal::partnerInMode(6, ball));
    CHECK(portal::partnerInMode(5, ship));
    CHECK(!portal::partnerInMode(5, cube));
    CHECK(portal::partnerInMode(16, ball));     // ball reads 0x9bb = m_isBall
    portal::ModeFlags wave{}; wave.dart = true;
    CHECK(!portal::partnerInMode(26, wave));    // the wave portal has no case in GD's jump table
    CHECK(portal::partnerInMode(33, spider));
    CHECK(!portal::partnerInMode(19, spider));
    CHECK(!portal::partnerInMode(0, cube));     // not a gamemode portal
    // the decision
    auto none = portal::GravityDecision{};
    CHECK(portal::dualGravity(false, false, false, 5, ship, false) == none);   // not dual
    CHECK(portal::dualGravity(true, true, false, 5, ship, false) == none);    // settings skip
    CHECK(portal::dualGravity(true, false, true, 5, ship, false) == none);    // unlinked
    CHECK(portal::dualGravity(true, false, false, 5, cube, false) == none);   // partner not yet in the mode
    auto d1 = portal::dualGravity(true, false, false, 5, ship, false);
    CHECK(d1.flip && d1.upsideDown);            // partner normal gravity -> the switcher is flipped
    auto d2 = portal::dualGravity(true, false, false, 5, ship, true);
    CHECK(d2.flip && !d2.upsideDown);
    auto d3 = portal::dualGravity(true, false, false, 6, cube, true);
    CHECK(d3.flip && !d3.upsideDown);
    CHECK(portal::dualGravity(true, false, false, 26, wave, false) == none);  // wave: never
    CHECK(portal::dualGravity(true, false, false, 16, ball, false).flip);     // ball links
}

}  // namespace

int main() {
    testPlansMatchAsm();
    testPlanInvariants();
    testSetEqualsIsolationTable();
    testGlitterAndNoEffects();
    testDualGravityRule();
    return gprl::test::finish("portal_model_tests");
}
