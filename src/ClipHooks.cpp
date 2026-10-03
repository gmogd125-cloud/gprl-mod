// Hooks of the clipping buffer (v0.6.0, stream M3; docs/CLIPPING.md). Kept apart from Hooks.cpp:
// these never touch gameplay state, they only hand the frame / the audio engine to the clipper
// and offer a pending clip when the end screen or the pause menu is up. Every target was checked
// in D:\GeodeMods\_research\GeometryDash.bro (2.2081 win) and is the same set the In-Game Clipper
// hooks (CCEGLView::swapBuffers, FMODAudioEngine::update, PlayLayer::showEndLayer).
//
// While "Clipping" is off every hook returns after a few atomic reads: no frame is read back, no
// FMOD DSP exists, no audio device is open, no thread and no ffmpeg process runs.
#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "Clipper.hpp"

using namespace geode::prelude;

class $modify(GPRLClipView, CCEGLView) {
    static void onModify(auto& self) {
        // Before mod menus that draw in their own swapBuffers hook at normal priority (they are not
        // part of the evidence), after post-effects that run first.
        if (auto res = self.setHookPriority("cocos2d::CCEGLView::swapBuffers", Priority::VeryEarly); res.isErr()) {
            log::warn("GPRL clip: could not set the swapBuffers hook priority: {}", res.unwrapErr());
        }
    }

    void swapBuffers() {
        gprl::clipper::onSwap();
        CCEGLView::swapBuffers();
    }
};

class $modify(GPRLClipAudio, FMODAudioEngine) {
    void update(float dt) {
        FMODAudioEngine::update(dt);
        gprl::clipper::onAudioTick(m_system);
    }
};

class $modify(GPRLClipPlayLayer, PlayLayer) {
    // The end screen is up: the popup for an exceptional completion opens here, one frame later
    // (the clip itself may still be "preparing"; the popup shows that and takes the choice anyway).
    void showEndLayer() {
        PlayLayer::showEndLayer();
        Loader::get()->queueInMainThread([] { gprl::clipper::offerPendingClip(true); });
    }
};

class $modify(GPRLClipPause, PauseLayer) {
    // A run the server asked about ended with a death: the choice is offered at the next pause,
    // never in the middle of play.
    void customSetup() {
        PauseLayer::customSetup();
        Loader::get()->queueInMainThread([] { gprl::clipper::offerPendingClip(true); });
    }
};
