// Level screen of the GPRL menu: the level being played (or the last one) as stat tiles, the
// timing-window solver's status + coverage and the background analysis; plus the Details popup
// with every raw diagnostic line the old Session tab printed.
#include <cmath>

#include "../../core/display.hpp"
#include "../../core/solver/trace_view.hpp"
#include "../../core/vocab.hpp"
#include "../Hud.hpp"
#include "../LocalStore.hpp"
#include "../Settings.hpp"
#include "../Tracker.hpp"
#include "../analyzer/Status.hpp"
#include "../solver/GdOracle.hpp"
#include "Menu.hpp"
#include "Widgets.hpp"

using namespace geode::prelude;

namespace gprl::ui {

namespace {

using namespace theme;

char const* trustText(TrustState t) {
    switch (t) {
        case TrustState::Allowed: return "allowed (normal menu / no menu)";
        case TrustState::NoclipModified: return "noclip: mechanical data allowed, nerve modified, no verified completion";
        case TrustState::Botting: return "botting: 0 sigma/s contribution";
        case TrustState::PhysicsChanged: return "physics changed (TPS bypass / speedhack): session invalid";
        case TrustState::UnknownMod: return "unknown gameplay mod: rated sigma/s disabled until verified";
    }
    return "";
}

char const* trustWord(TrustState t) {
    switch (t) {
        case TrustState::Allowed: return "ok";
        case TrustState::NoclipModified: return "Noclip";
        case TrustState::Botting: return "BOT";
        case TrustState::PhysicsChanged: return "PHYSICS";
        case TrustState::UnknownMod: return "Unknown mod";
    }
    return "ok";
}

bool liveNow(client::Status const& st) {
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
    return display::liveNow(f);
}

}  // namespace

void GprlMenu::buildLevel() {
    auto st = client::status();
    auto ses = tracker::session();
    auto sol = solver::oracle::status();
    auto ab = analyzer::ui::sessionBlock();
    auto site = client::siteData();
    TrustState trust = tracker::trust();
    ccColor3B covColor = kGrey;
    std::string coverage = coverageText(site, ses.levelId, covColor);
    bool live = liveNow(st);
    bool remote = st.sessionOpen && st.mode == client::SessionMode::Remote;

    std::string key = fmt::format("level|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", ses.levelId, ses.levelName, ses.levelOpen,
                                  ses.attempts, ses.deaths, ses.jumps, static_cast<int>(std::floor(ses.bestPercent)), ses.completions, ses.wouldBeDeaths,
                                  static_cast<int>(ses.levelActiveMs / 1000.0), static_cast<int>(ses.levelPracticeMs / 1000.0), ses.gdAttemptCount, ses.practice,
                                  static_cast<int>(trust), live, remote, st.mode == client::SessionMode::Unsent, st.levelCounts, sol.active, sol.measuring, sol.line,
                                  sol.emitted, sol.misses, sol.dropped, static_cast<int>(sol.coverage), ab.line1, ab.line2);
    key += "|" + coverage + "|" + ab.line3;
    if (!beginPage(key)) return;
    if (ses.levelId.empty()) {
        emptyState("No level played yet", "Enter a level: attempts, deaths and your timing windows show up here and on the HUD.",
                   "Every level feeds your calibration and sigma/s; rated demons are also on the levels list.");
        return;
    }
    float const W = kContentW, H = kContentH;
    auto menu = pageMenu();

    // ---- the level ----
    float const lcH = 46.f;
    float const lcY = H - 6.f - lcH;
    auto lc = card(m_page, 6.f, lcY, W - 12.f, lcH);
    text(lc, ses.levelName.empty() ? std::string("Unnamed level") : ses.levelName, 10.f, lcH - 13.f, 190.f, kWhite, 0.42f, kBig, {0.f, 0.5f});
    text(lc, fmt::format("ID {}   -   {}", ses.levelId, ses.levelOpen ? "playing now" : "last played"), 10.f, lcH - 31.f, 200.f, kGrey, 0.3f, kChat, {0.f, 0.5f});
    // chips, right to left, above the Details button
    struct Chip {
        std::string text;
        ccColor3B bg;
    };
    std::vector<Chip> chips;
    if (live) chips.push_back({"LIVE", kLiveRed});
    else if (st.sessionOpen && st.mode == client::SessionMode::Unsent) chips.push_back({"Offline: kept on disk", {90, 90, 100}});
    else if (st.sessionOpen && st.mode == client::SessionMode::Local) chips.push_back({"Local-only", {130, 100, 30}});
    if (remote) chips.push_back(st.levelCounts ? Chip{"On the levels list", {30, 110, 60}} : Chip{"Feeds calibration only", {70, 76, 100}});
    if (ses.practice) chips.push_back({"Practice", {120, 100, 30}});
    if (trust != TrustState::Allowed) chips.push_back({trustWord(trust), {150, 70, 40}});
    float right = W - 12.f - 8.f;
    for (auto it = chips.rbegin(); it != chips.rend(); ++it) {
        auto c = chip(it->text, it->bg, kWhite, 0.24f);
        right -= c->getContentSize().width;
        c->setPosition({right, lcH - 13.f});
        lc->addChild(c);
        right -= 4.f;
    }
    auto details = button(menu, "Details", "GJ_button_04.png", 0.36f, this, menu_selector(GprlMenu::onDetails));
    details->setPosition({W - 6.f - 8.f - details->getScaledContentSize().width / 2.f, lcY + 13.f});

    // ---- stat tiles (2 x 4) ----
    float const gap = 5.f;
    float const tileW = (W - 12.f - 3.f * gap) / 4.f;
    float const tileH = 27.f;
    float const row1Y = lcY - 6.f - tileH;
    float const row2Y = row1Y - gap - tileH;
    struct Stat {
        std::string value;
        char const* caption;
        ccColor3B color;
    };
    std::string fourth = ses.wouldBeDeaths > 0 ? std::to_string(ses.wouldBeDeaths) : (ses.gdAttemptCount >= 0 ? std::to_string(ses.gdAttemptCount) : "?");
    Stat const stats[8] = {
        {std::to_string(ses.attempts), "attempts", kWhite},
        {std::to_string(ses.deaths), "deaths", ses.deaths > 0 ? kOrange : kWhite},
        {std::to_string(ses.jumps), "jumps (presses)", kWhite},
        {fmt::format("{}%", static_cast<int>(std::floor(ses.bestPercent))), "best this level", kGreen},
        {std::to_string(ses.completions), "completions", ses.completions > 0 ? kGold : kWhite},
        {seconds(ses.levelActiveMs / 1000.0), "time playing", kWhite},
        {seconds(ses.levelPracticeMs / 1000.0), "in practice", kWhite},
        {fourth, ses.wouldBeDeaths > 0 ? "noclip would-be deaths" : "GD attempts (save)", ses.wouldBeDeaths > 0 ? kOrange : kGrey},
    };
    for (int i = 0; i < 8; ++i) {
        float x = 6.f + static_cast<float>(i % 4) * (tileW + gap);
        float y = i < 4 ? row1Y : row2Y;
        auto t = tile(tileW, tileH, stats[i].value, stats[i].caption, stats[i].color);
        t->setPosition({x, y});
        m_page->addChild(t);
    }

    // ---- timing windows (solver) and the background analysis share the rest ----
    float const top = row2Y - 6.f;
    float const total = top - 6.f;
    float const solverH = std::floor(total * 0.52f);
    float const analysisH = total - solverH - 6.f;
    auto sc = card(m_page, 6.f, 6.f + analysisH + 6.f, W - 12.f, solverH, "Timing windows");
    ccColor3B solColor = sol.measuring ? (sol.levelOnly ? kGold : kGreen) : (sol.active ? kOrange : kGold);
    text(sc, sol.line, kPad, solverH - 19.f, W - 12.f - 2.f * kPad, solColor, 0.32f, kChat, {0.f, 0.5f});
    if (sol.active) {
        float by = solverH - 33.f;
        text(sc, fmt::format("Inputs with a window: {}%", static_cast<int>(std::floor(sol.coverage))), kPad, by, 130.f, kGrey, 0.3f, kChat, {0.f, 0.5f});
        auto pb = bar(W - 12.f - 2.f * kPad - 140.f, 5.f, sol.coverage / 100.0, kCyan, {0, 0, 0}, 140);
        pb->setPosition({kPad + 140.f, by});
        sc->addChild(pb);
        std::string counts = fmt::format("{} measured   {} misses   {} dropped", sol.emitted, sol.misses, sol.dropped);
        if (!sol.mismatchKinds.empty()) counts += "  (" + sol.mismatchKinds + ")";
        if (sol.localSamples > 0) counts += fmt::format("   {} local samples", sol.localSamples);
        text(sc, counts, kPad, solverH - 46.f, W - 12.f - 2.f * kPad, kGrey, 0.3f, kChat, {0.f, 0.5f});
    }
    else {
        text(sc, "The solver measures the exact timing window of every click with hidden copies of the game; nothing here changes your run.", kPad, solverH - 33.f,
             W - 12.f - 2.f * kPad, kGrey, 0.3f, kChat, {0.f, 0.5f});
    }
    auto ac = card(m_page, 6.f, 6.f, W - 12.f, analysisH, "Level analysis");
    ccColor3B abColor = ab.problem ? kOrange : (ab.active ? kGreen : kGrey);
    float ly = analysisH - 19.f;
    text(ac, ab.line1.empty() ? std::string("Level analysis: off") : ab.line1, kPad, ly, W - 12.f - 2.f * kPad, abColor, 0.3f, kChat, {0.f, 0.5f});
    ly -= 11.f;
    if (ly > 4.f) text(ac, coverage, kPad, ly, W - 12.f - 2.f * kPad, covColor, 0.3f, kChat, {0.f, 0.5f});
    ly -= 11.f;
    std::string extra = !ab.line3.empty() ? ab.line3 : ab.line2;
    if (!extra.empty() && ly > 4.f) text(ac, extra, kPad, ly, W - 12.f - 2.f * kPad, kGrey, 0.28f, kChat, {0.f, 0.5f});
}

// ---- Details popup ----

void DetailsPopup::open() {
    auto p = new DetailsPopup();
    if (p->init()) {
        p->autorelease();
        p->show();
        return;
    }
    delete p;
}

bool DetailsPopup::init() {
    constexpr float kW = 440.f, kH = 270.f;
    if (!Popup::init(kW, kH, "GJ_square02.png")) return false;
    setTitle("Level details");
    panel(m_mainLayer, 10.f, 10.f, kW - 20.f, kH - 44.f, kPanel, 200);
    m_list = ScrollLayer::create(CCSize{kW - 28.f, kH - 52.f});
    m_list->setPosition({14.f, 14.f});
    m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(1.f));
    m_mainLayer->addChild(m_list);
    onTick(0.f);
    this->schedule(schedule_selector(DetailsPopup::onTick), 0.5f);
    return true;
}

std::vector<std::pair<std::string, ccColor3B>> DetailsPopup::lines() const {
    std::vector<std::pair<std::string, ccColor3B>> out;
    auto st = client::status();
    auto ses = tracker::session();
    auto view = tracker::attemptView();
    auto sol = solver::oracle::status();
    auto ab = analyzer::ui::sessionBlock();
    auto site = client::siteData();
    auto cal = client::displayCalibration();
    auto add = [&](std::string s, ccColor3B c = kGrey) { out.emplace_back(std::move(s), c); };
    if (ses.levelId.empty()) add("No level played yet this game session", kGold);
    else {
        add((ses.levelOpen ? "Level: " : "Last level: ") + ses.levelName + " (" + ses.levelId + ")", kGold);
        add(fmt::format("Jumps (presses) {}    releases {}", ses.jumps, ses.releases), kWhite);
        add(fmt::format("Attempts {}    deaths {}    would-be deaths (noclip) {}    completions {}    GD save attempts {} (untrusted)", ses.attempts, ses.deaths,
                        ses.wouldBeDeaths, ses.completions, ses.gdAttemptCount >= 0 ? std::to_string(ses.gdAttemptCount) : "?"),
            kWhite);
        std::string per = "Presses per gamemode:";
        for (int i = 0; i < kGamemodeCount; ++i) per += fmt::format("  {} {}", name(static_cast<Gamemode>(i)), ses.pressesByGamemode[i]);
        add(per);
        add(fmt::format("Practice {}    percent {}%    best {}%    noclip now {}    noclip seen {}    active {:.1f} s (level {:.0f} s: practice {:.0f}, startpos {:.0f})",
                        ses.practice ? "on" : "off", static_cast<int>(std::floor(ses.currentPercent)), static_cast<int>(std::floor(ses.bestPercent)),
                        ses.noclipNow ? "yes" : "no", ses.noclipSeen ? "yes" : "no", ses.attemptActiveMs / 1000.0, ses.levelActiveMs / 1000.0,
                        ses.levelPracticeMs / 1000.0, ses.levelStartPosMs / 1000.0),
            kWhite);
        bool hookProblem = ses.rawPresses > 0 && ses.jumps == 0;
        add(fmt::format("Hook check: handleButton {} / pushButton (P1) {} presses{}", ses.jumps, ses.rawPresses, hookProblem ? "  -  INPUT HOOK NOT FIRING, please report" : ""),
            hookProblem ? kRed : kGrey);
    }
    TrustState trust = tracker::trust();
    add(fmt::format("Trust: {}", trustText(trust)), trust == TrustState::Allowed ? kGreen : kOrange);
    std::string session = view.open || st.sessionOpen
        ? fmt::format("Session {} ({}), attempt #{} ({} this session), attempt inputs {}, tick {}", st.sessionId.empty() ? "opening" : st.sessionId,
                      client::name(st.mode), view.attemptNo, view.sessionAttemptCount, view.inputs, view.tick)
        : "No telemetry session open";
    add(session);
    add(fmt::format("Batches sent {}  spooled {}  failed {}  events sent {}  pending {}  dropped {}", st.batchesSent, st.batchesSpooled, st.batchesFailed, st.eventsSent,
                    st.eventsPending, st.droppedEvents));
    if (st.sessionOpen && st.mode == client::SessionMode::Remote) {
        std::string server = st.ratable ? "Server: session ratable" : "Server: session NOT ratable" + (st.ratableReason.empty() ? std::string() : " - " + st.ratableReason);
        std::string counts = display::levelCountsLine(st.levelCounts, st.levelCountsReason);
        if (!counts.empty()) server += "  |  " + counts;
        else if (!st.levelRatingText.empty()) server += "  |  level counts (" + st.levelRatingText + ")";
        add(server, st.ratable ? kGreen : kOrange);
    }
    if (!st.lastError.empty()) add("Last error: " + st.lastError, kRed);
    add(fmt::format("Calibration {}   sigma/s: {}   effective samples {} / {}   gamemodes {} / {}   source: {}", percent(cal.percent), cal.overallLocked ? "LOCKED" : "unlocked",
                    static_cast<int64_t>(std::llround(cal.effectiveSamples.current)), static_cast<int64_t>(std::llround(cal.effectiveSamples.required)),
                    static_cast<int>(cal.gamemodes.current), static_cast<int>(cal.gamemodes.required),
                    st.serverCalibration ? "server (" + st.serverCalibrationAt + ")" : "local (no server state yet)"),
        kGold);
    {
        ccColor3B color = kGrey;
        std::string coverage;
        if (ses.levelId.empty()) coverage = "Level Analysis Coverage: no level played yet";
        else {
            bool mine = site.coverageLevelId == ses.levelId;
            if (mine && site.coverageState.loaded) {
                color = site.coverage.complete ? kGreen : (site.coverage.levelCounts ? kWhite : kGrey);
                coverage = display::coverageLine(site.coverage);
            }
            else if (mine && site.coverageNotFound) coverage = "Level Analysis Coverage: 0% (the server has not seen this level yet)";
            else if (mine && !site.coverageState.error.empty()) {
                color = kOrange;
                coverage = "Level Analysis Coverage: unavailable - " + site.coverageState.error;
            }
            else coverage = site.online ? "Level Analysis Coverage: loading..." : "Level Analysis Coverage: not fetched (local-only mode / API not set)";
        }
        add(coverage, color);
    }
    add(sol.line, sol.measuring ? (sol.levelOnly ? kGold : kGreen) : (sol.active ? kOrange : kGold));
    if (sol.active) add(sol.line2);
    if (sol.active && !sol.line3.empty()) add(sol.line3, sol.replayBroken ? kOrange : kGrey);
    if (settings::get().traceMaxTicks > 0) {
        auto const& trace = hud::lastTrace();
        add(trace.any ? solver::trace_view::summaryLine(trace)
                      : fmt::format("Trace: no window <= {} ticks traced yet on this level (setting solver-trace-max-ticks)", settings::get().traceMaxTicks),
            trace.any ? kGold : kGrey);
    }
    add(ab.line1, ab.problem ? kOrange : (ab.active ? kGreen : kGrey));
    if (!ab.line2.empty()) add(ab.line2);
    if (!ab.line3.empty()) add(ab.line3);
    add(analyzer::ui::accountLine(), settings::get().recordSafe ? kGold : kGrey);
    if (st.apiPlaceholder) add("API: base URL not set (placeholder or invalid) - nothing is sent; set it in the mod settings", kOrange);
    else if (st.localOnly) add("Local-only mode: batches are written to the mod save folder, nothing is sent", kOrange);
    else add(fmt::format("API: {}   site: {}", st.apiBaseUrl, settings::get().siteOrigin));
    add(fmt::format("spool: {} records in {}", localstore::recordsWritten(), localstore::dir().string()), kDim);
    return out;
}

void DetailsPopup::onTick(float) {
    auto ls = lines();
    std::string key;
    for (auto const& l : ls) key += l.first + "\n";
    if (key == m_key) return;
    bool first = m_key.empty();
    m_key = key;
    m_list->m_contentLayer->removeAllChildren();
    float const rowW = m_list->getContentSize().width - 2.f;
    for (auto const& l : ls) {
        auto row = CCNode::create();
        row->setContentSize({rowW, 11.f});
        text(row, l.first, 2.f, 5.5f, rowW - 4.f, l.second, 0.3f, kChat, {0.f, 0.5f});
        m_list->m_contentLayer->addChild(row);
    }
    m_list->m_contentLayer->updateLayout();
    if (first) m_list->scrollToTop();
}

}  // namespace gprl::ui
