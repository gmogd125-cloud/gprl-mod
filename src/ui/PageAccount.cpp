// Account screen of the GPRL menu: the connection, the website (profile link / one-time sign-in
// code), clipping and the mod's data + Settings. Behaviour of every action is unchanged from the
// v0.2.1 - v0.10.0 popup (Connect.cpp, Clipper.cpp); only the arrangement is new.
#include <algorithm>
#include <cmath>

#include <chrono>

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

    std::string key = fmt::format("account|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", static_cast<int>(st.connection),
                                  st.connecting, st.connectMessage, st.displayName, st.username, st.identityVerified, st.localOnly, st.apiPlaceholder, st.apiBaseUrl,
                                  settings::get().siteOrigin, analysisLine, st.sessionOpen, st.sessionId, static_cast<int>(st.mode), st.batchesSent, st.batchesSpooled,
                                  st.batchesFailed, st.eventsPending, st.lastError, cl.bufferLine, clipLine, cl.config.enabled, cl.canClipLastAttempt, pending, retryId,
                                  linkId, codePanel, wc.code, wc.pending, wc.error, wc.secondsLeft > 0, busy, st.resetPending);
    key += fmt::format("|{}|{}|{}", st.webLoginPending, localstore::recordsWritten(), settings::get().recordSafe);
    std::string const plan = st.connected ? planText(st) : std::string();
    bool const patreonLinked = st.entitlement.valid && st.entitlement.patreonConnected;
    key += fmt::format("|{}|{}|{}|{}|{}", plan, patreonLinked, st.patreonConnectPending, st.patreonSyncPending, st.patreonConfirmPending);
    if (!beginPage(key)) return;
    m_retryClipId = retryId;
    m_linkClipId = linkId;
    float const W = kContentW, H = kContentH;
    float const cardW = W - 12.f;
    auto menu = pageMenu();
    float y = H - 6.f;

    // ---- 1. connection ----
    float const h1 = st.connected ? 67.f : 54.f;   // v0.12.1: + the plan line while connected
    y -= h1;
    auto c1 = card(m_page, 6.f, y, cardW, h1, "Connection");
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
        auto d = ring(4.f, 4.f, 1.0, dot, dot, 255);
        d->setPosition({kPad + 4.f, h1 - 24.f});
        c1->addChild(d);
        float const buttonRoom = 96.f;
        text(c1, line, kPad + 12.f, h1 - 24.f, cardW - kPad - 12.f - buttonRoom, color, 0.36f, kChat, {0.f, 0.5f});
        std::string sub;
        ccColor3B subColor = kGrey;
        if (!st.connecting && !st.connected && st.connectMessage.rfind("Connect failed", 0) == 0) {
            sub = st.connectMessage;
            subColor = kOrange;
        }
        else if (st.apiPlaceholder) {
            sub = "API URL not set (placeholder or invalid): nothing is sent - fix it in the mod settings";
            subColor = kOrange;
        }
        else if (st.localOnly) {
            sub = "Local-only mode: batches are written to the mod save folder, nothing is sent";
            subColor = kOrange;
        }
        else if (st.connected) sub = fmt::format("GPRL player {}   -   {}", st.username, st.apiBaseUrl);
        else sub = "Connect uses the Geometry Dash account you are logged in with; no website account is needed.";
        text(c1, sub, kPad, h1 - 39.f, cardW - 2.f * kPad - buttonRoom, subColor, 0.3f, kChat, {0.f, 0.5f});
        CCMenuItemSpriteExtra* btn = st.connected ? button(menu, "Disconnect", "GJ_button_06.png", 0.42f, this, menu_selector(GprlMenu::onDisconnect))
                                                  : button(menu, "Connect", "GJ_button_01.png", 0.55f, this, menu_selector(GprlMenu::onConnect));
        btn->setPosition({6.f + cardW - kPad - btn->getScaledContentSize().width / 2.f, y + h1 - 31.f});
        setButtonEnabled(btn, !busy);
        if (st.connected) {
            // v0.12.1 (docs/contracts/patreon.md): the plan as the SERVER answered it (GET /v1/me/entitlements,
            // memory only) with Connect Patreon + Enter Patreon code (not linked yet) or Sync Patreon (linked)
            // at the right end of the line
            float const lineY = y + h1 - 54.f;
            float right = 6.f + cardW - kPad;
            if (patreonLinked) {
                auto sync = button(menu, "Sync Patreon", "GJ_button_02.png", 0.3f, this, menu_selector(GprlMenu::onPatreonSync));
                sync->setPosition({right - sync->getScaledContentSize().width / 2.f, lineY});
                right -= sync->getScaledContentSize().width + 5.f;
                setButtonEnabled(sync, !st.patreonSyncPending);
            }
            else {
                auto code = button(menu, "Enter Patreon code", "GJ_button_01.png", 0.3f, this, menu_selector(GprlMenu::onPatreonCode));
                code->setPosition({right - code->getScaledContentSize().width / 2.f, lineY});
                right -= code->getScaledContentSize().width + 5.f;
                setButtonEnabled(code, !st.patreonConfirmPending);
                auto connectP = button(menu, "Connect Patreon", "GJ_button_01.png", 0.3f, this, menu_selector(GprlMenu::onPatreonConnect));
                connectP->setPosition({right - connectP->getScaledContentSize().width / 2.f, lineY});
                right -= connectP->getScaledContentSize().width + 5.f;
                setButtonEnabled(connectP, !st.patreonConnectPending);
            }
            text(c1, plan, kPad, h1 - 54.f, right - 6.f - kPad - 4.f, planColor(st), 0.3f, kChat, {0.f, 0.5f});
        }
    }

    // ---- 2. website ----
    float const h2 = 52.f;
    y -= 6.f + h2;
    auto c2 = card(m_page, 6.f, y, cardW, h2, codePanel ? "Website sign-in code" : "Website");
    if (codePanel) {
        bool const live = !wc.code.empty() && wc.secondsLeft > 0;
        std::string codeText = !wc.code.empty() ? wc.code : (wc.pending ? "requesting..." : "no code");
        text(c2, codeText, kPad, h2 - 24.f, cardW - 2.f * kPad - 110.f, !wc.code.empty() && live ? kWhite : kGrey, 0.72f, kChat, {0.f, 0.5f}, 0.35f);
        m_codeCountdown = text(c2, "", kPad, h2 - 40.f, cardW - 2.f * kPad - 110.f, kGrey, 0.3f, kChat, {0.f, 0.5f});
        m_copyBtn = button(menu, "Copy", "GJ_button_01.png", 0.42f, this, menu_selector(GprlMenu::onCopyCode));
        auto hide = button(menu, "Hide", "GJ_button_06.png", 0.36f, this, menu_selector(GprlMenu::onHideCode));
        float bx = 6.f + cardW - kPad;
        hide->setPosition({bx - hide->getScaledContentSize().width / 2.f, y + h2 / 2.f - 4.f});
        bx -= hide->getScaledContentSize().width + 6.f;
        m_copyBtn->setPosition({bx - m_copyBtn->getScaledContentSize().width / 2.f, y + h2 / 2.f - 4.f});
        setButtonEnabled(m_copyBtn, live);
    }
    else {
        text(c2, "Your profile, the leaderboard and run reviews live on the GPRL website. Sign in there with a one-time code from this menu.", kPad, h2 - 22.f,
             cardW - 2.f * kPad, kGrey, 0.3f, kChat, {0.f, 0.5f});
        auto profile = button(menu, "Open my profile", "GJ_button_02.png", 0.4f, this, menu_selector(GprlMenu::onProfile));
        auto code = button(menu, "Website code", "GJ_button_02.png", 0.4f, this, menu_selector(GprlMenu::onWebsiteCode));
        auto visit = button(menu, "Visit website", "GJ_button_04.png", 0.4f, this, menu_selector(GprlMenu::onVisitSite));
        float bx = 6.f + kPad;
        for (auto* b : {profile, code, visit}) {
            b->setPosition({bx + b->getScaledContentSize().width / 2.f, y + 12.f});
            bx += b->getScaledContentSize().width + 6.f;
        }
        bool canLogin = st.connected && !st.webLoginPending && !busy;
        setButtonEnabled(profile, canLogin);
        setButtonEnabled(code, canLogin);
    }

    // ---- 3. clipping ----
    float const h3 = 52.f;
    y -= 6.f + h3;
    auto c3 = card(m_page, 6.f, y, cardW, h3, "Clipping (evidence of exceptional runs)");
    {
        ccColor3B bufferColor = !cl.config.enabled ? kGrey : (cl.bufferProblem ? kOrange : (cl.capture.recording ? kGreen : kWhite));
        text(c3, cl.bufferLine, kPad, h3 - 22.f, cardW - 2.f * kPad, bufferColor, 0.3f, kChat, {0.f, 0.5f});
        text(c3, clipLine.empty() ? std::string("No clips yet - a run the server wants to verify, or Clip last attempt, makes one") : clipLine, kPad, h3 - 33.f,
             cardW - 2.f * kPad, clipColor, 0.28f, kChat, {0.f, 0.5f});
        struct ClipButton {
            bool show;
            bool enabled;
            char const* label;
            char const* texture;
            SEL_MenuHandler handler;
        };
        ClipButton const buttons[] = {
            {!linkId.empty(), true, "YouTube link", "GJ_button_01.png", menu_selector(GprlMenu::onClipLink)},
            {cl.config.enabled, cl.canClipLastAttempt, "Clip last attempt", "GJ_button_01.png", menu_selector(GprlMenu::onClipLast)},
            {pending || !retryId.empty(), true, "Clip choice", "GJ_button_02.png", menu_selector(GprlMenu::onClipChoice)},
            {!retryId.empty(), true, "Retry upload", "GJ_button_02.png", menu_selector(GprlMenu::onClipRetry)},
            {cl.config.enabled || !cl.clips.empty(), true, "Clips folder", "GJ_button_04.png", menu_selector(GprlMenu::onClipFolder)},
        };
        float bx = 6.f + kPad;
        for (auto const& cb : buttons) {
            if (!cb.show) continue;
            auto b = button(menu, cb.label, cb.texture, 0.32f, this, cb.handler);
            b->setPosition({bx + b->getScaledContentSize().width / 2.f, y + 10.f});
            bx += b->getScaledContentSize().width + 5.f;
            setButtonEnabled(b, cb.enabled);
        }
        if (!cl.config.enabled && cl.clips.empty()) {
            text(c3, "Clipping is off (mod settings): turn it on to keep a video of exceptional runs for review.", kPad, 10.f, cardW - 2.f * kPad, kDim, 0.28f, kChat,
                 {0.f, 0.5f});
        }
    }

    // ---- 4. the mod's data and tools (the rest of the room) ----
    y -= 6.f;
    float const h4 = y - 6.f;
    auto c4 = card(m_page, 6.f, 6.f, cardW, h4, "Mod");
    {
        float ly = h4 - 18.f;
        float const lw = cardW - 2.f * kPad;
        float const lwShort = lw - 180.f;   // the third line shares its height with the button row
        text(c4, analysisLine, kPad, ly, lw, settings::get().recordSafe ? kGold : kGrey, 0.3f, kChat, {0.f, 0.5f});
        ly -= 10.f;
        // one line shorter while connected (the plan line above): without room for a third line a
        // last error takes the session line's place (the counters stay in the Details popup)
        bool const room3 = h4 >= 60.f;
        std::string session = view.open || st.sessionOpen
            ? fmt::format("Session {} ({}), attempt #{}  -  ", st.sessionId.empty() ? "opening" : st.sessionId, client::name(st.mode), view.attemptNo)
            : std::string("No session open  -  ");
        session += fmt::format("batches sent {}  spooled {}  failed {}  -  events pending {}", st.batchesSent, st.batchesSpooled, st.batchesFailed, st.eventsPending);
        if (!room3 && !st.lastError.empty()) text(c4, "Last error: " + st.lastError, kPad, ly, lwShort, kRed, 0.3f, kChat, {0.f, 0.5f});
        else text(c4, session, kPad, ly, room3 ? lw : lwShort, kGrey, 0.3f, kChat, {0.f, 0.5f});
        ly -= 10.f;
        if (room3) {
            if (!st.lastError.empty()) text(c4, "Last error: " + st.lastError, kPad, ly, lwShort, kRed, 0.3f, kChat, {0.f, 0.5f});
            else text(c4, fmt::format("spool: {} records", localstore::recordsWritten()), kPad, ly, lwShort, kDim, 0.28f, kChat, {0.f, 0.5f});
        }
        auto settingsBtn = button(menu, "Settings", "GJ_button_05.png", 0.4f, this, menu_selector(GprlMenu::onSettings));
        auto flush = button(menu, "Flush", "GJ_button_04.png", 0.36f, this, menu_selector(GprlMenu::onFlush));
        auto reset = button(menu, "Reset data", "GJ_button_06.png", 0.36f, this, menu_selector(GprlMenu::onResetData));
        float bx = 6.f + cardW - kPad;
        float const by = 6.f + 12.f;
        for (auto* b : {settingsBtn, flush, reset}) {
            b->setPosition({bx - b->getScaledContentSize().width / 2.f, by});
            bx -= b->getScaledContentSize().width + 6.f;
        }
        setButtonEnabled(reset, st.connected && !busy && !st.resetPending);
        reset->setVisible(st.connected);
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
    m_codeCountdown->setString(s.c_str());
    m_codeCountdown->setScale(0.3f);
    m_codeCountdown->limitLabelWidth(kContentW - 12.f - 2.f * kPad - 110.f, 0.3f, 0.2f);
    m_codeCountdown->setColor(color);
    setButtonEnabled(m_copyBtn, live);
}

}  // namespace gprl::ui
