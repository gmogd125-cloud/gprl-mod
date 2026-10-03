// Board screen of the GPRL menu: the top 25 by verified sigma/s, the player's row highlighted,
// the player's own position in the footer.
#include "Menu.hpp"
#include "Widgets.hpp"

using namespace geode::prelude;

namespace gprl::ui {

namespace {
using namespace theme;
ccColor3B toCc(ranks::Color c) { return ccColor3B{c.r, c.g, c.b}; }
constexpr ccColor3B kMedal[3] = {{255, 215, 90}, {210, 215, 225}, {205, 140, 90}};
}  // namespace

void GprlMenu::buildBoard() {
    auto st = client::status();
    auto site = client::siteData();
    bool haveProfile = st.connected && site.profileState.loaded && site.profileUsername == st.username;
    int myPosition = haveProfile ? site.profile.leaderboardPosition : 0;
    std::string key = fmt::format("board|{}|{}|{}|{}|{}|{}|{}", site.boardState.generation, site.boardState.loaded, site.boardState.error, site.ranksState.generation,
                                  st.connected ? st.username : "", site.online, myPosition);
    if (!beginPage(key)) return;
    if (!siteState(site, site.boardState, "leaderboard")) return;
    auto const& board = site.board;
    float const W = kContentW, H = kContentH;
    if (board.rows.empty()) {
        emptyState("No verified ratings yet", "The first players are still calibrating.", "Your name appears here once your sigma/s unlocks and your runs are verified.");
        return;
    }
    ranks::RankList const* list = ladder(site);

    // footer: the player's own place
    std::string foot;
    ccColor3B footColor = kGrey;
    if (!st.connected) foot = "Connect (Account) to see your own place here";
    else if (myPosition > 0) {
        foot = fmt::format("Your position: #{}", myPosition);
        footColor = kGreen;
    }
    else foot = "Not on the board yet. It needs an unlocked sigma/s and verified runs.";
    text(m_page, foot, W / 2.f, 11.f, W - 20.f, footColor, kSmall, kChat, {0.5f, 0.5f});

    float const listW = W - 2.f * kGap, listH = H - 22.f - kGap;
    auto scroll = ScrollLayer::create(CCSize{listW, listH});
    scroll->setPosition({kGap, 22.f});
    scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(2.f));
    m_page->addChild(scroll);
    float const rowW = listW - 2.f;
    float const h = 26.f;

    for (auto const& row : board.rows) {
        bool you = st.connected && !st.username.empty() && row.username == st.username;
        auto node = CCNode::create();
        node->setContentSize({rowW, h});
        panel(node, 0.f, 0.f, rowW, h, you ? ccColor3B{30, 90, 50} : kCard, you ? 245 : 200, -1);
        int pos = row.position;
        ccColor3B posColor = pos >= 1 && pos <= 3 ? kMedal[pos - 1] : kGold;
        bool medalDrawn = false;
        if (pos == 1) {
            if (auto* medal = icon("rankIcon_1_001.png", 19.f)) {
                medal->setPosition({18.f, h / 2.f});
                node->addChild(medal);
                medalDrawn = true;
            }
        }
        if (!medalDrawn) text(node, fmt::format("#{}", pos), 8.f, h / 2.f, 36.f, posColor, 0.42f, kGoldFont, {0.f, 0.5f});
        ranks::Rank const* rank = nullptr;
        int division = 0;
        if (row.rank && list) {
            int i = ranks::findRank(*list, row.rank->rankId);
            if (i >= 0) rank = &list->ranks[static_cast<size_t>(i)];
            division = row.rank->division;
        }
        auto b = badge(rank, division, 22.f, false);
        b->setPosition({58.f, h / 2.f});
        node->addChild(b);
        text(node, row.displayName, 76.f, h / 2.f, 140.f, you ? kGreen : kWhite, 0.4f, kBig, {0.f, 0.5f}, 0.28f);
        std::string rankText = row.rank ? (rank ? ranks::bandName(*rank, division) : rankLabel(list, *row.rank)) : std::string();
        if (!rankText.empty()) text(node, rankText, rowW - 66.f, h / 2.f, 92.f, rank ? toCc(rank->color) : kGrey, kSmall, kChat, {1.f, 0.5f});
        std::string sigmaText = row.sigma ? ranks::formatSigma(*row.sigma) : std::string("-");
        text(node, sigmaText, rowW - 8.f, h / 2.f, 54.f, kWhite, 0.4f, kBig, {1.f, 0.5f});
        scroll->m_contentLayer->addChild(node);
    }
    scroll->m_contentLayer->updateLayout();
    scroll->scrollToTop();
}

}  // namespace gprl::ui
