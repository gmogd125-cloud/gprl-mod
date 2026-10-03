#pragma once
// PlayerStateSnapshot: the deterministic snapshot interface (ARCHITECTURE §6, SPEC §6, §48.7).
//
// Field-for-field mirror of `PlayerStateSnapshot` in shared/src/telemetry/schema.ts (schema
// gprl.telemetry/1). The TypeScript file is the single definition of the wire format; the key names
// below ARE the JSON keys (core/telemetry snapshotToJson / snapshotFromJson) and
// tests/fixtures/telemetry/batch-basic.json must round-trip byte for byte.
//
// This is the PORTABLE label of one physics tick: enough to build a timing fingerprint (SPEC §12),
// to re-derive windows server side, and to identify which history entry a solver trial re-runs
// from. It is NOT the full physics state: the Geode adapter (src/solver/GdOracle, Phase 3) keeps
// the complete field-by-field copy of PlayerObject (the ~200-field list proven in
// frame-perfect-counter's PlayerFields.inc plus touched rings and the four collision logs) in its
// own ring indexed by `SnapshotId`; that copy never leaves the game process.
//
// Filled by src/Tracker.cpp from PlayerObject (field names verified in GeometryDash.bro):
//   gamemode      m_isShip / m_isBall / m_isBird (ufo) / m_isDart (wave) / m_isRobot / m_isSpider / m_isSwing
//   speed         m_playerSpeed (0.7 slow, 0.9 normal, 1.1 fast, 1.3 faster, 1.6 fastest)
//   gravityFlipped m_isUpsideDown        mini  m_vehicleSize < 0.9        platformer m_isPlatformer
//   yVel          m_yVelocity (GD units per 1/60 s step, raw)   xVel m_platformerXVelocity (0 outside platformer)
//   isOnGround m_isOnGround   isOnSlope m_isOnSlope   isHolding m_holdingButtons[1]   isDashing m_isDashing
//   hasJumped m_hasEverJumped  touchedRing m_touchedRing  jumpBuffered m_jumpBuffered  ringJumpPending m_stateRingJump
//   nearPortal / lastPortalObjectId: the tracker's own portal log (GJBaseGameLayer::playerTouchedTrigger)
//   geometryHash: 0 until the Phase 3 adapter hashes the objects around the player.
#include <cstdint>

#include "vocab.hpp"

namespace gprl {

struct PlayerStateSnapshot {
    int player = 1;                   // 1 or 2 (dual mode)
    int64_t tick = 0;                 // 240 TPS tick index since the attempt start (m_currentProgress / 2)
    double levelTime = 0.0;           // seconds since level start (m_levelTime; holds the StartPos time from a StartPos)
    double subTick = 0.0;             // fraction [0,1) of the tick already simulated (CBF / half tick), else 0
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool gravityFlipped = false;
    bool mini = false;
    bool platformer = false;
    double x = 0.0;                   // position in GD units
    double y = 0.0;
    double yVel = 0.0;                // PlayerObject::m_yVelocity (GD units per 1/60 s step)
    double xVel = 0.0;                // platformer horizontal velocity, 0 otherwise
    double rotation = 0.0;            // degrees
    bool isOnGround = false;
    bool isOnSlope = false;
    bool isHolding = false;           // jump button held this tick
    bool isDashing = false;
    bool hasJumped = false;           // m_hasEverJumped within this attempt
    bool touchedRing = false;         // an orb is touchable / buffered this tick
    bool jumpBuffered = false;
    bool ringJumpPending = false;     // an orb jump is queued for this tick
    bool nearPortal = false;          // a portal was activated within the last few ticks
    int lastPortalObjectId = 0;       // object id of the last activated portal, 0 = none
    uint32_t geometryHash = 0;        // coarse hash of nearby geometry, 0 = unknown (Phase 3 fills it)

    /// Gravity in the fingerprint vocabulary (schema.ts snapshotGravity).
    Gravity gravity() const { return gravityFlipped ? Gravity::Flipped : Gravity::Normal; }

    bool operator==(PlayerStateSnapshot const&) const = default;
};

}  // namespace gprl
