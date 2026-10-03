// Ranks screen of the GPRL menu: the ladder as the server lists it, with the owner's disclaimer
// (rank levels = ~10,000 attempts on a 100-second level) and the player's own row marked.
#include <cmath>

#include "Menu.hpp"
#include "Widgets.hpp"

using namespace geode::prelude;

namespace gprl::ui {

namespace {
using namespace theme;
ccColor3B toCc(ranks::Color c) { return ccColor3B{c.r, c.g, c.b}; }
}  // namespace

void GprlMenu::buildRanks() {
    auto st = client::status();
    auto site = client::siteData();
    std::string key = fmt::format("ranks|{}|{}|{}|{}|{}|{}", site.ranksState.generation, site.ranksState.loaded, site.ranksState.error, site.profileState.generation,
                                  st.connected ? st.username : "", site.online);
    if (!beginPage(key)) return;
    if (!siteState(site, site.ranksState, "rank ladder")) return;
    auto const& list = site.ranks;
    if (list.ranks.empty()) {
        emptyState("No ranks yet", "The server lists no ranks.");
        return;
    }
    float const W = kContentW, H = kContentH;
    auto menu = pageMenu();

    // the disclaimer strip (owner wording, 2026-10-02) with the full text behind the info button
    float const stripH = 34.f;
    auto strip = card(m_page, 6.f, H - 6.f - stripH, W - 12.f, stripH, nullptr, {70, 58, 22}, 225);
    text(strip, "What the level ranges mean", kPad, stripH - 9.f, W - 60.f, kGold, 0.36f, kGoldFont, {0.f, 0.5f});
    text(strip, "A rank's levels are what you could beat in about 10,000 attempts on a 100-second level - not a fast or first-try beat.", kPad, stripH - 23.f, W - 48.f,
         kWhite, 0.3f, kChat, {0.f, 0.5f});
    auto info = iconButton(menu, "GJ_infoIcon_001.png", 16.f, this, menu_selector(GprlMenu::onRanksInfo));
    info->setPosition({W - 20.f, H - 6.f - stripH / 2.f});

    // where the player is: the official rank from the profile
    int youRank = -1;
    int youDivision = 0;
    bool haveProfile = st.connected && site.profileState.loaded && site.profileUsername == st.username;
    if (haveProfile && site.profile.rank) {
        youRank = ranks::findRank(list, site.profile.rank->rankId);
        youDivision = site.profile.rank->division;
    }

    float const listW = W - 12.f, listH = H - 18.f - stripH;
    auto scroll = ScrollLayer::create(CCSize{listW, listH});
    scroll->setPosition({6.f, 6.f});
    scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(3.f));
    m_page->addChild(scroll);
    float const rowW = listW - 2.f;

    for (size_t i = 0; i < list.ranks.size(); ++i) {
        auto const& r = list.ranks[i];
        bool you = static_cast<int>(i) == youRank;
        bool ascendant = r.kind == ranks::Kind::Ascendant;
        bool unreached = ascendant && !r.reached;
        float const h = ascendant ? 38.f : 56.f;
        ccColor3B tint = you ? toCc(ranks::scaled(r.color, 0.32)) : kCard;
        auto row = CCNode::create();
        row->setContentSize({rowW, h});
        panel(row, 0.f, 0.f, rowW, h, tint, unreached ? 90 : (you ? 240 : 200), -1);
        if (you) {
            auto accent = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
            accent->setContentSize({3.f, h - 8.f});
            accent->setAnchorPoint({0.f, 0.5f});
            accent->setPosition({1.5f, h / 2.f});
            accent->setColor(toCc(r.color));
            row->addChild(accent);
        }
        GLubyte const alpha = unreached ? 110 : 255;
        auto b = badge(&r, ascendant ? 0 : (you ? youDivision : 0), ascendant ? 28.f : 40.f, unreached);
        b->setPosition({26.f, h / 2.f});
        row->addChild(b);

        float const textX = 52.f;
        // the right-aligned tag first: the name is clipped to the room left of it
        float nameRoom = rowW - textX - 10.f;
        CCNode* tag = nullptr;
        if (unreached) tag = chip("Unreached", {60, 60, 70}, kGrey, 0.24f);
        else if (you) tag = chip("YOU ARE HERE", toCc(ranks::scaled(r.color, 0.55)), kWhite, 0.26f);
        else if (r.playerCount > 0) {
            auto t = text(row, fmt::format("{} player{}", r.playerCount, r.playerCount == 1 ? "" : "s"), rowW - 8.f, h - 11.f, 90.f, kGrey, 0.3f, kChat, {1.f, 0.5f});
            nameRoom -= t->getScaledContentSize().width + 10.f;
        }
        if (tag) {
            tag->setPosition({rowW - 8.f - tag->getContentSize().width, h - 11.f});
            row->addChild(tag);
            nameRoom -= tag->getContentSize().width + 10.f;
        }
        std::string title = ascendant ? fmt::format("{}   {} sigma/s", r.name, ranks::formatBands(r)) : r.name;
        auto nameLabel = text(row, title, textX, h - 11.f, std::max(60.f, nameRoom), unreached ? kDim : toCc(r.color), 0.44f, kGoldFont, {0.f, 0.5f});
        nameLabel->setOpacity(alpha);
        float const fullW = rowW - textX - 10.f;
        float y = h - 24.f;
        if (!ascendant) {
            auto bands = text(row, ranks::formatBands(r) + " sigma/s", textX, y, fullW, unreached ? kDim : kWhite, 0.36f, kChat, {0.f, 0.5f});
            bands->setOpacity(alpha);
            y -= 11.f;
        }
        auto req = text(row, "Needs: " + ranks::formatRequirements(r.requirements), textX, y, fullW, kGrey, 0.3f, kChat, {0.f, 0.5f});
        req->setOpacity(alpha);
        y -= 11.f;
        if (!ascendant && !r.description.empty()) {
            auto d = text(row, r.description, textX, y, fullW, kGrey, 0.3f, kChat, {0.f, 0.5f});
            d->setOpacity(alpha);
        }
        scroll->m_contentLayer->addChild(row);
    }
    scroll->m_contentLayer->updateLayout();
    scroll->scrollToTop();
}

}  // namespace gprl::ui
