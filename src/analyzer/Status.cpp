// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. Status texts come from the analyzer's own state; the popup is GPRL's own node tree.
#include "Status.hpp"

#include <cmath>

#include "../../core/analyzer_status.hpp"
#include "../../core/sim/job.hpp"
#include "../Settings.hpp"
#include "Analyzer.hpp"
#include "Modes.hpp"
#include "Worker.hpp"

using namespace geode::prelude;

namespace gprl::analyzer::ui {

namespace {

namespace sm = sim::modes;

constexpr float kWidth = 440.f;
constexpr float kHeight = 290.f;
constexpr char const* kChosenKey = "analysis-mode-chosen";

ModePopup* s_open = nullptr;
bool s_shownThisRun = false;

/// The View for the open visit (core/analyzer_status.hpp prints it).
status::View currentView(VisitView const& v, worker::Published const& p) {
    auto cfg = modes::config();
    status::View view;
    view.simulatorEnabled = sm::simulatorEnabled(cfg);
    view.recordSafe = cfg.recordSafe;
    if (!view.simulatorEnabled || !v.open) return view;
    if (v.isolationStopped) {
        view.stage = status::Stage::Stopped;
        return view;
    }
    if (v.failed) {
        view.stage = status::Stage::Failed;
        view.failReason = v.failReason;
        return view;
    }
    if (v.extracting) {
        view.stage = status::Stage::Extracting;
        view.extractPercent = v.extractPercent;
        return view;
    }
    if (!v.submitted || p.visitId != v.visitId) {
        view.stage = status::Stage::Waiting;
        view.wait = status::WaitReason::Queued;
        return view;
    }
    view.stage = p.stage;
    view.phase = sim::name(p.progress.phase);
    view.phasePercent = p.progress.percent;
    view.solvedPercent = p.hasResult ? p.solvedPercent : p.progress.solvedPercent;
    view.elapsedSeconds = p.progress.elapsedMs / 1000.0;
    view.hasVerification = p.hasVerification;
    view.verifiedPercent = p.verifiedPercent;
    view.cacheSource = p.cacheSource;
    view.upload = p.upload;
    view.failReason = p.failReason;
    if (p.stage == status::Stage::Running) {
        // why it does not run right now (the worker only polls; the reason is the main thread's facts)
        auto f = modes::facts();
        if (!sm::simAllowedNow(cfg, f)) {
            view.stage = status::Stage::Waiting;
            if (f.framePressure) view.wait = status::WaitReason::FramePressure;
            else if (cfg.recordSafe && f.attemptActive && !f.paused) view.wait = status::WaitReason::RecordSafeAttempt;
            else view.wait = status::WaitReason::Queued;
        }
    }
    return view;
}

CCLabelBMFont* text(CCNode* parent, std::string const& s, float x, float y, float width, float scale, ccColor3B color, bool center = false) {
    auto label = CCLabelBMFont::create(s.c_str(), "chatFont.fnt", width / scale, center ? kCCTextAlignmentCenter : kCCTextAlignmentLeft);
    label->setScale(scale);
    label->setAnchorPoint(center ? CCPoint{0.5f, 1.f} : CCPoint{0.f, 1.f});
    label->setPosition({x, y});
    label->setColor(color);
    parent->addChild(label);
    return label;
}

}  // namespace

std::string hudSuffix() {
    auto v = visitView();
    if (!v.open) return {};
    return status::hudLine(currentView(v, worker::published()));
}

SessionBlock sessionBlock() {
    SessionBlock b;
    auto cfg = modes::config();
    b.active = sm::simulatorEnabled(cfg);
    auto v = visitView();
    auto p = worker::published();
    std::string hud = v.open ? status::hudLine(currentView(v, p)) : std::string();
    if (!b.active) {
        // passive: the live solver still measures the player's windows (only Record-Safe stops it);
        // what passive leaves out is the background simulation and with it the level identity /
        // family notice (both come from the same read-only walk)
        b.line1 = std::string(modes::summary()) + (cfg.enabled ? " - no background level simulation, no level identity / family notice (setting analysis-mode)" : "");
        return b;
    }
    b.line1 = std::string(modes::summary()) + " | " + (hud.empty() ? std::string(v.open ? "Level analysis: idle" : "Level analysis: no level open") : hud);
    // v0.12.2: the analysis speed when it is not a plain low / normal - a plan speed, or a setting the
    // server's plan does not cover ("Analysis speed: Normal (Fast needs GPRL Plus)")
    auto choice = modes::cpuChoice();
    if (choice.limited || static_cast<int>(choice.effective) >= static_cast<int>(sm::CpuTier::Fast)) b.line1 += " | " + modes::speedLine();
    b.problem = v.isolationStopped || v.failed || p.stage == status::Stage::Failed;
    std::string l2;
    if (v.open && v.extracting) {
        l2 = fmt::format("Reading the level: {:.0f}% in {} slices ({:.1f} ms, max {:.0f} us per frame; budget {} us now)", v.extractPercent, v.slices, v.extractMs,
                         v.maxSliceUs, modes::extractionSliceUs());
    }
    else if (p.visitId != 0 && (!v.open || p.visitId == v.visitId)) {
        if (p.stage == status::Stage::Cached) l2 = "Cached analysis (" + p.cacheSource + ")";
        else l2 = status::ascii(p.progress.line);
        if (p.hasResult || p.progress.phase != sim::JobPhase::Idle) {
            l2 += fmt::format(" | physics {:.0f}%, solved {:.0f}%{}", p.physicsPercent, p.hasResult ? p.solvedPercent : p.progress.solvedPercent,
                              p.hasVerification ? fmt::format(", verified {:.0f}%", p.verifiedPercent) : std::string());
        }
        l2 += fmt::format(" | {} attempts recorded | world {} objects ({} gameplay, {} decoration){}", p.attemptsAdded, p.worldObjects, p.gameplayObjects,
                          p.decorationObjects, p.tooLarge ? ", too large" : "");
        if (p.unsupportedSpans > 0) l2 += fmt::format(", {} unsupported span{}", p.unsupportedSpans, p.unsupportedSpans == 1 ? "" : "s");
        if (!p.upload.empty()) l2 += " | " + p.upload;
    }
    if (v.open) {
        std::string rec = v.recording ? "recording this attempt" : (v.recordSkip.empty() ? std::string() : "not recording: " + v.recordSkip);
        if (!rec.empty()) l2 += (l2.empty() ? "" : " | ") + rec;
    }
    l2 += fmt::format("{}frame {:.1f} / target {:.1f} ms{}", l2.empty() ? "" : " | ", modes::frameAverageMs(), modes::frameTargetMs(),
                      modes::framePressure() ? " (pressure: paused)" : "");
    b.line2 = status::ascii(l2);
    b.line1 = status::ascii(b.line1);
    // level families (docs/LEVEL_FAMILY_DESIGN.md §8): the exact version's family, one line
    if (p.visitId != 0 && (!v.open || p.visitId == v.visitId)) {
        std::string l3;
        if (!p.familyLine.empty()) l3 = p.familyLine + (p.identityState.empty() ? "" : " | " + p.identityState);
        else if (!p.identityState.empty()) l3 = "Level identity: " + p.identityState;
        b.line3 = status::ascii(l3);
    }
    return b;
}

std::string accountLine() {
    // v0.12.2: "Analysis: full (live solver + level simulation) - Analysis speed: Normal (Fast needs GPRL Plus)"
    std::string line = modes::summary();
    if (sm::simulatorEnabled(modes::config())) line += " - " + modes::speedLine();
    return line;
}

namespace {

constexpr int kPopupMaxFrames = 1200;   // ~5-20 s of frames: a scene transition is far shorter

/// One frame's check (review LOW, 2026-10-02): the popup goes onto the running scene, so it is shown
/// only when that scene is the one holding THIS MenuLayer - never onto a CCTransitionScene (it would
/// vanish with it) or the scene of a level entered right after the menu appeared. Reads only:
/// CCDirector's running scene, the menu's parent / isRunning(); the popup is GPRL's own node.
void tryShowModePopup(Ref<MenuLayer> menu, int framesLeft, bool seenRunning) {
    if (Mod::get()->getSavedValue<bool>(kChosenKey, false) || ModePopup::isOpen()) return;
    auto* dir = CCDirector::sharedDirector();
    auto* scene = dir ? dir->getRunningScene() : nullptr;
    bool transition = scene && typeinfo_cast<CCTransitionScene*>(scene) != nullptr;
    bool running = menu && menu->isRunning();
    if (running && scene && !transition && menu->getParent() == scene) {
        ModePopup::open();
        return;
    }
    // the menu's scene entered and was left again (a level was opened right away): the next
    // MenuLayer::init tries again; so does a menu that never became the running scene in time
    bool left = seenRunning && (!running || (scene && !transition && menu->getParent() != scene));
    if (!menu || left || framesLeft <= 0) {
        s_shownThisRun = false;
        return;
    }
    // not on screen yet (the scene is swapped in after this frame's scheduler update) or a
    // CCTransitionScene is running: check again next frame
    Loader::get()->queueInMainThread([menu, framesLeft, seen = seenRunning || running] { tryShowModePopup(menu, framesLeft - 1, seen); });
}

}  // namespace

void maybeShowModePopup(MenuLayer* menu) {
    if (s_shownThisRun || !menu) return;
    if (Mod::get()->getSavedValue<bool>(kChosenKey, false)) return;
    s_shownThisRun = true;
    Ref<MenuLayer> keep(menu);
    Loader::get()->queueInMainThread([keep] { tryShowModePopup(keep, kPopupMaxFrames, false); });
}

void ModePopup::open() {
    if (s_open) return;
    auto ret = new ModePopup();
    if (!ret->init()) {
        delete ret;
        return;
    }
    ret->autorelease();
    ret->show();
}

bool ModePopup::isOpen() { return s_open != nullptr; }

bool ModePopup::init() {
    if (!Popup::init(kWidth, kHeight)) return false;
    s_open = this;
    setTitle("GPRL: how should levels be analysed?");
    float const x = 20.f, w = kWidth - 40.f;
    ccColor3B const white{255, 255, 255}, grey{170, 170, 170}, yellow{255, 220, 120};
    // 2026-10-02 (owner: "make it so I can't turn off that setting"): YOUR timing windows are
    // always measured; this choice is only about the background simulation of each level.
    text(m_mainLayer, "Your own timing windows are always measured (your sigma/s and the level ratings need them). Only Record-Safe Mode "
                      "below stops that, for list submissions.",
         x, 246.f, w, 0.5f, white);
    text(m_mainLayer, "Full (recommended): also simulate each level on a background thread from a read-only copy of its objects.", x, 210.f, w, 0.5f, white);
    text(m_mainLayer, "Passive: no level simulation.", x, 186.f, w, 0.5f, white);

    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu, 2);
    m_recordSafe = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(ModePopup::onToggle), 0.6f);
    m_recordSafe->toggle(settings::get().recordSafe);
    m_recordSafe->setPosition({x + 10.f, 152.f});
    menu->addChild(m_recordSafe);
    text(m_mainLayer, "Record-Safe Mode", x + 26.f, 160.f, 200.f, 0.55f, yellow);
    text(m_mainLayer, "For list submissions: no hidden clones in the live physics loop, no bot or replay automation, no noclip use; the offline simulation "
                      "only runs while no attempt is active. Telemetry stays passive. GPRL is designed to avoid affecting gameplay and completion eligibility; "
                      "it is not approved by AREDL, Pointercrate or any other list unless that list says so.",
         x, 138.f, w, 0.42f, grey);

    struct Choice {
        char const* label;
        char const* sprite;
        int tag;
    };
    Choice const choices[] = {{"Full (recommended)", "GJ_button_01.png", 2}, {"Passive", "GJ_button_04.png", 0}};
    float bx = 0.f;
    std::vector<CCMenuItemSpriteExtra*> buttons;
    for (auto const& c : choices) {
        auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create(c.label, "goldFont.fnt", c.sprite, 0.6f), this, menu_selector(ModePopup::onChoose));
        btn->setTag(c.tag);
        buttons.push_back(btn);
        bx += btn->getScaledContentSize().width + 10.f;
    }
    float cx = (kWidth - (bx - 10.f)) / 2.f;
    for (auto* btn : buttons) {
        float bw = btn->getScaledContentSize().width;
        btn->setPosition({cx + bw / 2.f, 50.f});
        menu->addChild(btn);
        cx += bw + 10.f;
    }
    text(m_mainLayer, "You can change this any time in the mod settings (Level analysis).", kWidth / 2.f, 24.f, w, 0.4f, grey, true);
    return true;
}

void ModePopup::onChoose(CCObject* sender) {
    int tag = sender ? static_cast<CCNode*>(sender)->getTag() : 1;
    char const* mode = tag == 0 ? "passive" : (tag == 2 ? "full" : "offline");
    bool recordSafe = m_recordSafe && m_recordSafe->isToggled();
    auto* mod = Mod::get();
    mod->setSavedValue<bool>(kChosenKey, true);
    mod->setSettingValue<std::string>("analysis-mode", mode);
    mod->setSettingValue<bool>("record-safe", recordSafe);
    log::info("GPRL analyzer: mode chosen in the first-run popup: {}{}", mode, recordSafe ? " + Record-Safe" : "");
    onClose(nullptr);
}

void ModePopup::onClose(CCObject* sender) {
    s_open = nullptr;
    Popup::onClose(sender);
}

}  // namespace gprl::analyzer::ui
