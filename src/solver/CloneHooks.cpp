// Side-effect guards for the hidden clones (docs/SOLVER_DESIGN.md §1.1 "Hooks.cpp side-effect
// guards", ported from frame-perfect-counter): a stepped clone must never touch triggers,
// collectibles, sounds, camera, streaks, checkpoints or level state. Each guard tests
// `oracle::stepping() || oracle::isClone(p)`; nothing else changes. v0.8.0: the activation gates,
// gravity flips, orb touches and the layer-writer tripwires live in IsolationHooks.cpp. The step hook, the input hooks
// and the destroyPlayer claim live in src/Hooks.cpp next to the tracker's hooks (one hook per
// target so the order between the claim and the tracker is fixed).
#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

#include "GdOracle.hpp"

using namespace geode::prelude;

namespace {
inline bool sim() { return gprl::solver::oracle::stepping(); }
inline bool cloneOrSim(PlayerObject* p) { return sim() || gprl::solver::oracle::isClone(p); }
}  // namespace

class $modify(GPRLClonePlayLayer, PlayLayer) {
    void playEndAnimationToPos(CCPoint pos) {
        if (sim()) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    void checkpointActivated(CheckpointGameObject* object) {
        if (sim()) return;
        PlayLayer::checkpointActivated(object);
    }
};

class $modify(GPRLCloneBaseLayer, GJBaseGameLayer) {
    void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
        if (cloneOrSim(player)) {
            gprl::solver::oracle::cloneInvalid(gprl::solver::oracle::isClone(player) ? player : gprl::solver::oracle::simClone(), "dual portal");
            return;
        }
        GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (cloneOrSim(player)) {
            gprl::solver::oracle::cloneInvalid(gprl::solver::oracle::isClone(player) ? player : gprl::solver::oracle::simClone(), "teleport portal");
            return;
        }
        GJBaseGameLayer::teleportPlayer(object, player);
    }

    void toggleFlipped(bool flip, bool noEffects) {
        if (sim()) return;
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void pickupItem(EffectGameObject* object) {
        if (sim()) return;
        GJBaseGameLayer::pickupItem(object);
    }

    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (sim()) return;
        GJBaseGameLayer::activateSFXTrigger(object);
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (sim()) return;
        GJBaseGameLayer::activateSongEditTrigger(object);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (sim()) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (sim()) return;
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (sim()) return;
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }
};

class $modify(GPRLClonePlayer, PlayerObject) {
    void playerDestroyed(bool noEffects) {
        if (cloneOrSim(this)) {
            // a clone's death outside PlayLayer::destroyPlayer (level boundary): claimed the same way
            gprl::solver::oracle::claimDestroy(PlayLayer::get(), this, nullptr);
            return;
        }
        PlayerObject::playerDestroyed(noEffects);
    }

    void incrementJumps() {
        if (sim()) return;
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (sim()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(GPRLCloneStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (sim()) return;
        HardStreak::addPoint(point);
    }
};

class $modify(GPRLCloneGameObject, GameObject) {
    void playShineEffect() {
        if (sim()) return;
        GameObject::playShineEffect();
    }
};

class $modify(GPRLCloneEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (sim()) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(GPRLCloneRing, RingObject) {
    void spawnCircle() {
        if (sim()) return;
        RingObject::spawnCircle();
    }
};

// Orb / pad / portal sounds a clone would trigger.
class $modify(GPRLCloneAudio, FMODAudioEngine) {
    int playEffect(gd::string path) {
        if (sim()) return 0;
        return FMODAudioEngine::playEffect(path);
    }

    int playEffect(gd::string path, float speed, float unknown, float volume) {
        if (sim()) return 0;
        return FMODAudioEngine::playEffect(path, speed, unknown, volume);
    }
};
