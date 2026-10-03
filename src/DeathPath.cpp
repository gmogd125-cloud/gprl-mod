// noclip-death-detector/2, game side. See DeathPath.hpp and docs/NOCLIP_DEATH_DETECTOR.md.
//
// GD 2.2081 facts this file relies on (disassembly, doc §2):
//   - every kill of a player is raised inside GJBaseGameLayer::checkCollisions(player, dt,
//     ignoreDamage), which only GJBaseGameLayer::update calls (once per player per step);
//   - checkCollisions returns 1 exactly when it found a hazard contact or the player out of bounds
//     for the second tick in a row - also when `ignoreDamage` made it skip the destroyPlayer call
//     (Click Between Frames calls it that way on the real player between its sub-steps, and GD's
//     "Ignore Damage" option does the same), so that return value alone is never read as a kill;
//   - PlayLayer::destroyPlayer treats a null player as player 1, returns at once while player 1 is
//     locked or m_playerDied is set, and only notes the anti-cheat spike (compared by unique id).
#include "DeathPath.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>

#include "../core/classify.hpp"
#include "Environment.hpp"
#include "Settings.hpp"
#include "Tracker.hpp"
#include "analyzer/Analyzer.hpp"
#include "solver/GdOracle.hpp"

using namespace geode::prelude;

namespace gprl::deathpath {

namespace {

namespace oracle = gprl::solver::oracle;

inline bool enabled() { return settings::get().enabled; }
inline bool debugOn() { return settings::get().enabled && settings::get().deathDebug; }

// ---- the scope "GD's own physics step is checking THIS PlayerObject" ----

struct Scope {
    PlayerObject* player = nullptr;
    bool ignoreDamage = false;
    bool sawKill = false;   // a destroyPlayer call for this player reached GPRL's hook inside this check
};
constexpr int kMaxScopes = 8;
Scope s_scopes[kMaxScopes];
int s_ccDepth = 0;        // nested checkCollisions calls (1 while the game checks one player)
int s_updateDepth = 0;    // inside the game's own GJBaseGameLayer::update
int s_resetDepth = 0;     // inside PlayLayer::resetLevel
// checkCollisions calls for a real live player inside GJBaseGameLayer::update since the game
// started: > 0 proves both scope hooks work, so "raised outside the live step" can be trusted
uint64_t s_liveChecks = 0;

// ---- "the game's own PlayLayer::destroyPlayer was reached" (innermost hook) ----

constexpr int kMaxNest = 8;
int s_destroyDepth = 0;                 // GPRL's outer destroyPlayer hook calls in progress
bool s_reached[kMaxNest + 1] = {};
bool s_deadAfterGd[kMaxNest + 1] = {};

// ---- counters for the summary ----

uint64_t s_killsHook = 0;       // kills that reached the outer hook for a real live player
uint64_t s_killsReturn = 0;     // kills only seen through checkCollisions' return value
uint64_t s_cloneKills = 0;

// ---- debug history + overlay ----

death::DebugHistory s_history{48};
Ref<CCDrawNode> s_draw;
Ref<CCLayerColor> s_panel;
Ref<CCLabelBMFont> s_text;
PlayLayer* s_overlayLayer = nullptr;
uint64_t s_drawnRevision = ~0ull;

Scope* topScope() { return s_ccDepth > 0 && s_ccDepth <= kMaxScopes ? &s_scopes[s_ccDepth - 1] : nullptr; }

death::Box boxOf(CCRect const& r) {
    death::Box b;
    b.valid = r.size.width > 0.f && r.size.height > 0.f;
    b.x = r.origin.x;
    b.y = r.origin.y;
    b.w = r.size.width;
    b.h = r.size.height;
    return b;
}

void logTail() {
    // the last entry's folded repeats stop growing now: write its final count once
    auto const& entries = s_history.entries();
    if (!entries.empty() && entries.back().repeats > 0) log::info("{} (final count)", death::format(entries.back()));
}

void store(death::DebugRecord const& rec) {
    bool hadTail = !s_history.entries().empty() && s_history.entries().back().repeats > 0;
    death::DebugRecord tail;
    if (hadTail) tail = s_history.entries().back();
    if (!s_history.add(rec)) return;
    if (hadTail) log::info("{} (final count)", death::format(tail));
    log::info("{}", death::format(rec));
}

/// The full §1 record. Only while `death-debug` is on: the hitboxes are read here, after the game
/// raised (or a caller reported) the kill, for display - never to decide anything.
void record(PlayLayer* pl, Pending const& p, death::Verdict const& verdict, bool deadAfterGd, bool cheap = false) {
    death::DebugRecord r;
    r.candidate = p.candidate;
    r.verdict = verdict;
    r.sessionGen = p.candidate.sessionGen;
    r.attemptGen = p.candidate.attemptGen;
    r.tick = p.candidate.tick;
    r.lastTick = r.tick;
    r.sessionId = tracker::sessionLocalId();
    r.attemptId = tracker::attemptView().id;
    r.playerPtr = reinterpret_cast<uintptr_t>(p.nullPlayerArg ? nullptr : p.player);
    r.nullPlayerArg = p.nullPlayerArg;
    r.deadAfterGd = deadAfterGd;
    r.collisionDepth = s_ccDepth;
    r.noclipObserved = verdict.decision == death::Decision::WouldBeDeath || verdict.decision == death::Decision::ContinuesContact;
    env::MenuState menus;
    if (tracker::lastMenus(menus)) {
        r.megahackLoaded = menus.megahackLoaded;
        r.menuNoclip = menus.noclip;
    }
    if (pl) {
        float pct = pl->getCurrentPercent();
        r.percent = std::isfinite(pct) ? std::clamp(static_cast<double>(pct), 0.0, 100.0) : 0.0;
    }
    if (p.player) {
        r.playerX = p.player->getPositionX();
        r.playerY = p.player->getPositionY();
        if (!cheap) r.playerBox = boxOf(p.player->getObjectRect());
    }
    if (p.object) {
        r.hasObject = true;
        r.objectId = p.object->m_objectID;
        r.objectUid = p.object->m_uniqueID;
        r.objectType = static_cast<int>(p.object->m_objectType);
        r.objectX = p.object->getPositionX();
        r.objectY = p.object->getPositionY();
        r.objectRotation = p.object->getRotation();
        r.objectScaleX = p.object->getScaleX();
        r.objectScaleY = p.object->getScaleY();
        r.objectRadius = p.object->m_objectRadius;
        // GD refreshed the rect before it tested it; a dirty one is not read (never recompute a GD cache here)
        if (!cheap && !p.object->m_isObjectRectDirty) r.objectBox = boxOf(p.object->getObjectRect());
    }
    store(r);
}

/// Fills everything known before the game's death path runs.
Pending prepare(PlayLayer* pl, PlayerObject* player, GameObject* object, death::Via via) {
    Pending p;
    p.nullPlayerArg = player == nullptr;
    p.player = player ? player : (pl ? pl->m_player1 : nullptr);   // PlayLayer::destroyPlayer: a null player is player 1
    p.object = object;
    auto& c = p.candidate;
    auto gen = tracker::generation();
    c.sessionGen = gen.session;
    c.attemptGen = gen.attempt;
    c.duringReset = s_resetDepth > 0;
    c.tick = pl ? classify::tickFromProgress(static_cast<int64_t>(pl->m_gameState.m_currentProgress)) : 0;
    c.origin = death::Origin::LivePlayLayer;
    c.via = via;
    c.playerSlot = liveSlot(pl, p.player);
    c.solverClone = oracle::isClone(p.player);
    c.solverStepping = oracle::stepping();
    c.liveStepKnown = s_liveChecks > 0;
    c.hasObject = object != nullptr;
    c.anticheatSpike = pl && object && pl->m_anticheatSpike && (object == pl->m_anticheatSpike || object->m_uniqueID == pl->m_anticheatSpike->m_uniqueID);
    c.wasDeadBefore = p.player && p.player->m_isDead;
    c.playerLocked = pl && pl->m_player1 && pl->m_player1->m_isLocked;
    c.layerPlayerDied = pl && pl->m_playerDied;
    return p;
}

/// Verdict -> analyzer (attempt end) -> tracker (event, attempt end) -> solver, then the debug record.
void process(PlayLayer* pl, Pending& p, bool deadAfterGd) {
    auto& c = p.candidate;
    death::Verdict v = tracker::judgeDeath(pl, c);
    if (v.accepted()) {
        bool died = v.decision == death::Decision::Death;
        // the analyzer first (it reads the tracker's still-open attempt for the noclip flag)
        if (died) analyzer::onDestroyPlayer(pl, p.player, false, true);
        tracker::applyDeath(pl, v, c, p.player, p.object);
        // the solver marks the real player's deaths and lethal contacts (every tick of a contact, as before)
        oracle::onRealDestroy(pl, p.player, p.object, false, died);
    }
    if (debugOn()) record(pl, p, v, deadAfterGd);
}

/// checkCollisions returned 1 for a real live player and no destroyPlayer call reached the outer
/// hook. Without ignoreDamage GD did call destroyPlayer: a hook that runs before GPRL's swallowed
/// it. With ignoreDamage GD raised no kill (the detector rejects it; it only shows in the debug log).
void collisionReturn(PlayLayer* pl, PlayerObject* player, bool ignoreDamage, bool wasDead) {
    ++s_killsReturn;
    Pending p = prepare(pl, player, nullptr, death::Via::CollisionReturn);
    auto& c = p.candidate;
    c.inLiveStep = true;
    c.ignoreDamage = ignoreDamage;
    c.wasDeadBefore = wasDead;
    // The one hazard whose contact is not lethal is GD's anti-cheat spike, which follows player 1
    // until its kill was noted. checkCollisions tests a hazard only when its rect overlaps the
    // player's, so a spike whose (fresh) rect does not overlap cannot be what returned 1; one that
    // does makes the return value ambiguous, and an ambiguous signal is never a would-be death.
    if (GameObject* spike = pl->m_anticheatSpike; spike && !spike->m_isObjectRectDirty && player->getObjectRect().intersectsRect(spike->getObjectRect())) {
        c.anticheatSpike = true;
    }
    c.gdDeathFired = false;
    c.deadAfter = player->m_isDead;
    process(pl, p, c.deadAfter);
}

// ---- overlay ----

constexpr ccColor4F kClear{0.f, 0.f, 0.f, 0.f};
constexpr ccColor4F kPlayerLive{0.2f, 1.f, 1.f, 1.f};       // cyan: an accepted kill of the real player
constexpr ccColor4F kPlayerRejected{0.7f, 0.7f, 0.7f, 0.9f};
constexpr ccColor4F kPlayerNoObject{1.f, 0.6f, 0.1f, 1.f};  // orange: a kill without an object (crush / bounds)
constexpr ccColor4F kHazard{1.f, 0.2f, 0.2f, 1.f};
constexpr ccColor4F kMarker{1.f, 1.f, 0.f, 1.f};
constexpr size_t kOverlayBoxes = 12;
constexpr size_t kOverlayLines = 7;

void removeNodes() {
    if (s_draw && s_draw->getParent()) s_draw->removeFromParent();
    if (s_panel && s_panel->getParent()) s_panel->removeFromParent();
    s_draw = nullptr;
    s_panel = nullptr;
    s_text = nullptr;
    s_overlayLayer = nullptr;
    s_drawnRevision = ~0ull;
}

void drawBox(death::Box const& b, ccColor4F color, float width = 0.6f) {
    if (!b.valid) return;
    CCPoint verts[4] = {{b.x, b.y}, {b.x + b.w, b.y}, {b.x + b.w, b.y + b.h}, {b.x, b.y + b.h}};
    s_draw->drawPolygon(verts, 4, kClear, width, color);
}

void drawCircle(float cx, float cy, float radius, ccColor4F color) {
    constexpr int kSides = 20;
    CCPoint verts[kSides];
    for (int i = 0; i < kSides; ++i) {
        float a = 6.2831853f * static_cast<float>(i) / kSides;
        verts[i] = CCPoint(cx + radius * std::cos(a), cy + radius * std::sin(a));
    }
    s_draw->drawPolygon(verts, kSides, kClear, 0.6f, color);
}

void redraw() {
    auto const& entries = s_history.entries();
    s_draw->clear();
    size_t firstBox = entries.size() > kOverlayBoxes ? entries.size() - kOverlayBoxes : 0;
    for (size_t i = firstBox; i < entries.size(); ++i) {
        auto const& r = entries[i];
        ccColor4F playerColor = !r.verdict.accepted() ? kPlayerRejected : r.hasObject ? kPlayerLive : kPlayerNoObject;
        drawBox(r.playerBox, playerColor);
        if (r.hasObject) {
            // GD tests a circle for objects with a radius (saws), the rect otherwise
            if (r.objectRadius > 0.f) drawCircle(r.objectX, r.objectY, r.objectRadius, kHazard);
            drawBox(r.objectBox, kHazard, r.objectRadius > 0.f ? 0.3f : 0.6f);
            float mx = 0.f, my = 0.f;
            if (death::overlapCentre(r.playerBox, r.objectBox, mx, my)) {
                CCPoint mark[4] = {{mx - 1.5f, my - 1.5f}, {mx + 1.5f, my - 1.5f}, {mx + 1.5f, my + 1.5f}, {mx - 1.5f, my + 1.5f}};
                s_draw->drawPolygon(mark, 4, kMarker, 0.f, kMarker);
            }
        }
    }
    auto dc = tracker::deathCounters();
    std::string text = fmt::format("GPRL death debug ({})  would-be P1 {} P2 {}  rejected: clone {} stale {} not-lethal {} simulated {}", death::kDetectorVersion,
                                   dc.wouldBeDeaths[1], dc.wouldBeDeaths[2], dc.rejectedClone, dc.rejectedStale, dc.rejectedNotLethal, dc.simulated);
    size_t shown = 0;
    for (size_t i = entries.size(); i > 0 && shown < kOverlayLines; --i, ++shown) {
        auto const& r = entries[i - 1];
        text += "\n" + death::overlayLabel(r);
        if (r.hasObject && r.playerBox.valid && r.objectBox.valid) text += death::overlaps(r.playerBox, r.objectBox) ? "  boxes overlap" : "  NO box overlap";
    }
    s_text->setString(text.c_str());
    auto size = s_text->getContentSize();
    s_panel->setContentSize({size.width * s_text->getScale() + 8.f, size.height * s_text->getScale() + 6.f});
}

}  // namespace

int liveSlot(PlayLayer* pl, PlayerObject* p) {
    if (!pl || !p) return 0;
    int slot = p == pl->m_player1 ? 1 : p == pl->m_player2 ? 2 : 0;
    if (!slot) return 0;
    if (pl != PlayLayer::get() || pl != tracker::layer()) return 0;   // the ACTIVE PlayLayer of the tracked level session
    if (oracle::isClone(p)) return 0;                                  // a hidden clone is never a live player
    return slot;
}

Pending begin(PlayLayer* pl, PlayerObject* player, GameObject* object) {
    Pending p = prepare(pl, player, object, death::Via::DestroyHook);
    auto& c = p.candidate;
    // GD's own physics step is checking exactly this PlayerObject right now
    if (Scope* scope = topScope()) {
        c.ignoreDamage = scope->ignoreDamage;
        if (s_updateDepth > 0 && scope->player == p.player) {
            c.inLiveStep = true;
            scope->sawKill = true;
        }
    }
    if (c.playerSlot) ++s_killsHook;
    p.token = ++s_destroyDepth;
    if (p.token <= kMaxNest) {
        s_reached[p.token] = false;
        s_deadAfterGd[p.token] = false;
    }
    return p;
}

void finish(PlayLayer* pl, Pending& p) {
    bool deadAfterGd = false;
    if (p.token > 0 && p.token <= kMaxNest) {
        p.candidate.gdDeathFired = s_reached[p.token];
        deadAfterGd = s_deadAfterGd[p.token];
    }
    if (s_destroyDepth > 0) --s_destroyDepth;
    p.candidate.deadAfter = p.player && p.player->m_isDead;
    process(pl, p, deadAfterGd);
}

void cloneKill(PlayLayer* pl, PlayerObject* player, GameObject* object) {
    ++s_cloneKills;
    Pending p = prepare(pl, player, object, death::Via::DestroyHook);
    // claimed by the solver: a clone's own pointer, or any pointer while a clone is being stepped
    if (!p.candidate.solverClone && !p.candidate.solverStepping) p.candidate.solverClone = true;
    death::Verdict v = tracker::judgeDeath(pl, p.candidate);
    if (debugOn()) record(pl, p, v, false, true);
}

void resetBegin(PlayLayer*) { ++s_resetDepth; }

void resetEnd(PlayLayer*) {
    if (s_resetDepth > 0) --s_resetDepth;
    if (debugOn()) logTail();
}

void forget() {
    if (debugOn()) logTail();
    removeNodes();
    s_history.clear();
    // the scope depths are balanced by their own hooks: never reset here (a layer can be released
    // while another one is inside its step)
}

void frame(PlayLayer* pl, float) {
    if (!debugOn() || !pl) {
        if (s_draw || s_panel) removeNodes();
        return;
    }
    if (s_overlayLayer != pl) removeNodes();
    if (!s_draw && pl->m_objectLayer) {
        s_overlayLayer = pl;
        s_draw = CCDrawNode::create();
        s_draw->setID("death-debug-overlay"_spr);
        pl->m_objectLayer->addChild(s_draw, 9998);
        CCNode* parent = pl->m_uiLayer ? static_cast<CCNode*>(pl->m_uiLayer) : static_cast<CCNode*>(pl);
        s_panel = CCLayerColor::create({0, 0, 0, 150});
        s_panel->setID("death-debug-panel"_spr);
        s_panel->ignoreAnchorPointForPosition(false);
        s_panel->setAnchorPoint({0.f, 0.f});
        s_panel->setPosition({4.f, 4.f});
        s_panel->setZOrder(1000);
        s_text = CCLabelBMFont::create("", "chatFont.fnt");
        s_text->setScale(0.42f);
        s_text->setAnchorPoint({0.f, 0.f});
        s_text->setPosition({4.f, 3.f});
        s_text->setAlignment(kCCTextAlignmentLeft);
        s_panel->addChild(s_text);
        parent->addChild(s_panel);
        s_drawnRevision = ~0ull;
    }
    if (!s_draw || s_history.revision() == s_drawnRevision) return;
    s_drawnRevision = s_history.revision();
    redraw();
}

std::string summary() {
    auto dc = tracker::deathCounters();
    return fmt::format("Death detector {}: physics-step scope {}, kills seen {} (hook) + {} (collision return), clone kills {}; this attempt: would-be P1 {} / P2 {}, same-contact {}, "
                       "rejected clone {} / stale {} / unknown player {} / not lethal {} / simulated {}",
                       death::kDetectorVersion, s_liveChecks > 0 ? "working" : "NOT SEEN YET", s_killsHook, s_killsReturn, s_cloneKills, dc.wouldBeDeaths[1],
                       dc.wouldBeDeaths[2], dc.continued, dc.rejectedClone, dc.rejectedStale, dc.rejectedUnknownPlayer, dc.rejectedNotLethal, dc.simulated);
}

}  // namespace gprl::deathpath

// ---- hooks ----

namespace {
using namespace gprl::deathpath;
}

// Innermost on PlayLayer::destroyPlayer: reached only when no other mod's hook swallowed the call,
// i.e. when the game's own function really ran (PROMPT §4 "before or after the menu suppresses it":
// the outer hook in src/Hooks.cpp sees every call, this one sees what is left).
class $modify(GPRLDeathInner, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::Last);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        int token = s_destroyDepth;   // the outer call this belongs to; 0 = GPRL off / not ours
        if (token > 0 && token <= kMaxNest) s_reached[token] = true;
        PlayLayer::destroyPlayer(player, object);
        if (token > 0 && token <= kMaxNest) {
            PlayerObject* target = player ? player : m_player1;
            s_deadAfterGd[token] = target && target->m_isDead;
        }
    }
};

class $modify(GPRLDeathScope, GJBaseGameLayer) {
    static void onModify(auto& self) {
        // innermost, so the scope covers the game's own step loop and nothing another mod's update
        // hook does around it (a trajectory / replay simulation on the real player runs outside)
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::Last);
    }

    void update(float dt) {
        ++s_updateDepth;
        GJBaseGameLayer::update(dt);
        if (s_updateDepth > 0) --s_updateDepth;
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        int depth = s_ccDepth++;
        if (depth < kMaxScopes) s_scopes[depth] = Scope{player, ignoreDamage, false};
        PlayLayer* pl = nullptr;
        bool live = false;
        bool wasDead = false;
        if (s_updateDepth > 0 && player && enabled()) {
            pl = PlayLayer::get();
            if (pl && static_cast<GJBaseGameLayer*>(pl) == this && liveSlot(pl, player) != 0 && !gprl::solver::oracle::stepping()) {
                live = true;
                wasDead = player->m_isDead;
                ++s_liveChecks;
            }
        }
        int result = GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
        if (s_ccDepth > 0) --s_ccDepth;
        bool sawKill = depth < kMaxScopes && s_scopes[depth].sawKill;
        // 1 = a hazard contact / out of bounds. Without a destroyPlayer call that reached GPRL's hook,
        // a hook in front of it swallowed the kill (hook order); with ignoreDamage GD raised none (a
        // sub-step probe by Click Between Frames, GD's own Ignore Damage) and the detector rejects it
        if (result == 1 && live && !sawKill) collisionReturn(pl, player, ignoreDamage, wasDead);
        return result;
    }
};
