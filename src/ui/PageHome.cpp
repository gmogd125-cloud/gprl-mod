// Home screen of the GPRL menu: the welcome screen while not connected, the player's profile
// once connected (badge, sigma/s hero or calibration ring, verification, gamemodes).
#include <algorithm>
#include <cmath>

#include "../../core/identity.hpp"
#include "../../core/vocab.hpp"
#include "../Connect.hpp"
#include "../Settings.hpp"
#include "Menu.hpp"
#include "Widgets.hpp"

using namespace geode::prelude;

namespace gprl::ui {

namespace {

using namespace theme;

std::string num(std::optional<double> v) { return v ? ranks::formatSigma(*v) : std::string("-"); }

/// "What to play next" while the rating is locked: the server's own words for the two counted
/// requirements (core/calibration calibrationHint, the exact mirror of the website's screen) and
/// plain advice for the rest. At most `max` lines.
std::vector<std::string> nextSteps(CalibrationState const& cal, CalibrationDisplay const& disp, size_t max) {
    std::vector<std::string> out;
    // Owner request 2026-10-02: first the gamemodes the sigma/s still waits for (the server's
    // `missingGamemodes`), then the rest.
    for (auto& s : missingGamemodeSteps(disp)) {
        if (out.size() >= max) return out;
        out.push_back(std::move(s));
    }
    if (cal.complete) {
        if (out.size() < max)
            out.push_back("Calibration complete: keep playing, the server's confidence in your rating grows with every rated session.");
        return out;
    }
    if (cal.effectiveSamples.current < cal.effectiveSamples.required) {
        auto hint = calibrationHint("effectiveSamples", cal.effectiveSamples.current, cal.effectiveSamples.required, false);
        out.push_back(hint.value_or(fmt::format("Keep playing: {} of {} effective timings so far.", static_cast<int>(std::floor(cal.effectiveSamples.current)),
                                                static_cast<int>(cal.effectiveSamples.required))));
    }
    if (cal.gamemodes.current < cal.gamemodes.required && out.size() < max) {
        auto hint = calibrationHint("gamemodes", cal.gamemodes.current, cal.gamemodes.required, false);
        out.push_back(hint.value_or(fmt::format("Play more gamemodes: {} of {} have enough timings.", static_cast<int>(cal.gamemodes.current),
                                                static_cast<int>(cal.gamemodes.required))));
    }
    if (cal.releaseData != ReleaseData::Sufficient && out.size() < max)
        out.push_back("Play sections with holds and releases (ship, wave, ufo, swing): release timings count too.");
    if (cal.difficultyCoverage < 1.0 && out.size() < max)
        out.push_back("Play close to your limit: timings you land 30-95% of the time teach the model the most.");
    if (out.empty()) out.push_back("Almost there: a few more rated sessions finish the calibration.");
    return out;
}

ccColor3B verificationColor(std::string const& status) {
    if (status == "auto_verified" || status == "verified") return kGreen;
    if (status == "pending_manual_verification") return kGold;
    if (status == "unverified") return kOrange;
    if (status == "invalid") return kRed;
    return kGrey;
}

ccColor3B toCc(ranks::Color c) { return ccColor3B{c.r, c.g, c.b}; }

}  // namespace

void GprlMenu::buildHome() {
    auto st = client::status();
    if (!st.connected) {
        buildWelcome(st);
        return;
    }
    buildProfile(st, client::siteData());
}

// ---- welcome (not connected) ----

void GprlMenu::buildWelcome(client::Status const& st) {
    bool const failed = !st.connecting && st.connectMessage.rfind("Connect failed", 0) == 0;
    std::string key = fmt::format("welcome|{}|{}|{}|{}|{}|{}", static_cast<int>(st.connection), st.connecting, st.connectMessage, st.localOnly,
                                  st.apiPlaceholder, connect::inProgress());
    if (!beginPage(key)) return;
    float const W = kContentW, H = kContentH;

    // hero: the sigma/s logo, a title and one sentence
    if (auto* logo = modSprite("gprl_icon.png")) {
        logo->setScale(56.f / std::max(1.f, logo->getContentSize().width));
        logo->setPosition({38.f, H - 40.f});
        m_page->addChild(logo);
    }
    text(m_page, "Welcome to GPRL", 74.f, H - 24.f, W - 110.f, kGold, 0.62f, kGoldFont, {0.f, 0.5f});
    paragraph(m_page,
              "The Geometry Precision Ranking List measures how precisely you click: the exact timing window of every input, fitted into one number - "
              "your sigma/s - and a rank.",
              74.f, H - 38.f, W - 100.f, 0.4f, kGrey);

    // three steps
    float const gap = 8.f;
    float const cardW = (W - 2.f * gap - 2.f * gap) / 3.f;
    float const cardH = 92.f;
    float const cardY = 66.f;
    struct Step {
        char const* title;
        char const* icon;
        char const* body;
    };
    Step const steps[3] = {
        {"1. Connect", "GJ_profileButton_001.png", "Press Connect: GPRL uses the Geometry Dash account you are logged in with. No website account, no password."},
        {"2. Play", "GJ_playBtn_001.png", "Play any level. The mod measures the timing window of each click and your calibration grows (top-right HUD)."},
        {"3. Get ranked", "GJ_bigStar_001.png", "At full calibration your sigma/s unlocks; at 90% Rating Confidence you get your rank and a board place."},
    };
    for (int i = 0; i < 3; ++i) {
        float x = gap + static_cast<float>(i) * (cardW + gap);
        auto c = card(m_page, x, cardY, cardW, cardH, nullptr, kCard, 230);
        if (auto* ic = icon(steps[i].icon, 22.f)) {
            ic->setPosition({16.f, cardH - 16.f});
            c->addChild(ic);
        }
        text(c, steps[i].title, 30.f, cardH - 16.f, cardW - 36.f, kGold, 0.42f, kGoldFont, {0.f, 0.5f});
        paragraph(c, steps[i].body, 8.f, cardH - 30.f, cardW - 16.f, 0.33f, kWhite);
    }

    // the one thing to do
    auto menu = pageMenu();
    bool busy = connect::inProgress() || st.connecting;
    auto btn = wideButton(menu, busy ? "Connecting..." : "Connect with my GD account", 230, "GJ_button_01.png", 0.7f, this, menu_selector(GprlMenu::onConnect));
    btn->setPosition({W / 2.f, 40.f});
    setButtonEnabled(btn, !busy);
    auto info = iconButton(menu, "GJ_infoIcon_001.png", 18.f, this, menu_selector(GprlMenu::onWhatIsSigma));
    info->setPosition({W - 14.f, H - 14.f});

    // the state in one line
    std::string status;
    ccColor3B color = kGrey;
    if (st.connecting) status = st.connectMessage.empty() ? std::string("Connecting...") : st.connectMessage;
    else if (failed) {
        status = st.connectMessage;
        color = kOrange;
    }
    else if (st.apiPlaceholder) {
        status = "The API URL is not set: nothing can be sent until you fix it in the mod settings";
        color = kOrange;
    }
    else if (st.localOnly) {
        status = "Local-only mode is on: telemetry stays on this computer and nothing counts for a rating";
        color = kOrange;
    }
    else status = identity::describe(st.connection, "");
    text(m_page, status, W / 2.f, 14.f, W - 20.f, color, 0.36f, kChat, {0.5f, 0.5f});
}

// ---- profile (connected) ----

void GprlMenu::buildProfile(client::Status const& st, client::SiteData const& site) {
    ranks::RankList const* list = ladder(site);
    bool const haveProfile = site.profileState.loaded && site.profileUsername == st.username;
    ranks::Profile const* p = haveProfile ? &site.profile : nullptr;
    CalibrationDisplay disp = client::displayRating();
    CalibrationState cal = client::displayCalibration();

    // the official rank: the site profile first, else the calibration answer's `ranked` label
    ranks::Rank const* rank = nullptr;
    int division = 0;
    std::optional<ranks::PlayerRank> officialRank;
    if (p) officialRank = p->rank;
    if (!officialRank && disp.state == RatingDisplay::Ranked && !disp.rankId.empty()) officialRank = ranks::PlayerRank{disp.rankId, disp.rankDivision};
    if (officialRank && list) {
        int i = ranks::findRank(*list, officialRank->rankId);
        if (i >= 0) rank = &list->ranks[static_cast<size_t>(i)];
        division = officialRank->division;
    }
    bool const sigmaVisible = (p && !p->ratingsLocked) || !disp.locked();
    bool const fromSite = p && !p->ratingsLocked;
    auto verified = fromSite ? p->verifiedSigma : disp.verifiedSigma;
    auto raw = fromSite ? p->rawSigma : disp.rawSigma;
    auto practical = fromSite ? p->practicalSigma : disp.practicalSigma;
    auto confidence = fromSite && p->confidence ? p->confidence : disp.confidence;
    std::optional<double> headline = verified ? verified : (practical ? practical : raw);
    std::string displayName = p && !p->displayName.empty() ? p->displayName : st.displayName;
    bool const gdVerified = p ? p->identityVerified : st.identityVerified;
    std::string priv = privateSigmaProfileText(disp);
    std::string privNote = priv.empty() ? std::string() : privateSigmaNoteText(disp, list ? list->eligibilityMinConfidence : std::nullopt);
    std::optional<ranks::Progress> progress;
    if (p) progress = ranks::profileProgress(*p, list);
    std::string hint;
    if (sigmaVisible && !officialRank) {
        hint = rankHintText(disp, list ? list->eligibilityMinConfidence : std::nullopt);
        if (hint.empty() && p && !p->rankHint.empty()) hint = p->rankHint;
        if (hint.empty() && disp.state != RatingDisplay::Ranked && list && list->eligibilityMinConfidence)
            hint = fmt::format("Rank at {} confidence", ranks::percentText(*list->eligibilityMinConfidence));
    }
    std::vector<std::string> steps = sigmaVisible ? std::vector<std::string>{} : nextSteps(cal, disp, 2);
    std::string const missingLock = missingGamemodesLockText(disp);

    std::string key = fmt::format("profile|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", site.profileState.generation, site.ranksState.generation,
                                  st.username, displayName, gdVerified, static_cast<int>(disp.state), num(headline), num(raw), num(practical),
                                  confidence ? *confidence : -1.0, disp.verificationLabel, disp.provisionalLabel, priv, cal.percent, cal.effectiveSamples.current,
                                  cal.gamemodes.current, site.profileNotFound, site.profileState.error, hint, progress ? progress->percent : -1.0,
                                  p ? p->leaderboardPosition : -1);
    for (auto const& s : steps) key += "|" + s;
    key += "|" + missingLock;
    if (p) {
        for (auto const& g : p->gamemodes) key += fmt::format("|{}:{}", g.sigma ? *g.sigma : -1.0, g.calibrationProgress);
    }
    if (!beginPage(key)) return;
    float const W = kContentW, H = kContentH;
    auto menu = pageMenu();

    // ---- left column: the badge and the rank ----
    float const leftCx = 64.f;
    auto b = badge(rank, division, 92.f, !p && !rank);
    b->setPosition({leftCx, H - 60.f});
    m_page->addChild(b);
    std::string rankText;
    ccColor3B rankColor = kDim;
    if (officialRank) {
        rankText = rank ? ranks::bandName(*rank, division) : rankLabel(list, *officialRank);
        rankColor = rank ? toCc(rank->color) : kWhite;
    }
    else if (disp.state == RatingDisplay::Ranked && !disp.rankName.empty()) {
        rankText = disp.rankName;
        rankColor = kWhite;
    }
    else rankText = "Unranked";
    text(m_page, rankText, leftCx, H - 116.f, 118.f, rankColor, 0.46f, kGoldFont, {0.5f, 0.5f});
    if (progress) {
        text(m_page, fmt::format("to {}  {}%", progress->nextName, static_cast<int>(std::floor(progress->percent + 1e-9))), leftCx, H - 130.f, 118.f, kGrey, 0.33f, kChat,
             {0.5f, 0.5f});
        auto pb = bar(104.f, 6.f, progress->percent / 100.0, kCyan);
        pb->setPosition({leftCx - 52.f, H - 140.f});
        m_page->addChild(pb);
    }
    else if (!hint.empty()) paragraph(m_page, hint, 10.f, H - 124.f, 108.f, 0.32f, kGold);
    else if (!sigmaVisible) text(m_page, "calibrating", leftCx, H - 130.f, 118.f, kDim, 0.33f, kChat, {0.5f, 0.5f});
    if (!p) {
        std::string state = site.profileNotFound ? "No GPRL profile yet" : (!site.profileState.error.empty() ? "Profile unavailable" : "Loading profile...");
        text(m_page, state, leftCx, H - 152.f, 118.f, site.profileNotFound ? kGold : kDim, 0.33f, kChat, {0.5f, 0.5f});
    }

    // ---- right column: the player and the number ----
    float const rx = 134.f;
    float const rw = W - rx - 10.f;
    auto nameLabel = text(m_page, displayName, rx, H - 12.f, 130.f, kWhite, 0.5f, kBig, {0.f, 0.5f}, 0.3f);
    auto vchip = chip(gdVerified ? "GD verified" : "GD not verified", gdVerified ? ccColor3B{30, 110, 60} : ccColor3B{140, 80, 40}, kWhite, 0.28f);
    vchip->setPosition({rx + nameLabel->getScaledContentSize().width + 8.f, H - 12.f});
    m_page->addChild(vchip);
    auto info = iconButton(menu, "GJ_infoIcon_001.png", 16.f, this, menu_selector(GprlMenu::onWhatIsSigma));
    info->setPosition({W - 12.f, H - 12.f});

    if (!sigmaVisible) {
        // the calibration ring and what to play next
        auto r = ring(27.f, 6.f, cal.percent, kCyan, {0, 0, 0}, 160);
        r->setPosition({rx + 30.f, H - 62.f});
        m_page->addChild(r);
        text(m_page, percent(cal.percent), rx + 30.f, H - 62.f, 40.f, kWhite, 0.34f, kBig, {0.5f, 0.5f});
        text(m_page, "Calibrating", rx + 66.f, H - 44.f, rw - 66.f, kGold, 0.5f, kGoldFont, {0.f, 0.5f});
        text(m_page,
             fmt::format("{} of {} effective timings  -  {} of {} gamemodes", static_cast<int>(std::floor(cal.effectiveSamples.current + 1e-9)),
                         static_cast<int>(cal.effectiveSamples.required), static_cast<int>(cal.gamemodes.current), static_cast<int>(cal.gamemodes.required)),
             rx + 66.f, H - 62.f, rw - 66.f, kWhite, 0.36f, kChat, {0.f, 0.5f});
        text(m_page,
             !st.serverCalibration     ? std::string("sigma/s LOCKED - waiting for the server's calibration state")
             : !missingLock.empty()    ? missingLock
                                       : std::string("sigma/s LOCKED until the calibration completes"),
             rx + 66.f, H - 76.f, rw - 66.f, kGrey, 0.33f, kChat, {0.f, 0.5f});
        float y = H - 98.f;
        if (!priv.empty()) {
            text(m_page, priv, rx, y, rw, kGreyBlue, 0.34f, kChat, {0.f, 0.5f});
            y -= 10.f;
            if (!privNote.empty()) {
                text(m_page, privNote, rx, y, rw, kGreyBlue, 0.3f, kChat, {0.f, 0.5f});
                y -= 10.f;
            }
        }
        // "what to play next" card fills the room down to the gamemode strip
        float const cardTop = y - 4.f, cardBottom = 72.f;
        if (cardTop - cardBottom > 30.f) {
            auto c = card(m_page, rx, cardBottom, rw, cardTop - cardBottom, "What to play next", kCard, 230);
            float ty = cardTop - cardBottom - 16.f;
            for (auto const& s : steps) {
                auto para = paragraph(c, "- " + s, kPad, ty, rw - 2.f * kPad, 0.33f, kWhite);
                ty -= para->getHeight() + 3.f;
                if (ty < 6.f) break;
            }
        }
    }
    else {
        // the hero number
        auto big = text(m_page, num(headline), rx, H - 44.f, 130.f, kWhite, 0.95f, kBig, {0.f, 0.5f});
        text(m_page, "sigma/s", rx + big->getScaledContentSize().width + 6.f, H - 50.f, 60.f, kGold, 0.42f, kGoldFont, {0.f, 0.5f});
        float cx = rx;
        float const chipY = H - 72.f;
        if (confidence) {
            auto c = chip(fmt::format("{} confidence", ranks::percentText(*confidence)), {30, 90, 130}, kWhite, 0.3f);
            c->setPosition({cx, chipY});
            m_page->addChild(c);
            cx += c->getContentSize().width + 5.f;
        }
        if (!disp.verificationLabel.empty()) {
            ccColor3B vc = verificationColor(disp.verificationStatus);
            auto c = chip(disp.verificationLabel, ccColor3B{static_cast<GLubyte>(vc.r / 3), static_cast<GLubyte>(vc.g / 3), static_cast<GLubyte>(vc.b / 3)}, vc, 0.3f);
            c->setPosition({cx, chipY});
            m_page->addChild(c);
            cx += c->getContentSize().width + 5.f;
        }
        else if (p && p->competitiveVerified) {
            auto c = chip(*p->competitiveVerified ? "Competitive Verified" : "Not yet verified", *p->competitiveVerified ? ccColor3B{30, 110, 60} : ccColor3B{140, 80, 40},
                          kWhite, 0.3f);
            c->setPosition({cx, chipY});
            m_page->addChild(c);
        }
        float y = H - 88.f;
        if (disp.provisional && !disp.provisionalLabel.empty()) {
            text(m_page, disp.provisionalLabel, rx, y, rw, kGold, 0.34f, kChat, {0.f, 0.5f});
            y -= 12.f;
        }
        text(m_page, fmt::format("raw {}   practical {}", num(raw), num(practical)), rx, y, rw, kGrey, 0.36f, kChat, {0.f, 0.5f});
        y -= 12.f;
        if (!priv.empty()) {
            text(m_page, priv, rx, y, rw, kGreyBlue, 0.33f, kChat, {0.f, 0.5f});
            y -= 11.f;
        }
        if (p) {
            std::string board = p->leaderboardPosition > 0 ? fmt::format("Leaderboard #{}", p->leaderboardPosition) : std::string("Not on the leaderboard yet");
            text(m_page, fmt::format("{}   -   {} verified run{}", board, p->verifiedRuns, p->verifiedRuns == 1 ? "" : "s"), rx, y, rw, kGrey, 0.36f, kChat, {0.f, 0.5f});
            y -= 12.f;
            if (p->unverifiedRankEquivalent) text(m_page, "Provisional rank equivalent: " + rankLabel(list, *p->unverifiedRankEquivalent), rx, y, rw, kGrey, 0.33f, kChat, {0.f, 0.5f});
        }
    }

    // ---- bottom strip: the 8 gamemodes ----
    float const stripH = 60.f;
    auto strip = card(m_page, 6.f, 6.f, W - 12.f, stripH, sigmaVisible ? "Gamemodes (verified sigma/s)" : "Gamemodes (calibration)", kCard, 230);
    float const colW = (W - 12.f - 2.f * kPad) / static_cast<float>(kGamemodeCount);
    for (int i = 0; i < kGamemodeCount; ++i) {
        float cx = kPad + colW * (static_cast<float>(i) + 0.5f);
        ranks::GamemodeRating const* g = p && i < static_cast<int>(p->gamemodes.size()) ? &p->gamemodes[static_cast<size_t>(i)] : nullptr;
        bool lit = g && g->sigma.has_value();
        auto ic = gamemodeIcon(i, 15.f, lit);
        ic->setPosition({cx, stripH - 22.f});
        strip->addChild(ic);
        std::string value = g ? (g->sigma ? ranks::formatSigma(*g->sigma) : (g->calibrationProgress > 0.0 ? percent(g->calibrationProgress) : "-")) : "-";
        text(strip, value, cx, stripH - 36.f, colW - 4.f, lit ? kWhite : (g && g->calibrationProgress > 0.0 ? kGrey : kDim), 0.3f, kBig, {0.5f, 0.5f});
        double fill = g ? (g->sigma ? std::clamp(*g->sigma / 300.0, 0.0, 1.0) : g->calibrationProgress) : 0.0;
        auto gb = bar(colW - 10.f, 4.f, fill, lit ? kGreen : kBlue, {0, 0, 0}, 140);
        gb->setPosition({cx - (colW - 10.f) / 2.f, 8.f});
        strip->addChild(gb);
    }
}

}  // namespace gprl::ui
