#include "Hud.hpp"

#include <Geode/ui/OverlayManager.hpp>

#include <algorithm>
#include <cmath>

#include "../core/display.hpp"
#include "../core/solver/trace_view.hpp"
#include "Settings.hpp"
#include "Clipper.hpp"
#include "Telemetry.hpp"
#include "Tracker.hpp"
#include "analyzer/Status.hpp"
#include "solver/GdOracle.hpp"

using namespace geode::prelude;

namespace gprl::hud {

// defined below (outside the anonymous namespace); used by the top-right panel
std::string rankNameFor(CalibrationDisplay const& disp, client::Status const& st);

namespace {

constexpr float kRefreshSeconds = 0.5f;

Ref<CCLabelBMFont> s_label;
Ref<CCScale9Sprite> s_windowPanel;                 // middle-right: the last 8 presses / releases (v0.5.1)
std::vector<Ref<CCLabelBMFont>> s_historyLabels;   // one label per history line, newest at the top
PlayLayer* s_layer = nullptr;
float s_accum = 0.f;
float s_windowAccum = 0.f;
std::string s_last;
std::string s_lastWindowText;

// ---- v0.12.0 top-right panel (owner request 2026-10-02): the sigma/s big, a small line under it,
// and a LIVE tag that shows ONLY while the server is accepting this level's data (display::liveNow)
Ref<CCScale9Sprite> s_topPanel;
Ref<CCLabelBMFont> s_topBig;
Ref<CCLabelBMFont> s_topSmall;
Ref<CCScale9Sprite> s_liveTag;    // red tag with "LIVE"; hidden = nothing is reaching the website
Ref<CCLayerColor> s_liveDot;
std::string s_lastTopBig, s_lastTopSmall;
int s_lastLive = -1;              // -1 = not drawn yet
float s_topRight = 0.f;           // right edge of the panel (left of GD's pause button)
float s_topTop = 0.f;             // top edge

constexpr float kTopPad = 5.f;
constexpr float kTopBigScale = 0.62f;
constexpr float kTopSmallScale = 0.42f;
constexpr float kLiveScale = 0.32f;
constexpr ccColor3B kLiveRed{230, 40, 40};

constexpr float kWindowRefreshSeconds = 0.1f;
constexpr float kHistoryScale = 0.36f;
constexpr float kHistoryLineHeight = 12.f;
constexpr float kHistoryPadX = 6.f;
constexpr float kHistoryPadY = 4.f;
constexpr float kHistoryMinWidth = 150.f;

constexpr ccColor3B kToneMeasured{120, 255, 140};
constexpr ccColor3B kToneMiss{255, 110, 110};
constexpr ccColor3B kToneDropped{190, 190, 190};
constexpr ccColor3B kToneIdle{230, 230, 230};

// ---- v0.7.0 debug view of the last traced timing (AUDIT §11, docs/TIMING_SOLVER_V2.md §2.12) ----
// Handed over by the solver facade (showTrace) and drawn only while `solver-trace-overlay` is on
// (and tracing, `solver-trace-max-ticks` > 0).
solver::trace_view::View s_trace;                // the last traced input (any = false: none yet)
int s_traceVersion = 0;                          // bumped by showTrace / attach
int s_traceDrawn = -1;                           // the version on screen (-1 = nothing drawn: redraw)
PlayLayer* s_traceLayer = nullptr;               // the level the overlay may draw into (independent of show-hud)
Ref<CCDrawNode> s_traceNode;                     // object layer: trajectories, hitboxes, killer rects
Ref<CCScale9Sprite> s_tracePanel;                // UI layer, middle-left: the AUDIT §11 fields
std::vector<Ref<CCLabelBMFont>> s_traceLabels;
float s_traceAccum = 0.f;

constexpr float kTraceRefreshSeconds = 0.1f;
constexpr float kTraceScale = 0.36f;
constexpr float kTraceLineHeight = 9.f;
constexpr float kTracePad = 5.f;
constexpr ccColor4F kTraceReference{1.f, 1.f, 1.f, 0.95f};
constexpr ccColor4F kTraceValid{0.47f, 1.f, 0.55f, 0.9f};
constexpr ccColor4F kTraceInvalid{1.f, 0.35f, 0.35f, 0.9f};
constexpr ccColor4F kTraceSaValid{0.3f, 0.85f, 1.f, 0.85f};
constexpr ccColor4F kTraceSaInvalid{1.f, 0.4f, 0.9f, 0.85f};
constexpr ccColor4F kTraceKiller{1.f, 0.65f, 0.1f, 0.9f};
constexpr ccColor4F kTraceInput{1.f, 0.9f, 0.2f, 1.f};
constexpr ccColor4F kTraceClear{0.f, 0.f, 0.f, 0.f};
constexpr ccColor3B kToneSaValid{80, 215, 255};
constexpr ccColor3B kToneSaInvalid{255, 110, 230};

/// v0.13.0 look: every HUD panel is a rounded, tinted nine-slice (square02b_001.png) instead of a
/// flat rectangle. Same content size / anchor semantics as the CCLayerColor it replaces.
CCScale9Sprite* makePanel(ccColor3B tint, GLubyte opacity, float w, float h) {
    auto p = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    p->setContentSize({std::max(8.f, w), std::max(8.f, h)});
    p->setColor(tint);
    p->setOpacity(opacity);
    return p;
}

constexpr ccColor3B kPanelTint{10, 12, 24};

bool traceOverlayWanted() {
    auto const& s = settings::get();
    return s.enabled && s.traceOverlay && s.traceMaxTicks > 0;
}

void removeTraceNodes() {
    if (s_traceNode && s_traceNode->getParent()) s_traceNode->removeFromParent();
    if (s_tracePanel && s_tracePanel->getParent()) s_tracePanel->removeFromParent();
    s_traceNode = nullptr;
    s_tracePanel = nullptr;
    s_traceLabels.clear();
    s_traceDrawn = -1;
}

void traceRect(float cx, float cy, float w, float h, ccColor4F color) {
    if (!(w > 0.f) || !(h > 0.f)) return;
    CCPoint verts[4] = {{cx - w / 2.f, cy - h / 2.f}, {cx + w / 2.f, cy - h / 2.f}, {cx + w / 2.f, cy + h / 2.f}, {cx - w / 2.f, cy + h / 2.f}};
    s_traceNode->drawPolygon(verts, 4, kTraceClear, 0.8f, color);
}

void tracePath(solver::trace_view::Path const& p, ccColor4F color, float width) {
    for (size_t i = 1; i < p.points.size(); ++i) {
        s_traceNode->drawSegment({p.points[i - 1].x, p.points[i - 1].y}, {p.points[i].x, p.points[i].y}, width, color);
    }
    if (!p.death.died) return;
    // where it died: the clone's hitbox, and the rect of the object that killed it (orange)
    traceRect(p.death.x, p.death.y, p.death.boxW, p.death.boxH, color);
    if (p.death.killerId >= 0 && p.death.rectW > 0.f && p.death.rectH > 0.f) {
        traceRect(p.death.rectX + p.death.rectW / 2.f, p.death.rectY + p.death.rectH / 2.f, p.death.rectW, p.death.rectH, kTraceKiller);
    }
}

ccColor3B traceLineColor(std::string const& line) {
    using solver::trace_view::Role;
    if (line.rfind("TRACE", 0) == 0) return {255, 255, 255};
    if (line.rfind("  why:", 0) == 0) return kToneMiss;
    Role r = solver::trace_view::roleOf(line);
    if (solver::trace_view::sequenceRole(r)) return solver::trace_view::invalidRole(r) ? kToneSaInvalid : kToneSaValid;
    if (solver::trace_view::invalidRole(r)) return kToneMiss;
    if (solver::trace_view::validRole(r)) return kToneMeasured;
    return kToneIdle;
}

ccColor4F tracePathColor(solver::trace_view::Role r) {
    if (solver::trace_view::sequenceRole(r)) return solver::trace_view::invalidRole(r) ? kTraceSaInvalid : kTraceSaValid;
    return solver::trace_view::invalidRole(r) ? kTraceInvalid : kTraceValid;
}

/// (Re)builds the overlay when a new trace arrived or the setting changed: valid paths first,
/// invalid ones over them, the real run on top; the panel lists trace_view panelLines.
void refreshTrace() {
    if (!s_traceLayer) return;
    if (!traceOverlayWanted()) {
        if (s_traceNode || s_tracePanel) removeTraceNodes();
        return;
    }
    if (!s_traceNode && s_traceLayer->m_objectLayer) {
        s_traceNode = CCDrawNode::create();
        s_traceNode->setID("trace-overlay"_spr);
        s_traceLayer->m_objectLayer->addChild(s_traceNode, 9999);
    }
    if (!s_tracePanel) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        CCNode* parent = s_traceLayer->m_uiLayer ? static_cast<CCNode*>(s_traceLayer->m_uiLayer) : static_cast<CCNode*>(s_traceLayer);
        s_tracePanel = makePanel(kPanelTint, 165, 120.f, kTraceLineHeight + 2.f * kTracePad);
        s_tracePanel->setID("trace-panel"_spr);
        s_tracePanel->ignoreAnchorPointForPosition(false);
        s_tracePanel->setAnchorPoint({0.f, 0.5f});
        s_tracePanel->setPosition({4.f, win.height * 0.5f});
        s_tracePanel->setZOrder(1000);
        parent->addChild(s_tracePanel);
        s_traceLabels.clear();
    }
    if (s_traceDrawn == s_traceVersion) return;
    s_traceDrawn = s_traceVersion;
    using solver::trace_view::Role;
    if (s_traceNode) {
        s_traceNode->clear();
        for (auto const& p : s_trace.paths) if (solver::trace_view::validRole(p.role)) tracePath(p, tracePathColor(p.role), 0.9f);
        for (auto const& p : s_trace.paths) if (solver::trace_view::invalidRole(p.role)) tracePath(p, tracePathColor(p.role), 0.9f);
        for (auto const& p : s_trace.paths) {
            if (p.role != Role::Reference) continue;
            tracePath(p, kTraceReference, 1.1f);
            if (!p.points.empty()) s_traceNode->drawDot({p.points.front().x, p.points.front().y}, 2.5f, kTraceInput);
        }
    }
    std::vector<std::string> lines = solver::trace_view::panelLines(s_trace);
    if (lines.empty()) {
        lines.push_back(fmt::format("TRACE: no window <= {} ticks traced yet on this level", settings::get().traceMaxTicks));
    }
    while (s_traceLabels.size() < lines.size()) {
        auto label = CCLabelBMFont::create("", "chatFont.fnt");
        label->setID(fmt::format("trace-line-{}", s_traceLabels.size()).c_str());
        label->setScale(kTraceScale);
        label->setAnchorPoint({0.f, 1.f});
        s_tracePanel->addChild(label);
        s_traceLabels.push_back(label);
    }
    float maxWidth = 0.f;
    for (size_t i = 0; i < s_traceLabels.size(); ++i) {
        auto& label = s_traceLabels[i];
        if (i >= lines.size()) {
            label->setVisible(false);
            continue;
        }
        label->setVisible(true);
        label->setString(lines[i].c_str());
        // long death lines shrink instead of covering the level (at most ~55 % of the screen)
        label->limitLabelWidth(CCDirector::sharedDirector()->getWinSize().width * 0.55f, kTraceScale, 0.15f);
        label->setColor(traceLineColor(lines[i]));
        maxWidth = std::max(maxWidth, label->getScaledContentSize().width);
    }
    float width = maxWidth + 2.f * kTracePad;
    float height = kTraceLineHeight * static_cast<float>(lines.size()) + 2.f * kTracePad;
    s_tracePanel->setContentSize({width, height});
    for (size_t i = 0; i < lines.size() && i < s_traceLabels.size(); ++i) {
        s_traceLabels[i]->setPosition({kTracePad, height - kTracePad - kTraceLineHeight * static_cast<float>(i)});
    }
    if (s_trace.any) log::info("GPRL trace: overlay shows input #{} ({} trajectories)", s_trace.header.inputIndex, s_trace.paths.size());
}

ccColor3B toneColor(display::Tone t) {
    switch (t) {
        case display::Tone::Measured: return kToneMeasured;
        case display::Tone::Miss: return kToneMiss;
        case display::Tone::Dropped: return kToneDropped;
        case display::Tone::Idle: return kToneIdle;
    }
    return kToneIdle;
}

/// The middle-right panel's lines: the last 8 finished measurements (core/display historyLines,
/// host-tested), newest first, or one status line while nothing finished yet.
std::vector<display::HistoryLine> historyLines() {
    auto st = solver::oracle::status();
    if (!st.active) return {};
    auto recent = solver::oracle::recentWindows(static_cast<size_t>(display::kDisplay.historyLines));
    if (recent.empty()) {
        display::HistoryLine idle;
        idle.text = st.measuring ? (st.levelOnly ? "GPRL: waiting for a click... (bot: level-only)" : "GPRL: waiting for a click...") : "GPRL: " + st.state;
        idle.tone = display::Tone::Idle;
        return {idle};
    }
    std::vector<display::HistoryEntry> entries;
    entries.reserve(recent.size());
    for (auto const& w : recent) {
        display::HistoryEntry e;
        e.ok = w.ok;
        e.down = w.down;
        e.miss = w.miss;
        e.levelOnly = w.levelOnly;
        e.widthMs = w.widthMs;
        e.earlyMs = w.earlyMs;
        e.lateMs = w.lateMs;
        e.boundedEarly = w.boundedEarly;
        e.boundedLate = w.boundedLate;
        e.reason = w.reason;
        e.suffix = w.v2;   // v0.7.0: "local 4.00 f / seq 10.75 f ok" once the input's timing_result arrived
        entries.push_back(std::move(e));
    }
    return display::historyLines(entries);
}

void refreshWindow() {
    if (!s_windowPanel) return;
    auto lines = historyLines();
    std::string joined;
    for (auto const& l : lines) joined += l.text + "\n";
    if (joined == s_lastWindowText) return;
    bool first = s_lastWindowText.empty();
    s_lastWindowText = joined;
    // labels are created once per slot and reused; unused slots are hidden
    while (s_historyLabels.size() < lines.size()) {
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID(fmt::format("hud-history-{}", s_historyLabels.size()).c_str());
        label->setScale(kHistoryScale);
        label->setAlignment(kCCTextAlignmentRight);
        label->setAnchorPoint({1.f, 1.f});
        s_windowPanel->addChild(label);
        s_historyLabels.push_back(label);
    }
    float maxWidth = 0.f;
    for (size_t i = 0; i < s_historyLabels.size(); ++i) {
        auto& label = s_historyLabels[i];
        if (i >= lines.size()) {
            label->setVisible(false);
            continue;
        }
        label->setVisible(true);
        label->setString(lines[i].text.c_str());
        label->setColor(toneColor(lines[i].tone));
        label->setOpacity(static_cast<GLubyte>(std::clamp(lines[i].opacity, 0, 255)));
        maxWidth = std::max(maxWidth, label->getScaledContentSize().width);
    }
    float width = std::max(kHistoryMinWidth, maxWidth + 2.f * kHistoryPadX);
    float height = kHistoryLineHeight * static_cast<float>(std::max<size_t>(1, lines.size())) + 2.f * kHistoryPadY;
    s_windowPanel->setContentSize({width, height});
    for (size_t i = 0; i < lines.size() && i < s_historyLabels.size(); ++i) {
        s_historyLabels[i]->setPosition({width - kHistoryPadX, height - kHistoryPadY - kHistoryLineHeight * static_cast<float>(i)});
    }
    if (first && settings::debugEnabled()) log::info("GPRL hud: history readout shows: {}", lines.empty() ? "(nothing)" : lines.front().text);
}

char const* trustWord(TrustState t) {
    switch (t) {
        case TrustState::Allowed: return "ok";
        case TrustState::NoclipModified: return "noclip";
        case TrustState::Botting: return "BOT";
        case TrustState::PhysicsChanged: return "PHYSICS";
        case TrustState::UnknownMod: return "unknown mod";
    }
    return "ok";
}

/// Lays the panel out around its labels: right-aligned, top-anchored, the LIVE tag under the text.
void layoutTop() {
    if (!s_topPanel) return;
    float bigW = s_topBig->getScaledContentSize().width, bigH = s_topBig->getScaledContentSize().height;
    float smallW = s_topSmall->getScaledContentSize().width, smallH = s_topSmall->getScaledContentSize().height;
    auto liveSize = s_liveTag->getContentSize();
    float w = std::max({bigW, smallW, liveSize.width}) + 2.f * kTopPad;
    float h = kTopPad + bigH + smallH + 3.f + liveSize.height + kTopPad;
    s_topPanel->setContentSize({w, h});
    s_topPanel->setPosition({s_topRight, s_topTop});
    float y = h - kTopPad;
    s_topBig->setPosition({w - kTopPad, y});
    y -= bigH;
    s_topSmall->setPosition({w - kTopPad, y});
    y -= smallH + 3.f;
    s_liveTag->setPosition({w - kTopPad - liveSize.width, y - liveSize.height});
}

void refreshTop() {
    if (!s_topPanel) return;
    auto cal = client::displayCalibration();
    auto disp = client::displayRating();
    auto st = client::status();
    // the private estimate never reaches an evidence clip (same rule as the bottom line)
    bool recording = clipper::status().capture.recording;
    auto top = topRightSigma(disp, cal.percent, rankNameFor(disp, st), !recording);
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
    int live = display::liveNow(f) ? 1 : 0;
    bool changed = false;
    if (top.big != s_lastTopBig) {
        s_lastTopBig = top.big;
        s_topBig->setString(top.big.c_str());
        changed = true;
    }
    if (top.detail != s_lastTopSmall) {
        s_lastTopSmall = top.detail;
        s_topSmall->setString(top.detail.c_str());
        changed = true;
    }
    if (live != s_lastLive) {
        s_lastLive = live;
        s_liveTag->setVisible(live == 1);
        changed = true;
    }
    if (changed) layoutTop();
}

void refresh() {
    // "Reset data" (Account tab, Telemetry worker): one uint32 compare per tick; on a change the
    // local history and calibration tracker are cleared so nothing stale survives the wipe.
    static uint32_t s_seenResetGen = client::resetGeneration();
    if (uint32_t gen = client::resetGeneration(); gen != s_seenResetGen) {
        s_seenResetGen = gen;
        solver::oracle::clearLocalData();
        s_last.clear();
    }
    refreshTop();
    if (!s_label) return;
    std::string text = line(!s_topPanel);
    if (text == s_last) return;
    s_last = text;
    s_label->setString(text.c_str());
}

}  // namespace

/// The rank the HUD names next to a ranked sigma/s: the server's own label, else the site
/// profile's official rank through the ladder when both are cached (they are only fetched while
/// the popup's Profile tab is open, so this is often empty and sigmaHudText says "ranked").
std::string rankNameFor(CalibrationDisplay const& disp, client::Status const& st) {
    if (disp.state != RatingDisplay::Ranked) return {};
    if (!disp.rankName.empty()) return disp.rankName;
    auto site = client::siteData();
    ranks::RankList const* list = site.ranksState.loaded ? &site.ranks : nullptr;
    if (list && !disp.rankId.empty()) {
        int i = ranks::findRank(*list, disp.rankId);
        if (i >= 0) return ranks::bandName(list->ranks[static_cast<size_t>(i)], disp.rankDivision);
    }
    bool haveProfile = st.connected && site.profileState.loaded && site.profileUsername == st.username;
    if (haveProfile && site.profile.rank && list) {
        int i = ranks::findRank(*list, site.profile.rank->rankId);
        if (i >= 0) return ranks::bandName(list->ranks[static_cast<size_t>(i)], site.profile.rank->division);
    }
    return {};
}

std::string line(bool withSigma) {
    auto cal = client::displayCalibration();
    auto disp = client::displayRating();
    auto st = client::status();
    auto ses = tracker::session();
    // The HUD is a child of PlayLayer, so the evidence clip buffer (and any stream recording)
    // captures it. The owner-only private estimate must never reach moderators through a clip:
    // while the buffer records, the suffix is dropped (privacy verifier finding, 2026-10-01).
    if (disp.privatePresent && clipper::status().capture.recording) disp.privatePresent = false;
    // docs/RANKS.md "Visibility thresholds": LOCKED (calibrating N%) | 123.4 (82% conf, unranked) | 123.4 | Master II
    // Owner decision 2026-10-01: while LOCKED, the player's own private estimate rides along once
    // the server sends one (calibration >= 50 %): "LOCKED (calibrating 62%) | private ~123.4".
    std::string text = withSigma ? fmt::format("GPRL: jumps {} | attempts {} | sigma/s {}", ses.jumps, ses.attempts,
                                               sigmaHudText(disp, cal.percent, rankNameFor(disp, st)))
                                 : fmt::format("GPRL: jumps {} | attempts {}", ses.jumps, ses.attempts);
    if (cal.overallLocked && !st.serverCalibration) {
        // local calibration from the solver's own samples (v0.4.0): visible progress before the
        // server's authoritative state arrives
        text += fmt::format(" | samples {}/{} (local)", static_cast<int>(std::floor(cal.effectiveSamples.current + 1e-9)),
                            static_cast<int>(cal.effectiveSamples.required));
    }
    // v0.5.1 (MASTER C6) / v0.8.4: the server said this level is not a rated demon - it still feeds calibration, it is not on the levels list
    if (st.sessionOpen && st.mode == client::SessionMode::Remote && !st.levelCounts) text += " | " + display::levelCountsLine(false, {});
    if (tracker::trust() != TrustState::Allowed) text += fmt::format(" | trust: {}", trustWord(tracker::trust()));
    if (st.localOnly) text += " | local-only";
    else if (!st.connected) text += " | not connected";
    else if (st.mode == client::SessionMode::Unsent) text += " | offline";
    // v0.12.0 background level analyzer: "Level analysis: searching 42%, 38 s" / "verified 96%" /
    // "cached" / "Record-Safe: waiting for the attempt to end" / "stopped (isolation)"
    if (auto analysis = analyzer::ui::hudSuffix(); !analysis.empty()) text += " | " + analysis;
    return text;
}

void attach(PlayLayer* pl) {
    detach();
    // the debug overlay belongs to the level, not to the HUD line (it works with show-hud off)
    s_traceLayer = pl && settings::get().enabled ? pl : nullptr;
    s_trace = {};
    ++s_traceVersion;
    s_traceDrawn = -1;
    s_traceAccum = kTraceRefreshSeconds;
    if (!pl || !settings::get().enabled) return;
    s_layer = pl;
    // v0.8.5: the bottom-left line and the middle-right timing-window history are independent
    // settings; the history (the "was that click frame perfect?" readout) used to be created only
    // when the line was on, so turning Show HUD off silently removed it
    if (settings::get().showHud) {
        s_label = CCLabelBMFont::create("", "bigFont.fnt");
        s_label->setID("hud-label"_spr);
        s_label->setScale(0.3f);
        s_label->setAnchorPoint({0.f, 0.f});
        s_label->setPosition({5.f, 4.f});
        s_label->setOpacity(210);
        s_label->setZOrder(1000);
        pl->addChild(s_label);
    }
    if (settings::get().showHud) {
        // v0.12.0 top-right panel on GD's UI layer (screen space), left of the pause button
        auto win = CCDirector::sharedDirector()->getWinSize();
        CCNode* parent = pl->m_uiLayer ? static_cast<CCNode*>(pl->m_uiLayer) : static_cast<CCNode*>(pl);
        s_topRight = win.width - 48.f;
        s_topTop = win.height - 4.f;
        if (pl->m_uiLayer) {
            if (auto* pause = pl->m_uiLayer->getChildByIDRecursive("pause-button"); pause && pause->getParent()) {
                auto bb = pause->boundingBox();
                auto worldMin = pause->getParent()->convertToWorldSpace(bb.origin);
                auto local = parent->convertToNodeSpace(worldMin);
                if (local.x > win.width * 0.5f && local.x < win.width) s_topRight = local.x - 6.f;
            }
        }
        s_topPanel = makePanel(kPanelTint, 170, 10.f, 10.f);
        s_topPanel->setID("hud-sigma-panel"_spr);
        s_topPanel->ignoreAnchorPointForPosition(false);
        s_topPanel->setAnchorPoint({1.f, 1.f});
        s_topPanel->setZOrder(1000);
        s_topBig = CCLabelBMFont::create("-", "bigFont.fnt");
        s_topBig->setScale(kTopBigScale);
        s_topBig->setAnchorPoint({1.f, 1.f});
        s_topSmall = CCLabelBMFont::create("", "chatFont.fnt");
        s_topSmall->setScale(kTopSmallScale);
        s_topSmall->setAnchorPoint({1.f, 1.f});
        s_topSmall->setColor({220, 220, 220});
        auto* liveText = CCLabelBMFont::create("LIVE", "bigFont.fnt");
        liveText->setScale(kLiveScale);
        float textW = liveText->getScaledContentSize().width, textH = liveText->getScaledContentSize().height;
        float dot = 4.f;
        s_liveTag = makePanel(kLiveRed, 235, textW + dot + 9.f, textH + 4.f);
        s_liveDot = CCLayerColor::create({255, 255, 255, 255}, dot, dot);
        s_liveDot->setPosition({3.f, (textH + 4.f - dot) * 0.5f});
        s_liveDot->runAction(CCRepeatForever::create(CCSequence::create(CCFadeTo::create(0.6f, 70), CCFadeTo::create(0.6f, 255), nullptr)));
        liveText->setAnchorPoint({0.f, 0.5f});
        liveText->setPosition({3.f + dot + 3.f, (textH + 4.f) * 0.5f});
        s_liveTag->addChild(s_liveDot);
        s_liveTag->addChild(liveText);
        s_liveTag->setVisible(false);
        s_topPanel->addChild(s_topBig);
        s_topPanel->addChild(s_topSmall);
        s_topPanel->addChild(s_liveTag);
        parent->addChild(s_topPanel);
        s_lastTopBig.clear();
        s_lastTopSmall.clear();
        s_lastLive = -1;
        layoutTop();
    }
    if (settings::get().showLastWindow) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        // A small dark panel on GD's own UI layer (screen space, above the level), middle-right:
        // the last 8 presses / releases, newest at the top (v0.5.1; one line before).
        CCNode* parent = pl->m_uiLayer ? static_cast<CCNode*>(pl->m_uiLayer) : static_cast<CCNode*>(pl);
        s_windowPanel = makePanel(kPanelTint, 165, kHistoryMinWidth, kHistoryLineHeight + 2.f * kHistoryPadY);
        s_windowPanel->setID("hud-last-window-panel"_spr);
        s_windowPanel->ignoreAnchorPointForPosition(false);
        s_windowPanel->setAnchorPoint({1.f, 0.5f});
        s_windowPanel->setPosition({win.width - 4.f, win.height * 0.5f});
        s_windowPanel->setZOrder(1000);
        parent->addChild(s_windowPanel);
        s_historyLabels.clear();
        s_windowAccum = kWindowRefreshSeconds;
        s_lastWindowText.clear();
        if (settings::debugEnabled()) {
            log::info("GPRL hud: history panel ({} lines) attached to {} at ({:.0f},{:.0f}), win {}x{}", display::kDisplay.historyLines,
                      pl->m_uiLayer ? "UILayer" : "PlayLayer", win.width - 4.f, win.height * 0.5f, win.width, win.height);
        }
    }
    s_accum = kRefreshSeconds;   // refresh on the first frame
    s_last.clear();
    tracker::refreshTrust();
    refresh();
}

void detach() {
    if (s_label && s_layer && s_label->getParent() == s_layer) s_label->removeFromParent();
    if (s_topPanel && s_topPanel->getParent()) s_topPanel->removeFromParent();
    if (s_windowPanel && s_windowPanel->getParent()) s_windowPanel->removeFromParent();
    removeTraceNodes();
    forget();
}

void forget() {
    s_label = nullptr;
    s_topPanel = nullptr;
    s_topBig = nullptr;
    s_topSmall = nullptr;
    s_liveTag = nullptr;
    s_liveDot = nullptr;
    s_lastTopBig.clear();
    s_lastTopSmall.clear();
    s_lastLive = -1;
    s_historyLabels.clear();
    s_windowPanel = nullptr;
    s_layer = nullptr;
    s_last.clear();
    s_lastWindowText.clear();
    // the trace nodes die with the layer; the last trace stays readable in the Session tab until
    // the next level starts (attach)
    s_traceNode = nullptr;
    s_tracePanel = nullptr;
    s_traceLabels.clear();
    s_traceLayer = nullptr;
    s_traceDrawn = -1;
}

void showTrace(solver::trace_view::View view) {
    s_trace = std::move(view);
    ++s_traceVersion;
}

solver::trace_view::View const& lastTrace() { return s_trace; }

void tick(float dt) {
    if (s_traceLayer) {
        s_traceAccum += dt;
        if (s_traceAccum >= kTraceRefreshSeconds) {
            s_traceAccum = 0.f;
            refreshTrace();
        }
    }
    if (s_windowPanel) {
        s_windowAccum += dt;
        if (s_windowAccum >= kWindowRefreshSeconds) {
            s_windowAccum = 0.f;
            refreshWindow();
        }
    }
    if (!s_layer) return;
    s_accum += dt;
    if (s_accum < kRefreshSeconds) return;
    s_accum = 0.f;
    tracker::refreshTrust();
    refresh();   // no-op without the line (show-hud off); the reset-generation check inside still runs
}

// ---- top-right notifications (v0.14.5, owner 2026-10-03: "make it so the notification pops up
// like the video notification in the top right and instead of a ! make it a i ... any other
// notifications will come through there") ----
// One look for every GPRL notification: the dark panel of the verification notice, top right,
// with a pulsing icon - a blue "i" for information, the red "!" for warnings and verification.
// On Geode's overlay layer, so a notification survives a scene change (menu <-> level) like
// Geode's own; several stack downward under the sigma/s panel instead of covering each other.

namespace {

constexpr int kToastTag = 0x6A7051;   // every GPRL top-right notification on the overlay
constexpr float kToastGap = 4.f;

/// The top edge for a new notification: under the sigma/s panel while a level shows it, then
/// under every GPRL notification still on screen.
float nextToastTop(CCNode* layer, float winH) {
    float top = winH - 6.f - (s_topPanel && s_topPanel->getParent() ? s_topPanel->getContentSize().height + 8.f : 0.f);
    if (auto* children = layer->getChildren()) {
        for (auto* child : CCArrayExt<CCNode*>(children)) {
            if (!child || child->getTag() != kToastTag) continue;
            // anchored top-right: its position is its top edge
            float bottom = child->getPositionY() - child->getContentSize().height;
            top = std::min(top, bottom - kToastGap);
        }
    }
    return top;
}

void showToast(std::string const& text, ToastKind kind, float seconds, char const* replaceId) {
    auto* director = CCDirector::sharedDirector();
    auto* layer = OverlayManager::get();
    if (!director || !layer) return;
    auto win = director->getWinSize();
    // a keyed notification (the verification notice) replaces its own older copy
    if (replaceId) {
        if (auto* old = layer->getChildByID(replaceId)) old->removeFromParent();
    }
    bool const warn = kind == ToastKind::Warning;
    auto* icon = CCLabelBMFont::create(warn ? "!" : "i", "bigFont.fnt");
    icon->setColor(warn ? ccColor3B{255, 70, 70} : ccColor3B{90, 170, 255});
    icon->setScale(0.85f);
    auto* label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt", 300.f, kCCTextAlignmentLeft);
    label->setScale(0.62f);
    label->setAnchorPoint({0.f, 0.5f});
    float iconW = std::max(icon->getScaledContentSize().width, 10.f);
    float iconH = icon->getScaledContentSize().height;
    float labelW = label->getScaledContentSize().width;
    float labelH = label->getScaledContentSize().height;
    float pad = 8.f;
    float w = pad + iconW + pad + labelW + pad;
    float h = std::max(iconH, labelH) + 2.f * pad;
    auto* panel = makePanel({24, 24, 30}, 230, w, h);
    if (replaceId) panel->setID(replaceId);
    panel->setTag(kToastTag);
    panel->ignoreAnchorPointForPosition(false);
    panel->setAnchorPoint({1.f, 1.f});
    panel->setPosition({win.width - 6.f, nextToastTop(layer, win.height)});
    panel->setZOrder(100000);
    panel->setCascadeOpacityEnabled(true);
    icon->setPosition({pad + iconW * 0.5f, h * 0.5f});
    label->setPosition({pad + iconW + pad, h * 0.5f});
    panel->addChild(icon);
    panel->addChild(label);
    icon->runAction(CCRepeatForever::create(CCSequence::create(CCScaleTo::create(0.4f, 1.0f), CCScaleTo::create(0.4f, 0.85f), nullptr)));
    panel->runAction(CCSequence::create(CCDelayTime::create(std::max(1.f, seconds)), CCFadeOut::create(0.6f), CCRemoveSelf::create(), nullptr));
    layer->addChild(panel);
    log::info("GPRL: notice ({}): {}", warn ? "!" : "i", text);
}

}  // namespace

void notify(std::string const& text, ToastKind kind, float seconds) { showToast(text, kind, seconds, nullptr); }

ToastKind toastKindOf(NotificationIcon icon) {
    return icon == NotificationIcon::Warning || icon == NotificationIcon::Error ? ToastKind::Warning : ToastKind::Info;
}

void verificationToast(std::string const& text, float seconds) {
    showToast(text, ToastKind::Warning, seconds, "verification-toast"_spr);
}

void familyNotice(std::vector<std::string> lines, float seconds) {
    if (lines.empty()) return;
    auto* director = CCDirector::sharedDirector();
    auto* scene = director ? director->getRunningScene() : nullptr;
    if (!scene) return;
    auto win = director->getWinSize();
    // one at a time, and never the verification toast's slot
    if (auto* old = scene->getChildByID("family-notice"_spr)) old->removeFromParent();
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) text += "\n";
        text += lines[i];
    }
    auto* label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt", 260.f, kCCTextAlignmentLeft);
    label->setScale(0.5f);
    label->setAnchorPoint({0.f, 0.5f});
    float labelW = label->getScaledContentSize().width;
    float labelH = label->getScaledContentSize().height;
    float pad = 6.f;
    float w = labelW + 2.f * pad;
    float h = labelH + 2.f * pad;
    auto* panel = makePanel({16, 18, 34}, 215, w, h);
    panel->setID("family-notice"_spr);
    panel->ignoreAnchorPointForPosition(false);
    panel->setAnchorPoint({0.f, 1.f});
    panel->setPosition({6.f, win.height - 6.f});
    panel->setZOrder(99999);
    panel->setCascadeOpacityEnabled(true);
    label->setPosition({pad, h * 0.5f});
    panel->addChild(label);
    panel->runAction(CCSequence::create(CCDelayTime::create(std::max(1.f, seconds)), CCFadeOut::create(0.6f), CCRemoveSelf::create(), nullptr));
    scene->addChild(panel);
    log::info("GPRL: family notice: {}", text);
}

}  // namespace gprl::hud
