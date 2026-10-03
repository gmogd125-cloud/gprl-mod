#pragma once
// Background level analyzer: the passive per-attempt recorder (docs/BACKGROUND_ANALYZER_DESIGN.md
// §4.5, AN-D3). One RecordedTick per 240 TPS tick of player 1 and every jump press / release with
// its tick and sub-tick, from the start state GD placed the player in. The tick bookkeeping is the
// pure core/analyzer_recorder.hpp AttemptRecorder (host-tested); this file only READS player 1.
//
// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. Reads (binding names): PlayerObject getPosition(), m_yVelocity, m_isUpsideDown,
// m_isShip / m_isBall / m_isBird / m_isDart / m_isRobot / m_isSpider / m_isSwing, m_vehicleSize,
// m_playerSpeed, m_isOnGround, m_holdingButtons[1], m_isDead; PlayLayer m_startPosObject
// (+ StartPosObject::m_startSettings), m_levelSettings (m_startSpeed, m_startDual, m_mirrorMode,
// m_reverseGameplay, m_platformerMode, m_twoPlayerMode), m_isPracticeMode, getCurrentPercent().
//
// Which attempts are recorded: not in practice mode (a checkpoint respawn restores velocity state
// a StartState cannot carry), not on platformer levels, not under a TPS bypass other than 240 or a
// speedhack (physics changed). Attempts with noclip seen are dropped at the end (the trajectory
// passed through hazards), and bot playback attempts in Record-Safe Mode.
#include <Geode/Geode.hpp>

#include <optional>
#include <string>

#include "../../core/analyzer_recorder.hpp"

namespace gprl::analyzer::recorder {

struct Finished {
    sim::RecordedAttempt attempt;
    AttemptMeta meta;
};

/// After PlayLayer::resetLevel AND tracker::onLevelReset (and at level enter for the first attempt,
/// after tracker::onLevelEnter): the menus come from tracker::lastMenus (read at this attempt start),
/// env::readMenus only when the tracker has none.
void onAttemptStart(PlayLayer* pl, int attemptIndex);
/// GJBaseGameLayer::processCommands PRE-hook: records the tick that just finished.
void onStepBegin(PlayLayer* pl);
/// PlayerObject::update POST-hook of player 1.
void onPlayerUpdate(PlayLayer* pl, PlayerObject* p, float dt);
/// PlayerObject::pushButton / releaseButton PRE-hook of player 1 (jump only).
void onInput(PlayLayer* pl, PlayerObject* p, int button, bool down);
/// Attempt end. `readFinal`: player 1 is read for the tick the end happened in (death / completion).
/// nullopt when nothing usable was recorded (see the header for the rules).
std::optional<Finished> onAttemptEnd(PlayLayer* pl, bool died, bool completed, bool readFinal);
/// Level exit without a layer to read (Fields destructor): drop the open attempt.
void abandon();

bool recording();
int recordedTicks();
/// Why the current / last attempt is not recorded ("" = it is).
std::string const& skipReason();

/// Player 1's portable reading (also used by the extraction for the level start).
PlayerReading readPlayer(PlayerObject* p);

}  // namespace gprl::analyzer::recorder
