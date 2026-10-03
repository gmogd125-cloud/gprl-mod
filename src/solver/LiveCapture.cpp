// LiveStateSnapshot capture (docs/LIVE_ISOLATION_DESIGN.md §3.1, Appendix B). Every field is a
// plain read of a Geode binding member; the member names are Appendix B's "source" column.
#include "LiveCapture.hpp"

#include <algorithm>
#include <cstdint>

using namespace geode::prelude;
namespace live = gprl::solver::live;

namespace gprl::clone {

namespace {

uint8_t gamemodeOf(PlayerObject* p) {
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

void capturePlayer(live::PlayerSnap& o, PlayerObject* p) {
    o = {};
    if (!p) return;
    auto pos = p->getPosition();
    o.posX = pos.x;
    o.posY = pos.y;
    o.mposX = p->m_position.x;
    o.mposY = p->m_position.y;
    o.yVelocity = p->m_yVelocity;
    o.upsideDown = p->m_isUpsideDown;
    o.gamemode = gamemodeOf(p);
    o.vehicleSize = p->m_vehicleSize;
    o.playerSpeed = p->m_playerSpeed;
    o.gravityConst = p->m_gravity;
    o.yStart = p->m_yStart;
    auto held = p->m_holdingButtons.find(1);
    o.held = held != p->m_holdingButtons.end() && held->second;
    o.dead = p->m_isDead;
    o.onGround = p->m_isOnGround;
    o.visible = p->isVisible();
    o.secondPlayer = p->m_isSecondPlayer;
    o.lastPortalId = p->m_lastActivatedPortal ? p->m_lastActivatedPortal->m_objectID : 0;
    o.lastPortalX = p->m_lastPortalPos.x;
    o.lastPortalY = p->m_lastPortalPos.y;
    o.rings = p->m_touchingRings ? static_cast<int>(p->m_touchingRings->count()) : 0;
    o.checkpointTimeout = p->m_checkpointTimeout;
}

}  // namespace

void captureLive(PlayLayer* pl, std::vector<EnhancedGameObject*> const& act, std::vector<float> const& actX, int step, live::Snapshot& out) {
    out.captureStep = step;
    if (!pl) {
        out.p1 = {};
        out.p2 = {};
        out.camera = {};
        out.layer = {};
        out.objectCount = 0;
        out.objectsHash = live::objectsHash(out);
        return;
    }
    capturePlayer(out.p1, pl->m_player1);
    capturePlayer(out.p2, pl->m_player2);

    auto const& gs = pl->m_gameState;
    auto& c = out.camera;
    c.posX = gs.m_cameraPosition.x;
    c.posY = gs.m_cameraPosition.y;
    c.pos2X = gs.m_cameraPosition2.x;
    c.pos2Y = gs.m_cameraPosition2.y;
    c.zoom = gs.m_cameraZoom;
    c.targetZoom = gs.m_targetCameraZoom;
    c.offX = gs.m_cameraOffset.x;
    c.offY = gs.m_cameraOffset.y;
    c.angle = gs.m_cameraAngle;
    c.follow6X = gs.m_unkPoint6.x;
    c.follow6Y = gs.m_unkPoint6.y;
    c.follow8X = gs.m_unkPoint8.x;
    c.follow8Y = gs.m_unkPoint8.y;
    c.follow17X = gs.m_unkPoint17.x;
    c.follow17Y = gs.m_unkPoint17.y;
    c.follow18X = gs.m_unkPoint18.x;
    c.follow18Y = gs.m_unkPoint18.y;
    c.follow22X = gs.m_unkPoint22.x;
    c.follow22Y = gs.m_unkPoint22.y;
    c.padding = gs.m_unkFloat2;
    c.easing = gs.m_unkFloat3;
    c.bool7 = gs.m_unkBool7;
    c.freeMode = gs.m_isFreeMode;
    c.bool9 = gs.m_unkBool9;
    c.stepDiffX = gs.m_cameraStepDiff.x;
    c.stepDiffY = gs.m_cameraStepDiff.y;
    c.edge0 = gs.m_cameraEdgeValue0;
    c.edge1 = gs.m_cameraEdgeValue1;
    c.edge2 = gs.m_cameraEdgeValue2;
    c.edge3 = gs.m_cameraEdgeValue3;
    c.tweens = static_cast<uint32_t>(gs.m_tweenActions.size());
    c.flip = pl->m_cameraFlip;
    if (pl->m_objectLayer) {
        auto lp = pl->m_objectLayer->getPosition();
        c.layerX = lp.x;
        c.layerY = lp.y;
        c.layerScale = pl->m_objectLayer->getScale();
    } else {
        c.layerX = c.layerY = 0.f;
        c.layerScale = 1.f;
    }

    auto& l = out.layer;
    l.dualMode = gs.m_isDualMode;
    l.unlinkGravity = gs.m_unkBool31;
    l.dualGround = gs.m_dualRelated;
    l.lastPortal1Id = gs.m_lastActivatedPortal1 ? gs.m_lastActivatedPortal1->m_objectID : 0;
    l.lastPortal2Id = gs.m_lastActivatedPortal2 ? gs.m_lastActivatedPortal2->m_objectID : 0;
    l.lastPortal1Ptr = reinterpret_cast<uint64_t>(gs.m_lastActivatedPortal1);
    l.lastPortal2Ptr = reinterpret_cast<uint64_t>(gs.m_lastActivatedPortal2);
    l.timeMod = gs.m_timeModRelated;
    l.timeModNoEffects = gs.m_timeModRelated2;
    l.betweenSteps = pl->m_isBetweenSteps;
    l.mirror = gs.m_levelFlipping;
    l.activatedObjectIDs = static_cast<uint32_t>(gs.m_activatedObjectIDs.size());
    l.stateObjects = static_cast<uint32_t>(gs.m_stateObjects.size());
    l.checkpoints = pl->m_checkpointArray ? static_cast<uint32_t>(pl->m_checkpointArray->count()) : 0u;
    l.playerDied = pl->m_playerDied;
    l.items = pl->m_effectManager ? static_cast<uint32_t>(pl->m_effectManager->m_itemCountMap.size()) : 0u;

    // objects around player 1 (the engine's orb / pad / portal list, sorted by x)
    out.objectCount = 0;
    float const x0 = out.p1.posX;
    auto a = std::lower_bound(actX.begin(), actX.end(), x0 - live::kObjectsBehind) - actX.begin();
    auto b = std::upper_bound(actX.begin(), actX.end(), x0 + live::kObjectsAhead) - actX.begin();
    for (auto i = a; i < b && out.objectCount < live::kMaxObjects; ++i) {
        auto* o = act[static_cast<size_t>(i)];
        if (!o) continue;
        auto& s = out.objects[static_cast<size_t>(out.objectCount++)];
        s.id = o->m_objectID;
        s.uid = o->m_uniqueID;
        s.x = actX[static_cast<size_t>(i)];
        s.isActivated = o->m_isActivated ? 1 : 0;
        s.activated = o->m_activated ? 1 : 0;
        s.slot1 = o->m_activatedByPlayer1 ? 1 : 0;
        s.slot2 = o->m_activatedByPlayer2 ? 1 : 0;
        s.disabled = o->m_isDisabled ? 1 : 0;
        s.powered = 0;   // RingObject's powered state has no binding member; m_activated covers the orb path
    }
    out.objectsHash = live::objectsHash(out);
}

}  // namespace gprl::clone
