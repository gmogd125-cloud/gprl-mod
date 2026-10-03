#pragma once
// Background level analyzer: what the player sees (docs/BACKGROUND_ANALYZER_DESIGN.md §5, §9).
//   - the HUD's bottom-left line gains "Level analysis: searching 42%, 38 s" / "verified 96%" /
//     "cached" / "Record-Safe: waiting for the attempt to end" / "stopped (isolation)" (the pure
//     texts are core/analyzer_status.hpp, host-tested),
//   - the popup's Session tab gets a two-line block (status + the job's progress / coverage / world
//     / upload), the Account tab one line (sim::modes::summary),
//   - the first-run mode popup (once; saved value `analysis-mode-chosen`): Full (recommended) /
//     Passive (the background simulation only - the live solver runs in every mode) and a
//     Record-Safe tick box with the §9 text; it writes the settings. Shown only while the MenuLayer
//     that asked for it is the running scene's layer.
//
// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field. This file reads the analyzer's
// own published state and draws cocos nodes in GPRL's own popup.
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

#include <string>

namespace gprl::analyzer::ui {

/// The HUD suffix ("" = nothing to show).
std::string hudSuffix();

struct SessionBlock {
    std::string line1;
    std::string line2;
    std::string line3;       // level families: "Family: <name> - gameplay 99.7% - presentation 68.1% - <label> | sent" ("" = nothing yet)
    bool active = false;     // the simulator is part of the configuration
    bool problem = false;    // stopped / failed (orange)
};
SessionBlock sessionBlock();

/// The Account tab line: "Analysis: offline simulation (Record-Safe)".
std::string accountLine();

/// MenuLayer::init: shows the mode popup once (saved value `analysis-mode-chosen`) - only while
/// `menu` is still the running scene's layer (a scene transition in progress is waited out for a
/// few seconds; a menu left before that gives the next MenuLayer::init the chance).
void maybeShowModePopup(MenuLayer* menu);

class ModePopup : public geode::Popup {
public:
    static void open();
    static bool isOpen();

protected:
    bool init();
    void onChoose(cocos2d::CCObject* sender);
    void onToggle(cocos2d::CCObject*) {}   // the tick box is read when a mode is chosen
    void onClose(cocos2d::CCObject* sender) override;

    CCMenuItemToggler* m_recordSafe = nullptr;
};

}  // namespace gprl::analyzer::ui
