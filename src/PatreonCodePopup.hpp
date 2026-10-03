#pragma once
// v0.12.2 "Enter Patreon code" (security review: the Patreon link confirmation). After Connect
// Patreon and "Allow" on Patreon, the GPRL page in the browser shows a one-time code of 8
// characters (letters and digits 2-9 without I, L, O: PATREON_CONFIRM_CODE_ALPHABET). This popup
// takes it (typed or pasted; lowercase is uppercased, a pasted space / dash is dropped) and sends
// it with this device's token: POST /v1/me/patreon/confirm on the telemetry
// worker (client::requestPatreonConfirm), so the link can only land on the GPRL account of this
// game. The result is shown here and as a notification; the code is never logged or stored.
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <cstdint>
#include <string>

namespace gprl::patreon {

class PatreonCodePopup : public geode::Popup {
public:
    static void open();
    static bool isOpen();

protected:
    bool init();
    void onTick(float dt);
    void refresh();
    void onClose(cocos2d::CCObject* sender) override;
    void onPaste(cocos2d::CCObject*);
    void onConfirm(cocos2d::CCObject*);
    void uppercaseSoon();

    uint32_t m_seenGen = 0;          // Status::patreonConfirmGen when the popup opened
    bool m_succeeded = false;
    std::string m_localMessage;      // a code that is not well formed (never sent)
    cocos2d::CCLabelBMFont* m_status = nullptr;
    geode::TextInput* m_input = nullptr;
    CCMenuItemSpriteExtra* m_confirmBtn = nullptr;
};

}  // namespace gprl::patreon
