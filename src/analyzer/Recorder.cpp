// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. Every read below is a plain binding member or a const-like getter (getPosition,
// getCurrentPercent); see Recorder.hpp for the list.
#include "Recorder.hpp"

#include <cmath>

#include "../../core/analyzer_extract.hpp"
#include "../../core/classify.hpp"
#include "../Environment.hpp"
#include "../Tracker.hpp"
#include "../solver/GdOracle.hpp"
#include "Modes.hpp"

using namespace geode::prelude;

namespace gprl::analyzer::recorder {

namespace {

AttemptRecorder s_rec;
PlayLayer* s_pl = nullptr;
std::string s_skip;
bool s_botAtStart = false;

bool realPlayer1(PlayLayer* pl, PlayerObject* p) {
    if (!pl || !p || p != pl->m_player1) return false;
    // the live solver's hidden clones are stepped inside processCommands: never theirs
    if (solver::oracle::stepping() || solver::oracle::isClone(p)) return false;
    return true;
}

LevelSettingsObject* activeSettings(PlayLayer* pl) {
    if (pl->m_startPosObject && pl->m_startPosObject->m_startSettings) return pl->m_startPosObject->m_startSettings;
    return pl->m_levelSettings;
}

}  // namespace

PlayerReading readPlayer(PlayerObject* p) {
    PlayerReading r;
    if (!p) return r;
    auto pos = p->getPosition();
    r.x = pos.x;
    r.y = pos.y;
    r.yVelocity = p->m_yVelocity;
    r.upsideDown = p->m_isUpsideDown;
    // tracker::gamemodeOf / speedOf use the same vocab order as sim::Gamemode / sim::Speed (cube..swing, slow..fastest)
    r.mode = static_cast<sim::Gamemode>(static_cast<uint8_t>(tracker::gamemodeOf(p)));
    r.mini = p->m_vehicleSize < 1.f - 1e-3f;
    r.speed = static_cast<sim::Speed>(static_cast<uint8_t>(tracker::speedOf(p)));
    r.onGround = p->m_isOnGround;
    auto held = p->m_holdingButtons.find(1);
    r.held = held != p->m_holdingButtons.end() && held->second;
    r.dead = p->m_isDead;
    return r;
}

void onAttemptStart(PlayLayer* pl, int attemptIndex) {
    s_rec.abandon();
    s_pl = pl;
    s_skip.clear();
    if (!pl || !pl->m_player1 || !modes::simulatorEnabled()) {
        s_skip = "analyzer off";
        return;
    }
    if (pl->m_isPracticeMode) {
        s_skip = "practice mode";
        return;
    }
    if (pl->m_levelSettings && pl->m_levelSettings->m_platformerMode) {
        s_skip = "platformer level";
        return;
    }
    // the tracker read the menus at this very attempt start (Hooks.cpp runs the recorder's attempt
    // start after tracker::onLevelEnter / onLevelReset); a second env::readMenus only without them
    env::MenuState menus;
    if (!tracker::lastMenus(menus)) menus = env::readMenus();
    if ((menus.tpsBypass && std::fabs(menus.tps - 240.0) > 1e-6) || (menus.speedhack && std::fabs(menus.speedhackValue - 1.0) > 1e-3)) {
        s_skip = "physics changed (TPS bypass / speedhack)";
        return;
    }
    s_botAtStart = menus.botState == 2;
    auto* p = pl->m_player1;
    auto r = readPlayer(p);
    sim::StartState start;
    start.x = r.x;
    start.y = r.y;
    start.mode = r.mode;
    start.mini = r.mini;
    start.upsideDown = r.upsideDown;
    if (auto* ls = activeSettings(pl)) {
        // the speed GD applies at the attempt start (updateTimeMod may still be queued at resetLevel)
        start.speed = extract::speedFromGeodeSpeed(static_cast<int>(ls->m_startSpeed));
        start.dual = ls->m_startDual;
        start.mirror = ls->m_mirrorMode;
        start.reversed = ls->m_reverseGameplay;
    }
    if (pl->m_levelSettings) {
        start.platformer = pl->m_levelSettings->m_platformerMode;
        start.twoPlayer = pl->m_levelSettings->m_twoPlayerMode;
    }
    float pct = pl->getCurrentPercent();
    start.percent = std::isfinite(pct) ? std::clamp(pct, 0.f, 100.f) : 0.f;
    AttemptMeta meta;
    if (pl->m_startPosObject) {
        meta.hasStartPos = true;
        meta.startPosX = pl->m_startPosObject->getPositionX();
    }
    s_rec.begin(start, attemptIndex, menus.cbfActive, meta);
}

void onStepBegin(PlayLayer* pl) {
    if (!s_rec.active() || pl != s_pl || !pl->m_player1) return;
    if (solver::oracle::stepping()) return;
    if (!s_rec.wantsTick()) return;
    s_rec.recordTick(readPlayer(pl->m_player1));
}

void onPlayerUpdate(PlayLayer* pl, PlayerObject* p, float dt) {
    if (!s_rec.active() || pl != s_pl || !realPlayer1(pl, p)) return;
    s_rec.onPlayerUpdate(static_cast<double>(dt));
}

void onInput(PlayLayer* pl, PlayerObject* p, int button, bool down) {
    if (!s_rec.active() || pl != s_pl || !realPlayer1(pl, p)) return;
    if (button != static_cast<int>(PlayerButton::Jump)) return;
    s_rec.onInput(down);
}

std::optional<Finished> onAttemptEnd(PlayLayer* pl, bool died, bool completed, bool readFinal) {
    if (!s_rec.active() || pl != s_pl) return std::nullopt;
    PlayerReading fin;
    bool haveFinal = readFinal && pl && pl->m_player1;
    if (haveFinal) fin = readPlayer(pl->m_player1);
    float pct = pl ? pl->getCurrentPercent() : 0.f;
    if (completed) pct = 100.f;
    AttemptMeta meta = s_rec.meta();
    Finished f;
    f.attempt = s_rec.finish(died, completed, std::isfinite(pct) ? std::clamp(pct, 0.f, 100.f) : 0.f, haveFinal ? &fin : nullptr);
    f.meta = meta;
    if (tracker::attemptView().noclipSeen) {
        s_skip = "noclip was on";
        return std::nullopt;
    }
    if (s_botAtStart && !modes::botPlaybackEvidenceAllowed()) {
        s_skip = "bot playback (Record-Safe)";
        return std::nullopt;
    }
    if (f.attempt.ticks.empty()) return std::nullopt;
    return f;
}

void abandon() {
    s_rec.abandon();
    s_pl = nullptr;
}

bool recording() { return s_rec.active(); }
int recordedTicks() { return s_rec.ticks(); }
std::string const& skipReason() { return s_skip; }

}  // namespace gprl::analyzer::recorder
