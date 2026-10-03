#pragma once
// GD 2.2081's dual-mode rules as pure functions (docs/LIVE_ISOLATION_DESIGN.md §2.7, ISO-D6),
// applied by the engine to a dual PAIR of clones (P1 clone + P2 clone) - never to the real
// players. PURE C++20, no Geode includes; host-tested in tests/dual_rules_tests.cpp against the
// asm mirrors re-read 2026-10-01 from the 2.2081 exe FILE (analysis kit, no GD process touched).
//
// Why this exists (design §1.2 D1 / D3): the game picks "the other player" of a dual link by unique
// id - `player->m_uniqueID == m_player1->m_uniqueID ? m_player2 : m_player1`. A hidden clone's uid
// is never 1, so up to v0.7.2 every clone that flipped gravity in linked dual flipped the REAL
// player 1 back (the owner's `trollmachine sp new` log, x~27285, 10 of 10 attempts: real vy -0.48
// unflipped vs shadow -0.96 flipped), and a clone passing a gamemode portal in dual took its
// gravity from the real player 1. Here "the other player" is always the pair partner.
//
// Player mode flags (PlayerObject, 2.2081 win offsets): ship 0x9b9, bird (ufo) 0x9ba, ball 0x9bb,
// dart (wave) 0x9bc, robot 0x9bd, spider 0x9be, swing 0x9c4, upside down 0x9bf.
#include <cstdint>

namespace gprl::solver::dual {

struct ModeFlags {
    bool ship = false;
    bool bird = false;    // ufo
    bool ball = false;
    bool dart = false;    // wave
    bool robot = false;
    bool spider = false;
    bool swing = false;
    constexpr bool operator==(ModeFlags const&) const = default;
};

/// No vehicle flag set: the cube.
constexpr bool isCube(ModeFlags m) { return !m.ship && !m.bird && !m.ball && !m.dart && !m.robot && !m.spider && !m.swing; }

/// The mode test of GJBaseGameLayer::flipGravity's dual link (0x212b7b-0x212bd9): ship, ball,
/// bird, spider, robot and swing must be equal. The DART (wave) flag is NOT compared there - a
/// cube P1 and a wave P2 count as "the same mode" for the link. Mirrored verbatim.
constexpr bool linkModesEqual(ModeFlags a, ModeFlags b) {
    return a.ship == b.ship && a.ball == b.ball && a.bird == b.bird && a.spider == b.spider && a.robot == b.robot && a.swing == b.swing;
}

/// What the gravity link reads (all from the TRIAL's own copy: LayerCopy + the pair's clones).
struct DualFacts {
    bool unlink = false;          // GJGameState +0x6b8 (layer +0x860, m_unkBool31): the Options trigger's unlink flag
    bool dualMode = false;        // m_gameState.m_isDualMode (layer +0x422)
    bool twoPlayerMode = false;   // m_levelSettings->m_twoPlayerMode (+0x154)
    ModeFlags p1;                 // the pair's player 1 (the game reads m_player1 / m_player2 here)
    ModeFlags p2;
};

enum class LinkTarget : uint8_t { None, Partner };

/// GJBaseGameLayer::flipGravity (0x212b00): after player->flipGravity(flip, noEffects) the OTHER
/// player gets flipGravity(!flip, noEffects) when !unlink && dual && !twoPlayer && the mode test
/// holds (0x212b3f-0x212bf8). The early return (player already has `flip`) is the caller's.
constexpr LinkTarget gravityLinkTarget(DualFacts const& f) {
    if (f.unlink || !f.dualMode || f.twoPlayerMode) return LinkTarget::None;
    if (!linkModesEqual(f.p1, f.p2)) return LinkTarget::None;
    return LinkTarget::Partner;
}

/// The LEGACY (= the game's own) choice of the other player for a flip done by the player with
/// uid `flippingUid`: real P2 when it is the real P1's uid, else the REAL P1 - what a clone (uid
/// never 1) triggered up to v0.7.2 (design §1.2 D1). Reproduced by the host test, never used.
enum class LegacyTarget : uint8_t { None, RealPlayer1, RealPlayer2 };
constexpr LegacyTarget legacyGravityLinkTarget(DualFacts const& f, int flippingUid, int realPlayer1Uid) {
    if (gravityLinkTarget(f) == LinkTarget::None) return LegacyTarget::None;
    return flippingUid == realPlayer1Uid ? LegacyTarget::RealPlayer2 : LegacyTarget::RealPlayer1;
}

/// PlayerObject::flipGravity (0x39a1d0): a flip that changes the gravity multiplies m_yVelocity
/// (+0x9a0) by 0.5 (0x39a2d4-0x39a2e4, the double at 0x622d98) unless +0x7e1 is set. The owner's
/// fingerprint is two of them: the real P1 flipped by the portal (x0.5) and flipped BACK by a
/// clone's link flip (x0.5 again: x0.25, gravity reverted) vs the shadow flipped once (x0.5):
/// real -0.48 vs shadow -0.96. Data for the reproduction test only; the engine never
/// re-implements PlayerObject::flipGravity (it calls the game's on the clone).
constexpr double kFlipVelocityFactor = 0.5;

/// The gamemode-portal part of GJBaseGameLayer::playerWillSwitchMode (0x213001-0x21311c): in dual
/// (and not a two-player level) the player switching through a portal of `portalType` gets the
/// gravity OPPOSITE to its partner's when the partner already has the portal's mode and the link
/// is not unlinked. The per-type test is the jump table at 0x213158 / 0x213138:
///   5 ship -> partner.ship, 6 cube -> partner is a cube (no flag at all), 16 ball -> partner.ball,
///   19 ufo -> partner.bird, 27 robot -> partner.robot, 33 spider -> partner.spider,
///   41 swing -> partner.swing, every other type (26 WAVE included) -> never.
/// Mirrored verbatim, including the wave entry that never links.
constexpr bool partnerHasTargetMode(int portalType, ModeFlags partner) {
    switch (portalType) {
        case 5: return partner.ship;
        case 6: return isCube(partner);
        case 16: return partner.ball;
        case 19: return partner.bird;
        case 27: return partner.robot;
        case 33: return partner.spider;
        case 41: return partner.swing;
        default: return false;   // 26 (wave) maps to the jump table's default entry
    }
}

struct ModeSwitchGravity {
    bool flip = false;          // call player->flipGravity(upsideDown, true)
    bool upsideDown = false;    // = !partner->m_isUpsideDown
    constexpr bool operator==(ModeSwitchGravity const&) const = default;
};

constexpr ModeSwitchGravity modeSwitchGravity(int portalType, bool dualMode, bool twoPlayerMode, bool unlink, ModeFlags partner, bool partnerUpsideDown) {
    if (!dualMode || twoPlayerMode) return {};
    if (unlink || !partnerHasTargetMode(portalType, partner)) return {};
    return {true, !partnerUpsideDown};
}

/// GJBaseGameLayer::handleButton (0x2338e0, simplified: the input-block list and the swap-players
/// game variable are not modelled): which players a jump input reaches.
///   player 1 <- isPlayer1 || !twoPlayer           (a player-2 key drives P1 too in a 1-player level)
///   player 2 <- dual && (!isPlayer1 || !twoPlayer) (in a 1-player dual level one key drives both,
///                                                    P1 first, then P2: 0x2339d9 then 0x233a15)
struct InputTargets {
    bool p1 = false;
    bool p2 = false;
    constexpr bool operator==(InputTargets const&) const = default;
};
constexpr InputTargets inputTargets(bool dualMode, bool twoPlayerMode, bool isPlayer1) {
    InputTargets t;
    t.p1 = isPlayer1 || !twoPlayerMode;
    t.p2 = dualMode && (!isPlayer1 || !twoPlayerMode);
    return t;
}

/// DOCUMENTATION of the dual entry (GJBaseGameLayer::toggleDualMode 0x2168d0, not transcribed):
/// the pair simulator never crosses a dual / solo portal (BLOCK-INVALID, design §2.10), so this is
/// only the DEV FIXTURE's entry rule (tests/kinematic_dual.hpp): P2 starts as a copy of P1 with the
/// opposite gravity and the mirrored y velocity.
struct EntryState {
    bool upsideDown = false;
    double yVelocity = 0.0;
};
constexpr EntryState p2EntryState(bool p1UpsideDown, double p1YVelocity) { return {!p1UpsideDown, -p1YVelocity}; }

}  // namespace gprl::solver::dual
