// Live-state isolation hooks (v0.8.0; docs/LIVE_ISOLATION_DESIGN.md §2.3, owner spec
// docs/LIVE_ISOLATION_SPEC.md). While the engine steps a hidden clone, GD's own collision code runs
// for it. These hooks make every path that would WRITE live state answer from the clone's own
// overlay, run a clone-local body, or trip and invalidate the clone. Nothing is restored, because
// nothing live is changed. The real players always get the original.
//
// Verified against the GD 2.2081 win disassembly (D:\claude-scratch\gprl-auditB\*.asm and
// gd-2.2081-analysis\checkCollisions.asm):
//  - canBeActivatedByPlayer 0x2178c0: multi-activate gate on m_activatedObjectIDs, else record +
//    !hasBeenActivatedByPlayer  -> activation::canActivate on the overlay (H2)
//  - hasBeenActivatedByPlayer 0x1a4af0 / activatedByPlayer 0x1a4a90 -> overlay (H3 / H4)
//  - GJBaseGameLayer::flipGravity 0x212b00: flips the player, then in LINKED dual flips "the other
//    player" chosen by unique id - a clone's id is never player 1's, so it flipped the REAL player 1
//    (the dual + gravity bug) -> clone-local flip only (H5)
//  - playerTouchedRing 0x217e40: inserts the ring into m_gameState.m_stateObjects, fires
//    GJGameEvent::OrbTouched, powers the live ring -> clone-local transcription (H6)
//  - checkCollisions passes the layer's m_isBetweenSteps (+0x3798) to postCollision; the clone
//    replays a recorded step whose half-tick flag may differ -> substituted (H7), the layer flag is
//    no longer written by the engine
//  - gamemode portals call playerWillSwitchMode / updateDualGround (camera Y-follow reset, dual
//    ground, m_lastActivatedPortal1 - the camera drift) -> never run for a clone (H2 BLOCK;
//    tripwires in H9 catch any other route). v0.8.1 (step 2): the PLAYER part of the branch is
//    replayed on the clone from core/solver/portal_model.hpp (H2-MODEL below)
#include <Geode/Geode.hpp>
#include <Geode/modify/CheckpointGameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

#include <cstddef>
#include <vector>

#include "../../core/solver/activation.hpp"
#include "../../core/solver/dual_rules.hpp"
#include "../../core/solver/isolation.hpp"
#include "../../core/solver/portal_model.hpp"
#include "GdOracle.hpp"

using namespace geode::prelude;
namespace oracle = gprl::solver::oracle;
namespace activation = gprl::solver::activation;
namespace isolation = gprl::solver::isolation;
namespace portal = gprl::solver::portal;
namespace dual = gprl::solver::dual;

namespace {

inline bool sim() { return oracle::stepping(); }
inline bool cloneOrSim(PlayerObject* p) { return sim() || oracle::isClone(p); }
inline PlayerObject* current(PlayerObject* p) { return oracle::isClone(p) ? p : oracle::simClone(); }

// The offsets the transcriptions rely on, checked against Geode's layout of GD 2.2081 at compile
// time (a wrong offset would mean the asm above was read against another build).
static_assert(offsetof(GJBaseGameLayer, m_isBetweenSteps) == 0x3798, "GJBaseGameLayer::m_isBetweenSteps moved");
static_assert(offsetof(PlayerObject, m_touchingRings) == 0xa38, "PlayerObject::m_touchingRings moved");
static_assert(offsetof(PlayerObject, m_touchedRings) == 0xa40, "PlayerObject::m_touchedRings moved");
static_assert(offsetof(PlayerObject, m_isUpsideDown) == 0x9bf, "PlayerObject::m_isUpsideDown moved");
static_assert(offsetof(PlayerObject, m_isPlatformer) == 0xb70, "PlayerObject::m_isPlatformer moved");
// the gamemode-portal model (portal_model.hpp header comment): the fields the branches write on
// the player, the flags the layer part reads, and the pointer the toggles must not follow
static_assert(offsetof(PlayerObject, m_lastPortalPos) == 0xa00, "PlayerObject::m_lastPortalPos moved");
static_assert(offsetof(PlayerObject, m_lastActivatedPortal) == 0xa80, "PlayerObject::m_lastActivatedPortal moved");
static_assert(offsetof(PlayerObject, m_pendingCheckpoint) == 0x890, "PlayerObject::m_pendingCheckpoint moved");
static_assert(offsetof(PlayerObject, m_gameLayer) == 0xc20, "PlayerObject::m_gameLayer moved");
static_assert(offsetof(GameObject, m_hasNoEffects) == 0x41c, "GameObject::m_hasNoEffects moved");
static_assert(offsetof(GJBaseGameLayer, m_gameState) + offsetof(GJGameState, m_isDualMode) == 0x422, "GJGameState::m_isDualMode moved");
static_assert(offsetof(GJBaseGameLayer, m_gameState) + offsetof(GJGameState, m_lastActivatedPortal2) == 0x408, "GJGameState::m_lastActivatedPortal2 moved");

// playerTouchedRing's auto-jump test (0x217ef7-0x217f22): not ship, not ufo, not wave, not swing,
// and the ring flag at +0x740 clear. Whichever RingObject bool Geode places at +0x740 is the one GD
// reads there.
template <class T, class = void>
struct RingFlag740 {
    static bool get(RingObject* r) { return r->m_claimTouch; }
};
constexpr bool kRing740IsSpawnOnly = offsetof(RingObject, m_isSpawnOnly) == 0x740;
constexpr bool kRing740IsClaimTouch = offsetof(RingObject, m_claimTouch) == 0x740;
static_assert(kRing740IsSpawnOnly || kRing740IsClaimTouch, "RingObject flag at +0x740 not found");
inline bool ringFlag740(RingObject* r) { return kRing740IsSpawnOnly ? r->m_isSpawnOnly : r->m_claimTouch; }

// H1: the clone's object list without the FILTER types (collectibles, touch triggers) and without
// anything outside the census (an overlapping unknown type invalidates the clone).
gd::vector<GameObject*> g_filtered;

void trip(char const* fn) {
    oracle::cloneInvalid(oracle::simClone(), fn);
    oracle::noteTripwire(fn);
}

// H2-MODEL (v0.8.1, step 2): the player part of GD's gamemode-portal branch, replayed on the clone
// from the plan in core/solver/portal_model.hpp. GD's `noEffects` (the portal's m_hasNoEffects, or
// portal 2's in dual) gates only spawnPortalCircle / a CCCircleWave added to the live parent layer
// (asm addresses in the header of portal_model.hpp); a clone passes true so no live node is made.
constexpr bool kCloneNoEffects = true;

dual::ModeFlags modeFlags(PlayerObject* p) {
    dual::ModeFlags m;
    m.ship = p->m_isShip;
    m.bird = p->m_isBird;
    m.ball = p->m_isBall;
    m.dart = p->m_isDart;
    m.robot = p->m_isRobot;
    m.spider = p->m_isSpider;
    m.swing = p->m_isSwing;
    return m;
}
portal::ModeFlags portalFlags(PlayerObject* p) {
    portal::ModeFlags m;
    m.ship = p->m_isShip;
    m.bird = p->m_isBird;
    m.ball = p->m_isBall;
    m.dart = p->m_isDart;
    m.robot = p->m_isRobot;
    m.spider = p->m_isSpider;
    m.swing = p->m_isSwing;
    return m;
}
/// The dual facts of the PAIR (the live layer / level settings are read, never written): the pair's
/// player 1 is the clone that is not m_isSecondPlayer (GD compares m_player1 against m_player2).
dual::DualFacts pairFacts(PlayerObject* clone, PlayerObject* partner) {
    dual::DualFacts f;
    auto* layer = clone->m_gameLayer;
    f.dualMode = layer && layer->m_gameState.m_isDualMode;
    f.unlink = layer && layer->m_gameState.m_unkBool31;
    f.twoPlayerMode = layer && layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    PlayerObject* a = clone->m_isSecondPlayer ? partner : clone;
    PlayerObject* b = clone->m_isSecondPlayer ? clone : partner;
    f.p1 = modeFlags(a);
    f.p2 = modeFlags(b);
    return f;
}

void modelGamemodePortal(PlayerObject* clone, EffectGameObject* object, bool multi) {
    auto* layer = clone->m_gameLayer;   // read only
    if (layer && layer->m_gameState.m_isDualMode) {
        // v0.8.3: the dual branch of playerWillSwitchMode (0x213001-0x213117) on the PAIR - the
        // switching clone takes the gravity opposite to its PARTNER's when the partner already has
        // the portal's mode (portal::dualGravity); a real player is never read as the partner
        auto* partner = oracle::simPartner();
        if (!partner) { trip("gamemode portal in dual without a pair partner"); return; }
        bool const twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
        auto dec = portal::dualGravity(true, twoPlayer, layer->m_gameState.m_unkBool31, static_cast<int>(object->m_objectType), portalFlags(partner), partner->m_isUpsideDown);
        if (dec.flip) clone->flipGravity(dec.upsideDown, true);
    }
    if (clone->m_pendingCheckpoint) {
        // removePendingCheckpoint inside the fly / bird / dart / swing toggles would release it
        trip("clone holds a pending checkpoint");
        return;
    }
    int type = static_cast<int>(object->m_objectType);
    auto plan = portal::planFor(type, kCloneNoEffects);
    if (plan.count == 0) { trip("gamemode portal without a plan"); return; }
    for (int i = 0; i < plan.count; ++i) {
        auto const& s = plan.steps[i];
        switch (s.op) {
            case portal::Op::SwitchedToMode: clone->switchedToMode(object->m_objectType); break;
            case portal::Op::ToggleFly: clone->toggleFlyMode(s.enable, s.noEffects); break;
            case portal::Op::ToggleBird: clone->toggleBirdMode(s.enable, s.noEffects); break;
            case portal::Op::ToggleRoll: clone->toggleRollMode(s.enable, s.noEffects); break;
            case portal::Op::ToggleDart: clone->toggleDartMode(s.enable, s.noEffects); break;
            case portal::Op::ToggleRobot: clone->toggleRobotMode(s.enable, s.noEffects); break;
            case portal::Op::ToggleSpider: clone->toggleSpiderMode(s.enable, s.noEffects); break;
            case portal::Op::ToggleSwing: clone->toggleSwingMode(s.enable, s.noEffects); break;
            case portal::Op::LastPortal:
                clone->m_lastPortalPos = object->getPosition();   // the two CCPoint copies at +0xa00
                clone->m_lastActivatedPortal = object;            // +0xa80
                break;
            case portal::Op::UpdatePlayerArt: clone->updatePlayerArt(); break;
            case portal::Op::UpdateDashArt: clone->updateDashArt(); break;
        }
    }
    // the common tail 0x214f45: playShineEffect (CloneHooks: no-op while stepping) and
    // portal->activatedByPlayer(player) -> the trial's overlay
    oracle::overlaySet(object, activation::activated(oracle::overlayGet(object), oracle::simSelfBit(), multi));
    oracle::notePortalModelled(type);
}

}  // namespace

class $modify(GPRLIsoBaseLayer, GJBaseGameLayer) {
    // H1 ---------------------------------------------------------------------------------------
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!cloneOrSim(player) || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        g_filtered.clear();
        g_filtered.reserve(static_cast<size_t>(objectCount));
        PlayerObject* clone = current(player);
        auto cloneRect = clone ? clone->getObjectRect() : CCRect{};
        int n = std::min<int>(objectCount, static_cast<int>(objects->size()));
        for (int i = 0; i < n; ++i) {
            GameObject* o = (*objects)[static_cast<size_t>(i)];
            if (!o) continue;
            switch (isolation::treatmentOf(static_cast<int>(o->m_objectType))) {
                case isolation::Treatment::Filter: continue;
                case isolation::Treatment::FilterInvalid:
                    if (clone && o->getObjectRect().intersectsRect(cloneRect)) trip("unisolated_object_type");
                    continue;
                default: g_filtered.push_back(o);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &g_filtered, static_cast<int>(g_filtered.size()), dt);
    }

    // H2 ---------------------------------------------------------------------------------------
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (!cloneOrSim(player) || !object) return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
        PlayerObject* clone = current(player);
        bool multi = object->canMultiActivate(clone ? clone->m_isPlatformer : false);
        auto d = activation::canActivate(oracle::overlayGet(object), oracle::simSelfBit(), multi);
        oracle::overlaySet(object, d.logical);
        if (!d.result) return false;
        switch (isolation::treatmentOf(static_cast<int>(object->m_objectType))) {
            case isolation::Treatment::Pass:
            case isolation::Treatment::Replace:
                return true;
            case isolation::Treatment::BlockModel:
                // gamemode portal: GD's branch writes the camera / dual ground inline, so it never
                // runs for a clone (false). The player part runs here instead (H2-MODEL).
                if constexpr (isolation::kModelGamemodePortals) {
                    if (clone) modelGamemodePortal(clone, object, multi);
                    else trip("gamemode portal without a clone");
                } else {
                    trip("gamemode portal (not modelled yet)");
                }
                return false;
            case isolation::Treatment::BlockIgnore:
                return false;
            case isolation::Treatment::BlockInvalid:
                trip(object->m_objectType == GameObjectType::TeleportPortal ? "teleport portal" : "dual portal");
                return false;
            default:
                trip("unisolated_object_type");
                return false;
        }
    }

    // H5 ---------------------------------------------------------------------------------------
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (!cloneOrSim(player)) {
            GJBaseGameLayer::flipGravity(player, flip, noEffects);
            return;
        }
        PlayerObject* clone = current(player);
        if (!clone) return;
        // 0x212b22: no-op when the player already has that gravity; then the player's own flip.
        // The linked-dual mirror onto "the other player" (0x212b3f-0x212bf8) is what flipped the
        // REAL player 1 for a clone: never run for a clone (dual pairs: step 4).
        if (clone->m_isUpsideDown == flip) return;
        clone->flipGravity(flip, true);
        // v0.8.3 (step 4): the linked-dual mirror (0x212b3f-0x212bf8), applied to the PAIR PARTNER - the
        // P2 (or P1) clone of the same trial - never to a real player (dual_rules.hpp gravityLinkTarget)
        if (auto* partner = oracle::simPartner()) {
            if (dual::gravityLinkTarget(pairFacts(clone, partner)) == dual::LinkTarget::Partner) partner->flipGravity(!flip, true);
        }
    }

    // H6 ---------------------------------------------------------------------------------------
    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (!cloneOrSim(player) || !ring) {
            GJBaseGameLayer::playerTouchedRing(player, ring);
            return;
        }
        PlayerObject* clone = current(player);
        if (!clone) return;
        // Transcription of 0x217e40 minus the live writes: no m_stateObjects insert, no OrbTouched
        // event, no powerOnObject on the live ring (its powered state lives in the overlay).
        uint8_t logical = oracle::overlayGet(ring);
        oracle::overlaySet(ring, static_cast<uint8_t>(logical | activation::kRingPowered));
        if (clone->m_touchingRings && !clone->m_touchingRings->containsObject(ring)) clone->m_touchingRings->addObject(ring);
        clone->m_touchedRings.insert(ring->m_uniqueID);
        if (!clone->m_isShip && !clone->m_isBird && !clone->m_isDart && !clone->m_isSwing && !ringFlag740(ring)) {
            clone->ringJump(ring, false);
        }
    }

    // H9 tripwires: layer / camera / dual / checkpoint writers a clone step must never reach ------
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (cloneOrSim(player)) { trip("unisolated_path: playerWillSwitchMode"); return; }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
    void updateDualGround(PlayerObject* player, int mode, bool instant, float duration) {
        if (cloneOrSim(player)) { trip("unisolated_path: updateDualGround"); return; }
        GJBaseGameLayer::updateDualGround(player, mode, instant, duration);
    }
    void animateInDualGroundNew(GameObject* object, float height, bool instant, float duration) {
        if (sim()) { trip("unisolated_path: animateInDualGroundNew"); return; }
        GJBaseGameLayer::animateInDualGroundNew(object, height, instant, duration);
    }
    void updateStaticCameraPos(CCPoint pos, bool staticX, bool staticY, bool followOrSmoothEase, float time, int easingType, float easingRate) {
        if (sim()) { trip("unisolated_path: updateStaticCameraPos"); return; }
        GJBaseGameLayer::updateStaticCameraPos(pos, staticX, staticY, followOrSmoothEase, time, easingType, easingRate);
    }
    void destroyObject(GameObject* object) {
        if (sim()) { trip("unisolated_path: destroyObject"); return; }
        GJBaseGameLayer::destroyObject(object);
    }
    void lightningFlash(CCPoint from, CCPoint to, ccColor3B color, float lineWidth, float duration, int displacement, bool flash, float opacity) {
        if (sim()) return;   // H8: visual only (size portals)
        GJBaseGameLayer::lightningFlash(from, to, color, lineWidth, duration, displacement, flash, opacity);
    }
};

class $modify(GPRLIsoEnhancedObject, EnhancedGameObject) {
    // H3 / H4
    bool hasBeenActivatedByPlayer(PlayerObject* player) {
        if (!cloneOrSim(player)) return EnhancedGameObject::hasBeenActivatedByPlayer(player);
        PlayerObject* clone = current(player);
        bool multi = this->canMultiActivate(clone ? clone->m_isPlatformer : false);
        return activation::hasBeen(oracle::overlayGet(this), oracle::simSelfBit(), multi);
    }
    void activatedByPlayer(PlayerObject* player) {
        if (!cloneOrSim(player)) { EnhancedGameObject::activatedByPlayer(player); return; }
        PlayerObject* clone = current(player);
        bool multi = this->canMultiActivate(clone ? clone->m_isPlatformer : false);
        oracle::overlaySet(this, activation::activated(oracle::overlayGet(this), oracle::simSelfBit(), multi));
    }
    // H9: both slots of the live object
    void triggerActivated(float xPosition) {
        if (sim()) { trip("unisolated_path: triggerActivated"); return; }
        EnhancedGameObject::triggerActivated(xPosition);
    }
    // H8: visual / power state (the overlay keeps kRingPowered)
    void powerOnObject(int state) {
        if (sim()) return;
        EnhancedGameObject::powerOnObject(state);
    }
};

class $modify(GPRLIsoEffectObject, EffectGameObject) {
    void triggerActivated(float xPosition) {
        if (sim()) { trip("unisolated_path: triggerActivated"); return; }
        EffectGameObject::triggerActivated(xPosition);
    }
};

class $modify(GPRLIsoCheckpointObject, CheckpointGameObject) {
    void triggerActivated(float xPosition) {
        if (sim()) { trip("unisolated_path: triggerActivated"); return; }
        CheckpointGameObject::triggerActivated(xPosition);
    }
};

class $modify(GPRLIsoRing, RingObject) {
    void powerOnObject(int state) {
        if (sim()) return;
        RingObject::powerOnObject(state);
    }
};

class $modify(GPRLIsoEffectManager, GJEffectManager) {
    void storeTriggeredID(int objectUniqueID, int playerUniqueID) {
        if (sim()) { trip("unisolated_path: storeTriggeredID"); return; }
        GJEffectManager::storeTriggeredID(objectUniqueID, playerUniqueID);
    }
};

class $modify(GPRLIsoPlayLayer, PlayLayer) {
    void removeCheckpoint(bool first) {
        if (sim()) { trip("unisolated_path: removeCheckpoint"); return; }
        PlayLayer::removeCheckpoint(first);
    }
    void playGravityEffect(bool flip) {
        if (sim()) return;   // H8
        PlayLayer::playGravityEffect(flip);
    }
    void toggleGlitter(bool visible) {
        if (sim()) return;   // H8
        PlayLayer::toggleGlitter(visible);
    }
};

class $modify(GPRLIsoPlayer, PlayerObject) {
    // H7: the recorded step's half-tick flag instead of the layer's live m_isBetweenSteps
    void postCollision(float dt, bool betweenSteps) {
        if (cloneOrSim(this)) betweenSteps = oracle::simHalfTick();
        PlayerObject::postCollision(dt, betweenSteps);
    }
};
