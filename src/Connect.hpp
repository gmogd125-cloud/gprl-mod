#pragma once
// "Connect" / "Open my profile" (v0.2.0): the player never makes a website account. Identity comes
// from the Geometry Dash account that is logged in:
//
//   Connect         main thread: require a GD login (GJAccountManager::m_accountID > 0), start
//                   argon::startAuth() on Geode's async runtime (several seconds; the game keeps
//                   running, the popup shows "Connecting..."), then hand
//                   { accountId, userId, username, argonToken, modList } to the telemetry worker,
//                   which calls POST /v1/client/connect and saves the device token together with
//                   the GD account id (Telemetry.cpp). The server validates the Argon token with
//                   argon.globed.dev; the GD password / GJP never leaves the game.
//   Open my profile POST /v1/client/web-login on the worker -> one-time URL -> validated against
//                   the `site-url` origin (core/identity profileUrlAllowed) -> default browser.
//   Website code    (v0.2.1) the same POST, but the response's `code` (XXXXX-XXXXX-XXXXX-XXXXX,
//                   validated by core/identity formatWebLoginCode) is shown in the popup's
//                   "Website sign-in code" panel with a Copy button and a countdown, for the
//                   website's paste box. Only one web-login request is in flight at a time, and
//                   s_webLoginUse remembers which button asked. The code is never logged.
//   Disconnect      forgets the device token locally (Telemetry.cpp) and the website code.
//
// Threads: every function here runs on the main thread. The worker reports back through
// onServerResult / onWebLoginResult (queued with queueInMainThread).
#include "Api.hpp"

namespace gprl::connect {

/// Popup "Connect" (and the Retry of the identity-failure dialog).
void start();
/// True while Argon auth or the server request runs.
bool inProgress();
/// Popup "Open my profile".
void openProfile();

/// Popup "Website code": asks for a code to show (never opens the browser).
void requestWebsiteCode();

/// What the popup's "Website sign-in code" panel shows (main-thread snapshot, cheap: call it on
/// every popup tick for the countdown).
struct WebsiteCode {
    bool pending = false;        // a Website code request is in flight
    std::string code;            // display form; empty = no code yet (or forgotten)
    int secondsLeft = 0;         // 0 = expired (code still shown, greyed)
    std::string error;           // why the last request gave no code (empty = none)
};
WebsiteCode websiteCode();
/// Copies the current code to the clipboard; false when there is none or it expired.
bool copyWebsiteCode();
/// Forgets the current code (popup Hide, Disconnect).
void clearWebsiteCode();
/// Popup "Disconnect" (asks first).
void disconnect();
/// Popup "Reset data" (asks first): the server removes the player's own progress, the PC its spool
/// (docs/contracts/reset.md). Connection and account stay.
void resetData();

/// Worker -> main thread: outcome of POST /v1/client/connect.
void onServerResult(api::ConnectResult const& result, int64_t accountId);
/// Worker -> main thread: outcome of POST /v1/client/web-login.
void onWebLoginResult(api::WebLoginResult const& result);
/// Worker -> main thread: outcome of POST /v1/me/reset.
void onResetResult(api::ResetResult const& result);

}  // namespace gprl::connect
