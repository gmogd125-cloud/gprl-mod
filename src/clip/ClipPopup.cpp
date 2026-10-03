#include "ClipPopup.hpp"

#include <algorithm>
#include <cmath>

#include "../../core/clip_flow.hpp"
#include "../Clipper.hpp"

using namespace geode::prelude;

namespace gprl::clip {

namespace {

constexpr float kWidth = 400.f;
constexpr float kHeight = 250.f;
constexpr float kInputGuardSeconds = kFlow.popupInputGuardSeconds;   // buttons ignore input this long after the popup opened
constexpr float kTickSeconds = kFlow.popupTickSeconds;

constexpr ccColor3B kWhite{255, 255, 255};
constexpr ccColor3B kGrey{170, 170, 170};
constexpr ccColor3B kGreen{140, 255, 140};
constexpr ccColor3B kOrange{255, 150, 100};
constexpr ccColor3B kYellow{255, 220, 120};

// popup button order: row 1 = Save | Send, row 2 = Save + send | Do nothing
constexpr Choice kOrder[4] = {Choice::SaveLocal, Choice::Send, Choice::SaveAndSend, Choice::Nothing};
constexpr char const* kButtonSprite[4] = {"GJ_button_01.png", "GJ_button_02.png", "GJ_button_02.png", "GJ_button_06.png"};

ClipPopup* s_current = nullptr;

char const* whyText(std::string const& rule) {
    if (rule == "server_hint") return "The GPRL server asks for evidence of this run.";
    if (rule == "local_completion") return "You completed a level that counts. A clip lets the GPRL moderators verify the run.";
    return "You asked for a clip of this attempt.";
}

CCLabelBMFont* makeLabel(CCNode* parent, char const* font, float scale, float y, ccColor3B color) {
    auto label = CCLabelBMFont::create("", font);
    label->setScale(scale);
    label->setPosition({kWidth / 2.f, y});
    label->setColor(color);
    parent->addChild(label);
    return label;
}

void setText(CCLabelBMFont* label, std::string const& text, float scale, ccColor3B color) {
    label->setString(text.c_str());
    label->setColor(color);
    label->limitLabelWidth(kWidth - 36.f, scale, 0.2f);
}

}  // namespace

void ClipPopup::open(std::string const& clipId) {
    if (s_current) return;
    auto ret = new ClipPopup();
    if (!ret->init(clipId)) {
        delete ret;
        return;
    }
    ret->autorelease();
    ret->show();
}

bool ClipPopup::isOpen() { return s_current != nullptr; }

ClipPopup::~ClipPopup() {
    if (s_current == this) s_current = nullptr;
}

bool ClipPopup::init(std::string const& clipId) {
    if (!Popup::init(kWidth, kHeight)) return false;
    s_current = this;
    m_clipId = clipId;
    setTitle("Keep a clip of this run?");

    m_level = makeLabel(m_mainLayer, "bigFont.fnt", 0.5f, 198.f, kYellow);
    m_state = makeLabel(m_mainLayer, "chatFont.fnt", 0.5f, 178.f, kWhite);
    m_why = makeLabel(m_mainLayer, "chatFont.fnt", 0.45f, 160.f, kGrey);

    auto privacy = CCLabelBMFont::create("Nothing is uploaded unless you press a Send button. Sent clips are private:\nonly the GPRL moderators assigned to your run can watch them.", "chatFont.fnt");
    privacy->setAlignment(kCCTextAlignmentCenter);
    privacy->setScale(0.42f);
    privacy->limitLabelWidth(kWidth - 36.f, 0.42f, 0.2f);
    privacy->setPosition({kWidth / 2.f, 136.f});
    privacy->setColor(kGrey);
    m_mainLayer->addChild(privacy);

    m_block = makeLabel(m_mainLayer, "chatFont.fnt", 0.42f, 114.f, kOrange);

    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu, 2);
    for (int i = 0; i < 4; ++i) {
        auto sprite = ButtonSprite::create(label(kOrder[i]), 150, true, "goldFont.fnt", kButtonSprite[i], 28.f, 0.55f);
        auto btn = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(ClipPopup::onChoice));
        btn->setTag(i);
        float x = kWidth / 2.f + (i % 2 == 0 ? -92.f : 92.f);
        float y = i < 2 ? 84.f : 48.f;
        btn->setPosition({x, y});
        menu->addChild(btn);
        m_buttons[i] = btn;
    }

    auto later = CCLabelBMFont::create("Closing this window decides later: the clip waits in the GPRL menu > Account.", "chatFont.fnt");
    later->setScale(0.38f);
    later->limitLabelWidth(kWidth - 36.f, 0.38f, 0.2f);
    later->setPosition({kWidth / 2.f, 20.f});
    later->setColor(kGrey);
    m_mainLayer->addChild(later);

    refresh();
    this->schedule(schedule_selector(ClipPopup::onTick), kTickSeconds);
    return true;
}

void ClipPopup::onTick(float dt) {
    m_age += dt;
    refresh();
}

void ClipPopup::refresh() {
    auto record = clipper::findClip(m_clipId);
    auto st = clipper::status();
    bool guard = m_age < kInputGuardSeconds;
    bool canChoose = false;
    std::string block;
    if (!record) {
        setText(m_level, "This clip is gone", 0.5f, kYellow);
        setText(m_state, "It was discarded (too many clips were waiting for a choice).", 0.5f, kOrange);
        m_why->setString("");
    }
    else {
        auto const& r = *record;
        std::string head = (r.levelName.empty() ? "level " + r.levelId : r.levelName)
            + fmt::format("   {}%   attempt {}", r.completed ? 100 : static_cast<int>(std::floor(std::clamp(r.percent, 0.0, 100.0))), r.attemptNo);
        setText(m_level, head, 0.5f, kYellow);
        setText(m_why, whyText(r.rule), 0.45f, kGrey);
        switch (r.state) {
            case ClipState::Preparing:
                setText(m_state, r.choiceMade ? std::string("Preparing the clip... then: ") + label(r.choice) : std::string("Preparing the clip... you can already choose"),
                        0.5f, kWhite);
                canChoose = !r.choiceMade;
                break;
            case ClipState::Ready: {
                std::string text = fmt::format("Clip ready: {}, {}, {}x{}, {}{}{}", formatDuration(r.durationMs), formatBytes(r.sizeBytes), r.width, r.height,
                                               r.hasGameAudio ? "game sound" : "no sound", r.hasMicAudio ? " + MICROPHONE track" : "",
                                               r.wholeAttempt ? "" : " (the start of the attempt had left the buffer)");
                setText(m_state, text, 0.5f, kGreen);
                canChoose = true;
                break;
            }
            case ClipState::UploadFailed:
                // The file is still here. The four choices apply again (core/clip_flow onChoice):
                // Send / Save + send = retry, Save to computer = keep it and give up the upload,
                // Do nothing = give up (deletes a clip that was never saved). Without this a clip
                // whose upload failed could only ever be retried.
                setText(m_state, statusLine(r, st.uploadFraction), 0.5f, kOrange);
                canChoose = true;
                break;
            default:
                setText(m_state, statusLine(r, st.uploadFraction), 0.5f, r.state == ClipState::Failed || r.state == ClipState::UploadFailed ? kOrange : kWhite);
                break;
        }
        // why a Send button is off (the Save / Do nothing buttons always work)
        if (canChoose) {
            if (r.state == ClipState::Preparing) {
                // size and hash are not known yet: only what is known now can block
                if (st.gate.localOnly) block = "local-only mode is on (or the API is not set), so nothing is sent";
                else if (!st.gate.connected) block = "not connected: press Connect in the GPRL menu first";
                else if (!r.levelCounts) block = "this level does not count for GPRL (not a rated demon), so there is nothing to verify";
            }
            else block = uploadBlockReason(r, st.gate);
        }
    }
    setText(m_block, block.empty() ? std::string() : "Send is not available: " + block, 0.42f, kOrange);
    for (int i = 0; i < 4; ++i) {
        bool send = wantsSend(kOrder[i]);
        bool on = canChoose && !guard && !m_decided && !(send && !block.empty());
        m_buttons[i]->setEnabled(on);
        m_buttons[i]->setOpacity(on ? 255 : 110);
    }
}

void ClipPopup::onChoice(CCObject* sender) {
    int i = static_cast<CCNode*>(sender)->getTag();
    if (i < 0 || i >= 4 || m_decided || m_age < kInputGuardSeconds) return;
    m_decided = true;
    // the ONLY path to an upload: this explicit button press (or Retry in the Account tab)
    clipper::applyChoice(m_clipId, kOrder[i]);
    this->onClose(nullptr);
}

}  // namespace gprl::clip
