#pragma once
// v0.10.0 (owner decision 2026-10-01): "Verify with a YouTube link". A run the server asked to
// verify whose clip is too long for the mod's upload (docs/CLIPPING.md §9: 95 MB, about 2.6
// minutes at 720p) is saved on this computer instead; this popup keeps the three steps in one
// place - open the clips folder, upload the saved clip to YouTube (Unlisted or Public, never
// Private: the moderators could not watch it), paste the link, Send. The link is sent to
// POST /v1/me/evidence/links as PRIVATE evidence (only the moderators assigned to the run see it).
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <string>

namespace gprl::clip {

class ClipLinkPopup : public geode::Popup {
public:
    static void open(std::string const& clipId);
    static bool isOpen();

protected:
    bool init(std::string const& clipId);
    void onTick(float dt);
    void refresh();
    void onClose(cocos2d::CCObject* sender) override;
    void onFolder(cocos2d::CCObject*);
    void onPaste(cocos2d::CCObject*);
    void onSend(cocos2d::CCObject*);

    std::string m_clipId;
    std::string m_lastMessage;
    cocos2d::CCLabelBMFont* m_level = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    geode::TextInput* m_input = nullptr;
    CCMenuItemSpriteExtra* m_sendBtn = nullptr;
};

}  // namespace gprl::clip
