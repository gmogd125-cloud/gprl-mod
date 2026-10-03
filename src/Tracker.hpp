#pragma once
// Gameplay tracker (SPEC §2, §13, §19, §48.5): the main-thread state machine behind the hooks.
// It turns PlayLayer / GJBaseGameLayer / PlayerObject callbacks into `gprl.telemetry/1` events
// with `t` (level time since the attempt start), `tick` (m_currentProgress / 2) and, for inputs,
// the sub-tick fraction (elapsed player-update delta inside the current 240 TPS tick, which is
// what Click Between Frames splits ticks by), and hands them to the telemetry client.
//
// Attempts: one per resetLevel (and the level start), identified by "<session>-a<n>" with the GD
// attempt number, the session attempt count, the start percent (StartPos / practice checkpoint),
// the StartPos tick and the noclip state. Deaths and would-be deaths are judged by
// noclip-death-detector/2 (core/death_detector, src/DeathPath): deaths end an attempt; would-be deaths under noclip do
// not (SPEC §19). Level complete / exit / restart end it with that reason.
//
// Every function here runs on the game thread. Nothing blocks, nothing allocates beyond the event
// itself (SPEC §46).
#include <Geode/Geode.hpp>

#include <string>

#include "../core/death_detector.hpp"
#include "../core/snapshot.hpp"
#include "../core/vocab.hpp"

namespace gprl::env {
struct MenuState;
}

namespace gprl::tracker {

/// PlayLayer::setupHasCompleted - opens the session (environment event) and the first attempt.
void onLevelEnter(PlayLayer* pl);
/// After PlayLayer::resetLevel - ends the open attempt (restart) and starts the next one.
void onLevelReset(PlayLayer* pl);
/// After PlayLayer::togglePracticeMode.
void onPracticeToggle(PlayLayer* pl, bool practice);
/// PlayerObject::update for the tracked player 1: sub-tick accounting.
void onPlayerUpdate(PlayerObject* p, float dt);
/// GJBaseGameLayer::handleButton (inside processCommands): press / release with the sub-tick.
void onButton(GJBaseGameLayer* layer, bool down, int button, bool isPlayer1);
/// GJBaseGameLayer::playerTouchedTrigger for a portal object: remembers it for gamemode events.
void onPortal(GJBaseGameLayer* layer, PlayerObject* p, int objectId);
/// After any PlayerObject::toggle*Mode: emits gamemode_change when the effective mode changed.
void onGamemodeToggle(PlayerObject* p);
/// noclip-death-detector/2 (core/death_detector, src/DeathPath): the session / attempt generation
/// a kill is stamped with WHEN THE GAME RAISES IT. Both counters only ever grow, so a kill of an
/// earlier attempt or level can never match the current one.
struct Generation {
    uint32_t session = 0;
    uint32_t attempt = 0;
    bool open = false;   // an attempt is open
};
Generation generation();
/// The detector's verdict for one kill candidate; changes the detector's state only (a real death
/// closes its attempt, a new lethal contact is counted). A candidate for another layer is stale.
death::Verdict judgeDeath(PlayLayer* pl, death::Candidate const& candidate);
/// Applies an ACCEPTED verdict: the `death` event (real death: the attempt ends; new would-be
/// death: the attempt is marked noclip, SPEC §19), the session counters. `player` is the real live
/// player the verdict names, `object` the object the game passed (may be null).
void applyDeath(PlayLayer* pl, death::Verdict const& verdict, death::Candidate const& candidate, PlayerObject* player, GameObject* object);
/// The open (or last) attempt's detector counters, for the debug summary.
death::Counters deathCounters();
/// PlayLayer::levelComplete.
void onLevelComplete(PlayLayer* pl);
/// PlayLayer::onQuit (layer still valid) / PlayLayer destruction (layerValid = false).
void onQuit(PlayLayer* pl, bool layerValid);
/// Geode GameEvent Exiting (the game is closing; runs before static destructors): ends the open
/// attempt with reason exit WITHOUT touching the layer and forgets the level. It does not end the
/// telemetry session: client::shutdown() spools what is pending and leaves the remote session for
/// the server to close (no network on the exit path, Telemetry.hpp).
void onGameExit();
/// PlayLayer::postUpdate: progress events, periodic state samples, environment poll.
void onFrame(PlayLayer* pl, float dt);

bool active();
PlayLayer* layer();
/// The level session's local id (the prefix of every attemptId, "<id>-a<n>"); "" outside a level.
std::string sessionLocalId();
/// v0.12.0: sha256 of the open level's string (env::hashLevel, computed once at level entry for
/// the session); "" outside a level. The background analyzer's World::levelHash.
std::string levelHash();
/// Live client-side trust classification (polled by onFrame and the HUD twice a second).
TrustState trust();
/// Re-reads the mod menus: updates trust / noclip, marks the open attempt (noclipSeen,
/// untrusted), and emits a new `environment` event when the report changed since the last one
/// (core/classify environmentChanged: trust, noclip, bot, TPS bypass / tps, CBF, integrity).
void refreshTrust();
/// v0.12.0: the mod menus as last read for the open level (every attempt start and twice a second,
/// see refreshTrust). False outside a level or before the first read; the analyzer's recorder
/// uses this instead of reading the menus a second time at the same attempt start.
bool lastMenus(env::MenuState& out);

/// PlayerObject::pushButton / releaseButton of player 1: diagnostic counter only (the telemetry
/// input comes from handleButton). The Session tab compares the two so a hook that stopped firing
/// is visible instead of silently "not counting". Player 1 only, because one handleButton press
/// pushes both players in dual mode.
void onRawButton(PlayerObject* p, bool down);

/// Live counters of the current (or last) level for the Session tab and the HUD (SPEC §10 shows
/// no sigma/s; these are plain counts). Kept after the level is left, reset on the next level
/// entry. Updated on the game thread; the popup reads a copy on its 0.5 s tick.
struct SessionCounters {
    bool levelOpen = false;
    std::string levelId;
    std::string levelName;
    int attempts = 0;                       // attempt_start events this level (restarts, practice respawns included)
    int deaths = 0;                         // real deaths (attempt ended)
    int wouldBeDeaths = 0;                  // deaths a noclip swallowed (SPEC §19)
    int completions = 0;
    int jumps = 0;                          // presses (jump / left / right, both players) = telemetry `input` down events
    int releases = 0;
    int pressesByGamemode[kGamemodeCount] = {};   // presses per gamemode of the pressing player
    int rawPresses = 0;                     // PlayerObject::pushButton calls seen for player 1 (diagnostic)
    int rawReleases = 0;
    bool practice = false;                  // the current / last attempt was in practice mode
    double currentPercent = 0.0;            // of the open attempt (last known when closed)
    double bestPercent = 0.0;               // highest percent reached this level (any attempt)
    bool noclipNow = false;
    bool noclipSeen = false;                // noclip was on during any attempt this level
    int attemptInputs = 0;                  // inputs of the open attempt
    // timing-window solver (src/solver/GdOracle, v0.4.0): windows emitted / dropped this level
    int windowsEmitted = 0;
    int windowsMisses = 0;
    int windowsDropped = 0;
    // v0.5.1 practice / attempt time capture (attempt_end.activeMs / practiceMs / startPosMs, unpaused
    // wall clock) and the GD save's attempt count (UNTRUSTED, MASTER §10; -1 = unknown)
    double attemptActiveMs = 0.0;           // of the open attempt (last known when closed)
    double levelActiveMs = 0.0;             // sum over the ended attempts of this level
    double levelPracticeMs = 0.0;
    double levelStartPosMs = 0.0;
    int64_t gdAttemptCount = -1;            // GJGameLevel::m_attempts as last read
};
SessionCounters session();
/// Solver bookkeeping for the Session tab (called by solver/GdOracle on the game thread).
void noteWindow(bool emitted, bool miss);

/// Attempt facts for the popup.
struct AttemptView {
    bool open = false;
    std::string id;
    int attemptNo = 0;
    int sessionAttemptCount = 0;
    double fromPercent = 0.0;
    bool practice = false;
    bool noclipSeen = false;
    int inputs = 0;
    int64_t tick = 0;
    double levelTime = 0.0;
    double subTick = 0.0;
    int clampedEvents = 0;   // events whose t / tick had to be clamped to stay monotonic (debug)
};
AttemptView attemptView();

/// Portable snapshot of a player (PlayerStateSnapshot field mapping, snapshot.hpp header).
PlayerStateSnapshot snapshotOf(PlayLayer* pl, PlayerObject* p, int slot);
Gamemode gamemodeOf(PlayerObject* p);
Speed speedOf(PlayerObject* p);

}  // namespace gprl::tracker
