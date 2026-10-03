#pragma once
// Pure classification of what the game reports (SPEC §6, §9, §13, §19, §21; SPEC §45 "input
// classification" and "gamemode detection"). Plain values in, plain values out: no Geode / cocos
// includes, so tests/classify_tests.cpp and tests/determinism_tests.cpp compile it on the host.
//
// src/Tracker.cpp is the game-side caller: it reads the PlayerObject / GJBaseGameLayer fields
// (names verified in D:\GeodeMods\_research\GeometryDash.bro, GD 2.2081 win) and hands the plain
// values in here, so the rules the telemetry depends on are tested without the game:
//
//   PlayerObject flag tuple        -> Gamemode      gamemodeFromFlags   (m_isShip, m_isBall, m_isBird,
//                                                                         m_isDart, m_isRobot, m_isSpider,
//                                                                         m_isSwing; none = cube)
//   PlayerObject::m_playerSpeed    -> Speed         speedFromMultiplier (GD 2.2 speed constants)
//   handleButton(down, btn, isP1)  -> input fields  classifyButton      (player slot, button, press/release)
//   PlayerObject::update(dt) sums  -> tSubTick      SubTickClock        (half ticks, Click Between Frames)
//   destroyPlayer before / after   -> death kind    classifyDestroy     (would-be deaths under noclip)
//   environment report changes     -> re-emit       environmentChanged  (mid-session trust changes)
//
// Every constant lives in ClassifyParams (one place). They are GD facts (GD_PHYSICS_NOTES.md,
// snapshot.hpp), not tunables.
#include <cstdint>
#include <optional>

#include "telemetry.hpp"
#include "vocab.hpp"

namespace gprl::classify {

struct ClassifyParams {
    // GD 2.2 PlayerObject::m_playerSpeed set by the speed portals (snapshot.hpp field notes).
    double speedSlow = 0.7;
    double speedNormal = 0.9;
    double speedFast = 1.1;
    double speedFaster = 1.3;
    double speedFastest = 1.6;
    // Class boundaries: the midpoint between adjacent constants. A value AT a boundary belongs to
    // the faster speed (v < boundary -> slower). Kept explicit (not computed) so the boundary
    // values are exact decimal numbers the tests can hit.
    double boundarySlowNormal = 0.8;
    double boundaryNormalFast = 1.0;
    double boundaryFastFaster = 1.2;
    double boundaryFasterFastest = 1.45;
    // PlayerObject::m_vehicleSize is 1.0 regular and 0.6 mini; below this the player is mini.
    double miniVehicleSizeBelow = 0.9;
    // PlayerObject::update(dt) receives dt in 1/60 s units: one tick = updateUnitsPerSecond / tps
    // (0.25 at 240 TPS). GD_PHYSICS_NOTES.md "Step loop".
    double updateUnitsPerSecond = 60.0;
    double ticksPerSecond = kTicksPerSecond;
    // An Eclipse Physics Bypass TPS at or below this is ignored (division guard, 240 TPS assumed).
    double minBypassTps = 1.0;
    // Accumulated update deltas within this much of a full tick close the tick (float sums of the
    // CBF split steps are not exact).
    double tickBoundaryEpsilon = 1e-4;
    // Largest sub-tick fraction reported: schema.ts requires tSubTick < 1.
    double maxSubTick = 0.999;
    // Two environment reports whose tps differ by more than this are a change.
    double tpsChangeEpsilon = 1e-6;
};

/// The defaults every caller uses unless a test overrides them.
inline constexpr ClassifyParams kParams{};

// ---- gamemode (SPEC §9) ----

/// PlayerObject's gamemode flags as plain values. GD sets at most one; if several are set (a mode
/// toggle in progress) the precedence is the order below, the same as the snapshot mapping.
struct ModeFlags {
    bool ship = false;     // m_isShip
    bool ball = false;     // m_isBall
    bool ufo = false;      // m_isBird
    bool wave = false;     // m_isDart
    bool robot = false;    // m_isRobot
    bool spider = false;   // m_isSpider
    bool swing = false;    // m_isSwing
};

Gamemode gamemodeFromFlags(ModeFlags const& f);

// ---- speed (SPEC §12 fingerprint, PlayerStateSnapshot.speed) ----

/// m_playerSpeed -> Speed: the first boundary the value is strictly below selects the class, at or
/// above the last boundary is Fastest (so 0 / negative -> Slow, +inf -> Fastest, as
/// tests/fixtures/classification/speed-from-player-speed.json pins). NaN (never produced by GD) is
/// Normal, the vocabulary's default, instead of falling through to Fastest.
Speed speedFromMultiplier(double playerSpeed, ClassifyParams const& p = kParams);

/// The GD constant of a speed class (inverse of speedFromMultiplier on the constants).
double multiplierOf(Speed s, ClassifyParams const& p = kParams);

/// PlayerStateSnapshot.mini from PlayerObject::m_vehicleSize.
bool isMini(double vehicleSize, ClassifyParams const& p = kParams);

// ---- inputs (SPEC §13) ----

struct ClassifiedInput {
    int player = 1;                     // 1 or 2 (dual mode / 2-player)
    Button button = Button::Jump;
    InputKind kind = InputKind::Press;  // press = button down, release = button up
    bool down = true;
};

/// GJBaseGameLayer::handleButton(down, button, isPlayer1). GD's PlayerButton: 1 jump, 2 left,
/// 3 right; anything else is not an input GPRL records (nullopt).
std::optional<ClassifiedInput> classifyButton(bool down, int gdButton, bool isPlayer1);

// ---- sub-tick clock (SPEC §6: sub-frame windows are real with CBF) ----

/// One tick in PlayerObject::update units for the TPS in effect: 0.25 at 240 TPS; with an
/// Eclipse Physics Bypass active, updateUnitsPerSecond / tps.
double tickDtFor(bool tpsBypass, double tps, ClassifyParams const& p = kParams);

/// Accumulates player 1's PlayerObject::update deltas inside the current tick. An input's
/// tSubTick is the fraction of the tick already simulated when handleButton runs: 0 on a tick
/// boundary, 0.5 in the second half of a GD half tick, arbitrary when Click Between Frames splits
/// the tick at the input time. GD calls processCommands (-> handleButton) BEFORE the step's
/// update, so the fraction is read before the delta of that step is added.
class SubTickClock {
public:
    SubTickClock() : SubTickClock(kParams) {}   // not explicit: usable in aggregate `State{}` members
    explicit SubTickClock(ClassifyParams const& p) : m_p(p), m_tickDt(tickDtFor(false, 0.0, p)) {}

    /// New attempt (or TPS change): empty accumulator, tick length `tickDt` (> 0).
    void reset(double tickDt);
    /// One PlayerObject::update(dt) of the tracked player.
    void onPlayerUpdate(double dt);
    /// Fraction of the current tick already simulated, in [0, maxSubTick].
    double fraction() const;

    double tickDt() const { return m_tickDt; }
    double elapsed() const { return m_elapsed; }

private:
    ClassifyParams m_p;
    double m_tickDt;
    double m_elapsed = 0.0;
};

// ---- deaths (SPEC §19) ----

enum class DestroyVerdict : uint8_t {
    Ignore,         // not an attempt death (no open attempt, already dead, other mods' clones, GD's anti-cheat spike)
    Death,          // the player died: the attempt ends
    WouldBeDeath,   // destroyPlayer ran but the player is alive afterwards (a noclip menu swallowed it)
};

/// What PlayLayer::destroyPlayer told us, as plain values.
struct DestroyFacts {
    bool attemptOpen = true;
    bool wasDeadBefore = false;   // m_isDead before the call
    int playerSlot = 1;           // 1 / 2 for m_player1 / m_player2, 0 for any other PlayerObject
    bool anticheatSpike = false;  // the object is GJBaseGameLayer::m_anticheatSpike (GD_PHYSICS_NOTES)
    bool deadAfter = true;        // m_isDead after the call
};

DestroyVerdict classifyDestroy(DestroyFacts const& f);

/// v0.10.0 (owner report: "on Mega Hack the noclip deaths go crazy"): GD calls `destroyPlayer`
/// on EVERY physics tick the player is inside a hazard, and for every hazard touched in a tick,
/// so a noclip menu that swallows the call makes one death look like dozens. Eclipse Menu's rule
/// (src/hacks/Player/Noclip.cpp: destroyPlayer sets `m_wouldDieFrame`, and once per physics step
/// `GJBaseGameLayer::processCommands` adds a death only when it is set and `m_deadLastFrame` is
/// not): a death is one CONTIGUOUS run of TICKS with a swallowed destroy; a tick without one ends
/// the run. Only player 1 / 2 count and GD's anti-cheat spike never does (classifyDestroy).
///
/// v0.14.1 fix (owner report 2026-10-02: "noclip deaths are still going wild"): the streak was
/// fed `GJGameState::m_currentProgress`, which GD advances by 2 per tick (GD_PHYSICS_NOTES), so
/// consecutive ticks were never contiguous and every tick inside a hazard counted as a death
/// (live data: median gap 1 tick between would-be deaths, 470 in one attempt). The streak runs on
/// ticks: always pass `tickFromProgress(m_currentProgress)`.
inline int64_t tickFromProgress(int64_t currentProgress) { return currentProgress / 2; }

struct WouldBeDeathStreak {
    int64_t lastTick = -1000;   // tick of the last swallowed destroy; -1000 = none yet
};
/// True when a swallowed destroy at `tick` (tickFromProgress) starts a NEW would-be death: the
/// same tick or the next one continues the current death. Updates the streak either way.
bool startsWouldBeDeath(WouldBeDeathStreak& streak, int64_t tick);

// ---- environment (SPEC §19-§21, TELEMETRY.md §6: "sent at session start and when it changes") ----

/// Integrity summary of a trust state (the moderation UI vocabulary): flagged for bot / physics,
/// warnings for noclip / unknown gameplay mods, clean otherwise, unknown when the module list could
/// not be read.
Integrity integrityFor(TrustState trust, bool modulesReadable);

/// True when `now` differs from the last emitted report in a field that changes how the server
/// must treat the evidence that follows: trust, noclip, bot, TPS bypass / tps, CBF, integrity.
/// FPS, mods, modules and hashes do not trigger a re-emit.
bool environmentChanged(telemetry::EnvironmentPayload const& lastEmitted, telemetry::EnvironmentPayload const& now,
                        ClassifyParams const& p = kParams);

}  // namespace gprl::classify
