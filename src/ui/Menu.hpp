#pragma once
// The GPRL menu (v0.13.0 redesign): one popup with a sidebar of five screens, opened from the
// pause menu and the main menu (Hooks.cpp). Replaces the v0.3.0 tabbed Popup.cpp.
//
//   Home     not connected: a welcome screen (what GPRL is, three steps, one big Connect button);
//            connected: the rank badge, the sigma/s hero (or the calibration ring while locked
//            with "what to play next"), the verification state and the 8 gamemode columns
//   Ranks    the ladder from GET /api/ranks (whatever the server lists, in its order) with the
//            owner's 10,000-attempts disclaimer and a "you are here" row
//   Board    GET /api/leaderboard?limit=25 with the player's row highlighted
//   Level    the level being played (or the last one): stat tiles, the timing-window solver's
//            status + coverage, the background analysis; "Details" opens every raw line
//   Account  connection, website (profile link / sign-in code), clipping, mod data + Settings
//
// Everything shown comes from cached state (client::status, client::siteData, tracker counters,
// solver / analyzer status): the popup never networks on the game thread. It re-reads on a 0.5 s
// tick and rebuilds a screen only when the text it would show changed (a per-screen key), so
// buttons are stable under the cursor and scroll positions survive the tick.
//
// Screen code lives in one file each (PageHome.cpp, PageRanks.cpp, PageBoard.cpp, PageLevel.cpp,
// PageAccount.cpp); the shell, shared helpers and actions are in Menu.cpp. Sizes and colours:
// Theme.hpp; drawing pieces: Widgets.hpp.
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>

#include <string>
#include <vector>

#include "../../core/calibration.hpp"
#include "../../core/ranks.hpp"
#include "../Telemetry.hpp"

namespace gprl::ui {

/// Opens the menu (pause-menu / main-menu button).
void open();

enum class Screen : int { Home = 0, Ranks, Board, Level, Account };
constexpr int kScreenCount = 5;

class GprlMenu : public geode::Popup {
public:
    static GprlMenu* create();

protected:
    bool init();
    void onClose(cocos2d::CCObject* sender) override;

    // ---- shell (Menu.cpp) ----
    void buildSidebar();
    void selectScreen(Screen screen);
    void onTab(cocos2d::CCObject* sender);
    void onTick(float dt);
    void refresh();
    void refreshHeader();
    void requestData();
    /// Clears the screen when `key` differs from the last build's; false = nothing changed.
    bool beginPage(std::string const& key);
    /// A menu at the screen's origin (buttons positioned in screen-local coordinates).
    cocos2d::CCMenu* pageMenu();
    /// Centered "Loading..." / error text for a site-data state; true when the data is usable.
    bool siteState(client::SiteData const& site, client::SiteFetchState const& st, char const* what);
    /// Centered empty state (a title line and up to two explanation lines).
    void emptyState(std::string const& title, std::string const& line1, std::string const& line2 = {});
    ranks::RankList const* ladder(client::SiteData const& site) const;
    std::string rankLabel(ranks::RankList const* list, ranks::PlayerRank const& pr) const;
    /// "Level Analysis Coverage: 83% / Missing: ..." for a level, with its tone.
    std::string coverageText(client::SiteData const& site, std::string const& levelId, cocos2d::ccColor3B& color) const;

    // ---- screens ----
    void buildHome();                                                      // PageHome.cpp
    void buildWelcome(client::Status const& st);                           // PageHome.cpp
    void buildProfile(client::Status const& st, client::SiteData const& site);   // PageHome.cpp
    void buildRanks();                                                     // PageRanks.cpp
    void buildBoard();                                                     // PageBoard.cpp
    void buildLevel();                                                     // PageLevel.cpp
    void buildAccount();                                                   // PageAccount.cpp
    void tickAccount();                                                    // PageAccount.cpp: countdown without a rebuild

    // ---- actions ----
    void onConnect(cocos2d::CCObject*);
    void onDisconnect(cocos2d::CCObject*);
    void onResetData(cocos2d::CCObject*);
    void onProfile(cocos2d::CCObject*);
    void onWebsiteCode(cocos2d::CCObject*);
    void onCopyCode(cocos2d::CCObject*);
    void onHideCode(cocos2d::CCObject*);
    void onVisitSite(cocos2d::CCObject*);
    void onFlush(cocos2d::CCObject*);
    void onSettings(cocos2d::CCObject*);
    void onDetails(cocos2d::CCObject*);
    void onWhatIsSigma(cocos2d::CCObject*);
    void onRanksInfo(cocos2d::CCObject*);
    void onGoAccount(cocos2d::CCObject*);
    void onClipLast(cocos2d::CCObject*);
    void onClipChoice(cocos2d::CCObject*);
    void onClipRetry(cocos2d::CCObject*);
    void onClipFolder(cocos2d::CCObject*);
    void onClipLink(cocos2d::CCObject*);
    // v0.12.1 Patreon plans (docs/contracts/patreon.md): the worker posts, the browser opens on the main thread
    void onPatreonConnect(cocos2d::CCObject*);
    void onPatreonSync(cocos2d::CCObject*);
    void onPatreonCode(cocos2d::CCObject*);

    Screen m_screen = Screen::Home;
    cocos2d::CCNode* m_page = nullptr;      // the screen (local coordinates 0..kContentW x 0..kContentH)
    cocos2d::CCNode* m_header = nullptr;    // title + status chips
    cocos2d::CCNode* m_tileBg[kScreenCount] = {};
    cocos2d::CCNode* m_tileAccent[kScreenCount] = {};
    cocos2d::CCNode* m_tileIcon[kScreenCount] = {};
    cocos2d::CCLabelBMFont* m_tileLabel[kScreenCount] = {};
    std::string m_pageKey;
    std::string m_headerKey;
    // Account screen: the sign-in code countdown is updated in place (no rebuild per second)
    bool m_codeVisible = false;
    cocos2d::CCLabelBMFont* m_codeCountdown = nullptr;
    CCMenuItemSpriteExtra* m_copyBtn = nullptr;
    std::string m_retryClipId;
    std::string m_linkClipId;
};

/// "Details" of the Level screen: every raw diagnostic line (trust, session, batches, solver
/// counters, analyzer, coverage, trace) in a scrollable list that follows the game on a 0.5 s tick.
class DetailsPopup : public geode::Popup {
public:
    static void open();

protected:
    bool init();
    void onTick(float dt);
    std::vector<std::pair<std::string, cocos2d::ccColor3B>> lines() const;

    geode::ScrollLayer* m_list = nullptr;
    std::string m_key;
};

}  // namespace gprl::ui
