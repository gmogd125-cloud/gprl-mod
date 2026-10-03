#pragma once
// Activation-flag isolation of the lockstep clones (docs/SOLVER_DESIGN.md §12, GD_PHYSICS_NOTES
// "Activation slots"). PURE C++20, no Geode includes; host-tested in tests/activation_tests.cpp.
//
// GD 2.2081 keeps two one-shot activation slots per orb / pad / portal (EnhancedGameObject::
// m_activatedByPlayer1 / m_activatedByPlayer2) and picks the slot by the PLAYER'S GameObject::
// m_uniqueID, not by m_isSecondPlayer (win 0x1a4af0 / 0x1a4a90 / 0x1a4a80, verified 2026-09-30):
//
//   hasBeenActivatedByPlayer(p) = !canMultiActivate(p->m_isPlatformer) && (p->m_uniqueID == 1 ? slot1 : slot2)
//   activatedByPlayer(p)        : m_activated = 1; if (!canMultiActivate) (p->m_uniqueID == 1 ? slot1 : slot2) = 1
//   triggerActivated()          : slot1 = slot2 = 1
//
// GJBaseGameLayer::collisionCheckObjects skips every object whose hasBeenActivatedByPlayer(player)
// is true before any per-type handling (rings, pads, portals). A hidden clone is a later
// GameObject with another unique id, so the game gates and records ITS activations in slot 2
// while the real player 1 lives in slot 1. The v0.4.x swap copied the real slots verbatim: a
// control spawned after the real player had used an orb saw "not activated" in slot 2, touched the
// orb again, and reported one more touching ring than the real player ("touching rings 1 vs 0",
// every release job near an orb). The logical world below fixes that without touching the game:
//
//   logical bit0 (kSelf)  = "activated by the simulated player 1" (the real player 1's own slot)
//   logical bit1 (kOther) = the other slot (player 2 in dual mode; never written by a clone)
//   logical bit2          = GameObject::m_isActivated
//   logical bit3          = EnhancedGameObject::m_activated
//
// Presenting a logical value to a clone writes bit0 into BOTH physical slots (whatever slot the
// clone's unique id selects, it reads the real player 1's state); packing the physical bits after
// the clone's step folds either slot back into bit0 and keeps bit1 from before the step.
#include <cstdint>

namespace gprl::solver::activation {

constexpr uint8_t kSelf = 1;         // activated by the simulated player 1
constexpr uint8_t kOther = 2;        // the other player's slot
constexpr uint8_t kIsActivated = 4;  // GameObject::m_isActivated
constexpr uint8_t kActivated = 8;    // EnhancedGameObject::m_activated
// v0.8.0 overlay bits (docs/LIVE_ISOLATION_DESIGN.md §2.4): the clone's own view of what GD keeps
// in GJGameState::m_activatedObjectIDs ((object uid, player uid) -> command index) and of a ring's
// powered state. Never written into the game.
constexpr uint8_t kMapSelf = 16;     // m_activatedObjectIDs holds (object, simulated player 1)
constexpr uint8_t kMapOther = 32;    // ... (object, the other player)
constexpr uint8_t kRingPowered = 64; // RingObject powered by the clone (powerOnObject not called)

/// The physical one-shot bits of an object, in the game's own fields.
struct Flags {
    bool slot1 = false;        // m_activatedByPlayer1
    bool slot2 = false;        // m_activatedByPlayer2
    bool isActivated = false;  // GameObject::m_isActivated
    bool activated = false;    // EnhancedGameObject::m_activated

    constexpr bool operator==(Flags const&) const = default;
};

/// The slot the game selects for a player: slot 1 iff its unique id is 1.
constexpr bool usesSlot2(int playerUniqueId) { return playerUniqueId != 1; }

/// Pack the REAL world's bits into the logical form. `realUsesSlot2` = the real player 1's own
/// slot is slot 2 (its unique id is not 1; never seen in 2.2081, handled anyway).
constexpr uint8_t packReal(Flags f, bool realUsesSlot2) {
    bool self = realUsesSlot2 ? f.slot2 : f.slot1;
    bool other = realUsesSlot2 ? f.slot1 : f.slot2;
    return static_cast<uint8_t>((self ? kSelf : 0) | (other ? kOther : 0) | (f.isActivated ? kIsActivated : 0) | (f.activated ? kActivated : 0));
}

/// The physical bits a clone must see for a logical value: both slots mirror bit0, so the
/// game's gate reads the simulated player 1's state whichever slot the clone's id selects.
constexpr Flags presentToClone(uint8_t logical) {
    Flags f;
    f.slot1 = (logical & kSelf) != 0;
    f.slot2 = f.slot1;
    f.isActivated = (logical & kIsActivated) != 0;
    f.activated = (logical & kActivated) != 0;
    return f;
}

/// The logical value after a clone's step from the physical bits it left behind: an activation
/// in either slot is the simulated player 1's; the other player's bit is what it was before.
constexpr uint8_t packAfterClone(Flags f, uint8_t before) {
    return static_cast<uint8_t>(((f.slot1 || f.slot2) ? kSelf : 0) | (before & kOther) | (f.isActivated ? kIsActivated : 0) | (f.activated ? kActivated : 0));
}

/// The v0.4.x rule (verbatim copy of the physical bits), kept for the regression test only.
constexpr uint8_t packLegacy(Flags f) {
    return static_cast<uint8_t>((f.slot1 ? 1 : 0) | (f.slot2 ? 2 : 0) | (f.isActivated ? 4 : 0));
}
constexpr Flags unpackLegacy(uint8_t v) {
    Flags f;
    f.slot1 = (v & 1) != 0;
    f.slot2 = (v & 2) != 0;
    f.isActivated = (v & 4) != 0;
    return f;
}

// ---- the game's rules as pure functions (asm mirrors, for the host test) ----

/// EnhancedGameObject::hasBeenActivatedByPlayer (win 0x1a4af0).
constexpr bool gameHasBeenActivatedByPlayer(Flags f, int playerUniqueId, bool multiActivate) {
    if (multiActivate) return false;
    return usesSlot2(playerUniqueId) ? f.slot2 : f.slot1;
}

/// EnhancedGameObject::activatedByPlayer (win 0x1a4a90).
constexpr Flags gameActivatedByPlayer(Flags f, int playerUniqueId, bool multiActivate) {
    f.activated = true;
    if (!multiActivate) {
        if (usesSlot2(playerUniqueId)) f.slot2 = true;
        else f.slot1 = true;
    }
    return f;
}

/// EnhancedGameObject::triggerActivated (win 0x1a4a80).
constexpr Flags gameTriggerActivated(Flags f) {
    f.slot1 = true;
    f.slot2 = true;
    return f;
}

// ---- v0.8.0: the game's rules applied to a clone's OVERLAY instead of the live object ----
//
// A stepped clone never reads or writes the object's physical slots; every hooked gate reads the
// logical value the trial keeps for that object and the trial's own bit (`selfBit` = kSelf for a
// player-1 trial, kOther for the player-2 clone of a dual pair).

struct Decision {
    bool result;
    uint8_t logical;   // the overlay value after the call
};

constexpr uint8_t mapBitFor(uint8_t selfBit) { return selfBit == kOther ? kMapOther : kMapSelf; }

/// EnhancedGameObject::hasBeenActivatedByPlayer (win 0x1a4af0) on the overlay.
constexpr bool hasBeen(uint8_t logical, uint8_t selfBit, bool multiActivate) {
    if (multiActivate) return false;
    return (logical & selfBit) != 0;
}

/// EnhancedGameObject::activatedByPlayer (win 0x1a4a90) on the overlay.
constexpr uint8_t activated(uint8_t logical, uint8_t selfBit, bool multiActivate) {
    logical = static_cast<uint8_t>(logical | kActivated);
    if (!multiActivate) logical = static_cast<uint8_t>(logical | selfBit);
    return logical;
}

/// GJBaseGameLayer::canBeActivatedByPlayer (win 0x2178c0) on the overlay, verified against the
/// disassembly 2026-10-01: multi-activate objects are gated by the (object, player) entry of
/// m_activatedObjectIDs (found -> false; else the entry is made and the object may activate);
/// one-shot objects always record the entry and may activate unless hasBeenActivatedByPlayer.
constexpr Decision canActivate(uint8_t logical, uint8_t selfBit, bool multiActivate) {
    uint8_t const map = mapBitFor(selfBit);
    if (multiActivate) {
        if (logical & map) return {false, logical};
        return {true, static_cast<uint8_t>(logical | map)};
    }
    logical = static_cast<uint8_t>(logical | map);
    return {!hasBeen(logical, selfBit, false), logical};
}

}  // namespace gprl::solver::activation
