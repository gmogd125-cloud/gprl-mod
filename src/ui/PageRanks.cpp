// Ranks screen of the GPRL menu: the ladder as the server lists it, with the owner's disclaimer
// (rank levels = ~10,000 attempts on a 100-second level) and the player's own row marked.
#include <algorithm>
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
    float const stripW = W - 2.f * kGap;
    auto note = makeParagraph("A rank's levels are what you could beat in about 10,000 attempts on a 100-second level - not a fast or first-try beat.",
                              stripW - 2.f * kPad - 24.f, kSmall, kWhite, 3);
    float const stripH = 20.f + note->getHeight() + 6.f;
    float const stripY = H - kGap - stripH;
    auto strip = card(m_page, kGap, stripY, stripW, stripH, nullptr, {70, 58, 22}, 225);
    text(strip, "What the level ranges mean", kPad, stripH - 5.f, stripW - 2.f * kPad - 24.f, kGold, 0.4f, kGoldFont);
    place(strip, note, kPad, stripH - 20.f);
    auto info = iconButton(menu, "GJ_infoIcon_001.png", 16.f, this, menu_selector(GprlMenu::onRanksInfo));
    info->setPosition({kGap + stripW - 14.f, stripY + stripH / 2.f});

    // where the player is: the official rank from the profile
    int youRank = -1;
    int youDivision = 0;
    bool haveProfile = st.connected && site.profileState.loaded && site.profileUsername == st.username;
    if (haveProfile && site.profile.rank) {
        youRank = ranks::findRank(list, site.profile.rank->rankId);
        youDivision = site.profile.rank->division;
    }

    float const listW = W - 2.f * kGap, listH = stripY - 2.f * kGap;
    auto scroll = ScrollLayer::create(CCSize{listW, listH});
    scroll->setPosition({kGap, kGap});
    scroll->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(3.f));
    m_page->addChild(scroll);
    float const rowW = listW - 2.f;
    float const textX = 54.f;
    float const fullW = rowW - textX - 10.f;

    for (size_t i = 0; i < list.ranks.size(); ++i) {
        auto const& r = list.ranks[i];
        bool you = static_cast<int>(i) == youRank;
        bool ascendant = r.kind == ranks::Kind::Ascendant;
        bool unreached = ascendant && !r.reached;
        GLubyte const alpha = unreached ? 110 : 255;
        // the description wraps, so the row's height follows it
        SimpleTextArea* desc = nullptr;
        if (!ascendant && !r.description.empty()) desc = makeParagraph(r.description, fullW, kTiny, kGrey, 2);
        float const h = ascendant ? 38.f : std::max(46.f, 6.f + 14.f + 11.f + 10.f + (desc ? desc->getHeight() + 3.f : 0.f) + 6.f);
        ccColor3B tint = you ? toCc(ranks::scaled(r.color, 0.32)) : kCard;
        auto row = CCNode::create();
        row->setContentSize({rowW, h});
        panel(row, 0.f, 0.f, rowW, h, tint, unreached ? 90 : (you ? 245 : 200), -1);
        auto b = badge(&r, ascendant ? 0 : (you ? youDivision : 0), ascendant ? 28.f : 40.f, unreached);
        b->setPosition({27.f, h / 2.f});
        row->addChild(b);

        // the right-aligned tag first: the name is clipped to the room left of it
        float const nameY = h - 13.f;
        float nameRoom = fullW;
        CCNode* tag = nullptr;
        if (unreached) tag = chip("Unreached", {60, 60, 70}, kGrey, 0.28f);
        else if (you) tag = chip("YOU ARE HERE", toCc(ranks::scaled(r.color, 0.55)), kWhite, 0.28f);
        else if (r.playerCount > 0) {
            auto t = text(row, fmt::format("{} player{}", r.playerCount, r.playerCount == 1 ? "" : "s"), rowW - 8.f, nameY, 90.f, kGrey, kTiny, kChat, {1.f, 0.5f});
            nameRoom -= t->getScaledContentSize().width + 10.f;
        }
        if (tag) {
            tag->setPosition({rowW - 8.f - tag->getContentSize().width, nameY});
            row->addChild(tag);
            nameRoom -= tag->getContentSize().width + 10.f;
        }
        std::string title = ascendant ? fmt::format("{}   {} sigma/s", r.name, ranks::formatBands(r)) : r.name;
        auto nameLabel = text(row, title, textX, nameY, std::max(60.f, nameRoom), unreached ? kDim : toCc(r.color), 0.46f * kGoldToBig, kBig, {0.f, 0.5f});
        nameLabel->setOpacity(alpha);
        if (ascendant) {
            auto req = text(row, "Needs: " + ranks::formatRequirements(r.requirements), textX, h - 27.f, fullW, kGrey, kTiny, kChat, {0.f, 0.5f});
            req->setOpacity(alpha);
        }
        else {
            auto bands = text(row, ranks::formatBands(r) + " sigma/s", textX, h - 26.f, fullW, kWhite, 0.46f, kChat, {0.f, 0.5f});
            bands->setOpacity(alpha);
            auto req = text(row, "Needs: " + ranks::formatRequirements(r.requirements), textX, h - 37.f, fullW, kGrey, kTiny, kChat, {0.f, 0.5f});
            req->setOpacity(alpha);
            if (desc) place(row, desc, textX, h - 44.f);
        }
        scroll->m_contentLayer->addChild(row);
    }
    scroll->m_contentLayer->updateLayout();
    scroll->scrollToTop();
}

}  // namespace gprl::ui
