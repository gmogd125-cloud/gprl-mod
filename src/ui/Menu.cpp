#include "Menu.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/MDPopup.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <cctype>

#include "../../core/display.hpp"
#include "../../core/identity.hpp"
#include "../Clipper.hpp"
#include "../Connect.hpp"
#include "../PatreonCodePopup.hpp"
#include "../Settings.hpp"
#include "../Tracker.hpp"
#include "../clip/ClipLinkPopup.hpp"
#include "Widgets.hpp"
#include "../Hud.hpp"

using namespace geode::prelude;

namespace gprl::ui {

namespace {

constexpr float kTickSeconds = 0.5f;

struct TabSpec {
    char const* label;
    char const* icon;
    char const* title;
};
constexpr TabSpec kTabs[kScreenCount] = {
    {"Home", "GJ_profileButton_001.png", "GPRL"},
    {"Ranks", "GJ_bigStar_001.png", "Rank ladder"},
    {"Board", "GJ_levelLeaderboardBtn_001.png", "Leaderboard"},
    {"Level", "GJ_playBtn_001.png", "This level"},
    {"Account", "GJ_optionsBtn_001.png", "Account"},
};

/// Remembered while the game runs: the menu reopens on the screen it was closed on.
Screen s_lastScreen = Screen::Home;
bool s_everOpened = false;

}  // namespace

GprlMenu* GprlMenu::create() {
    auto ret = new GprlMenu();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool GprlMenu::init() {
    using namespace theme;
    if (!Popup::init(kWidth, kHeight, "GJ_square02.png")) return false;

    // sidebar + content backgrounds
    panel(m_mainLayer, kSideX, 8.f, kSideW, kHeight - 16.f, kPanel, 200);
    panel(m_mainLayer, kContentX, kContentY, kContentW, kContentH, kPanel, 200);

    buildSidebar();

    m_header = CCNode::create();
    m_header->setPosition({0.f, 0.f});
    m_mainLayer->addChild(m_header, 2);

    m_page = CCNode::create();
    m_page->setContentSize({kContentW, kContentH});
    m_page->setAnchorPoint({0.f, 0.f});
    m_page->setPosition({kContentX, kContentY});
    m_mainLayer->addChild(m_page, 1);

    // first open: Home shows the welcome screen when not connected; later opens remember the screen
    Screen start = s_everOpened ? s_lastScreen : Screen::Home;
    s_everOpened = true;
    m_codeVisible = !connect::websiteCode().code.empty();
    selectScreen(start);
    this->schedule(schedule_selector(GprlMenu::onTick), kTickSeconds);
    return true;
}

void GprlMenu::onClose(CCObject* sender) {
    s_lastScreen = m_screen;
    Popup::onClose(sender);
}

// ---- sidebar ----

void GprlMenu::buildSidebar() {
    using namespace theme;
    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_mainLayer->addChild(menu, 3);
    float const cx = kSideX + kSideW / 2.f;
    for (int i = 0; i < kScreenCount; ++i) {
        auto holder = CCNode::create();
        holder->setContentSize({kTileW, kTileH});
        holder->setAnchorPoint({0.5f, 0.5f});
        auto bg = panel(holder, 0.f, 0.f, kTileW, kTileH, kCard, 190, -1);
        m_tileBg[i] = bg;
        // selected marker: a thin cyan bar along the left edge
        auto accent = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        accent->setContentSize({3.f, kTileH - 10.f});
        accent->setAnchorPoint({0.f, 0.5f});
        accent->setPosition({1.5f, kTileH / 2.f});
        accent->setColor(kCyan);
        accent->setVisible(false);
        holder->addChild(accent, 1);
        m_tileAccent[i] = accent;
        CCNode* ic = icon(kTabs[i].icon, 19.f);
        if (!ic) {
            auto l = CCLabelBMFont::create(kTabs[i].label, kBig);
            l->setScale(0.3f);
            ic = l;
        }
        ic->setPosition({kTileW / 2.f, kTileH - 15.f});
        holder->addChild(ic, 1);
        m_tileIcon[i] = ic;
        auto label = text(holder, kTabs[i].label, kTileW / 2.f, 9.f, kTileW - 6.f, kGrey, 0.3f, kBig, {0.5f, 0.5f});
        m_tileLabel[i] = label;
        auto btn = CCMenuItemSpriteExtra::create(holder, this, menu_selector(GprlMenu::onTab));
        btn->setTag(i);
        btn->setPosition({cx, kSideTop - kTileH / 2.f - static_cast<float>(i) * (kTileH + kTileGap)});
        menu->addChild(btn);
    }
    // the mod version at the bottom of the sidebar
    text(m_mainLayer, "v" + Mod::get()->getVersion().toNonVString(), cx, 17.f, kSideW - 8.f, kDim, 0.3f, kChat, {0.5f, 0.5f});
}

void GprlMenu::onTab(CCObject* sender) {
    int i = static_cast<CCNode*>(sender)->getTag();
    if (i < 0 || i >= kScreenCount) return;
    selectScreen(static_cast<Screen>(i));
}

void GprlMenu::selectScreen(Screen screen) {
    using namespace theme;
    m_screen = screen;
    s_lastScreen = screen;
    for (int i = 0; i < kScreenCount; ++i) {
        bool on = i == static_cast<int>(screen);
        if (auto* bg = typeinfo_cast<CCScale9Sprite*>(m_tileBg[i])) {
            bg->setColor(on ? kCardSelected : kCard);
            bg->setOpacity(on ? 235 : 190);
        }
        m_tileAccent[i]->setVisible(on);
        if (m_tileLabel[i]) m_tileLabel[i]->setColor(on ? kWhite : kGrey);
        if (auto* rgba = typeinfo_cast<CCSprite*>(m_tileIcon[i])) rgba->setOpacity(on ? 255 : 170);
    }
    m_pageKey.clear();
    m_codeCountdown = nullptr;
    m_copyBtn = nullptr;
    refresh();
}

// ---- tick ----

void GprlMenu::onTick(float) { refresh(); }

void GprlMenu::refresh() {
    requestData();
    refreshHeader();
    switch (m_screen) {
        case Screen::Home: buildHome(); break;
        case Screen::Ranks: buildRanks(); break;
        case Screen::Board: buildBoard(); break;
        case Screen::Level: buildLevel(); break;
        case Screen::Account:
            buildAccount();
            tickAccount();
            break;
    }
}

void GprlMenu::requestData() {
    auto st = client::status();
    // ask the worker for what the screen shows (cached per TTL; a no-op most ticks)
    switch (m_screen) {
        case Screen::Home:
        case Screen::Ranks:
            client::requestSiteData(client::SiteKind::Ranks);
            if (st.connected) client::requestSiteData(client::SiteKind::Profile, st.username);
            break;
        case Screen::Board:
            client::requestSiteData(client::SiteKind::Board);
            client::requestSiteData(client::SiteKind::Ranks);
            break;
        case Screen::Level: {
            auto ses = tracker::session();
            if (!ses.levelId.empty()) client::requestSiteData(client::SiteKind::Coverage, ses.levelId);
            break;
        }
        case Screen::Account: break;
    }
}

void GprlMenu::refreshHeader() {
    using namespace theme;
    auto st = client::status();
    // status chips, right-aligned: connection, LIVE, local-only, Record-Safe, offline
    struct Chip {
        std::string text;
        ccColor3B bg;
    };
    std::vector<Chip> chips;
    if (st.connecting) chips.push_back({"Connecting...", {120, 100, 30}});
    else if (st.connected) chips.push_back({"Connected", {30, 110, 60}});
    else chips.push_back({"Not connected", {140, 70, 40}});
    display::LiveFacts f;
    f.enabled = settings::get().enabled;
    f.localOnly = st.localOnly;
    f.connected = st.connected;
    f.sessionOpen = st.sessionOpen;
    f.remote = st.mode == client::SessionMode::Remote;
    f.nowMs = client::steadyNowMs();
    f.lastOkMs = st.lastBatchOkMs;
    f.lastFailMs = st.lastBatchFailMs;
    f.eventsPending = st.eventsPending;
    if (display::liveNow(f)) chips.push_back({"LIVE", kLiveRed});
    else if (st.sessionOpen && st.mode == client::SessionMode::Unsent) chips.push_back({"Offline", {90, 90, 100}});
    if (st.localOnly) chips.push_back({st.apiPlaceholder ? "API not set" : "Local-only", {130, 100, 30}});
    if (settings::get().recordSafe) chips.push_back({"Record-Safe", {110, 80, 150}});
    if (!settings::get().enabled) chips.push_back({"Mod disabled", {140, 50, 50}});

    std::string key = kTabs[static_cast<int>(m_screen)].title;
    for (auto const& c : chips) key += "|" + c.text;
    if (key == m_headerKey) return;
    m_headerKey = key;
    m_header->removeAllChildren();
    text(m_header, kTabs[static_cast<int>(m_screen)].title, kContentX + 4.f, kHeaderY, 170.f, kGold, 0.55f, kGoldFont, {0.f, 0.5f});
    float right = kContentX + kContentW;
    for (auto it = chips.rbegin(); it != chips.rend(); ++it) {
        auto c = chip(it->text, it->bg, kWhite, 0.3f);
        right -= c->getContentSize().width;
        c->setPosition({right, kHeaderY});
        m_header->addChild(c);
        right -= 5.f;
    }
}

// ---- shared helpers ----

bool GprlMenu::beginPage(std::string const& key) {
    if (key == m_pageKey) return false;
    m_pageKey = key;
    m_page->removeAllChildren();
    m_codeCountdown = nullptr;
    m_copyBtn = nullptr;
    return true;
}

CCMenu* GprlMenu::pageMenu() {
    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    m_page->addChild(menu, 5);
    return menu;
}

void GprlMenu::emptyState(std::string const& title, std::string const& line1, std::string const& line2) {
    using namespace theme;
    float const cx = kContentW / 2.f;
    text(m_page, title, cx, kContentH * 0.62f, kContentW - 40.f, kGold, 0.55f, kGoldFont, {0.5f, 0.5f});
    if (!line1.empty()) text(m_page, line1, cx, kContentH * 0.62f - 22.f, kContentW - 40.f, kWhite, 0.42f, kChat, {0.5f, 0.5f});
    if (!line2.empty()) text(m_page, line2, cx, kContentH * 0.62f - 36.f, kContentW - 40.f, kGrey, 0.38f, kChat, {0.5f, 0.5f});
}

bool GprlMenu::siteState(client::SiteData const& site, client::SiteFetchState const& st, char const* what) {
    using namespace theme;
    if (st.loaded) return true;
    if (!st.error.empty()) {
        emptyState(fmt::format("Could not load the {}", what), st.error,
                   site.online ? "Retrying every 10 s while this screen is open" : "Turn off local-only mode / set the API URL in the mod settings");
        return false;
    }
    if (!site.online) {
        emptyState("Offline", fmt::format("The {} needs the GPRL server.", what), "Local-only mode is on or the API URL is not set (mod settings).");
        return false;
    }
    emptyState("Loading...", fmt::format("Asking the GPRL server for the {}.", what));
    return false;
}

ranks::RankList const* GprlMenu::ladder(client::SiteData const& site) const { return site.ranksState.loaded ? &site.ranks : nullptr; }

std::string GprlMenu::rankLabel(ranks::RankList const* list, ranks::PlayerRank const& pr) const {
    if (list) {
        int i = ranks::findRank(*list, pr.rankId);
        if (i >= 0) return ranks::bandName(list->ranks[static_cast<size_t>(i)], pr.division);
    }
    std::string name = pr.rankId;
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (pr.division > 0) name += " " + ranks::divisionNumeral(pr.division);
    return name;
}

std::string GprlMenu::coverageText(client::SiteData const& site, std::string const& levelId, ccColor3B& color) const {
    using namespace theme;
    color = kGrey;
    if (levelId.empty()) return "Level Analysis Coverage: no level played yet";
    if (!site.online && !site.coverageState.loaded) {
        if (settings::effectiveLocalOnly()) return "Level Analysis Coverage: not fetched (local-only mode / API not set)";
    }
    bool mine = site.coverageLevelId == levelId;
    if (mine && site.coverageState.loaded) {
        color = site.coverage.complete ? kGreen : (site.coverage.levelCounts ? kWhite : kGrey);
        return display::coverageLine(site.coverage);
    }
    if (mine && site.coverageNotFound) return "Level Analysis Coverage: 0% (the server has not seen this level yet)";
    if (mine && !site.coverageState.error.empty()) {
        color = kOrange;
        return "Level Analysis Coverage: unavailable - " + site.coverageState.error;
    }
    return "Level Analysis Coverage: loading...";
}

// ---- actions ----

void GprlMenu::onConnect(CCObject*) {
    connect::start();
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onDisconnect(CCObject*) { connect::disconnect(); }

void GprlMenu::onResetData(CCObject*) { connect::resetData(); }

void GprlMenu::onProfile(CCObject*) {
    connect::openProfile();
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onWebsiteCode(CCObject*) {
    connect::requestWebsiteCode();   // async: the worker posts, the result lands in connect::websiteCode()
    m_codeVisible = client::status().connected;
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onCopyCode(CCObject*) {
    if (connect::copyWebsiteCode()) hud::notify("GPRL: code copied");
}

void GprlMenu::onHideCode(CCObject*) {
    m_codeVisible = false;
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onVisitSite(CCObject*) { web::openLinkInBrowser(settings::get().siteOrigin); }

void GprlMenu::onFlush(CCObject*) {
    client::requestFlush();
    hud::notify("GPRL: flushing pending events");
}

void GprlMenu::onSettings(CCObject*) { geode::openSettingsPopup(Mod::get()); }

void GprlMenu::onDetails(CCObject*) { DetailsPopup::open(); }

void GprlMenu::onGoAccount(CCObject*) { selectScreen(Screen::Account); }

// Both explainers are scrollable Markdown popups (owner feedback 2026-10-02: a plain FLAlertLayer
// let the text run off the screen).
void GprlMenu::onWhatIsSigma(CCObject*) {
    MDPopup::create("What is sigma/s?",
        "**sigma/s** is GPRL's precision score: one divided by the spread of your timing error, in seconds.\n\n"
        "At **100 sigma/s** most of your inputs land within about 10 ms of the ideal moment; at **200** within about 5 ms.\n\n"
        "While you play, the mod finds the exact timing window of every click with a hidden copy of the game. The GPRL server fits your "
        "precision from thousands of those windows: the mod measures, only the server rates.\n\n"
        "Your sigma/s stays **LOCKED** until your calibration is complete and the server is confident enough. Your rank follows once the "
        "Rating Confidence reaches the ladder's minimum (90%).",
        "OK")
        ->show();
}

void GprlMenu::onRanksInfo(CCObject*) {
    MDPopup::create("What the rank levels mean",
        "The levels named for each rank are what you could beat in about **10,000 attempts**, if the level were **100 seconds** long.\n\n"
        "It is not how fast or how first-try you would beat it. Fewer attempts, or a longer level, needs a higher sigma/s. More attempts "
        "or a shorter level needs less.\n\n"
        "Every threshold on this ladder is GPRL server configuration and can change; nothing is hardcoded in the mod.",
        "OK")
        ->show();
}

// ---- clipping (every upload starts from one of these explicit presses or the clip popup) ----

void GprlMenu::onClipLast(CCObject*) {
    clipper::preserveLastAttempt();   // opens the four-choice popup for it
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onClipChoice(CCObject*) { clipper::offerPendingClip(false); }

void GprlMenu::onClipRetry(CCObject*) {
    if (!m_retryClipId.empty()) clipper::retryUpload(m_retryClipId);
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onClipFolder(CCObject*) { clipper::openClipsFolder(); }

void GprlMenu::onClipLink(CCObject*) {
    if (!m_linkClipId.empty()) clip::ClipLinkPopup::open(m_linkClipId);
}

// ---- Patreon (v0.12.1): both requests run on the telemetry worker; the outcome is a notification ----

void GprlMenu::onPatreonConnect(CCObject*) {
    client::requestPatreonConnect();
    m_pageKey.clear();
    refresh();
}

void GprlMenu::onPatreonSync(CCObject*) {
    client::requestPatreonSync();
    m_pageKey.clear();
    refresh();
}

// security review: the one-time code from the browser page confirms the link (PatreonCodePopup)
void GprlMenu::onPatreonCode(CCObject*) { patreon::PatreonCodePopup::open(); }

void open() {
    if (auto p = GprlMenu::create()) p->show();
}

}  // namespace gprl::ui
