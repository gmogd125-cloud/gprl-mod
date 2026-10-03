#pragma once
// The isolated physics engine (docs/BACKGROUND_ANALYZER_DESIGN.md §4.2, `gprl-sim/1`). PURE
// C++20, no GD symbols. One `Engine` simulates ONE player (P1) through a `World` at 240 TPS with
// GD's step order (GD_PHYSICS_NOTES "Step loop"): input -> update (gravity, velocity, position)
// -> collisions -> rotation. Deterministic: the same World + StartState + inputs give the same
// trajectory bit for bit (tests/sim_engine_tests.cpp goldens).
//
// Users: search.cpp (beam search over inputs; needs cheap save / restore and a state hash),
// windows.cpp (shifted replays), verify.cpp (replay of recorded inputs vs RecordedTick).
#include <cstdint>
#include <string>
#include <vector>

#include "world.hpp"

namespace gprl::sim {

/// Everything the physics needs to continue from here. Plain data: copyable, hashable.
struct PlayerState {
    double x = 0.0, y = 0.0;          // centre of the player (GD position)
    double lastX = 0.0, lastY = 0.0;  // position before the last update (m_lastPosition)
    double yVelocity = 0.0;           // per 1/60 s (GD units)
    double rotation = 0.0;            // degrees (cosmetic for the search; kept for traces)
    Gamemode mode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool mini = false;
    bool upsideDown = false;
    bool onGround = false;
    bool held = false;                // the jump button is down
    bool dead = false;
    bool dashing = false;             // unsupported dash: stays false in /1
    bool accelerating = false;        // m_isAccelerating (after a boost)
    bool jumpBuffered = false;        // m_jumpBuffered (press not yet consumed)
    bool ringJumpArmed = false;       // m_stateRingJump / m_stateJumpBuffered copies
    double robotHold = 0.0;           // m_accelerationOrSpeed for the robot hold
    double slopeVelocity = 0.0;
    int onSlopeIndex = -1;            // World::objects index of the slope under the player, -1 none
    int lastGroundIndex = -1;
    int killerIndex = -1;             // World::objects index of what killed the player, -1 none
    uint8_t deathReason = 0;          // 0 none, 1 hazard, 2 solid (wall / head), 3 floor/ceiling, 4 out of bounds
    // physics constants that depend on speed (PlayerObject::updateTimeMod writes them)
    double gravity = 0.958199024;
    double yStart = 11.1800318;
    double speedMultiplier = 5.77000189;
    double playerSpeed = static_cast<double>(0.9f);   // m_playerSpeed is a float (+0x9f4): 0.89999997615814209

    // ---- added by the ENGINE-PHYSICS builder (2026-10-02), ADDITIVE ONLY ----
    // GD state the frozen list above lacked but the step order needs; they live in PlayerState so
    // that EngineSnapshot (= PlayerState + activations) still restores the whole physics state.
    // Readers of the fields above are unaffected.
    bool boosted = false;             // m_maybeIsBoosted (+0xa1c): set by a jump / pad / orb, cleared when playerIsFallingBugged
    bool onGround2 = false;           // m_isOnGround2 (+0xa0c): cosmetic twin of onGround kept for hitGroundNoJump fidelity
    bool touchedPad = false;          // m_touchedPad (+0x99c): a pad was used since the last jump (robot hover off)
    bool wasOnSlope = false;          // m_wasOnSlope: onSlope of the previous step
    bool touchedRingThisStep = false; // m_touchedRing (+0x98b): one plain ring per step (cleared at the end of update)
    bool currentSlopeTop = false;     // m_isCurrentSlopeTop: the slope under the player is a ceiling slope
    int ringUsedSincePress = -1;      // m_ringRelatedSet reduced to the one ring a press can consume
    int snappedObjectIndex = -1;      // m_objectSnappedTo (checkSnapJumpToObject), World::objects index
    double snapDistance = 0.0;        // m_snapDistance
    double lastFlipTime = -1.0;       // m_lastFlipTime in seconds, -1 = never (GD uses 0.0 = never)
    double lastSpiderFlipTime = -1.0; // m_lastSpiderFlipTime
    double modeChangedTime = -1.0;    // m_gameModeChangedTime
    double lastLandTime = -1.0;       // m_lastLandTime
    double slopeStartTime = 0.0;      // m_slopeStartTime
    double slopeEndTime = -1.0;       // m_slopeEndTime
    double currentSlopeYVelocity = 0.0; // m_currentSlopeYVelocity
    double slopeAngle = 0.0;          // m_slopeAngleRadians of the slope under the player
    double slopeRadiusExtra = 0.0;    // unk_584 = playerRadOnSlope - playerRadius

    // ---- added by the SIM-FIX builder (2026-10-02, gprl-sim/2), ADDITIVE ONLY ----
    double slopeContactY = 0.0;       // m_unk3d0 (+0x940, double): the last slope contact's newPlayerY as double(float);
                                      // a repeat while wasOnSlope ends the slope (the plateau exit, gdp collidedWithSlopeInternal 161-165)
    bool slopeDownhill = false;       // m_slopeFlipGravityRelated (+0x68c): the last slope contact was downhill (gdp "playerUphill");
                                      // postCollision's launch comparison direction reads it (2.2081 0x38d8e0-0x38d92c)
};

/// Snapshot = PlayerState + the run's one-shot activation state near the player.
struct EngineSnapshot {
    PlayerState player;
    int tick = 0;                      // whole ticks stepped since reset
    double subTick = 0.0;              // part of the current tick already stepped (CBF)
    std::vector<int> activatedIndices; // World::objects indices of orbs / pads / portals used (sorted); only objects
                                       // still inside the scan window (activations behind it can never matter again)
    std::vector<int> touchingRings;    // m_touchingRings between steps (sorted): the rings kept by the last step's
                                       // resetTouchedRings plus the rings overlapped during the last pass, minus the
                                       // ones consumed. A press at the next step's processCommands ring-jumps all of them.
    int nextObject = 0;                // index of the first object with right() >= player.x - margin (scan cursor)
    // ---- added by the SIM-FIX builder (2026-10-02), ADDITIVE ONLY ----
    std::vector<int> touchedRings;     // m_touchedRings: the rings overlapped during the last collision pass (sorted);
                                       // the next step's resetTouchedRings prunes touchingRings to exactly these
};

class Engine {
public:
    explicit Engine(World const* world);   // the world outlives the engine; never copied

    /// Player at a start state (level start or a StartPos), tick 0, no activations.
    void reset(StartState const& start);

    /// Queue the next input: applied at the start of the next `step` / `stepPart` (processCommands).
    void queueInput(bool down);
    /// Apply a queued input NOW (at the current sub-tick position) - used by stepPart callers. Like
    /// GD's processCommands it runs BEFORE the next sub-step's resetTouchedRings: the press sees the
    /// whole m_touchingRings set (gprl-sim/2; /1 pruned it first and lost the previous pass's rings).
    void applyQueuedInput();

    /// One whole 240 TPS tick: input -> update(dt = 0.25) -> collisions -> rotation.
    void step();
    /// Part of a tick (CBF split): `fraction` in (0, 1]; the caller completes the tick with the
    /// remainder. Two parts of 0.5 must equal GD's half-tick semantics (betweenSteps).
    void stepPart(double fraction);

    PlayerState const& player() const;
    World const& world() const;
    int tick() const;
    double frame() const;              // tick + subTick
    double percent() const;            // x / world.endX * 100, clamped to [0, 100]
    bool dead() const;
    bool completed() const;            // x >= world.endX
    /// The player is inside an UnsupportedSpan (coverage ends here; the caller stops).
    bool inUnsupportedSpan() const;
    /// Object index (World::objects) of the nearest object ahead within `range` units, -1 none.
    int nearestAhead(float range) const;

    EngineSnapshot save() const;
    void restore(EngineSnapshot const& s);
    /// Quantized hash of the physics state for search de-duplication (tick, y / 0.5, yVel / 0.05,
    /// discrete flags, nearby activations). Two states with equal hashes behave the same for the
    /// search's purposes; collisions are harmless (the beam loses a duplicate).
    uint64_t stateHash() const;

    /// Settle facts for core/solver/settle.hpp (flying mode, onGround, dashing, touching ring).
    /// touchingRing() = m_touchingRings non-empty, exactly what the live clone solver reads
    /// (src/solver/CloneEngine.cpp `m_touchingRings->count()`).
    bool flying() const;
    bool touchingRing() const;

    /// Debug: one line of the current state.
    std::string describe() const;

private:
    struct Impl;
    Impl* m_impl;
public:
    ~Engine();
    Engine(Engine const&) = delete;
    Engine& operator=(Engine const&) = delete;
};

}  // namespace gprl::sim
