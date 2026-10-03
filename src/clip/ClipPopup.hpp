#pragma once
// The four-choice popup for a preserved attempt (MASTER §16, SPEC §28):
//
//     Save to computer   |   Send to GPRL moderators
//     Save + send        |   Do nothing
//
// Shown on the end screen after an exceptional completion, on the next pause for a run the server
// asked about, and from the GPRL menu > Account. The clip may still be "preparing" when it opens:
// a choice made then is applied as soon as the file is ready (core/clip_flow). A clip whose upload
// failed is offered again from the Account tab with the same four choices (Send = retry).
//
// Nothing is ever uploaded by opening or closing it: only the two Send buttons upload, and they
// are disabled (with the reason) while the upload is not possible. Closing it with the X / Esc
// keeps the clip pending ("decide later"). The buttons ignore input for the first second so a
// jump click that was already on its way cannot pick a choice.
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

#include <string>

namespace gprl::clip {

class ClipPopup : public geode::Popup {
public:
    static void open(std::string const& clipId);
    static bool isOpen();

protected:
    bool init(std::string const& clipId);
    ~ClipPopup() override;

    void onTick(float dt);
    void refresh();
    void onChoice(cocos2d::CCObject* sender);

    std::string m_clipId;
    float m_age = 0.f;
    bool m_decided = false;
    cocos2d::CCLabelBMFont* m_level = nullptr;
    cocos2d::CCLabelBMFont* m_state = nullptr;
    cocos2d::CCLabelBMFont* m_why = nullptr;
    cocos2d::CCLabelBMFont* m_block = nullptr;
    CCMenuItemSpriteExtra* m_buttons[4] = {};
};

}  // namespace gprl::clip
