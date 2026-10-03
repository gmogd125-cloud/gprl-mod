#include "identity.hpp"

#include <cstdio>
#include <cstdlib>

#include "config.hpp"

namespace gprl::identity {

ConnectionState connectionState(bool hasToken, int64_t tokenAccountId, int64_t currentAccountId) {
    if (!hasToken) return ConnectionState::NotConnected;
    if (tokenAccountId <= 0) return ConnectionState::LegacyToken;
    if (currentAccountId <= 0) return ConnectionState::GdLoggedOut;
    if (tokenAccountId != currentAccountId) return ConnectionState::OtherAccount;
    return ConnectionState::Connected;
}

std::string describe(ConnectionState s, std::string_view displayName) {
    switch (s) {
        case ConnectionState::Connected:
            return "Connected as " + std::string(displayName) + " (GD account verified)";
        case ConnectionState::NotConnected: return "Not connected - press Connect";
        case ConnectionState::OtherAccount:
            return "Not connected - you are logged in to a different GD account than the one you connected; press Connect";
        case ConnectionState::GdLoggedOut:
            return "Not connected - log in to your Geometry Dash account (Settings > Account), then press Connect";
        case ConnectionState::LegacyToken:
            return "Not connected - link codes are no longer used; press Connect to verify your GD account";
    }
    return "Not connected";
}

std::string normalizeSiteOrigin(std::string_view input) {
    std::string url = config::normalizeApiBaseUrl(input);   // trims, scheme, lowercase host, drops ?#
    if (url.empty()) return kDefaultSite;
    auto sep = url.find("://");
    if (sep == std::string::npos) return kDefaultSite;
    auto slash = url.find('/', sep + 3);
    std::string origin = slash == std::string::npos ? url : url.substr(0, slash);
    if (!config::apiBaseUrlIsValid(origin)) return kDefaultSite;
    return origin;
}

bool profileUrlAllowed(std::string_view url, std::string_view origin) {
    if (origin.empty() || url.size() > kMaxProfileUrlLength) return false;
    // the origin must itself be a clean origin (what normalizeSiteOrigin produces)
    std::string o(origin);
    if (!config::apiBaseUrlIsValid(o)) return false;
    auto sep = o.find("://");
    if (sep == std::string::npos || o.find('/', sep + 3) != std::string::npos) return false;

    if (url.size() <= origin.size() || url.substr(0, origin.size()) != origin) return false;
    if (url[origin.size()] != '/') return false;
    for (char ch : url) {
        auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7f || c == '\\' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`') return false;
    }
    return true;
}

// ---- Website sign-in code ----

namespace {

bool inAlphabet(char c) {
    for (char const* p = kWebLoginCodeAlphabet; *p; ++p)
        if (*p == c) return true;
    return false;
}

bool asciiSpace(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d); }

/// Reads exactly `digits` decimal digits at `pos` (advances it); -1 when anything else is there.
int readDigits(std::string_view s, size_t& pos, size_t digits) {
    if (pos + digits > s.size()) return -1;
    int v = 0;
    for (size_t i = 0; i < digits; ++i) {
        char c = s[pos + i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    pos += digits;
    return v;
}

bool expectChar(std::string_view s, size_t& pos, char c) {
    if (pos >= s.size() || s[pos] != c) return false;
    ++pos;
    return true;
}

/// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t const era = (y >= 0 ? y : y - 399) / 400;
    unsigned const yoe = static_cast<unsigned>(y - era * 400);
    unsigned const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

}  // namespace

std::string normalizeWebLoginCode(std::string_view input) {
    if (input.size() > kWebLoginCodeInputMax) return {};
    std::string out;
    out.reserve(kWebLoginCodeLength);
    for (char ch : input) {
        auto c = static_cast<unsigned char>(ch);
        if (asciiSpace(c) || c == '-') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
        if (c == 'O') c = '0';
        else if (c == 'I' || c == 'L') c = '1';
        if (!inAlphabet(static_cast<char>(c))) return {};
        if (out.size() == kWebLoginCodeLength) return {};   // 21st character: not a code
        out.push_back(static_cast<char>(c));
    }
    if (out.size() != kWebLoginCodeLength) return {};
    return out;
}

std::string formatWebLoginCode(std::string_view input) {
    std::string normal = normalizeWebLoginCode(input);
    if (normal.empty()) return {};
    std::string out;
    out.reserve(kWebLoginCodeDisplayLength);
    for (size_t i = 0; i < normal.size(); ++i) {
        if (i > 0 && i % kWebLoginCodeGroup == 0) out.push_back('-');
        out.push_back(normal[i]);
    }
    return out;
}

std::string webLoginCodeFromResponse(std::string_view code, std::string_view url) {
    std::string display = formatWebLoginCode(code);
    if (!display.empty()) return display;
    auto hash = url.find("#code=");
    if (hash == std::string_view::npos) return {};
    return formatWebLoginCode(url.substr(hash + 6));
}

int64_t parseIsoUtcSeconds(std::string_view iso) {
    size_t pos = 0;
    int year = readDigits(iso, pos, 4);
    if (year < 0 || !expectChar(iso, pos, '-')) return -1;
    int month = readDigits(iso, pos, 2);
    if (month < 1 || month > 12 || !expectChar(iso, pos, '-')) return -1;
    int day = readDigits(iso, pos, 2);
    if (day < 1 || day > 31 || !expectChar(iso, pos, 'T')) return -1;
    int hour = readDigits(iso, pos, 2);
    if (hour < 0 || hour > 23 || !expectChar(iso, pos, ':')) return -1;
    int minute = readDigits(iso, pos, 2);
    if (minute < 0 || minute > 59 || !expectChar(iso, pos, ':')) return -1;
    int second = readDigits(iso, pos, 2);
    if (second < 0 || second > 60) return -1;
    if (pos < iso.size() && iso[pos] == '.') {
        ++pos;
        size_t digits = 0;
        while (pos < iso.size() && iso[pos] >= '0' && iso[pos] <= '9') { ++pos; ++digits; }
        if (digits == 0) return -1;
    }
    std::string_view rest = iso.substr(pos);
    if (rest != "Z" && rest != "+00:00" && rest != "-00:00") return -1;
    int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

int webLoginCodeSecondsLeft(std::string_view expiresAtIso, int64_t nowEpochSeconds) {
    int64_t expires = parseIsoUtcSeconds(expiresAtIso);
    if (expires < 0) return kWebLoginCodeTtlSeconds;
    int64_t left = expires - nowEpochSeconds;
    if (left < 0) return 0;
    if (left > kWebLoginCodeTtlSeconds) return kWebLoginCodeTtlSeconds;
    return static_cast<int>(left);
}

std::string formatCountdown(int seconds) {
    if (seconds < 0) seconds = 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", seconds / 60, seconds % 60);
    return buf;
}

std::string webLoginCodeStatus(int secondsLeft) {
    if (secondsLeft <= 0) return "expired - press Website code again";
    return "expires in " + formatCountdown(secondsLeft);
}

std::string webLoginCodeHint(std::string_view siteOrigin) {
    std::string origin = normalizeSiteOrigin(siteOrigin);
    std::string_view shown = origin;
    if (shown.rfind("https://", 0) == 0) shown.remove_prefix(8);
    return "Paste it at " + std::string(shown) + " (Sign in)";
}

}  // namespace gprl::identity
