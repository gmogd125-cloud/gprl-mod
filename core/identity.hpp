#pragma once
// Pure rules of the GD-account connection (v0.2.0): whether a saved device token may be used for
// the Geometry Dash account that is logged in right now, which "Open my profile" links the mod
// is allowed to open in the browser, and (v0.2.1) the "Website code" panel: the one-time sign-in
// code's display form and its countdown. No Geode includes: compiled by the mod AND by the host
// tests (tests/identity_tests.cpp).
//
// Connection model (docs: geode/README.md "Connecting"):
//   Connect = Argon proves GD account ownership -> POST /v1/client/connect -> device token.
//   The token is saved together with the GD account id it was issued for. A token is only used
//   while that same GD account is logged in; tokens without a saved account id (v0.1.x link-code
//   tokens) are never used again - the player presses Connect once.
#include <cstdint>
#include <string>
#include <string_view>

namespace gprl::identity {

/// The GPRL website (production). Default of the `site-url` setting; the only origin the
/// "Open my profile" link may point at unless the player configured another site.
inline constexpr char const* kDefaultSite = "https://gprl.pages.dev";

/// Longest profile link the mod opens (the server sends ~80 characters).
inline constexpr size_t kMaxProfileUrlLength = 2048;

enum class ConnectionState : uint8_t {
    NotConnected,   // no device token saved
    Connected,      // token saved for the GD account that is logged in now
    OtherAccount,   // token saved for a different GD account than the logged-in one
    GdLoggedOut,    // token saved, but no GD account is logged in
    LegacyToken,    // token from the v0.1.x link-code flow (no GD account saved with it)
};

constexpr char const* name(ConnectionState s) {
    switch (s) {
        case ConnectionState::NotConnected: return "not_connected";
        case ConnectionState::Connected: return "connected";
        case ConnectionState::OtherAccount: return "other_account";
        case ConnectionState::GdLoggedOut: return "gd_logged_out";
        case ConnectionState::LegacyToken: return "legacy_token";
    }
    return "not_connected";
}

/// hasToken: a device token is saved. tokenAccountId: the GD account id saved with it (<= 0 =
/// none). currentAccountId: GJAccountManager::m_accountID right now (<= 0 = not logged in).
ConnectionState connectionState(bool hasToken, int64_t tokenAccountId, int64_t currentAccountId);

/// Only Connected lets the telemetry worker use the token (sessions, batches, calibration,
/// web-login). Every other state behaves exactly like "not connected".
constexpr bool tokenUsable(ConnectionState s) { return s == ConnectionState::Connected; }

/// One-line popup / HUD explanation of a state (displayName used for Connected only).
std::string describe(ConnectionState s, std::string_view displayName);

/// Cleans the `site-url` setting down to an origin "scheme://host[:port]":
///   - trims whitespace, lowercases scheme and host, drops any path / query / fragment
///   - no scheme -> "https://"; "http://" only stays for localhost / 127.0.0.1 / [::1]
///   - empty (after trimming) or unusable -> kDefaultSite
std::string normalizeSiteOrigin(std::string_view input);

/// True when `url` (as returned by POST /v1/client/web-login) may be opened in the browser:
///   - `origin` is a usable origin (normalizeSiteOrigin output)
///   - url starts with exactly origin + "/" (so "https://gprl.pages.dev.evil.com/...",
///     "https://gprl.pages.dev@evil.com/..." and "https://gprl.pages.dev:444/..." are refused)
///   - no whitespace, control characters, backslashes or quotes anywhere; length <= kMaxProfileUrlLength
bool profileUrlAllowed(std::string_view url, std::string_view origin);

// ---- Website sign-in code (v0.2.1; mirror of shared/src/api/webLoginCode.ts) ----
//
// POST /v1/client/web-login answers { url, expiresAt, code }: `code` is a one-time sign-in code
// of 20 Crockford base32 characters (100 bits) in its display form XXXXX-XXXXX-XXXXX-XXXXX, the
// same value the url carries in its #code= fragment. The popup shows it so the player can paste
// it into the website's sign-in box; the server accepts it with or without dashes, in any case,
// and with the Crockford aliases (O -> 0, I / L -> 1). The mod only ever shows / copies a value
// that passes formatWebLoginCode (so a misbehaving server cannot push arbitrary text into the
// popup or the clipboard) and never logs it.

/// Crockford base32 (no I, L, O, U).
inline constexpr char const* kWebLoginCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
/// Characters in a code (normal form): 20 x 5 bits = 100 bits.
inline constexpr size_t kWebLoginCodeLength = 20;
/// Characters per dash-separated group in the display form.
inline constexpr size_t kWebLoginCodeGroup = 5;
/// Length of the display form XXXXX-XXXXX-XXXXX-XXXXX.
inline constexpr size_t kWebLoginCodeDisplayLength = kWebLoginCodeLength + kWebLoginCodeLength / kWebLoginCodeGroup - 1;
/// Lifetime of a code (V1_WEB_LOGIN_CODE_TTL_SECONDS); also the countdown's ceiling.
inline constexpr int kWebLoginCodeTtlSeconds = 300;
/// Longest input normalizeWebLoginCode looks at.
inline constexpr size_t kWebLoginCodeInputMax = 128;

/// Normal form of a code (20 alphabet characters): ASCII letters uppercased, ASCII whitespace and
/// '-' removed, O -> 0 and I / L -> 1. Empty string when the result is not exactly 20 alphabet
/// characters or the input is longer than kWebLoginCodeInputMax. (The website additionally
/// forgives Unicode dashes; the mod only ever formats what the server sent.)
std::string normalizeWebLoginCode(std::string_view input);

/// Display form XXXXX-XXXXX-XXXXX-XXXXX of any accepted spelling, empty when `input` is not a code.
std::string formatWebLoginCode(std::string_view input);

/// The code to show for a web-login response: `code` when the server sent one, else the #code=
/// fragment of `url` (servers from before `code` existed), as display form; empty when neither
/// is a code (e.g. the old 43-character format: the server needs updating).
std::string webLoginCodeFromResponse(std::string_view code, std::string_view url);

/// Seconds until `expiresAtIso` (ISO 8601 UTC as JavaScript's toISOString writes it,
/// "2026-09-29T20:10:00.000Z"; fractional seconds optional, "+00:00" accepted for "Z") seen
/// from `nowEpochSeconds`, clamped to [0, kWebLoginCodeTtlSeconds]. A clock that runs ahead can
/// therefore only shorten the countdown, never show more than the TTL. Unparseable ->
/// kWebLoginCodeTtlSeconds (the TTL counted from now), so a countdown is always shown.
int webLoginCodeSecondsLeft(std::string_view expiresAtIso, int64_t nowEpochSeconds);

/// "2026-09-29T20:10:00.000Z" -> Unix seconds (fraction dropped); -1 when not that format.
int64_t parseIsoUtcSeconds(std::string_view iso);

/// m:ss ("4:59", "0:07", "5:00"); negative -> "0:00".
std::string formatCountdown(int seconds);

/// The popup's countdown line: "expires in 4:59", or "expired - press Website code again" at 0.
std::string webLoginCodeStatus(int secondsLeft);

/// The popup's hint line: "Paste it at gprl.pages.dev (Sign in)" - the site's host (origin as
/// normalizeSiteOrigin gives it, scheme dropped; "https://" only, an http:// dev site keeps it).
std::string webLoginCodeHint(std::string_view siteOrigin);

}  // namespace gprl::identity
