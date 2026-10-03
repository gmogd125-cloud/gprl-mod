// core/identity host tests: which saved device token may be used for the logged-in GD account
// (account-switch detection), which "Open my profile" links the mod opens, and the "Website
// code" panel rules (code display form, countdown).
#include "test_util.hpp"

#include "../core/identity.hpp"

#include <string>

using namespace gprl::identity;

namespace {

void testConnectionState() {
    SECTION("connectionState / tokenUsable");
    CHECK(connectionState(false, 0, 0) == ConnectionState::NotConnected);
    CHECK(connectionState(false, 123, 123) == ConnectionState::NotConnected);
    CHECK(connectionState(true, 123, 123) == ConnectionState::Connected);
    CHECK(connectionState(true, 123, 456) == ConnectionState::OtherAccount);
    CHECK(connectionState(true, 123, 0) == ConnectionState::GdLoggedOut);
    CHECK(connectionState(true, 123, -1) == ConnectionState::GdLoggedOut);
    CHECK(connectionState(true, 0, 123) == ConnectionState::LegacyToken);
    CHECK(connectionState(true, -5, 123) == ConnectionState::LegacyToken);
    CHECK(connectionState(true, 0, 0) == ConnectionState::LegacyToken);
    // account ids above 2^31 still compare exactly
    CHECK(connectionState(true, 4294967296LL, 4294967296LL) == ConnectionState::Connected);
    CHECK(connectionState(true, 4294967296LL, 0x7fffffff) == ConnectionState::OtherAccount);

    CHECK(tokenUsable(ConnectionState::Connected));
    CHECK(!tokenUsable(ConnectionState::NotConnected));
    CHECK(!tokenUsable(ConnectionState::OtherAccount));
    CHECK(!tokenUsable(ConnectionState::GdLoggedOut));
    CHECK(!tokenUsable(ConnectionState::LegacyToken));

    CHECK(describe(ConnectionState::Connected, "RobTop") == "Connected as RobTop (GD account verified)");
    CHECK(describe(ConnectionState::NotConnected, "x").rfind("Not connected", 0) == 0);
    CHECK(describe(ConnectionState::OtherAccount, "x").find("different GD account") != std::string::npos);
    CHECK(describe(ConnectionState::GdLoggedOut, "x").find("Settings > Account") != std::string::npos);
    CHECK(describe(ConnectionState::LegacyToken, "x").find("Connect") != std::string::npos);
    CHECK(std::string(name(ConnectionState::OtherAccount)) == "other_account");
}

void testSiteOrigin() {
    SECTION("normalizeSiteOrigin");
    CHECK(normalizeSiteOrigin("") == kDefaultSite);
    CHECK(normalizeSiteOrigin("   ") == kDefaultSite);
    CHECK(normalizeSiteOrigin("https://gprl.pages.dev") == "https://gprl.pages.dev");
    CHECK(normalizeSiteOrigin("https://gprl.pages.dev/") == "https://gprl.pages.dev");
    CHECK(normalizeSiteOrigin("gprl.pages.dev") == "https://gprl.pages.dev");
    CHECK(normalizeSiteOrigin("HTTPS://GPRL.Pages.Dev/auth/mod?x=1#y") == "https://gprl.pages.dev");
    CHECK(normalizeSiteOrigin("http://gprl.pages.dev") == "https://gprl.pages.dev");
    CHECK(normalizeSiteOrigin("http://localhost:5173/") == "http://localhost:5173");
    CHECK(normalizeSiteOrigin("http://127.0.0.1:8788/x") == "http://127.0.0.1:8788");
    CHECK(normalizeSiteOrigin("https://preview.gprl.pages.dev") == "https://preview.gprl.pages.dev");
    CHECK(normalizeSiteOrigin("ftp://gprl.pages.dev") == kDefaultSite);
    CHECK(normalizeSiteOrigin("https://bad_host.dev") == kDefaultSite);
    CHECK(normalizeSiteOrigin("gprl pages.dev") == kDefaultSite);
}

void testProfileUrl() {
    SECTION("profileUrlAllowed");
    std::string const site = kDefaultSite;
    CHECK(profileUrlAllowed("https://gprl.pages.dev/auth/mod#code=abcDEF123_-", site));
    CHECK(profileUrlAllowed("https://gprl.pages.dev/", site));
    CHECK(profileUrlAllowed("http://localhost:5173/auth/mod#code=x", "http://localhost:5173"));
    // other hosts / look-alikes
    CHECK(!profileUrlAllowed("https://evil.com/auth/mod#code=x", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev.evil.com/auth/mod", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev@evil.com/auth/mod", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev:444/auth/mod", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.devx/auth/mod", site));
    CHECK(!profileUrlAllowed("http://gprl.pages.dev/auth/mod", site));
    CHECK(!profileUrlAllowed("HTTPS://GPRL.PAGES.DEV/auth/mod", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev", site));   // origin alone, no path
    CHECK(!profileUrlAllowed("javascript:alert(1)", site));
    CHECK(!profileUrlAllowed("file:///C:/Windows/system32/calc.exe", site));
    CHECK(!profileUrlAllowed("", site));
    // characters that could break out of a shell / URL handler
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/auth/mod#code=a b", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/auth/mod\n#code=a", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/\\\\evil.com", site));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/\"&calc", site));
    CHECK(!profileUrlAllowed(std::string("https://gprl.pages.dev/a\0b", 25), site));
    // length cap
    CHECK(!profileUrlAllowed(site + "/" + std::string(kMaxProfileUrlLength, 'a'), site));
    CHECK(profileUrlAllowed(site + "/" + std::string(kMaxProfileUrlLength - site.size() - 1, 'a'), site));
    // the origin must be a clean origin
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/auth/mod", ""));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/auth/mod", "https://gprl.pages.dev/auth"));
    CHECK(!profileUrlAllowed("https://gprl.pages.dev/auth/mod", "gprl.pages.dev"));
}

void testWebLoginCodeFormat() {
    SECTION("normalizeWebLoginCode / formatWebLoginCode");
    std::string const normal = "ABCDEFGHJKMNPQRSTVW0";
    std::string const display = "ABCDE-FGHJK-MNPQR-STVW0";
    CHECK(std::string(kWebLoginCodeAlphabet).size() == 32);
    CHECK(kWebLoginCodeLength == 20);
    CHECK(kWebLoginCodeDisplayLength == 23);
    CHECK(kWebLoginCodeTtlSeconds == 300);
    CHECK(display.size() == kWebLoginCodeDisplayLength);

    CHECK(normalizeWebLoginCode(normal) == normal);
    CHECK(normalizeWebLoginCode(display) == normal);
    CHECK(normalizeWebLoginCode("abcde-fghjk-mnpqr-stvw0") == normal);
    CHECK(normalizeWebLoginCode(" abcde fghjk mnpqr stvwo ") == normal);
    CHECK(normalizeWebLoginCode("\tABCDE\n FGHJK - MNPQR\r\nSTVW0") == normal);
    CHECK(normalizeWebLoginCode("OOOOO-IIIII-LLLLL-oilOI") == "00000111111111101101");
    // not a code
    CHECK(normalizeWebLoginCode("").empty());
    CHECK(normalizeWebLoginCode("   ").empty());
    CHECK(normalizeWebLoginCode("not a code").empty());
    CHECK(normalizeWebLoginCode("ABCDE-FGHJK-MNPQR-STVW").empty());     // 19
    CHECK(normalizeWebLoginCode("ABCDE-FGHJK-MNPQR-STVW0A").empty());   // 21
    CHECK(normalizeWebLoginCode("ABCDE-FGHJK-MNPQR-STVWU").empty());    // U
    CHECK(normalizeWebLoginCode("ABCDE-FGHJK-MNPQR-STVW*").empty());
    CHECK(normalizeWebLoginCode("ABCDE_FGHJK_MNPQR_STVW0").empty());    // underscore is not a dash
    CHECK(normalizeWebLoginCode("ABCDE\xe2\x80\x93" "FGHJK-MNPQR-STVW0").empty());   // en dash: the site's job, not the mod's
    CHECK(normalizeWebLoginCode(std::string(43, 'A')).empty());          // the old base64url format
    CHECK(normalizeWebLoginCode(std::string(kWebLoginCodeInputMax + 1, ' ') + normal).empty());
    CHECK(normalizeWebLoginCode(std::string(kWebLoginCodeInputMax - normal.size(), ' ') + normal) == normal);
    CHECK(normalizeWebLoginCode(std::string("ABCDE\0FGHJK-MNPQR-STVW0", 23)).empty());

    CHECK(formatWebLoginCode(normal) == display);
    CHECK(formatWebLoginCode(display) == display);
    CHECK(formatWebLoginCode(" abcde fghjk mnpqr stvwo ") == display);
    CHECK(formatWebLoginCode("bad").empty());
    CHECK(formatWebLoginCode("").empty());
    for (char c : formatWebLoginCode(display)) CHECK(c == '-' || std::string(kWebLoginCodeAlphabet).find(c) != std::string::npos);

    SECTION("webLoginCodeFromResponse");
    std::string const url = "https://gprl.pages.dev/auth/mod#code=" + display;
    CHECK(webLoginCodeFromResponse(display, url) == display);
    CHECK(webLoginCodeFromResponse(normal, "") == display);
    CHECK(webLoginCodeFromResponse("", url) == display);                                   // server without `code` yet
    CHECK(webLoginCodeFromResponse("", "https://gprl.pages.dev/auth/mod#code=" + normal) == display);
    CHECK(webLoginCodeFromResponse("garbage", url) == display);                            // bad `code`, good url
    CHECK(webLoginCodeFromResponse("", "https://gprl.pages.dev/auth/mod#code=" + std::string(43, 'x')).empty());   // old format
    CHECK(webLoginCodeFromResponse("", "https://gprl.pages.dev/auth/mod").empty());
    CHECK(webLoginCodeFromResponse("", "").empty());
    CHECK(webLoginCodeFromResponse("<script>", "javascript:alert(1)").empty());
}

void testWebLoginCountdown() {
    SECTION("parseIsoUtcSeconds");
    CHECK(parseIsoUtcSeconds("1970-01-01T00:00:00.000Z") == 0);
    CHECK(parseIsoUtcSeconds("1970-01-01T00:00:00Z") == 0);
    CHECK(parseIsoUtcSeconds("1970-01-02T00:00:00.000Z") == 86400);
    CHECK(parseIsoUtcSeconds("2000-03-01T00:00:00.000Z") == 951868800);      // after the 2000-02-29 leap day
    CHECK(parseIsoUtcSeconds("2026-09-29T20:10:00.000Z") == 1790712600);
    CHECK(parseIsoUtcSeconds("2026-09-29T20:10:00.5Z") == 1790712600);       // fraction dropped
    CHECK(parseIsoUtcSeconds("2026-09-29T20:10:00+00:00") == 1790712600);
    CHECK(parseIsoUtcSeconds("2038-01-19T03:14:08Z") == 2147483648LL);        // past 2^31
    CHECK(parseIsoUtcSeconds("") == -1);
    CHECK(parseIsoUtcSeconds("2026-09-29") == -1);
    CHECK(parseIsoUtcSeconds("2026-09-29T20:10:00") == -1);                  // no zone
    CHECK(parseIsoUtcSeconds("2026-09-29T20:10:00+02:00") == -1);            // only UTC
    CHECK(parseIsoUtcSeconds("2026-09-29T20:10:00.Z") == -1);
    CHECK(parseIsoUtcSeconds("2026-13-29T20:10:00.000Z") == -1);
    CHECK(parseIsoUtcSeconds("2026-09-29T24:10:00.000Z") == -1);
    CHECK(parseIsoUtcSeconds("2026-09-29 20:10:00.000Z") == -1);
    CHECK(parseIsoUtcSeconds("garbage") == -1);
    CHECK(parseIsoUtcSeconds("1790712600") == -1);

    SECTION("webLoginCodeSecondsLeft");
    int64_t const now = 1790712600;   // 2026-09-29T20:10:00Z
    CHECK(webLoginCodeSecondsLeft("2026-09-29T20:15:00.000Z", now) == 300);
    CHECK(webLoginCodeSecondsLeft("2026-09-29T20:14:59.000Z", now) == 299);
    CHECK(webLoginCodeSecondsLeft("2026-09-29T20:10:07.000Z", now) == 7);
    CHECK(webLoginCodeSecondsLeft("2026-09-29T20:10:00.000Z", now) == 0);
    CHECK(webLoginCodeSecondsLeft("2026-09-29T20:09:00.000Z", now) == 0);           // already expired
    CHECK(webLoginCodeSecondsLeft("2026-09-29T21:00:00.000Z", now) == 300);         // never above the TTL (clock skew)
    CHECK(webLoginCodeSecondsLeft("2026-09-29T20:14:59.900Z", now) == 299);         // fraction dropped, never rounded up
    CHECK(webLoginCodeSecondsLeft("", now) == 300);                                 // unparseable: the TTL from now
    CHECK(webLoginCodeSecondsLeft("soon", now) == 300);

    SECTION("formatCountdown / webLoginCodeStatus");
    CHECK(formatCountdown(300) == "5:00");
    CHECK(formatCountdown(299) == "4:59");
    CHECK(formatCountdown(60) == "1:00");
    CHECK(formatCountdown(59) == "0:59");
    CHECK(formatCountdown(7) == "0:07");
    CHECK(formatCountdown(0) == "0:00");
    CHECK(formatCountdown(-5) == "0:00");
    CHECK(formatCountdown(3600) == "60:00");
    CHECK(webLoginCodeStatus(299) == "expires in 4:59");
    CHECK(webLoginCodeStatus(1) == "expires in 0:01");
    CHECK(webLoginCodeStatus(0) == "expired - press Website code again");
    CHECK(webLoginCodeStatus(-1) == "expired - press Website code again");

    SECTION("webLoginCodeHint");
    CHECK(webLoginCodeHint("https://gprl.pages.dev") == "Paste it at gprl.pages.dev (Sign in)");
    CHECK(webLoginCodeHint("") == "Paste it at gprl.pages.dev (Sign in)");
    CHECK(webLoginCodeHint("GPRL.pages.dev/x") == "Paste it at gprl.pages.dev (Sign in)");
    CHECK(webLoginCodeHint("http://localhost:5173") == "Paste it at http://localhost:5173 (Sign in)");
    CHECK(webLoginCodeHint("https://preview.gprl.pages.dev/") == "Paste it at preview.gprl.pages.dev (Sign in)");
}

}  // namespace

int main() {
    testConnectionState();
    testSiteOrigin();
    testProfileUrl();
    testWebLoginCodeFormat();
    testWebLoginCountdown();
    return gprl::test::finish("identity_tests");
}
