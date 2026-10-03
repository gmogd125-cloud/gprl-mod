// Game hooks (SPEC §48.5): attempts, deaths, progress, gamemode changes, inputs with sub-tick
// timestamps, practice / StartPos / checkpoints, the timing-window solver's step loop (v0.4.0,
// docs/SOLVER_DESIGN.md §3), plus the pause-menu button and the HUD. Every hook target was checked
// in D:\GeodeMods\_research\GeometryDash.bro (2.2081 win). The hooks only observe the real
// players; the solver's hidden clones are stepped inside processCommands by CloneEngine and their
// side effects are guarded in src/solver/CloneHooks.cpp.
//
// Solver integration points (one hook per target so the order is fixed):
//   GJBaseGameLayer::processCommands  pre  -> oracle::stepBegin  (timeline step, shadow, clone jobs)
//   PlayerObject::update              post -> tracker sub-tick clock + oracle::playerSubUpdate
//   PlayerObject::pushButton/release  pre  -> oracle::onPushButton (job + pre-input snapshot), then
//                                             the original, then the tracker's diagnostic counter
//   GJBaseGameLayer::handleButton     post -> tracker::onButton -> client::push -> oracle::bindLastInput
//   PlayLayer::destroyPlayer          pre  -> oracle::claimDestroy (clone deaths, anti-cheat spike),
//                                     then deathpath::begin / the game's death path / deathpath::finish:
//                                     noclip-death-detector/2 (src/DeathPath, core/death_detector) is the
//                                     one judge of deaths and would-be deaths and feeds the analyzer, the
//                                     tracker and oracle::onRealDestroy with what it accepted
//
// v0.12.0 background level analyzer (src/analyzer/Analyzer.hpp, docs/BACKGROUND_ANALYZER_DESIGN.md):
// READ-ONLY at every point below - it reads player 1 and the level's objects, never writes a GD
// field, never steps anything (GJBaseGameLayer::update is not hooked for it; v0.14.9 src/DeathPath
// hooks update / checkCollisions only to count a scope depth). Level enter / reset /
// death / complete / quit drive its attempts (resetLevel: the attempt ends before the tracker's
// reset and the next one starts after it); processCommands (pre) and PlayerObject::update (post)
// its tick clock (a real processCommands also clears its pause flag); pushButton / releaseButton
// (pre) its inputs; postUpdate its extraction slices and the frame-pressure sampler; pauseGame /
// resume its Record-Safe facts. Nothing it does can throw into these hooks (Analyzer.cpp guarded).
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>

#include "DeathPath.hpp"
#include "Hud.hpp"
#include "ui/Menu.hpp"
#include "Settings.hpp"
#include "Tracker.hpp"
#include "analyzer/Analyzer.hpp"
#include "analyzer/Status.hpp"
#include "solver/GdOracle.hpp"

using namespace geode::prelude;

namespace {

inline bool enabled() { return gprl::settings::get().enabled; }
namespace oracle = gprl::solver::oracle;
namespace analyzer = gprl::analyzer;

}  // namespace

class $modify(GPRLPlayLayer, PlayLayer) {
    struct Fields {
        PlayLayer* self = nullptr;
        ~Fields() {
            // Safety net: the level left without onQuit (editor test end, scene swap) - close the
            // attempt and the session without touching the dying layer.
            if (self) {
                analyzer::forgetLayer(self);
                oracle::forgetLayer(self);
                gprl::tracker::onQuit(self, false);
            }
            gprl::deathpath::forget();
            gprl::hud::forget();
        }
    };

    static void onModify(auto& self) {
        // Outermost so a noclip menu's early return cannot hide the call: after the original returns
        // we see whether the player actually died (would-be death otherwise, SPEC §19), and a
        // clone's death is claimed before any other mod sees it (SOLVER_DESIGN §4.5).
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_fields->self = this;
        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (!enabled()) return;
        gprl::tracker::onLevelEnter(this);
        oracle::setup(this);
        gprl::hud::attach(this);
        analyzer::onLevelEnter(this);
    }

    void resetLevel() {
        // a kill raised while the level resets belongs to no attempt (noclip-death-detector/2)
        gprl::deathpath::resetBegin(this);
        PlayLayer::resetLevel();
        gprl::deathpath::resetEnd(this);
        if (!enabled()) return;
        // the analyzer's attempt end first: the ending attempt's noclip flag is still the tracker's
        // open attempt; its next attempt starts after the tracker's (which reads the menus once for both)
        analyzer::onLevelReset(this);
        oracle::reset(this);
        gprl::tracker::onLevelReset(this);
        analyzer::onAttemptStarted(this);
    }

    void togglePracticeMode(bool practice) {
        PlayLayer::togglePracticeMode(practice);
        if (enabled()) gprl::tracker::onPracticeToggle(this, practice);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!enabled()) return PlayLayer::destroyPlayer(player, object);
        // a hidden clone's death (or its touch of GD's anti-cheat spike) never reaches the game - also
        // when the solver is not active for this layer: a clone must never run the level's death
        if (oracle::claimDestroy(this, player, object) || oracle::isClone(player)) {
            gprl::deathpath::cloneKill(this, player, object);
            return;
        }
        // noclip-death-detector/2: the facts before the game's death path, the path itself (every other
        // mod's hook, then the game), then the verdict - a death, one would-be death per continuous
        // lethal contact of the REAL player, or a rejection with its reason
        auto pending = gprl::deathpath::begin(this, player, object);
        PlayLayer::destroyPlayer(player, object);
        gprl::deathpath::finish(this, pending);
    }

    void levelComplete() {
        if (oracle::stepping()) return;   // a clone never completes the level
        PlayLayer::levelComplete();
        if (!enabled()) return;
        analyzer::onLevelComplete(this);
        oracle::onLevelComplete(this);
        gprl::tracker::onLevelComplete(this);
    }

    void onQuit() {
        if (enabled()) {
            analyzer::onQuit(this);
            oracle::teardown();
            gprl::tracker::onQuit(this, true);
        }
        gprl::deathpath::forget();
        gprl::hud::detach();
        m_fields->self = nullptr;
        PlayLayer::onQuit();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!enabled()) return;
        oracle::frameEnd(this, dt);
        gprl::tracker::onFrame(this, dt);
        // after the solver's frame work: one read-only extraction slice + the frame-time sample
        analyzer::onFrame(this);
        gprl::hud::tick(dt);
        gprl::deathpath::frame(this, dt);   // the death-debug overlay (nothing unless the setting is on)
    }

    // v0.12.0: the pause menu is a time the Record-Safe simulator may run (docs/BACKGROUND_ANALYZER_DESIGN.md §5).
    // Any resume path these hooks miss is caught by analyzer::onStepBegin: a real physics step
    // clears the pause flag (review fix 2026-10-02)
    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
        if (enabled()) analyzer::onPause(this);
    }
    void resume() {
        PlayLayer::resume();
        if (enabled()) analyzer::onResume(this);
    }
    void resumeAndRestart(bool fromStart) {
        PlayLayer::resumeAndRestart(fromStart);
        if (enabled()) analyzer::onResume(this);
    }
};

class $modify(GPRLBaseLayer, GJBaseGameLayer) {
    // One step of the timeline (1 or 0.5 frames): snapshot player 1 BEFORE the step's inputs are
    // processed, step the shadow and the clone jobs through the previous step (SOLVER_DESIGN §3.1).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (enabled()) {
            // the analyzer's passive recorder reads player 1's end state of the tick that just
            // finished (read-only), before the solver's analysis block of this step
            analyzer::onStepBegin(this);
            oracle::stepBegin(this, dt, isHalfTick);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (enabled()) gprl::tracker::onButton(this, down, button, isPlayer1);
    }

    // v0.8.0 isolation: a clone never runs a trigger (portals never pass through here in 2.2081,
    // docs/LIVE_ISOLATION_DESIGN.md §1.4; touch triggers are filtered out of the clone's object
    // list anyway). The real player's branch is unchanged.
    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (enabled() && (oracle::isClone(player) || oracle::stepping())) return;
        if (enabled() && object) {
            gprl::tracker::onPortal(this, player, object->m_objectID);
            oracle::onRealPortal(player, object->m_objectID);
        }
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }
};

class $modify(GPRLPlayer, PlayerObject) {
    static void onModify(auto& self) {
        // Run inside the Click Between Frames split so every sub-step delta is seen (sub-tick timing).
        (void)self.setHookPriorityAfterPre("PlayerObject::update", "syzzi.click_between_frames");
    }

    void update(float dt) {
        PlayerObject::update(dt);
        if (!enabled()) return;
        gprl::tracker::onPlayerUpdate(this, dt);
        oracle::playerSubUpdate(this, dt);
        analyzer::onPlayerUpdate(this, dt);
    }

    // Every input path (keyboard, Click Between Frames, bots calling handleButton or the player
    // directly, auto-clickers) ends here: the solver logs the input and opens its measurement
    // BEFORE the button state changes (pre-input snapshot); the telemetry input event itself comes
    // from handleButton above, and the tracker's counter is the Session tab's hook check.
    bool pushButton(PlayerButton button) {
        if (enabled()) {
            analyzer::onInput(this, static_cast<int>(button), true);
            oracle::onPushButton(this, static_cast<int>(button), true);
        }
        bool r = PlayerObject::pushButton(button);
        if (enabled()) gprl::tracker::onRawButton(this, true);
        return r;
    }
    bool releaseButton(PlayerButton button) {
        if (enabled()) {
            analyzer::onInput(this, static_cast<int>(button), false);
            oracle::onPushButton(this, static_cast<int>(button), false);
        }
        bool r = PlayerObject::releaseButton(button);
        if (enabled()) gprl::tracker::onRawButton(this, false);
        return r;
    }

    void toggleFlyMode(bool enable, bool noEffects) {
        PlayerObject::toggleFlyMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
    void toggleBirdMode(bool enable, bool noEffects) {
        PlayerObject::toggleBirdMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
    void toggleRollMode(bool enable, bool noEffects) {
        PlayerObject::toggleRollMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
    void toggleDartMode(bool enable, bool noEffects) {
        PlayerObject::toggleDartMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
    void toggleRobotMode(bool enable, bool noEffects) {
        PlayerObject::toggleRobotMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
    void toggleSpiderMode(bool enable, bool noEffects) {
        PlayerObject::toggleSpiderMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
    void toggleSwingMode(bool enable, bool noEffects) {
        PlayerObject::toggleSwingMode(enable, noEffects);
        if (enabled()) gprl::tracker::onGamemodeToggle(this);
    }
};

namespace {
/// v0.14.8 (owner 2026-10-03: the website's icon "the main icon for everything"): the GPRL
/// button is the brand icon itself (gprl_icon.png, the website favicon rendered by
/// branding/render_icon.py), `width` points wide whatever the file's pixel size; the old cyan
/// "GPRL" circle only when the sprite is missing.
CCNode* gprlButtonSprite(float width, CircleBaseSize fallbackSize, float fallbackLabelScale) {
    if (auto* icon = CCSprite::create("gprl_icon.png"_spr)) {
        float w = icon->getContentSize().width;
        if (w > 0.f) icon->setScale(width / w);
        return icon;
    }
    auto* label = CCLabelBMFont::create("GPRL", "bigFont.fnt");
    label->setScale(fallbackLabelScale);
    return CircleButtonSprite::create(label, CircleBaseColor::Cyan, fallbackSize);
}
}  // namespace

// Main menu: the GPRL button in the bottom-left menu (node id "bottom-menu", set by Geode's own
// MenuLayer hook) opens the same popup as the pause menu one: the brand icon, 46 points wide.
class $modify(GPRLMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto spr = gprlButtonSprite(46.f, CircleBaseSize::MediumAlt, 0.45f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(GPRLMenuLayer::onGprl));
        btn->setID("gprl-button"_spr);
        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        }
        else {
            auto own = CCMenu::create();
            own->setPosition({30.f, 30.f});
            own->addChild(btn);
            this->addChild(own, 10);
        }
        // v0.12.0: the analysis-mode popup, once (docs/BACKGROUND_ANALYZER_DESIGN.md §5), shown only
        // while THIS menu is the running scene's layer (Status.cpp)
        if (enabled()) gprl::analyzer::ui::maybeShowModePopup(this);
        return true;
    }

    void onGprl(CCObject*) { gprl::ui::open(); }
};

class $modify(GPRLPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!enabled()) return;
        // v0.14.8: the brand icon here too (it was a "GPRL" label on a cyan circle)
        auto spr = gprlButtonSprite(34.f, CircleBaseSize::Small, 0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(GPRLPauseLayer::onGprl));
        btn->setID("gprl-button"_spr);
        if (auto menu = this->getChildByID("left-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        }
        else {
            auto own = CCMenu::create();
            own->setPosition({30.f, 130.f});
            own->addChild(btn);
            this->addChild(own, 10);
        }
    }

    void onGprl(CCObject*) { gprl::ui::open(); }

    // Editor test end: the play layer is left without onQuit.
    void goEdit() {
        if (enabled()) {
            if (auto* pl = PlayLayer::get()) analyzer::onQuit(pl);
            oracle::teardown();
        }
        PauseLayer::goEdit();
    }
};
