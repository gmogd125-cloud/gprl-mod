#include "Tracker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "../core/classify.hpp"
#include "../core/display.hpp"
#include "../core/telemetry.hpp"
#include "Clipper.hpp"
#include "Environment.hpp"
#include "Settings.hpp"
#include "Telemetry.hpp"
#include "solver/GdOracle.hpp"

using namespace geode::prelude;

namespace gprl::tracker {

namespace {

using telemetry::Event;

constexpr int kStateSampleEveryTicks = 24;   // 0.1 s at 240 TPS, per player
constexpr int kNearPortalTicks = 12;         // "a portal was activated within the last few ticks"
constexpr float kEnvironmentPollSeconds = 0.5f;   // menu state (noclip / bot / TPS) re-read this often

// Portal object ids (GD 2.2): gravity 10/11, cube 12, ship 13, ball 47, ufo 111, wave 660,
// robot 745, spider 1331, swing 1933, mirror 45/46, mini 99 / regular 101, dual 286 / 287,
// speed 200-203 + 1334, teleport 747.
constexpr int kPortalIds[] = {10, 11, 12, 13, 47, 111, 660, 745, 1331, 1933, 45, 46, 99, 101, 286, 287, 200, 201, 202, 203, 1334, 747};

bool isPortal(int id) {
    for (int p : kPortalIds) if (p == id) return true;
    return false;
}

struct PortalMemory {
    int objectId = 0;
    int64_t tick = -1;
};

struct Attempt {
    bool open = false;
    std::string id;
    int no = 0;
    int sessionCount = 0;
    double fromPercent = 0.0;
    bool practice = false;
    bool noclipSeen = false;      // noclip on at any poll of this attempt, or a would-be death (SPEC §19)
    classify::WouldBeDeathStreak wouldBe;   // v0.10.0: one would-be death per contiguous run of swallowed destroys (Eclipse's rule)
    bool untrustedSeen = false;   // any poll of this attempt classified the environment as not Allowed
    int inputs = 0;
    double baseLevelTime = 0.0;   // m_levelTime at the attempt start (StartPos time)
    double lastT = 0.0;           // monotonic guards for the server's invariants
    int64_t lastTick = 0;
    int lastPercent = -1;
    double lastKnownPercent = 0.0;
    int64_t lastSampleBucket = -1;
    std::chrono::steady_clock::time_point startedAt{};
    int clamped = 0;
    // v0.5.1 practice / attempt time capture (core/display AttemptClock, host-tested): unpaused
    // wall-clock ms fed once per rendered frame from onFrame; the pause menu, alt-tab and hitches
    // show up as frame deltas over 0.5 s and are discarded.
    display::AttemptClock clock;
    std::chrono::steady_clock::time_point lastFrameAt{};
    bool haveLastFrame = false;
};

/// GJGameLevel::m_attempts (the GD save's total, MASTER §10: untrusted context) or nullopt.
std::optional<int64_t> gdAttemptCount(PlayLayer* pl) {
    if (!pl || !pl->m_level) return std::nullopt;
    int64_t n = static_cast<int64_t>(pl->m_level->m_attempts.value());
    return n >= 0 ? std::optional<int64_t>(n) : std::nullopt;
}

struct State {
    PlayLayer* pl = nullptr;
    std::string sessionLocalId;
    int sessionAttempts = 0;
    Attempt attempt;
    Gamemode lastMode[3] = {Gamemode::Cube, Gamemode::Cube, Gamemode::Cube};
    PortalMemory portal[3];
    classify::SubTickClock clock;   // player 1's update deltas inside the current tick (core/classify)
    TrustState trust = TrustState::Allowed;
    bool noclipNow = false;
    std::string levelId;
    std::string levelHash;   // v0.12.0: the session's level hash (the analyzer's World::levelHash)
    bool haveEnvironment = false;
    telemetry::EnvironmentPayload lastEnvironment;   // the last `environment` payload emitted this session
    float environmentPollAccum = 0.f;
    // v0.12.0 review fix: the menus as last read this level (pollEnvironment: every attempt start
    // and twice a second) - the analyzer's recorder uses them instead of a second env::readMenus
    bool haveMenus = false;
    env::MenuState lastMenus;
} s;

/// Session tab / HUD counters: outlive `s` (kept after the level is left, reset on the next entry).
SessionCounters s_counters;

int64_t currentTick(PlayLayer* pl) { return static_cast<int64_t>(pl->m_gameState.m_currentProgress / 2u); }
double currentLevelTime(PlayLayer* pl) { return pl->m_gameState.m_levelTime; }

double currentPercent(PlayLayer* pl) {
    float p = pl->getCurrentPercent();
    if (!std::isfinite(p)) return s.attempt.lastKnownPercent;
    return std::clamp(static_cast<double>(p), 0.0, 100.0);
}

double subTickNow() { return s.clock.fraction(); }

int slotOf(PlayLayer* pl, PlayerObject* p) {
    if (!pl || !p) return 0;
    if (p == pl->m_player1) return 1;
    if (p == pl->m_player2) return 2;
    return 0;
}

/// Event skeleton for the open attempt with monotonic t / tick (validate.ts checkBatchInvariants).
Event makeEvent(bool live = true) {
    Event e;
    e.attemptId = s.attempt.id;
    if (s.pl) {
        e.t = std::max(0.0, currentLevelTime(s.pl) - s.attempt.baseLevelTime);
        e.tick = std::max<int64_t>(0, currentTick(s.pl));
    }
    if (live) {
        if (e.t < s.attempt.lastT || e.tick < s.attempt.lastTick) ++s.attempt.clamped;
        e.t = std::max(e.t, s.attempt.lastT);
        e.tick = std::max(e.tick, s.attempt.lastTick);
        s.attempt.lastT = e.t;
        s.attempt.lastTick = e.tick;
    }
    return e;
}

void emit(Event e) {
    GPRL_DEBUG("GPRL event {} t={:.4f} tick={} attempt={}", telemetry::kindName(e.kind()), e.t, e.tick, e.attemptId);
    client::push(std::move(e));
}

std::string makeSessionLocalId() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return fmt::format("{:x}", static_cast<uint64_t>(now) & 0xffffffffffull);
}

void endAttempt(AttemptEndReason reason, bool completed, double percent) {
    if (!s.attempt.open) return;
    Event e = makeEvent();
    telemetry::AttemptEndPayload p;
    p.reason = reason;
    p.percent = std::clamp(percent, 0.0, 100.0);
    p.completed = completed;
    p.legit = s.trust == TrustState::Allowed && !s.attempt.untrustedSeen && !s.attempt.noclipSeen;
    p.noclipSeen = s.attempt.noclipSeen;
    // v0.5.1: unpaused wall-clock time of the attempt (0.1 ms resolution keeps the JSON short)
    auto tenth = [](double ms) { return std::round(ms * 10.0) / 10.0; };
    auto const& clock = s.attempt.clock;
    p.activeMs = tenth(clock.activeMs());
    p.practiceMs = tenth(clock.practiceMs());
    p.startPosMs = tenth(clock.startPosMs());
    // rounding can only shrink a part below the whole by < 0.1 ms; never let the parts exceed it
    if (*p.practiceMs + *p.startPosMs > *p.activeMs) p.activeMs = *p.practiceMs + *p.startPosMs;
    s_counters.levelActiveMs += *p.activeMs;
    s_counters.levelPracticeMs += *p.practiceMs;
    s_counters.levelStartPosMs += *p.startPosMs;
    GPRL_DEBUG("GPRL attempt {} ended ({}): active {:.1f} ms (practice {:.1f}, startpos {:.1f}), {} frames counted, {} discarded (paused / hitch)",
               s.attempt.id, name(reason), *p.activeMs, *p.practiceMs, *p.startPosMs, clock.countedFrames(), clock.discardedFrames());
    double endT = e.t;
    int64_t endTick = e.tick;
    bool legit = p.legit;
    e.payload = p;
    emit(std::move(e));
    s.attempt.open = false;
    // v0.6.0 clipping buffer: the attempt's range ends here; an exceptional run is preserved for the
    // player's choice (src/Clipper, core/clip decidePreserve). No-op while clipping is off.
    clipper::onAttemptEnd(s.attempt.id, completed, p.percent, legit, endT, endTick);
}

/// `environment` event (session-scoped: exempt from the per-attempt t / tick guards, validate.ts).
/// Its attemptId is the current (or upcoming) attempt's, which validate.ts requires non-empty.
void emitEnvironment(telemetry::EnvironmentPayload payload, bool atAttemptStart) {
    payload.droppedEvents = client::droppedEvents();
    Event env = makeEvent(false);
    if (atAttemptStart) {
        env.t = 0.0;
        env.tick = 0;
    }
    s.lastEnvironment = payload;
    s.haveEnvironment = true;
    env.payload = std::move(payload);
    emit(std::move(env));
}

/// Reads the menus, marks the open attempt, and re-emits the environment when it changed.
env::MenuState pollEnvironment(bool atAttemptStart) {
    auto menus = env::readMenus();
    s.lastMenus = menus;
    s.haveMenus = true;
    s.trust = env::classify(menus);
    s.noclipNow = menus.noclip;
    s_counters.noclipNow = menus.noclip;
    // the solver's gate follows the trust state (SOLVER_DESIGN §5): paused under TPS bypass / bot
    if (s.pl) solver::oracle::refreshGate(s.trust, menus);
    if (s.attempt.open) {
        if (menus.noclip) {
            s.attempt.noclipSeen = true;
            s_counters.noclipSeen = true;
        }
        if (s.trust != TrustState::Allowed) s.attempt.untrustedSeen = true;
    }
    if (s.pl && s.haveEnvironment) {
        auto now = env::refreshed(s.lastEnvironment, menus);
        if (classify::environmentChanged(s.lastEnvironment, now)) {
            log::info("GPRL: environment changed mid-session (trust {} -> {}), re-reported", name(s.lastEnvironment.trust.value_or(TrustState::Allowed)),
                      name(now.trust.value_or(TrustState::Allowed)));
            emitEnvironment(std::move(now), atAttemptStart);
        }
    }
    return menus;
}

void startAttempt(PlayLayer* pl, bool firstOfSession, telemetry::EnvironmentPayload const* environment) {
    Attempt a;
    a.open = true;
    a.no = pl->m_attempts;
    a.sessionCount = ++s.sessionAttempts;
    a.id = fmt::format("{}-a{}", s.sessionLocalId, a.sessionCount);
    a.practice = pl->m_isPracticeMode;
    a.baseLevelTime = currentLevelTime(pl);
    a.fromPercent = currentPercent(pl);
    a.lastKnownPercent = a.fromPercent;
    a.lastPercent = static_cast<int>(std::floor(a.fromPercent));
    a.startedAt = std::chrono::steady_clock::now();
    a.lastFrameAt = a.startedAt;
    a.haveLastFrame = true;
    s.attempt = a;
    ++s_counters.attempts;
    s_counters.practice = a.practice;
    s_counters.currentPercent = a.fromPercent;
    s_counters.attemptInputs = 0;
    s_counters.attemptActiveMs = 0.0;
    if (auto n = gdAttemptCount(pl)) s_counters.gdAttemptCount = *n;
    if (firstOfSession && environment) emitEnvironment(*environment, true);
    // Every attempt re-reads the menus: marks this attempt (noclipSeen / untrusted) and re-reports
    // an environment that changed since the last report, BEFORE the attempt_start it applies to.
    auto menus = pollEnvironment(true);
    s.environmentPollAccum = 0.f;
    s.clock.reset(classify::tickDtFor(menus.tpsBypass, menus.tps));
    for (int slot = 1; slot <= 2; ++slot) {
        PlayerObject* p = slot == 1 ? pl->m_player1 : pl->m_player2;
        s.lastMode[slot] = p ? gamemodeOf(p) : Gamemode::Cube;
        s.portal[slot] = PortalMemory{};
    }

    Event e = makeEvent(false);
    e.t = 0.0;
    e.tick = 0;
    s.attempt.lastT = 0.0;
    s.attempt.lastTick = 0;
    telemetry::AttemptStartPayload p;
    p.attemptNo = a.no;
    p.fromPercent = a.fromPercent;
    p.practice = a.practice;
    if (pl->m_startPosObject) p.startPosTick = static_cast<int64_t>(std::llround(a.baseLevelTime * kTicksPerSecond));
    p.noclip = s.noclipNow;
    p.sessionAttemptCount = a.sessionCount;
    // v0.5.1 (MASTER §10): the GD save's total attempt count, UNTRUSTED context the server keeps
    // apart from its own observed count (attempt_start.gdAttemptCount)
    p.gdAttemptCount = gdAttemptCount(pl);
    e.payload = p;
    emit(std::move(e));
    clipper::onAttemptStart(a.id, a.no, a.fromPercent, a.practice, pl->m_startPosObject != nullptr);
}

}  // namespace

// ---- snapshot helpers ----

// The field reads stay here (Geode types); the rules are core/classify (host-tested).
Gamemode gamemodeOf(PlayerObject* p) {
    classify::ModeFlags f;
    f.ship = p->m_isShip;
    f.ball = p->m_isBall;
    f.ufo = p->m_isBird;
    f.wave = p->m_isDart;
    f.robot = p->m_isRobot;
    f.spider = p->m_isSpider;
    f.swing = p->m_isSwing;
    return classify::gamemodeFromFlags(f);
}

Speed speedOf(PlayerObject* p) { return classify::speedFromMultiplier(static_cast<double>(p->m_playerSpeed)); }

PlayerStateSnapshot snapshotOf(PlayLayer* pl, PlayerObject* p, int slot) {
    PlayerStateSnapshot snap;
    snap.player = slot;
    snap.tick = std::max<int64_t>(0, currentTick(pl));
    snap.levelTime = std::max(0.0, currentLevelTime(pl));
    snap.subTick = subTickNow();
    snap.gamemode = gamemodeOf(p);
    snap.speed = speedOf(p);
    snap.gravityFlipped = p->m_isUpsideDown;
    snap.mini = classify::isMini(static_cast<double>(p->m_vehicleSize));
    snap.platformer = p->m_isPlatformer;
    snap.x = p->getPositionX();
    snap.y = p->getPositionY();
    snap.yVel = p->m_yVelocity;
    snap.xVel = p->m_isPlatformer ? p->m_platformerXVelocity : 0.0;
    snap.rotation = p->getRotation();
    snap.isOnGround = p->m_isOnGround;
    snap.isOnSlope = p->m_isOnSlope;
    if (auto it = p->m_holdingButtons.find(1); it != p->m_holdingButtons.end()) snap.isHolding = it->second;
    snap.isDashing = p->m_isDashing;
    snap.hasJumped = p->m_hasEverJumped;
    snap.touchedRing = p->m_touchedRing;
    snap.jumpBuffered = p->m_jumpBuffered;
    snap.ringJumpPending = p->m_stateRingJump;
    auto const& portal = s.portal[slot];
    snap.lastPortalObjectId = std::max(0, portal.objectId);
    snap.nearPortal = portal.tick >= 0 && snap.tick - portal.tick <= kNearPortalTicks;
    snap.geometryHash = solver::oracle::geometryHashAt(p->getPositionX());   // core/geometry_hash low 32 bits (0 = no engine)
    return snap;
}

// ---- lifecycle ----

void onLevelEnter(PlayLayer* pl) {
    if (!settings::get().enabled || !pl || !pl->m_level) return;
    // GD resets the level once while it loads (before setupHasCompleted), and onLevelReset already
    // opened the session for this layer then. Keep it instead of ending it and opening a second one.
    if (s.pl == pl) return;
    if (s.pl) onQuit(s.pl, false);
    s = State{};
    s.pl = pl;
    s_counters = SessionCounters{};
    s_counters.levelOpen = true;
    s_counters.levelName = std::string(pl->m_level->m_levelName);
    s.sessionLocalId = makeSessionLocalId();
    s.levelId = std::to_string(pl->m_level->m_levelID.value());
    s_counters.levelId = s.levelId;
    std::string levelHash = env::hashLevel(pl->m_level->m_levelString, s.levelId);
    s.levelHash = levelHash;
    auto snapshot = env::capture(levelHash);
    snapshot.payload.droppedEvents = client::droppedEvents();
    s.trust = snapshot.trust;
    s.noclipNow = snapshot.menus.noclip;

    client::SessionStart start;
    start.levelId = s.levelId;
    start.levelName = std::string(pl->m_level->m_levelName);
    start.levelHash = levelHash;
    start.modList = snapshot.modList;
    start.integrity.state = snapshot.payload.integrity;
    start.integrity.gdHash = snapshot.payload.hashes.gd;
    start.integrity.geodeHash = snapshot.payload.hashes.geode;
    start.integrity.gprlHash = snapshot.payload.hashes.gprl;
    // v0.5.1 level hints (MASTER C6): what GD shows of the level - GJGameLevel::m_stars, m_demon,
    // m_demonDifficulty, m_levelName (names checked in GeometryDash.bro 2.2081). The server decides
    // whether the level counts from the GD servers; these can never upgrade a level.
    {
        auto* level = pl->m_level;
        int stars = level->m_stars.value();
        bool demon = level->m_demon.value() != 0;
        start.hints.stars = std::clamp(stars, 0, 10);
        start.hints.isDemon = demon;
        start.hints.demonDifficulty = display::demonDifficultyName(level->m_demonDifficulty, demon);
        start.hints.name = display::levelNameHint(std::string(level->m_levelName));
        // level families (docs/LEVEL_FAMILY_DESIGN.md §1): the copied-from id (GD key 30), a HINT only
        start.hints.originalLevelId = std::max<int64_t>(0, static_cast<int64_t>(level->m_originalLevel.value()));
    }
    s_counters.gdAttemptCount = gdAttemptCount(pl).value_or(-1);
    // Log before handing `start` over: after the move its strings are empty.
    log::info("GPRL: level {} ({}) entered, trust {}, {} mods, {} modules; GD shows stars {}, demon {}{}, GD attempts {} (untrusted)", s.levelId,
              start.levelName, name(s.trust), snapshot.modList.size(), snapshot.payload.modules ? snapshot.payload.modules->size() : 0,
              start.hints.stars.value_or(0), *start.hints.isDemon ? "yes" : "no",
              start.hints.demonDifficulty.empty() ? "" : " (" + start.hints.demonDifficulty + ")", s_counters.gdAttemptCount);
    {
        // v0.6.0 clipping buffer: a level is open (GD's own "rated demon" data is the local stand-in
        // for the server's levelCounts while there is no server verdict)
        clipper::LevelInfo info;
        info.levelId = s.levelId;
        info.levelName = start.levelName;
        info.levelHash = levelHash;
        info.localRatedDemon = start.hints.isDemon.value_or(false) && start.hints.stars.value_or(0) > 0;
        clipper::onLevelEnter(std::move(info), s.sessionLocalId);
    }
    client::beginSession(std::move(start));
    startAttempt(pl, true, &snapshot.payload);
}

void onLevelReset(PlayLayer* pl) {
    if (pl != s.pl) {
        // resetLevel without a prior setupHasCompleted (should not happen) - treat as a level entry
        if (settings::get().enabled) onLevelEnter(pl);
        return;
    }
    // GD may call resetLevel while the level is being set up: a fresh attempt with no input yet
    // that is a few ms old is the same attempt, not a restart.
    if (s.attempt.open && s.attempt.inputs == 0 && currentTick(pl) == 0
        && std::chrono::steady_clock::now() - s.attempt.startedAt < std::chrono::milliseconds(150)) {
        GPRL_DEBUG("GPRL: resetLevel during setup ignored (attempt {})", s.attempt.id);
        return;
    }
    if (s.attempt.open) endAttempt(AttemptEndReason::Restart, false, s.attempt.lastKnownPercent);
    startAttempt(pl, false, nullptr);
}

void onPracticeToggle(PlayLayer* pl, bool practice) {
    if (pl != s.pl) return;
    GPRL_DEBUG("GPRL: practice mode {}", practice ? "on" : "off");
    // the next attempt_start carries the flag; GD restarts the level on toggle
}

void onPlayerUpdate(PlayerObject* p, float dt) {
    if (!s.pl || p != s.pl->m_player1) return;
    s.clock.onPlayerUpdate(static_cast<double>(dt));
}

void onButton(GJBaseGameLayer* layer, bool down, int button, bool isPlayer1) {
    if (!s.pl || layer != static_cast<GJBaseGameLayer*>(s.pl)) return;
    auto in = classify::classifyButton(down, button, isPlayer1);
    if (!in) return;
    // Session counters first: they count every classified press of this level, attempt open or not
    if (in->down) {
        ++s_counters.jumps;
        PlayerObject* who = in->player == 2 ? s.pl->m_player2 : s.pl->m_player1;
        if (who) ++s_counters.pressesByGamemode[static_cast<int>(gamemodeOf(who))];
    }
    else ++s_counters.releases;
    if (!s.attempt.open) return;
    ++s_counters.attemptInputs;
    Event e = makeEvent();
    telemetry::InputPayload p;
    p.player = in->player;
    p.button = in->button;
    p.down = in->down;
    p.tSubTick = subTickNow();
    e.payload = p;
    ++s.attempt.inputs;
    double t = e.t;
    int64_t tick = e.tick;
    double subTick = p.tSubTick;
    int64_t droppedBefore = client::droppedEvents();
    emit(std::move(e));
    // The solver opened a measurement in the pushButton pre-hook inside this handleButton call:
    // link it to the input event's seq (SOLVER_DESIGN §3.2). A refused push (ring full) cancels it:
    // no window for an input the server never sees. v0.7.0: the event's sub-tick fraction goes
    // with it, so timing_window and timing_result carry actualMs = t x 1000 + sub-tick ms, the same
    // canonical axis as the input event (docs/TIMING_SOLVER_V2.md RC-minor 6; 0 without CBF).
    if (in->player == 1 && in->button == Button::Jump) {
        bool dropped = client::droppedEvents() > droppedBefore;
        solver::oracle::bindLastInput(client::lastAssignedSeq(), t, tick, s.attempt.id, s.attempt.practice, dropped, subTick);
    }
}

void onPortal(GJBaseGameLayer* layer, PlayerObject* p, int objectId) {
    if (!s.pl || layer != static_cast<GJBaseGameLayer*>(s.pl) || !isPortal(objectId)) return;
    int slot = slotOf(s.pl, p);
    if (!slot) return;
    s.portal[slot] = {objectId, currentTick(s.pl)};
}

void onGamemodeToggle(PlayerObject* p) {
    if (!s.pl || !s.attempt.open) return;
    int slot = slotOf(s.pl, p);
    if (!slot) return;
    Gamemode now = gamemodeOf(p);
    if (now == s.lastMode[slot]) return;
    Event e = makeEvent();
    telemetry::GamemodeChangePayload gp;
    gp.player = slot;
    gp.from = s.lastMode[slot];
    gp.to = now;
    gp.portalObjectId = std::max(0, s.portal[slot].objectId);
    if (gp.portalObjectId == 0) {
        GameObject* last = slot == 1 ? s.pl->m_gameState.m_lastActivatedPortal1 : s.pl->m_gameState.m_lastActivatedPortal2;
        if (last) gp.portalObjectId = std::max(0, last->m_objectID);
    }
    e.payload = gp;
    s.lastMode[slot] = now;
    emit(std::move(e));
}

void onDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* object, bool wasDead, bool isDead) {
    if (pl != s.pl) return;
    classify::DestroyFacts facts;
    facts.attemptOpen = s.attempt.open;
    facts.wasDeadBefore = wasDead;
    facts.playerSlot = slotOf(pl, player);   // 0 = other mods' clones
    facts.anticheatSpike = object && pl->m_anticheatSpike && object == pl->m_anticheatSpike;   // GD_PHYSICS_NOTES
    facts.deadAfter = isDead;
    auto verdict = classify::classifyDestroy(facts);
    if (verdict == classify::DestroyVerdict::Ignore) return;
    // v0.10.0: a noclip menu swallows the destroy on every frame inside the hazard (and once per
    // hazard touched in a frame); like Eclipse's counter, a death is one contiguous run of frames
    if (verdict == classify::DestroyVerdict::WouldBeDeath
        && !classify::startsWouldBeDeath(s.attempt.wouldBe, static_cast<int64_t>(pl->m_gameState.m_currentProgress)))
        return;
    double percent = currentPercent(pl);
    s.attempt.lastKnownPercent = percent;
    Event e = makeEvent();
    telemetry::DeathPayload d;
    d.percent = percent;
    d.x = player->getPositionX();
    d.objectId = object ? std::max(0, object->m_objectID) : 0;
    d.wouldBe = verdict == classify::DestroyVerdict::WouldBeDeath;
    e.payload = d;
    emit(std::move(e));
    if (verdict == classify::DestroyVerdict::Death) {
        ++s_counters.deaths;
        s_counters.currentPercent = percent;
        endAttempt(AttemptEndReason::Death, false, percent);
    }
    else {
        ++s_counters.wouldBeDeaths;
        s.attempt.noclipSeen = true;   // SPEC §19: the run continues, the evidence is marked
        s_counters.noclipSeen = true;
    }
}

void onLevelComplete(PlayLayer* pl) {
    if (pl != s.pl || !s.attempt.open) return;
    double percent = pl->m_isPlatformer ? 100.0 : std::max(currentPercent(pl), 100.0);
    ++s_counters.completions;
    s_counters.currentPercent = percent;
    s_counters.bestPercent = std::max(s_counters.bestPercent, percent);
    endAttempt(AttemptEndReason::Complete, true, percent);
}

void onQuit(PlayLayer* pl, bool layerValid) {
    if (pl != s.pl) return;
    // v0.5.1 (MASTER §10): the GD save's attempt count when the level is left, sent with the
    // session end as untrusted context (only while the layer is still valid to read)
    std::optional<int64_t> reported = layerValid ? gdAttemptCount(pl) : std::nullopt;
    if (s.attempt.open) {
        double percent = layerValid ? currentPercent(pl) : s.attempt.lastKnownPercent;
        if (!layerValid) s.pl = nullptr;   // makeEvent must not touch a dying layer
        endAttempt(AttemptEndReason::Exit, false, percent);
    }
    clipper::onLevelExit();
    client::endSession(SessionEndReason::LevelExit, reported);
    log::info("GPRL: level {} left after {} attempts (GD save says {} attempts, untrusted); active {:.1f} s this level (practice {:.1f} s, startpos {:.1f} s)",
              s.levelId, s.sessionAttempts, reported ? std::to_string(*reported) : "?", s_counters.levelActiveMs / 1000.0,
              s_counters.levelPracticeMs / 1000.0, s_counters.levelStartPosMs / 1000.0);
    s = State{};
    s_counters.levelOpen = false;
    s_counters.noclipNow = false;
}

void onGameExit() {
    if (!s.pl) return;
    if (s.attempt.open) {
        double percent = s.attempt.lastKnownPercent;   // updated every frame by onFrame
        s.pl = nullptr;                                // the director is being purged: never touch the layer
        endAttempt(AttemptEndReason::Exit, false, percent);
    }
    clipper::onLevelExit();
    log::info("GPRL: game exiting inside level {} after {} attempts", s.levelId, s.sessionAttempts);
    s = State{};
    s_counters.levelOpen = false;
}

void onFrame(PlayLayer* pl, float dt) {
    if (pl != s.pl) return;
    // Environment poll independent of the HUD (which may be hidden): a noclip / bot / TPS change
    // mid-attempt is re-reported and marks the attempt (SPEC §19-§21).
    s.environmentPollAccum += dt;
    if (s.environmentPollAccum >= kEnvironmentPollSeconds) {
        s.environmentPollAccum = 0.f;
        pollEnvironment(false);
    }
    if (!s.attempt.open) return;
    // v0.5.1 attempt clock: real elapsed time since the previous rendered frame while the level is
    // not paused (PlayLayer::m_isPaused); the first frame after the pause menu closed carries the
    // whole pause and is discarded by the clock's 0.5 s guard (core/display AttemptClock).
    {
        auto now = std::chrono::steady_clock::now();
        if (s.attempt.haveLastFrame) {
            double elapsedMs = std::chrono::duration<double, std::milli>(now - s.attempt.lastFrameAt).count();
            s.attempt.clock.frame(elapsedMs, pl->m_isPaused, pl->m_isPracticeMode, pl->m_startPosObject != nullptr);
        }
        s.attempt.lastFrameAt = now;
        s.attempt.haveLastFrame = true;
        s_counters.attemptActiveMs = s.attempt.clock.activeMs();
    }
    // progress: one event per whole percent gained
    double percent = currentPercent(pl);
    s.attempt.lastKnownPercent = percent;
    s_counters.currentPercent = percent;
    s_counters.bestPercent = std::max(s_counters.bestPercent, percent);
    int whole = static_cast<int>(std::floor(percent));
    if (whole > s.attempt.lastPercent) {
        s.attempt.lastPercent = whole;
        int saved = pl->m_isPracticeMode ? pl->m_level->m_practicePercent : pl->m_level->m_normalPercent.value();
        Event e = makeEvent();
        e.payload = telemetry::ProgressPayload{static_cast<double>(whole), whole > saved};
        emit(std::move(e));
    }
    // periodic state samples (per player), cheap: one small event every 24 ticks
    int64_t tick = currentTick(pl);
    int64_t bucket = tick / kStateSampleEveryTicks;
    if (bucket != s.attempt.lastSampleBucket) {
        s.attempt.lastSampleBucket = bucket;
        bool dual = pl->m_gameState.m_isDualMode && pl->m_player2;
        for (int slot = 1; slot <= (dual ? 2 : 1); ++slot) {
            PlayerObject* p = slot == 1 ? pl->m_player1 : pl->m_player2;
            if (!p) continue;
            Event e = makeEvent();
            e.payload = telemetry::StateSamplePayload{snapshotOf(pl, p, slot)};
            emit(std::move(e));
        }
    }
}

void onRawButton(PlayerObject* p, bool down) {
    // Player 1 only: one handleButton press pushes BOTH players in dual mode (single control), so
    // counting every PlayerObject would show 2 pushButton per 1 handleButton and read like a bug.
    // Other mods' clones (slot 0) are not counted either.
    if (!s.pl || slotOf(s.pl, p) != 1) return;
    if (down) ++s_counters.rawPresses;
    else ++s_counters.rawReleases;
}

SessionCounters session() { return s_counters; }

void noteWindow(bool emitted, bool miss) {
    if (emitted) {
        ++s_counters.windowsEmitted;
        if (miss) ++s_counters.windowsMisses;
    }
    else ++s_counters.windowsDropped;
}

bool active() { return s.pl != nullptr; }
PlayLayer* layer() { return s.pl; }
std::string sessionLocalId() { return s.sessionLocalId; }
std::string levelHash() { return s.levelHash; }
TrustState trust() { return s.trust; }

bool lastMenus(env::MenuState& out) {
    if (!s.pl || !s.haveMenus) return false;
    out = s.lastMenus;
    return true;
}

void refreshTrust() { pollEnvironment(false); }

AttemptView attemptView() {
    AttemptView v;
    v.open = s.attempt.open;
    v.id = s.attempt.id;
    v.attemptNo = s.attempt.no;
    v.sessionAttemptCount = s.attempt.sessionCount;
    v.fromPercent = s.attempt.fromPercent;
    v.practice = s.attempt.practice;
    v.noclipSeen = s.attempt.noclipSeen;
    v.inputs = s.attempt.inputs;
    v.clampedEvents = s.attempt.clamped;
    if (s.pl) {
        v.tick = currentTick(s.pl);
        v.levelTime = currentLevelTime(s.pl);
        v.subTick = subTickNow();
    }
    return v;
}

}  // namespace gprl::tracker
