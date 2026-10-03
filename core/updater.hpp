#pragma once
// v0.12.1 self-update (owner request 2026-10-02: a friend who was handed the .geode once gets every
// later version without being sent the file again). Releases are GitHub releases of the public
// mirror repo kRepo, each carrying the package as the asset kAssetName (tools/release-mod.ps1).
//
// This file holds the pure decisions src/Updater.cpp acts on, host-tested in
// tests/updater_tests.cpp:
//   - which release to take: GET https://api.github.com/repos/<kRepo>/releases/latest, a plain
//     "vX.Y.Z" tag (no "-beta" etc.), not a draft / prerelease, the asset present with a size and
//     a "sha256:<hex>" digest, and a download URL inside this repo's releases
//   - whether it is newer than the running copy (strictly greater; never a downgrade)
//   - whether the downloaded bytes are that asset (size + SHA-256 + zip header)
// The mod still checks the package's own mod.json (id + version) before it replaces itself.
#include <cstdint>
#include <string>
#include <string_view>

#include "json.hpp"

namespace gprl::updater {

inline constexpr char const* kRepo = "gmogd125-cloud/gprl-mod";
inline constexpr char const* kAssetName = "gmo12.gprl.geode";
/// The mod package is ~5 MB; anything far bigger is not ours.
inline constexpr int64_t kMaxAssetBytes = 64ll * 1024 * 1024;

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

/// "v1.2.3" or "1.2.3" only. A prerelease / build suffix ("v1.2.3-beta.1", "1.2.3+x"), missing
/// parts, leading zeros beyond "0", or numbers above 99999 -> false.
bool parseVersion(std::string_view text, Version& out);
/// <0 when a is older than b, 0 when equal, >0 when a is newer
int compare(Version const& a, Version const& b);
/// "v1.2.3"
std::string toString(Version const& v);
/// only strictly newer releases are installed
inline bool isNewer(Version const& installed, Version const& release) { return compare(release, installed) > 0; }

struct Release {
    std::string tag;          // "v0.12.1"
    Version version;
    std::string title;        // the release name ("" when GitHub has none)
    std::string assetUrl;     // browser_download_url
    int64_t assetSize = 0;
    std::string sha256Hex;    // lower-case hex from the asset's "digest": "sha256:<hex>"
};

/// The body of GET /repos/<kRepo>/releases/latest -> the release, or false with `why` (one line,
/// for the log).
bool parseLatestRelease(json::Value const& body, Release& out, std::string& why);

/// The downloaded bytes are the described asset: exact size, SHA-256 equal, and a zip file (a
/// .geode is a zip). false with `why` otherwise.
bool verifyAsset(std::string_view bytes, Release const& release, std::string& why);

// ---- v0.14.2 check schedule (owner 2026-10-03: "make it check every minute") ----
// The first check runs on the first main menu; afterwards one check a minute for the whole
// session, so a release published while the game is open is downloaded within about a minute.
// GitHub allows 60 unauthenticated API requests per hour per IP, but a CONDITIONAL request
// (If-None-Match with the last ETag) answered 304 Not Modified does not count against it, so the
// steady state costs nothing. Never while a level runs unpaused (no request, no download, no file
// write in front of gameplay); a rate-limit answer backs off 15 minutes.
inline constexpr int64_t kCheckIntervalMs = 60'000;
inline constexpr int64_t kRateLimitBackoffMs = 15 * 60'000;

enum class CheckOutcome {
    UpToDate,      // 200: the latest release is not newer
    NotModified,   // 304: nothing changed since the last ETag
    Failed,        // network error, unreadable answer, a download or install that failed
    RateLimited,   // 403 / 429 from GitHub
};

/// When the next check may run after one that ended at `lastCheckMs` with `outcome`.
int64_t nextCheckAtMs(int64_t lastCheckMs, CheckOutcome outcome);

/// Whether a check starts now: the updater is idle (not checking, downloading, or holding a
/// downloaded update), the check is due, and no level is running unpaused.
bool shouldCheckNow(int64_t nowMs, int64_t nextCheckAt, bool idle, bool playingUnpaused);

/// GitHub's answer to a release check, as the schedule sees it (HTTP status code).
CheckOutcome outcomeOfStatus(int httpStatus);

// ---- v0.14.3 when the player is told (owner 2026-10-03: "make sure the player is tabbed into gd
// when you show the notification") ----
/// The "downloaded" notification shows only while Geometry Dash is the foreground window and no
/// level runs unpaused; otherwise it waits (the updater's ticker retries every few seconds).
inline bool canNotifyNow(bool gameFocused, bool playingUnpaused) { return gameFocused && !playingUnpaused; }
/// The Later / Restart popup: the same, and only on the main menu.
inline bool canAskNow(bool gameFocused, bool playingUnpaused, bool onMainMenu) {
    return canNotifyNow(gameFocused, playingUnpaused) && onMainMenu;
}

}  // namespace gprl::updater
