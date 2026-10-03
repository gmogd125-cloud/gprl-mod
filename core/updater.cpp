#include "updater.hpp"

#include <cctype>

#include "crypto.hpp"

namespace gprl::updater {

namespace {

/// One decimal part "0" or "[1-9][0-9]{0,4}"; advances `i`.
bool parsePart(std::string_view s, size_t& i, int& out) {
    size_t start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    size_t len = i - start;
    if (len == 0 || len > 5) return false;
    if (len > 1 && s[start] == '0') return false;
    int v = 0;
    for (size_t k = start; k < i; ++k) v = v * 10 + (s[k] - '0');
    out = v;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isHex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

}  // namespace

bool parseVersion(std::string_view text, Version& out) {
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    Version v;
    if (!parsePart(text, i, v.major)) return false;
    if (i >= text.size() || text[i] != '.') return false;
    ++i;
    if (!parsePart(text, i, v.minor)) return false;
    if (i >= text.size() || text[i] != '.') return false;
    ++i;
    if (!parsePart(text, i, v.patch)) return false;
    if (i != text.size()) return false;   // "-beta.1", "+build", trailing junk
    out = v;
    return true;
}

int compare(Version const& a, Version const& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string toString(Version const& v) {
    return "v" + std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

bool parseLatestRelease(json::Value const& body, Release& out, std::string& why) {
    if (!body.isObject()) {
        why = "the release answer is not a JSON object";
        return false;
    }
    if (body.getBool("draft") || body.getBool("prerelease")) {
        why = "the latest release is a draft or a prerelease";
        return false;
    }
    Release r;
    r.tag = body.getString("tag_name");
    if (!parseVersion(r.tag, r.version)) {
        why = "the release tag '" + r.tag + "' is not a plain vX.Y.Z version";
        return false;
    }
    r.title = body.getString("name");

    auto const* assets = body.find("assets");
    if (!assets || !assets->isArray()) {
        why = "the release lists no assets";
        return false;
    }
    json::Value const* asset = nullptr;
    for (auto const& a : assets->asArray()) {
        if (a.isObject() && a.getString("name") == kAssetName) {
            asset = &a;
            break;
        }
    }
    if (!asset) {
        why = std::string("the release has no ") + kAssetName;
        return false;
    }
    if (asset->getString("state", "uploaded") != "uploaded") {
        why = "the package upload is not finished";
        return false;
    }

    r.assetUrl = asset->getString("browser_download_url");
    std::string const prefix = std::string("https://github.com/") + kRepo + "/releases/download/";
    if (r.assetUrl.compare(0, prefix.size(), prefix) != 0) {
        why = "the package URL is outside " + prefix;
        return false;
    }

    r.assetSize = asset->getInt("size", 0);
    if (r.assetSize <= 0 || r.assetSize > kMaxAssetBytes) {
        why = "the package size " + std::to_string(r.assetSize) + " is not plausible";
        return false;
    }

    // GitHub publishes "digest": "sha256:<hex>" for every uploaded release asset; without it the
    // download cannot be checked, so it is not installed
    std::string digest = asset->getString("digest");
    if (digest.rfind("sha256:", 0) != 0 || !isHex64(std::string_view(digest).substr(7))) {
        why = "the package has no sha256 digest";
        return false;
    }
    r.sha256Hex = lower(digest.substr(7));

    out = std::move(r);
    return true;
}

bool verifyAsset(std::string_view bytes, Release const& release, std::string& why) {
    if (static_cast<int64_t>(bytes.size()) != release.assetSize) {
        why = "downloaded " + std::to_string(bytes.size()) + " bytes, the release says " + std::to_string(release.assetSize);
        return false;
    }
    if (bytes.size() < 4 || bytes.substr(0, 4) != std::string_view("PK\x03\x04", 4)) {
        why = "the download is not a .geode (zip) file";
        return false;
    }
    std::string actual = crypto::toHex(crypto::sha256(bytes));
    if (actual != release.sha256Hex) {
        why = "SHA-256 mismatch (" + actual + " != " + release.sha256Hex + ")";
        return false;
    }
    return true;
}

int64_t nextCheckAtMs(int64_t lastCheckMs, CheckOutcome outcome) {
    return lastCheckMs + (outcome == CheckOutcome::RateLimited ? kRateLimitBackoffMs : kCheckIntervalMs);
}

bool shouldCheckNow(int64_t nowMs, int64_t nextCheckAt, bool idle, bool playingUnpaused) {
    return idle && !playingUnpaused && nowMs >= nextCheckAt;
}

CheckOutcome outcomeOfStatus(int httpStatus) {
    if (httpStatus == 304) return CheckOutcome::NotModified;
    if (httpStatus == 403 || httpStatus == 429) return CheckOutcome::RateLimited;
    if (httpStatus >= 200 && httpStatus < 300) return CheckOutcome::UpToDate;
    return CheckOutcome::Failed;
}

}  // namespace gprl::updater
