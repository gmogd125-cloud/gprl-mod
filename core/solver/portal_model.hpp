#pragma once
// Gamemode-portal clone model (docs/LIVE_ISOLATION_DESIGN.md §2.5; live-state isolation step 2,
// mod v0.8.1). PURE C++20, no Geode includes; host-tested in tests/portal_model_tests.cpp.
//
// GD's collisionCheckObjects (win 0x214960) handles the eight gamemode portals in eight inline
// branches (cube 0x214e86, ship 0x214f61, ufo 0x215019, swing 0x2150d1, ball 0x215196, wave
// 0x215290, robot 0x215355, spider 0x21544f). Each one: canBeActivatedByPlayer ->
// playerWillSwitchMode 0x212ef0 (LAYER writes: m_lastActivatedPortal1, the camera free-mode /
// padding bytes +0x311 / +0x312 / +0x2cc / +0x2d0, updateDualGround, gameEventTriggered,
// toggleGlitter, the linked-dual gravity rule) -> per-PLAYER calls -> the common tail 0x214f45
// (playShineEffect on the portal, portal->activatedByPlayer(player)).
//
// A stepped clone must never run the layer part, so the engine blocks the branch at the gate
// (isolation::Treatment::BlockModel) and replays the PLAYER part itself from the plan below, in
// GD's order, with GD's arguments. The plan is data so the host tests pin it to the asm.
//
// What the branches call on the player (verified in D:\claude-scratch\gprl-auditB\step2\*.asm;
// none of these reads PlayerObject::m_gameLayer (+0xc20)):
//   switchedToMode(type)              0x39be30: toggles every OTHER mode off
//   toggle<Mode>(enable, noEffects)   0x39a4f0 fly, 0x39a820 bird, 0x39b570 roll, 0x39af90 dart,
//                                     0x39b6f0 robot, 0x39ba70 spider, 0x39ab20 swing
//   m_lastPortalPos / m_lastActivatedPortal (+0xa00 / +0xa80) = portal position / portal
//   updatePlayerArt 0x398860, updateDashArt 0x3961c0 (cube branch only)
// `noEffects` gates ONLY spawnPortalCircle / a CCCircleWave added to the live parent layer
// (0x39a786, 0x39aa8a, 0x39b633, 0x39b0b3 + 0x39b1c8, 0x39b906, 0x39bc9b, 0x39ac21): no physics
// behind it, so the clone passes `true` (no live node) where GD passes the portal's m_hasNoEffects.
// removePendingCheckpoint 0x3a3250 (inside the fly / bird / dart / swing toggles) releases the
// player's own m_pendingCheckpoint (+0x890): a clone never owns one; the executor checks.
#include <cstdint>

namespace gprl::solver::portal {

constexpr char const* kVersion = "gprl-portal-model/1";

enum class Op : uint8_t {
    SwitchedToMode,   // PlayerObject::switchedToMode(portal type)
    ToggleFly,
    ToggleBird,
    ToggleRoll,
    ToggleDart,
    ToggleRobot,
    ToggleSpider,
    ToggleSwing,
    LastPortal,       // m_lastPortalPos = portal position; m_lastActivatedPortal = portal
    UpdatePlayerArt,
    UpdateDashArt,
};

struct Step {
    Op op = Op::LastPortal;
    bool enable = false;      // toggles only
    bool noEffects = false;   // toggles only
    constexpr bool operator==(Step const&) const = default;
};

constexpr int kMaxSteps = 12;

struct Plan {
    Step steps[kMaxSteps] = {};
    int count = 0;
    constexpr void add(Op op, bool enable = false, bool noEffects = false) {
        if (count < kMaxSteps) steps[count++] = Step{op, enable, noEffects};
    }
};

/// ship 5, cube 6, ball 16, ufo 19, wave 26, robot 27, spider 33, swing 41
constexpr bool isGamemodePortal(int type) {
    return type == 5 || type == 6 || type == 16 || type == 19 || type == 26 || type == 27 || type == 33 || type == 41;
}

/// playerWillSwitchMode 0x212fde-0x212ffb: toggleGlitter(false) for every portal type NOT in the
/// mask 0x20004080020 (ship, ufo, wave, swing keep it: their branch toggles it on afterwards).
/// Visual only (hooked away for clones); here so the census of the branch is complete.
constexpr bool keepsGlitter(int type) { return type == 5 || type == 19 || type == 26 || type == 41; }

/// GD's `noEffects` for the enabling toggle (0x214fc5-0x214ff9 and the same block in every
/// enabling branch): the flag of the layer's m_lastActivatedPortal2 when the layer is dual and has
/// one, else the portal's own m_hasNoEffects (+0x41c). The clone executor does not use this value
/// (it passes true, see the header comment); documented and tested so the transcription is whole.
constexpr bool gdNoEffects(bool dual, bool havePortal2, bool portal2NoEffects, bool portalNoEffects) {
    return (dual && havePortal2) ? portal2NoEffects : portalNoEffects;
}

namespace detail {
// the explicit "modes off" lists, in GD's order: fly, bird, roll, dart, robot, spider, swing
constexpr Op kOffOrder[7] = {Op::ToggleFly, Op::ToggleBird, Op::ToggleRoll, Op::ToggleDart, Op::ToggleRobot, Op::ToggleSpider, Op::ToggleSwing};
constexpr void allOff(Plan& p) {
    for (Op op : kOffOrder) p.add(op, false, false);
}
constexpr void offExcept(Plan& p, Op keep) {
    for (Op op : kOffOrder) {
        if (op != keep) p.add(op, false, false);
    }
}
}  // namespace detail

/// The player part of the branch for `type`, in GD's order. `noEffects` is what the enabling
/// toggle receives (the clone passes true). Empty plan for a non-gamemode type.
constexpr Plan planFor(int type, bool noEffects) {
    Plan p;
    switch (type) {
        case 6:   // cube 0x214e86-0x214f5c: position, all seven off (0x214ee2-0x214f30), art
            p.add(Op::LastPortal);
            detail::allOff(p);
            p.add(Op::UpdatePlayerArt);
            p.add(Op::UpdateDashArt);
            break;
        case 5:   // ship 0x214f61-0x215014
            p.add(Op::SwitchedToMode);
            p.add(Op::LastPortal);
            p.add(Op::ToggleFly, true, noEffects);
            break;
        case 19:  // ufo 0x215019-0x2150cc
            p.add(Op::SwitchedToMode);
            p.add(Op::LastPortal);
            p.add(Op::ToggleBird, true, noEffects);
            break;
        case 41:  // swing 0x2150d1-0x215191 (GD skips the whole branch in platformer: layer +0x309e)
            p.add(Op::SwitchedToMode);
            p.add(Op::LastPortal);
            p.add(Op::ToggleSwing, true, noEffects);
            break;
        case 26:  // wave 0x215290-0x215350 (platformer: the same skip)
            p.add(Op::SwitchedToMode);
            p.add(Op::LastPortal);
            p.add(Op::ToggleDart, true, noEffects);
            break;
        case 16:  // ball 0x215196-0x21528b: six off, position, roll on (0x215274 / 0x215286 are one call)
            detail::offExcept(p, Op::ToggleRoll);
            p.add(Op::LastPortal);
            p.add(Op::ToggleRoll, true, noEffects);
            break;
        case 27:  // robot 0x215355-0x21544a
            detail::offExcept(p, Op::ToggleRobot);
            p.add(Op::LastPortal);
            p.add(Op::ToggleRobot, true, noEffects);
            break;
        case 33:  // spider 0x21544f-0x215544
            detail::offExcept(p, Op::ToggleSpider);
            p.add(Op::LastPortal);
            p.add(Op::ToggleSpider, true, noEffects);
            break;
        default:
            break;
    }
    return p;
}

// ---- the linked-dual gravity rule of playerWillSwitchMode (0x213001-0x213117): step 4 applies it ----

struct ModeFlags {
    bool ship = false, bird = false, ball = false, dart = false, robot = false, spider = false, swing = false;
};

/// 0x213058-0x2130f2: the partner already is in the portal's mode. The cases read ship 0x9b9, bird
/// 0x9ba, ball 0x9bb, robot 0x9bd, spider 0x9be, swing 0x9c4 and the cube (all seven flags clear,
/// dart 0x9bc included); the WAVE portal (26) has no case and never links (dual_rules.hpp agrees).
constexpr bool partnerInMode(int type, ModeFlags const& p) {
    switch (type) {
        case 6: return !p.ship && !p.bird && !p.ball && !p.swing && !p.dart && !p.robot && !p.spider;
        case 5: return p.ship;
        case 19: return p.bird;
        case 41: return p.swing;
        case 16: return p.ball;
        case 27: return p.robot;
        case 33: return p.spider;
        default: return false;
    }
}

struct GravityDecision {
    bool flip = false;         // call player->flipGravity(upsideDown, true) (PlayerObject's, 0x39a1d0)
    bool upsideDown = false;   // = !partner.m_isUpsideDown (0x213107 sete)
    constexpr bool operator==(GravityDecision const&) const = default;
};

/// In dual (layer +0x422), unless the level-settings byte at +0x154 [D: two-player mode] or the
/// unlink flag (layer +0x860, m_unkBool31) is set: when the partner already has the mode, the
/// switching player takes the gravity opposite to the partner's. The partner is "the player whose
/// unique id differs" (0x213033-0x213042): for a clone that is the real player 1 itself, which is
/// how v0.7.x flipped the live player (design D3). Pure; step 4 applies it to the clone PAIR.
constexpr GravityDecision dualGravity(bool dual, bool settingsSkip, bool unlinked, int type, ModeFlags const& partner, bool partnerUpsideDown) {
    if (!dual || settingsSkip || unlinked) return {};
    if (!partnerInMode(type, partner)) return {};
    return GravityDecision{true, !partnerUpsideDown};
}

}  // namespace gprl::solver::portal
