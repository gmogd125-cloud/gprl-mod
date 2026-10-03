#include "Connect.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/web.hpp>

#include <argon/argon.hpp>

#include <chrono>

#include "../core/identity.hpp"
#include "Environment.hpp"
#include "Settings.hpp"
#include "Telemetry.hpp"

using namespace geode::prelude;

namespace gprl::connect {

namespace {

using Clock = std::chrono::steady_clock;

/// Argon auth normally takes a few seconds (it may retry); after this long a stuck attempt no
/// longer blocks a new Connect (the old one is cancelled).
constexpr auto kStaleConnect = std::chrono::seconds(90);
constexpr char const* kIdentityInvalid = "gd_identity_invalid";   // /v1/client/connect 401: ApiErrorBody details.reason (code is "unauthorized")

Clock::time_point s_startedAt{};

/// Which popup button asked for the web-login request in flight (one at a time: both entry points
/// return while Status.webLoginPending is set).
enum class WebLoginUse { Profile, Code };
WebLoginUse s_webLoginUse = WebLoginUse::Profile;

/// The website sign-in code shown in the popup (main thread only). The deadline is a local
/// steady-clock point set from the server's expiresAt (core/identity webLoginCodeSecondsLeft), so
/// the countdown is immune to later clock changes.
struct {
    std::string code;            // display form, validated; empty = none
    Clock::time_point deadline{};
    std::string error;
} s_websiteCode;

int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}


/// Argon's future runs on Geode's async runtime and its callback on the main thread. The holder is
/// deliberately never destroyed: closing the popup must not cancel a Connect in progress, and a
/// static destructor must not touch the async runtime after it has shut down at game exit.
async::TaskHolder<Result<std::string>>& argonTask() {
    static auto* holder = new async::TaskHolder<Result<std::string>>();
    return *holder;
}

void notify(std::string const& text, NotificationIcon icon, float seconds = NOTIFICATION_DEFAULT_TIME) {
    Notification::create(text, icon, seconds)->show();
}

/// A level is running and not paused: never put a dialog in front of gameplay.
bool playingUnpaused() {
    auto* pl = PlayLayer::get();
    return pl && !pl->m_isPaused && !pl->m_hasCompletedLevel;
}

void offerRetry(std::string const& text) {
    if (playingUnpaused()) {
        notify("GPRL: connect failed - pause and open the GPRL popup to try again", NotificationIcon::Error, 5.f);
        return;
    }
    createQuickPopup("GPRL", text, "Cancel", "Retry", 360.f, [](FLAlertLayer*, bool retry) {
        if (retry) start();
    });
}

/// Refuses a web-login request before it is sent; returns false (with the notification) when it
/// must not go out.
bool webLoginPreflight(char const* what) {
    auto st = client::status();
    if (!st.connected) {
        notify(std::string("GPRL: press Connect first to ") + what, NotificationIcon::Warning);
        return false;
    }
    if (st.webLoginPending) return false;
    if (settings::effectiveLocalOnly()) {
        notify("GPRL: local-only mode is on - nothing is sent to the server", NotificationIcon::Warning);
        return false;
    }
    return true;
}

}  // namespace

bool inProgress() {
    return client::status().connecting && Clock::now() - s_startedAt < kStaleConnect;
}

void start() {
    if (inProgress()) {
        notify("GPRL: already connecting...", NotificationIcon::Info);
        return;
    }
    argonTask().cancel();   // a stale attempt (see kStaleConnect), if any
    if (settings::effectiveLocalOnly()) {
        notify("GPRL: local-only mode is on (or the API URL is invalid) - turn it off in the mod settings to connect", NotificationIcon::Warning);
        return;
    }
    auto account = argon::getGameAccountData();   // GJAccountManager m_accountID / m_username, GameManager m_playerUserID
    if (account.accountId <= 0) {
        client::connectFailed("Not connected - log in to your Geometry Dash account first (Settings > Account)");
        notify("Log in to your Geometry Dash account first (Settings > Account)", NotificationIcon::Warning, 4.f);
        return;
    }
    if (!account.valid()) {
        client::connectFailed("Not connected - your GD login looks incomplete; refresh login in Settings > Account");
        notify("Your Geometry Dash login looks incomplete: refresh login in Settings > Account, then Connect", NotificationIcon::Warning, 4.f);
        return;
    }

    s_startedAt = Clock::now();
    client::setConnecting("Connecting... (verifying your GD account with Argon, this can take a few seconds)");
    auto modList = env::capture("").modList;
    log::info("GPRL: connecting GD account {} ({})", account.username, account.accountId);

    argon::AuthOptions options;
    options.account = account;
    options.progress = [](argon::AuthProgress p) {
        // runs on the async runtime: Status is mutex-protected
        client::setConnectProgress(fmt::format("Connecting... ({})", argon::authProgressToString(p)));
    };
    argonTask().spawn(
        "gprl-argon-auth", argon::startAuth(std::move(options)),
        [account, modList = std::move(modList)](Result<std::string> result) mutable {
            if (result.isErr()) {
                std::string err = result.unwrapErr();
                log::warn("GPRL: Argon authentication failed: {}", err);
                client::connectFailed("Connect failed: could not verify your GD account (Argon)");
                offerRetry("Could not verify your Geometry Dash account:\n\n" + err);
                return;
            }
            api::ConnectRequest rq;
            rq.accountId = account.accountId;
            rq.userId = account.userId;
            rq.username = account.username;
            rq.argonToken = std::move(result).unwrap();
            rq.modList = std::move(modList);
            s_startedAt = Clock::now();
            client::requestConnect(std::move(rq));
        });
}

void onServerResult(api::ConnectResult const& result, int64_t accountId) {
    if (result.ok) {
        if (result.identityVerified) notify("Connected as " + result.displayName, NotificationIcon::Success);
        else notify("Connected as " + result.displayName + " (this server did NOT verify the GD account)", NotificationIcon::Warning, 4.f);
        return;
    }
    if (result.reason == kIdentityInvalid || result.code == kIdentityInvalid) {
        // the Argon token was rejected (stale / other account / name mismatch): drop it so the
        // next attempt makes a fresh one (Argon README)
        argon::clearToken(static_cast<int>(accountId));
        offerRetry("The GPRL server could not verify your Geometry Dash account.\n\n" + result.error +
                   "\n\nIf you changed your GD name recently, refresh your login in Settings > Account first. Retry verifies it again.");
        return;
    }
    notify("GPRL: connect failed - " + result.error, NotificationIcon::Error, 5.f);
}

void openProfile() {
    if (!webLoginPreflight("open your profile")) return;
    s_webLoginUse = WebLoginUse::Profile;
    client::requestWebLogin();
    notify("GPRL: opening your profile...", NotificationIcon::Loading, 1.5f);
}

void requestWebsiteCode() {
    if (!webLoginPreflight("get a website code")) return;
    s_webLoginUse = WebLoginUse::Code;
    s_websiteCode.error.clear();
    client::requestWebLogin();
}

WebsiteCode websiteCode() {
    WebsiteCode view;
    view.pending = s_webLoginUse == WebLoginUse::Code && client::status().webLoginPending;
    view.code = s_websiteCode.code;
    view.error = s_websiteCode.error;
    if (!view.code.empty()) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(s_websiteCode.deadline - Clock::now()).count();
        // ceil to whole seconds: the panel says 5:00 right after the code arrived, 0:01 until it is gone
        view.secondsLeft = left <= 0 ? 0 : static_cast<int>((left + 999) / 1000);
    }
    return view;
}

bool copyWebsiteCode() {
    auto view = websiteCode();
    if (view.code.empty() || view.secondsLeft <= 0) return false;
    if (!geode::utils::clipboard::write(view.code)) {
        notify("GPRL: could not copy the code to the clipboard - type it instead", NotificationIcon::Error, 4.f);
        return false;
    }
    notify("Copied - paste it into the website's sign-in box", NotificationIcon::Success, 2.5f);
    return true;
}

void clearWebsiteCode() {
    s_websiteCode.code.clear();
    s_websiteCode.error.clear();
    s_websiteCode.deadline = {};
}

void onWebLoginResult(api::WebLoginResult const& result) {
    bool const forCode = s_webLoginUse == WebLoginUse::Code;
    if (!result.ok) {
        if (forCode) {
            s_websiteCode.error = result.error;
            notify("GPRL: could not get a website code - " + result.error, NotificationIcon::Error, 5.f);
        }
        else notify("GPRL: could not open your profile - " + result.error, NotificationIcon::Error, 5.f);
        return;
    }
    if (forCode) {
        // only a well-formed code is ever shown or copied; never log it (it signs the website in)
        std::string display = identity::webLoginCodeFromResponse(result.loginCode, result.url);
        if (display.empty()) {
            s_websiteCode.error = "this server did not send a usable code (it may need updating)";
            log::warn("GPRL: web-login response without a usable website code ({} / {} characters)", result.loginCode.size(),
                      result.url.size());
            notify("GPRL: the server did not send a usable website code", NotificationIcon::Error, 5.f);
            return;
        }
        int left = identity::webLoginCodeSecondsLeft(result.expiresAt, nowEpochSeconds());
        s_websiteCode.code = std::move(display);
        s_websiteCode.deadline = Clock::now() + std::chrono::seconds(left);
        s_websiteCode.error.clear();
        notify("GPRL: website code ready - press Copy, then paste it on the website", NotificationIcon::Success, 3.f);
        return;
    }
    std::string const& origin = settings::get().siteOrigin;
    if (!identity::profileUrlAllowed(result.url, origin)) {
        // never log the whole URL: it carries a one-time sign-in code
        log::warn("GPRL: refused to open a profile link outside {} ({} characters)", origin, result.url.size());
        notify("GPRL: refused to open a link outside " + origin, NotificationIcon::Error, 5.f);
        return;
    }
    web::openLinkInBrowser(result.url);
    notify("Opened your GPRL profile in the browser (the sign-in link works once, for 5 minutes)", NotificationIcon::Success, 4.f);
}

void disconnect() {
    auto st = client::status();
    std::string who = st.displayName.empty() ? std::string("your GPRL player") : st.displayName;
    createQuickPopup("GPRL",
                     "Disconnect this PC from " + who +
                         "?\n\nYour evidence stays on the server. New levels are only kept on disk until you press Connect again.",
                     "Cancel", "Disconnect", 360.f, [](FLAlertLayer*, bool yes) {
                         if (!yes) return;
                         clearWebsiteCode();
                         client::disconnect();
                         notify("GPRL: disconnected", NotificationIcon::Info);
                     });
}

void resetData() {
    auto st = client::status();
    if (!st.connected) {
        notify("GPRL: press Connect first to reset your data", NotificationIcon::Warning);
        return;
    }
    if (st.resetPending) return;
    if (settings::effectiveLocalOnly()) {
        notify("GPRL: local-only mode is on - nothing is sent to the server", NotificationIcon::Warning);
        return;
    }
    std::string who = st.displayName.empty() ? std::string("your GPRL player") : st.displayName;
    createQuickPopup("GPRL",
                     "Reset ALL GPRL data of " + who +
                         "?\n\nEvery session, timing sample, rating and calibration is removed on the server and the mod's local copies on this PC are deleted. "
                         "Your account and this connection stay. This cannot be undone.",
                     "Cancel", "Reset", 380.f, [](FLAlertLayer*, bool yes) {
                         if (!yes) return;
                         client::requestReset();
                         notify("GPRL: resetting your data...", NotificationIcon::Loading, 2.f);
                     });
}

void onResetResult(api::ResetResult const& result) {
    if (!result.ok) {
        notify("GPRL: reset failed - " + result.error, NotificationIcon::Error, 5.f);
        return;
    }
    notify(fmt::format("GPRL: data reset - {} sessions and {} timing samples removed; calibration starts over", result.sessions, result.timingSamples),
           NotificationIcon::Success, 5.f);
}

}  // namespace gprl::connect
