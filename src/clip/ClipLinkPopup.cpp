#include "ClipLinkPopup.hpp"

#include <Geode/utils/cocos.hpp>

#include <algorithm>
#include <cmath>

#include "../../core/clip_flow.hpp"
#include "../Clipper.hpp"
#include "../Hud.hpp"

using namespace geode::prelude;

namespace gprl::clip {

namespace {

constexpr float kWidth = 420.f;
constexpr float kHeight = 262.f;
constexpr float kTickSeconds = 0.25f;

constexpr ccColor3B kWhite{255, 255, 255};
constexpr ccColor3B kGrey{170, 170, 170};
constexpr ccColor3B kGreen{140, 255, 140};
constexpr ccColor3B kOrange{255, 150, 100};
constexpr ccColor3B kYellow{255, 220, 120};

ClipLinkPopup* s_current = nullptr;

CCLabelBMFont* line(CCNode* parent, std::string const& text, float scale, float y, ccColor3B color, float x = kWidth / 2.f, bool left = false) {
    auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
    label->setScale(scale);
    label->limitLabelWidth(kWidth - 36.f, scale, 0.2f);
    label->setAnchorPoint(left ? CCPoint{0.f, 0.5f} : CCPoint{0.5f, 0.5f});
    label->setPosition({x, y});
    label->setColor(color);
    parent->addChild(label);
    return label;
}

}  // namespace

void ClipLinkPopup::open(std::string const& clipId) {
    if (s_current) return;
    auto ret = new ClipLinkPopup();
    if (!ret->init(clipId)) {
        delete ret;
        return;
    }
    ret->autorelease();
    ret->show();
}

bool ClipLinkPopup::isOpen() { return s_current != nullptr; }

bool ClipLinkPopup::init(std::string const& clipId) {
    if (!Popup::init(kWidth, kHeight)) return false;
    s_current = this;
    m_clipId = clipId;
    setTitle("Verify your run with a YouTube link");

    m_level = line(m_mainLayer, "", 0.5f, 212.f, kYellow);
    line(m_mainLayer, "This clip is longer than GPRL can upload, so it was saved on this computer instead.", 0.42f, 192.f, kGrey);
    line(m_mainLayer, "1. Clips folder: find the saved clip (GPRL <level> ... .mp4).", 0.45f, 172.f, kWhite, 18.f, true);
    line(m_mainLayer, "2. Upload it to YouTube as Unlisted or Public (NOT Private - the moderators must be able to watch it).", 0.45f, 156.f, kWhite, 18.f, true);
    line(m_mainLayer, "3. Paste the video link below and press Send. Only the GPRL moderators assigned to your run see it.", 0.45f, 140.f, kWhite, 18.f, true);

    m_input = TextInput::create(kWidth - 120.f, "https://youtu.be/...", "chatFont.fnt");
    m_input->setCommonFilter(CommonFilter::Any);
    m_input->setMaxCharCount(500);
    m_input->setPosition({kWidth / 2.f - 42.f, 104.f});
    m_mainLayer->addChild(m_input);

    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu, 2);
    auto paste = CCMenuItemSpriteExtra::create(ButtonSprite::create("Paste", "goldFont.fnt", "GJ_button_04.png", 0.45f), this, menu_selector(ClipLinkPopup::onPaste));
    paste->setPosition({kWidth - 52.f, 104.f});
    menu->addChild(paste);
    auto folder = CCMenuItemSpriteExtra::create(ButtonSprite::create("Clips folder", 120, true, "goldFont.fnt", "GJ_button_04.png", 26.f, 0.5f), this, menu_selector(ClipLinkPopup::onFolder));
    folder->setPosition({kWidth / 2.f - 80.f, 58.f});
    menu->addChild(folder);
    m_sendBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Send link", 120, true, "goldFont.fnt", "GJ_button_01.png", 26.f, 0.5f), this, menu_selector(ClipLinkPopup::onSend));
    m_sendBtn->setPosition({kWidth / 2.f + 80.f, 58.f});
    menu->addChild(m_sendBtn);

    m_status = line(m_mainLayer, "", 0.42f, 26.f, kGrey);
    refresh();
    this->schedule(schedule_selector(ClipLinkPopup::onTick), kTickSeconds);
    return true;
}

void ClipLinkPopup::onClose(CCObject* sender) {
    s_current = nullptr;
    Popup::onClose(sender);
}

void ClipLinkPopup::onTick(float) { refresh(); }

void ClipLinkPopup::refresh() {
    auto record = clipper::findClip(m_clipId);
    auto st = clipper::status();
    if (!record) {
        m_level->setString("This clip is gone");
        m_status->setString("");
        return;
    }
    auto const& r = *record;
    std::string head = (r.levelName.empty() ? "level " + r.levelId : r.levelName)
        + fmt::format("   {}%   attempt {}   {} {}", r.completed ? 100 : static_cast<int>(std::floor(std::clamp(r.percent, 0.0, 100.0))), r.attemptNo,
                      formatDuration(r.durationMs), formatBytes(r.sizeBytes));
    m_level->setString(head.c_str());
    m_level->limitLabelWidth(kWidth - 36.f, 0.5f, 0.2f);
    std::string message;
    ccColor3B color = kGrey;
    if (r.linkSent) {
        message = "YouTube link sent: " + r.linkUrl;
        color = kGreen;
    }
    else if (st.linkSending && st.linkClipId == r.clipId) {
        message = "Sending the link...";
        color = kWhite;
    }
    else if (!st.linkMessage.empty() && st.linkClipId == r.clipId) {
        message = st.linkMessage;
        color = kOrange;
    }
    else if (!r.path.empty()) message = "Saved as: " + r.path;
    m_status->setString(message.c_str());
    m_status->limitLabelWidth(kWidth - 36.f, 0.42f, 0.2f);
    m_status->setColor(color);
    bool busy = st.linkSending;
    m_sendBtn->setEnabled(!busy && !r.linkSent);
    m_sendBtn->setOpacity(!busy && !r.linkSent ? 255 : 120);
}

void ClipLinkPopup::onFolder(CCObject*) { clipper::openClipsFolder(); }

void ClipLinkPopup::onPaste(CCObject*) {
    auto text = utils::clipboard::read();
    if (!text.empty()) m_input->setString(text, false);
}

void ClipLinkPopup::onSend(CCObject*) {
    std::string url = m_input->getString();
    if (youtubeVideoId(url).empty()) {
        hud::notify("GPRL: that is not a link to a YouTube video (youtube.com/watch?v=... or youtu.be/...)", hud::ToastKind::Warning, 5.f);
        return;
    }
    clipper::sendLink(m_clipId, url);
    refresh();
}

}  // namespace gprl::clip
