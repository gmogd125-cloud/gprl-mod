#include "PatreonCodePopup.hpp"

#include <Geode/ui/Notification.hpp>
#include <Geode/utils/general.hpp>

#include "../core/entitlements.hpp"
#include "Telemetry.hpp"

using namespace geode::prelude;

namespace gprl::patreon {

namespace {

constexpr float kWidth = 360.f;
constexpr float kHeight = 210.f;
constexpr float kTickSeconds = 0.25f;

constexpr ccColor3B kWhite{255, 255, 255};
constexpr ccColor3B kGrey{170, 170, 170};
constexpr ccColor3B kGreen{140, 255, 140};
constexpr ccColor3B kOrange{255, 150, 100};

PatreonCodePopup* s_current = nullptr;

CCLabelBMFont* line(CCNode* parent, std::string const& text, float scale, float y, ccColor3B color) {
    auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
    label->setScale(scale);
    label->limitLabelWidth(kWidth - 30.f, scale, 0.2f);
    label->setPosition({kWidth / 2.f, y});
    label->setColor(color);
    parent->addChild(label);
    return label;
}

}  // namespace

void PatreonCodePopup::open() {
    if (s_current) return;
    auto ret = new PatreonCodePopup();
    if (!ret->init()) {
        delete ret;
        return;
    }
    ret->autorelease();
    ret->show();
}

bool PatreonCodePopup::isOpen() { return s_current != nullptr; }

bool PatreonCodePopup::init() {
    if (!Popup::init(kWidth, kHeight)) return false;
    s_current = this;
    setTitle("Enter Patreon code");
    m_seenGen = client::status().patreonConfirmGen;

    line(m_mainLayer, "After you allowed GPRL on Patreon, the GPRL page in your browser", 0.45f, 168.f, kWhite);
    line(m_mainLayer, "shows a code of 8 characters. Type or paste it here and press Confirm.", 0.45f, 154.f, kWhite);
    line(m_mainLayer, "It works once, only for the GPRL account connected in this game.", 0.4f, 138.f, kGrey);

    m_input = TextInput::create(150.f, "ABCD2345", "bigFont.fnt");
    m_input->setFilter(entitlements::kPatreonCodeInputFilter);
    m_input->setMaxCharCount(entitlements::kPatreonCodeLength);
    m_input->setPosition({kWidth / 2.f - 30.f, 104.f});
    // lowercase typed is shown uppercased: the change is made on the next frame, outside the
    // input's own text-changed call
    m_input->setCallback([this](std::string const&) { uppercaseSoon(); });
    m_mainLayer->addChild(m_input);

    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu, 2);
    auto paste = CCMenuItemSpriteExtra::create(ButtonSprite::create("Paste", "goldFont.fnt", "GJ_button_04.png", 0.45f), this,
                                               menu_selector(PatreonCodePopup::onPaste));
    paste->setPosition({kWidth / 2.f + 85.f, 104.f});
    menu->addChild(paste);
    m_confirmBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Confirm", 110, true, "goldFont.fnt", "GJ_button_01.png", 26.f, 0.5f), this,
                                                 menu_selector(PatreonCodePopup::onConfirm));
    m_confirmBtn->setPosition({kWidth / 2.f, 62.f});
    menu->addChild(m_confirmBtn);

    m_status = line(m_mainLayer, "", 0.42f, 30.f, kGrey);
    refresh();
    this->schedule(schedule_selector(PatreonCodePopup::onTick), kTickSeconds);
    return true;
}

void PatreonCodePopup::onClose(CCObject* sender) {
    s_current = nullptr;
    Popup::onClose(sender);
}

void PatreonCodePopup::onTick(float) { refresh(); }

void PatreonCodePopup::uppercaseSoon() {
    Ref<TextInput> input = m_input;
    Loader::get()->queueInMainThread([input] {
        if (!input || !input->getParent()) return;
        std::string now = input->getString();
        std::string shown = entitlements::patreonCodeInputText(now);
        if (shown != now) input->setString(shown, false);
    });
}

void PatreonCodePopup::refresh() {
    auto st = client::status();
    std::string message;
    ccColor3B color = kGrey;
    if (!st.connected) {
        message = "Connect this game to GPRL first (Account tab, Connect)";
        color = kOrange;
    }
    else if (st.patreonConfirmPending) {
        message = "Checking the code...";
        color = kWhite;
    }
    else if (st.patreonConfirmGen != m_seenGen && !st.patreonConfirmText.empty()) {
        message = st.patreonConfirmText;
        color = st.patreonConfirmOk ? kGreen : kOrange;
        if (st.patreonConfirmOk) m_succeeded = true;
    }
    else if (!m_localMessage.empty()) {
        message = m_localMessage;
        color = kOrange;
    }
    m_status->setString(message.c_str());
    m_status->limitLabelWidth(kWidth - 30.f, 0.42f, 0.2f);
    m_status->setColor(color);
    bool can = st.connected && !st.patreonConfirmPending && !m_succeeded;
    m_confirmBtn->setEnabled(can);
    m_confirmBtn->setOpacity(can ? 255 : 120);
}

void PatreonCodePopup::onPaste(CCObject*) {
    auto text = utils::clipboard::read();
    // a pasted code with spaces / dashes ("ABCD-2345") is taken whole; anything else is filtered
    std::string shown = entitlements::normalizePatreonCode(text);
    if (shown.empty()) shown = entitlements::patreonCodeInputText(text);
    if (!shown.empty()) m_input->setString(shown, false);
}

void PatreonCodePopup::onConfirm(CCObject*) {
    std::string code = entitlements::normalizePatreonCode(std::string(m_input->getString()));
    if (code.empty()) {
        m_localMessage = entitlements::kPatreonCodeHint;
        // an older result must not cover the hint
        m_seenGen = client::status().patreonConfirmGen;
        refresh();
        return;
    }
    m_localMessage.clear();
    m_seenGen = client::status().patreonConfirmGen;
    client::requestPatreonConfirm(std::move(code));
    refresh();
}

}  // namespace gprl::patreon
