// Account screen of the GPRL menu: the connection (+ the Patreon plan), the website (profile
// link / one-time sign-in code), clipping and the mod's data + Settings. Behaviour of every action
// is unchanged from the v0.2.1 - v0.12.2 popup (Connect.cpp, Clipper.cpp, Telemetry.cpp); only the
// arrangement is new.
//
// Layout rules (owner feedback 2026-10-03): every button is scaled as a whole to a small exact
// height (Widgets `buttonRow`), body text is 8-9 px, and a card's height follows its text.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "../../core/clip_flow.hpp"
#include "../../core/display.hpp"
#include "../../core/entitlements.hpp"
#include "../../core/identity.hpp"
#include "../Clipper.hpp"
#include "../Connect.hpp"
#include "../LocalStore.hpp"
#include "../PatreonCodePopup.hpp"
#include "../Settings.hpp"
#include "../Tracker.hpp"
#include "../analyzer/Status.hpp"
#include "Menu.hpp"
#include "Widgets.hpp"

using namespace geode::prelude;

namespace gprl::ui {

namespace {

using namespace theme;

constexpr float kCodeButtonRoom = 96.f;   // Copy + Hide at the right of the sign-in code

/// v0.12.1: "Plan: GPRL Pro (Patreon)   Last synchronized: 2 min ago" (core/entitlements texts,
/// host-tested), or why the plan is not known yet. Never a plan the server did not send.
std::string planText(client::Status const& st) {
    auto const& e = st.entitlement;
    if (e.valid) {
        std::string text = entitlements::planLine(e);
        if (e.patreonConnected) {
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            text += "   " + entitlements::lastSyncLine(e.lastSyncAt, now);
        }
        return text;
    }
    if (!st.entitlementError.empty()) return "Plan: not known (" + display::asciiDash(st.entitlementError) + ") - Free until the server answers";
    if (st.localOnly || st.apiPlaceholder) return "Plan: not fetched (local-only mode / API not set)";
    return "Plan: asking the GPRL server...";
}

ccColor3B planColor(client::Status const& st) {
    auto const& e = st.entitlement;
    if (!e.valid) return st.entitlementError.empty() ? kGrey : kOrange;
    if (e.status == entitlements::Membership::PastDue) return kOrange;
    return e.plan == entitlements::Plan::Free ? kGrey : ccColor3B{255, 200, 90};
}

}  // namespace

void GprlMenu::buildAccount() {
    auto st = client::status();
    auto view = tracker::attemptView();
    auto cl = clipper::status();
    auto wc = connect::websiteCode();
    bool const busy = connect::inProgress();
    if (!st.connected) m_codeVisible = false;
    bool const codePanel = st.connected && m_codeVisible;

    // the newest clip's state
    std::string clipLine;
    ccColor3B clipColor = kDim;
    if (!cl.clips.empty()) {
        auto const& r = cl.clips.front();
        clipColor = kWhite;
        if (r.state == clip::ClipState::UploadFailed || r.state == clip::ClipState::Failed) clipColor = kOrange;
        else if (r.state == clip::ClipState::Uploaded || r.state == clip::ClipState::Saved) clipColor = kGreen;
        else if (clip::awaitingChoice(r)) clipColor = kGold;
        clipLine = clip::statusLine(r, r.clipId == cl.uploadClipId ? cl.uploadFraction : 0.0);
    }
    bool const pending = clipper::pendingClip().has_value();
    std::string retryId;
    for (auto const& r : cl.clips) {
        if (r.state == clip::ClipState::UploadFailed) {
            retryId = r.clipId;
            break;
        }
    }
    std::string linkId = cl.linkNeeded.empty() ? std::string() : cl.linkNeeded.front().clipId;
    std::string analysisLine = analyzer::ui::accountLine();
    std::string const plan = st.connected ? planText(st) : std::string();
    bool const patreonLinked = st.entitlement.valid && st.entitlement.patreonConnected;

    std::string key = fmt::format("account|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", static_cast<int>(st.connection),
                                  st.connecting, st.connectMessage, st.displayName, st.username, st.identityVerified, st.localOnly, st.apiPlaceholder, st.apiBaseUrl,
                                  settings::get().siteOrigin, analysisLine, st.sessionOpen, view.attemptNo, static_cast<int>(st.mode), st.batchesSent, st.batchesSpooled,
                                  st.batchesFailed, st.eventsPending, st.lastError, cl.bufferLine, clipLine, cl.config.enabled, cl.canClipLastAttempt, pending, retryId,
                                  linkId, codePanel, wc.code, wc.pending, wc.error, wc.secondsLeft > 0, busy, st.resetPending);
    key += fmt::format("|{}|{}|{}|{}", st.webLoginPending, localstore::recordsWritten(), settings::get().recordSafe, view.open);
    key += fmt::format("|{}|{}|{}|{}|{}", plan, patreonLinked, st.patreonConnectPending, st.patreonSyncPending, st.patreonConfirmPending);
    if (!beginPage(key)) return;
    m_retryClipId = retryId;
    m_linkClipId = linkId;
    float const W = kContentW, H = kContentH;
    float const cardX = kGap, cardW = W - 2.f * kGap;
    float const inner = cardW - 2.f * kPad;
    float const rightEdge = cardX + cardW - kPad;   // page x of a card's inner right edge
    auto menu = pageMenu();
    float y = H - kGap;   // top of the next card

    // ---- 1. connection (+ the plan line while connected) ----
    {
        std::string line;
        ccColor3B color = kWhite;
        ccColor3B dot = kDim;
        if (st.connecting) {
            line = st.connectMessage.empty() ? std::string("Connecting...") : st.connectMessage;
            dot = kGold;
        }
        else if (st.connected) {
            line = st.identityVerified ? identity::describe(st.connection, st.displayName) : fmt::format("Connected as {} (GD account NOT verified by this server)", st.displayName);
            color = st.identityVerified ? kGreen : kOrange;
            dot = color;
        }
        else {
            line = identity::describe(st.connection, "");
            dot = kOrange;
        }
        std::string sub;
        ccColor3B subColor = kGrey;
        if (!st.connecting && !st.connected && st.connectMessage.rfind("Connect failed", 0) == 0) {
            sub = st.connectMessage;
            subColor = kOrange;
        }
        else if (st.apiPlaceholder) {
            sub = "API URL not set: nothing is sent. Fix it in the mod settings.";
            subColor = kOrange;
        }
        else if (st.localOnly) {
            sub = "Local-only mode: telemetry stays on this computer.";
            subColor = kOrange;
        }
        else if (st.connected) sub = fmt::format("GPRL player {}   -   {}", st.username, st.apiBaseUrl);
        else sub = "Uses the GD account you are logged in with. No website account needed.";

        float const buttonRoom = 92.f;
        float const textW = inner - 12.f - buttonRoom;
        auto linePara = makeParagraph(line, textW, kBody, color, 2);
        float const lineH = linePara->getHeight();
        float const planRow = st.connected ? 18.f : 0.f;
        float const h1 = kTitleH + lineH + 3.f + 8.f + planRow + 6.f;
        y -= h1;
        auto c1 = card(m_page, cardX, y, cardW, h1, "Connection");
        float const top = h1 - kTitleH;
        auto d = ring(3.5f, 3.5f, 1.0, dot, dot, 255);
        d->setPosition({kPad + 4.f, top - 4.5f});
        c1->addChild(d);
        place(c1, linePara, kPad + 12.f, top);
        float const subY = top - lineH - 3.f - 4.f;
        text(c1, sub, kPad, subY, inner - buttonRoom, subColor, kSmall, kChat, {0.f, 0.5f});
        // Connect / Disconnect: right side, centred on the two text lines
        float const blockMid = y + top - (lineH + 11.f) / 2.f;
        auto main = st.connected ? button(menu, "Disconnect", "GJ_button_06.png", 16.f, this, menu_selector(GprlMenu::onDisconnect))
                                 : button(menu, "Connect", "GJ_button_01.png", 20.f, this, menu_selector(GprlMenu::onConnect));
        main->setPosition({rightEdge - sizeOf(main).width / 2.f, blockMid});
        setButtonEnabled(main, !busy);
        if (st.connected) {
            // v0.12.1 (docs/contracts/patreon.md): the plan as the SERVER answered it (GET /v1/me/entitlements,
            // memory only) with Connect Patreon + Enter Patreon code (not linked yet) or Sync Patreon (linked)
            // at the right end of the line
            float const rowY = subY - 13.f;
            std::vector<ButtonSpec> specs;
            if (patreonLinked) specs.push_back({"Sync Patreon", "GJ_button_02.png", menu_selector(GprlMenu::onPatreonSync), !st.patreonSyncPending});
            else {
                specs.push_back({"Enter Patreon code", "GJ_button_01.png", menu_selector(GprlMenu::onPatreonCode), !st.patreonConfirmPending});
                specs.push_back({"Connect Patreon", "GJ_button_01.png", menu_selector(GprlMenu::onPatreonConnect), !st.patreonConnectPending});
            }
            float used = 0.f;
            buttonRow(menu, specs, rightEdge, y + rowY, 13.f, inner * 0.55f, this, true, 5.f, &used);
            text(c1, plan, kPad, rowY, inner - used - 8.f, planColor(st), kSmall, kChat, {0.f, 0.5f});
        }
    }

    // ---- 2. website ----
    {
        float const h2 = 54.f;
        y -= kGap + h2;
        auto c2 = card(m_page, cardX, y, cardW, h2, codePanel ? "Website sign-in code" : "Website");
        float const top = h2 - kTitleH;
        if (codePanel) {
            bool const live = !wc.code.empty() && wc.secondsLeft > 0;
            std::string codeText = !wc.code.empty() ? wc.code : (wc.pending ? "requesting..." : "no code");
            text(c2, codeText, kPad, top - 8.f, inner - kCodeButtonRoom, !wc.code.empty() && live ? kWhite : kGrey, 0.9f, kChat, {0.f, 0.5f}, 0.5f);
            m_codeCountdown = text(c2, "", kPad, top - 24.f, inner - kCodeButtonRoom, kGrey, kSmall, kChat, {0.f, 0.5f});
            auto row = buttonRow(menu,
                                 {{"Hide", "GJ_button_06.png", menu_selector(GprlMenu::onHideCode), true},
                                  {"Copy", "GJ_button_01.png", menu_selector(GprlMenu::onCopyCode), live}},
                                 rightEdge, y + top - 14.f, 17.f, kCodeButtonRoom - 8.f, this, true);
            m_copyBtn = row.size() > 1 ? row[1] : nullptr;
        }
        else {
            text(c2, "Your profile, the board and run reviews are on the GPRL website.", kPad, top - 4.5f, inner, kGrey, kSmall, kChat, {0.f, 0.5f});
            bool const canLogin = st.connected && !st.webLoginPending && !busy;
            buttonRow(menu,
                      {{"Open my profile", "GJ_button_02.png", menu_selector(GprlMenu::onProfile), canLogin},
                       {"Website code", "GJ_button_02.png", menu_selector(GprlMenu::onWebsiteCode), canLogin},
                       {"Visit website", "GJ_button_04.png", menu_selector(GprlMenu::onVisitSite), true}},
                      cardX + kPad, y + 14.f, 16.f, inner, this);
        }
    }

    // ---- 3. clipping ----
    {
        float const h3 = 61.f;
        y -= kGap + h3;
        auto c3 = card(m_page, cardX, y, cardW, h3, "Clipping (evidence of exceptional runs)");
        float const top = h3 - kTitleH;
        ccColor3B bufferColor = !cl.config.enabled ? kGrey : (cl.bufferProblem ? kOrange : (cl.capture.recording ? kGreen : kWhite));
        text(c3, cl.bufferLine, kPad, top - 4.5f, inner, bufferColor, kSmall, kChat, {0.f, 0.5f});
        text(c3, clipLine.empty() ? std::string("No clips yet: a run the server wants to verify, or Clip last attempt, makes one.") : clipLine, kPad, top - 15.f, inner,
             clipColor, kTiny, kChat, {0.f, 0.5f});
        std::vector<ButtonSpec> specs;
        if (!linkId.empty()) specs.push_back({"YouTube link", "GJ_button_01.png", menu_selector(GprlMenu::onClipLink), true});
        if (cl.config.enabled) specs.push_back({"Clip last attempt", "GJ_button_01.png", menu_selector(GprlMenu::onClipLast), cl.canClipLastAttempt});
        if (pending || !retryId.empty()) specs.push_back({"Clip choice", "GJ_button_02.png", menu_selector(GprlMenu::onClipChoice), true});
        if (!retryId.empty()) specs.push_back({"Retry upload", "GJ_button_02.png", menu_selector(GprlMenu::onClipRetry), true});
        if (cl.config.enabled || !cl.clips.empty()) specs.push_back({"Clips folder", "GJ_button_04.png", menu_selector(GprlMenu::onClipFolder), true});
        if (!specs.empty()) buttonRow(menu, specs, cardX + kPad, y + 13.f, 14.f, inner, this);
        else text(c3, "Clipping is off: turn it on in the mod settings to keep a video of exceptional runs.", kPad, 13.f, inner, kDim, kTiny, kChat, {0.f, 0.5f});
    }

    // ---- 4. the mod's data and tools (the rest of the room) ----
    {
        y -= kGap;
        float const h4 = y - kGap;
        auto c4 = card(m_page, cardX, kGap, cardW, h4, "Mod");
        float const top = h4 - kTitleH;
        text(c4, analysisLine, kPad, top - 4.5f, inner, settings::get().recordSafe ? kGold : kGrey, kSmall, kChat, {0.f, 0.5f});
        // the bottom row: the session in a few words on the left, the tools on the right
        std::vector<ButtonSpec> specs = {{"Settings", "GJ_button_05.png", menu_selector(GprlMenu::onSettings), true},
                                         {"Flush", "GJ_button_04.png", menu_selector(GprlMenu::onFlush), true}};
        if (st.connected) specs.push_back({"Reset data", "GJ_button_06.png", menu_selector(GprlMenu::onResetData), !busy && !st.resetPending});
        float used = 0.f;
        float const rowY = 13.f;
        buttonRow(menu, specs, rightEdge, kGap + rowY, 15.f, inner * 0.6f, this, true, 5.f, &used);
        std::string session = view.open || st.sessionOpen
            ? fmt::format("Session: {}   attempt #{}   sent {}   failed {}   pending {}", client::name(st.mode), view.attemptNo, st.batchesSent, st.batchesFailed,
                          st.eventsPending)
            : fmt::format("No session open   sent {}   spooled {}   failed {}", st.batchesSent, st.batchesSpooled, st.batchesFailed);
        bool const room3 = top - 4.5f - rowY >= 20.f;   // a line of its own between the analysis line and the row
        if (room3) {
            if (!st.lastError.empty()) text(c4, "Last error: " + st.lastError, kPad, top - 15.f, inner, kRed, kTiny, kChat, {0.f, 0.5f});
            else text(c4, fmt::format("Spool: {} records kept on this computer", localstore::recordsWritten()), kPad, top - 15.f, inner, kDim, kTiny, kChat, {0.f, 0.5f});
            text(c4, session, kPad, rowY, inner - used - 8.f, kGrey, kTiny, kChat, {0.f, 0.5f});
        }
        else if (!st.lastError.empty()) text(c4, "Last error: " + st.lastError, kPad, rowY, inner - used - 8.f, kRed, kTiny, kChat, {0.f, 0.5f});
        else text(c4, session, kPad, rowY, inner - used - 8.f, kGrey, kTiny, kChat, {0.f, 0.5f});
    }
}

void GprlMenu::tickAccount() {
    if (!m_codeCountdown) return;
    auto wc = connect::websiteCode();
    bool const live = !wc.code.empty() && wc.secondsLeft > 0;
    std::string s;
    ccColor3B color = kGrey;
    if (!wc.code.empty()) {
        s = identity::webLoginCodeStatus(wc.secondsLeft) + "  -  " + identity::webLoginCodeHint(settings::get().siteOrigin);
        color = live ? (wc.secondsLeft <= 30 ? kGold : kGreen) : kOrange;
    }
    else if (wc.pending) s = "asking the GPRL server for a one-time code";
    else {
        s = wc.error.empty() ? std::string("press Website code again") : "failed - " + wc.error;
        color = kOrange;
    }
    fitLabel(m_codeCountdown, s, kContentW - 2.f * kGap - 2.f * kPad - kCodeButtonRoom, kSmall);
    m_codeCountdown->setColor(color);
    setButtonEnabled(m_copyBtn, live);
}

}  // namespace gprl::ui
