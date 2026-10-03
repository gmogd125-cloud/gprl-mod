// v0.12.1 self-update (owner request 2026-10-02): a copy handed to a friend once keeps itself up to
// date from the GitHub releases of core/updater.hpp kRepo (published by tools/release-mod.ps1).
//
// Only RELEASE builds do this (GPRL_RELEASE_BUILD, defined by the release script's configure): a
// development build from geode/build.ps1 never replaces itself, so a newer local build is never
// swapped for the last release.
//
// Flow, all on the main thread through Geode's async runtime (no own thread):
//   1. the first MenuLayer, then once a minute all session (v0.14.2; setting "auto-update" on,
//      never while a level runs unpaused): GET api.github.com/repos/<repo>/releases/latest,
//      conditional on the last ETag (a 304 does not count against GitHub's hourly limit)
//   2. core/updater decides: a plain vX.Y.Z release, strictly newer than this copy, with a sha256
//      digest and a download URL inside the repo's releases
//   3. download the asset, check size + SHA-256 + zip header, write it next to this package as
//      "<package>.part" (Geode only loads *.geode), check its mod.json (id gmo12.gprl, the release's
//      version), then rename it over this package - the same file Geode's own updater replaces
//   4. "GPRL vX downloaded" notification (not over unpaused gameplay) and, on the main menu, a
//      Later / Restart popup (again on every main-menu visit). The running copy stays loaded
//      until the game restarts.
// Any failure only logs ("GPRL updater: ...") and leaves the installed copy alone.
#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "../core/json.hpp"
#include "../core/updater.hpp"
#include "Settings.hpp"

using namespace geode::prelude;

namespace {

namespace up = gprl::updater;

#ifdef GPRL_RELEASE_BUILD
constexpr bool kReleaseBuild = true;
#else
constexpr bool kReleaseBuild = false;
#endif

// v0.14.2: Off = a development build (never checks); Idle = waiting for the next check (one a
// minute, core/updater kCheckIntervalMs); Ready = an update is installed and waits for a restart.
enum class State { Idle, Checking, Downloading, Ready, Off };

// main thread only
State s_state = State::Idle;
std::string s_readyTag;
bool s_popupShown = false;          // per main-menu visit (reset when the menu is entered)
int64_t s_nextCheckAt = 0;          // steady ms; 0 = the first main menu checks at once
std::string s_etag;                 // ETag of the last 200 answer: later checks are conditional (304s are free)
bool s_tickerStarted = false;
bool s_loggedSettingOff = false;
bool s_loggedUpToDate = false;

/// Never destroyed, like Connect.cpp's Argon holder: no static destructor may touch the async
/// runtime after it shut down at game exit.
async::TaskHolder<web::WebResponse>& checkTask() {
    static auto* holder = new async::TaskHolder<web::WebResponse>();
    return *holder;
}
async::TaskHolder<web::WebResponse>& downloadTask() {
    static auto* holder = new async::TaskHolder<web::WebResponse>();
    return *holder;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

up::Version installedVersion() {
    auto v = Mod::get()->getVersion();
    return up::Version{static_cast<int>(v.getMajor()), static_cast<int>(v.getMinor()), static_cast<int>(v.getPatch())};
}

/// A level is running and not paused: nothing is put in front of gameplay.
bool playingUnpaused() {
    auto* pl = PlayLayer::get();
    return pl && !pl->m_isPaused && !pl->m_hasCompletedLevel;
}

bool onMainMenu() {
    auto* scene = CCDirector::get()->getRunningScene();
    return scene && scene->getChildByType<MenuLayer>(0);
}

web::WebRequest request(char const* accept, int timeoutSeconds) {
    web::WebRequest req;
    req.userAgent(gprl::settings::clientBuild());
    req.header("Accept", accept);
    req.timeout(std::chrono::seconds(timeoutSeconds));
    return req;
}

/// One check (or download / install) is over: back to Idle, the next check scheduled by the
/// outcome. `quiet` = the steady "nothing new" answer, logged once per session only.
void finish(std::string const& line, up::CheckOutcome outcome, bool quiet = false) {
    if (!quiet) log::info("GPRL updater: {}", line);
    s_state = State::Idle;
    s_nextCheckAt = up::nextCheckAtMs(nowMs(), outcome);
}

void showReadyPopup() {
    if (s_state != State::Ready || s_popupShown || !onMainMenu()) return;
    s_popupShown = true;
    createQuickPopup("GPRL update",
                     fmt::format("GPRL <cg>{}</c> is downloaded.\nRestart Geometry Dash to start using it.\n<cy>(You are on {} until then.)</c>", s_readyTag,
                                 Mod::get()->getVersion().toVString()),
                     "Later", "Restart", [](FLAlertLayer*, bool restart) {
                         if (restart) geode::utils::game::restart(true);
                     });
}

/// A download or install that failed: the next check must fetch the full answer again (a 304
/// would otherwise hide the release that still needs installing).
void failedInstall(std::string const& line) {
    s_etag.clear();
    finish(line, up::CheckOutcome::Failed);
}

void install(up::Release const& rel, web::WebResponse const& res) {
    if (!res.ok()) {
        std::string_view msg = res.errorMessage();
        return failedInstall(fmt::format("download of {} failed (HTTP {}{}{}) - keeping {}", rel.tag, res.code(), msg.empty() ? "" : ": ", msg,
                                         Mod::get()->getVersion().toVString()));
    }
    auto const& data = res.data();
    std::string_view bytes(reinterpret_cast<char const*>(data.data()), data.size());
    std::string why;
    if (!up::verifyAsset(bytes, rel, why)) return failedInstall(fmt::format("{} NOT installed: {}", rel.tag, why));

    std::filesystem::path const target = Mod::get()->getPackagePath();
    std::filesystem::path part = target;
    part += ".part";
    if (auto w = file::writeBinary(part, data); !w)
        return failedInstall(fmt::format("{} NOT installed: cannot write {}: {}", rel.tag, utils::string::pathToString(part), w.unwrapErr()));

    std::error_code ec;
    auto meta = ModMetadata::createFromGeodeFile(part);
    auto mv = meta.getVersion();
    bool idOk = std::string_view(meta.getID()) == std::string_view(Mod::get()->getID());
    bool versionOk = static_cast<int>(mv.getMajor()) == rel.version.major && static_cast<int>(mv.getMinor()) == rel.version.minor &&
                     static_cast<int>(mv.getPatch()) == rel.version.patch;
    if (meta.hasErrors() || !idOk || !versionOk) {
        std::filesystem::remove(part, ec);
        return failedInstall(fmt::format("{} NOT installed: the package's mod.json is {} {} (errors: {})", rel.tag, std::string_view(meta.getID()),
                                         mv.toVString(), meta.hasErrors() ? "yes" : "no"));
    }

    // replace this package; std::filesystem::rename replaces an existing file on Windows
    std::filesystem::rename(part, target, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::copy_file(part, target, std::filesystem::copy_options::overwrite_existing, ec2);
        std::filesystem::remove(part, ec);
        if (ec2) return failedInstall(fmt::format("{} NOT installed: cannot replace {}: {}", rel.tag, utils::string::pathToString(target), ec2.message()));
    }

    s_state = State::Ready;   // no further checks this session: the update waits for the restart
    s_readyTag = rel.tag;
    log::info("GPRL updater: {} installed to {} ({} bytes, sha256 {}) - active after a restart", rel.tag, utils::string::pathToString(target), data.size(), rel.sha256Hex);
    if (!playingUnpaused()) Notification::create(fmt::format("GPRL {} downloaded - restart Geometry Dash to update", rel.tag), NotificationIcon::Success, 4.f)->show();
    showReadyPopup();
}

void download(up::Release rel) {
    s_state = State::Downloading;
    log::info("GPRL updater: {} is newer than {} - downloading {} ({} bytes)", rel.tag, Mod::get()->getVersion().toVString(), rel.assetUrl, rel.assetSize);
    // GitHub answers the download URL with a redirect to its file host (followed by default)
    auto req = request("application/octet-stream", 180);
    downloadTask().spawn("gprl-update-download", req.get(rel.assetUrl), [rel](web::WebResponse res) { install(rel, res); });
}

void check() {
    s_state = State::Checking;
    auto req = request("application/vnd.github+json", 20);
    req.header("X-GitHub-Api-Version", "2022-11-28");
    // conditional after the first full answer: GitHub answers 304 (not counted against the
    // 60-per-hour unauthenticated limit) until a new release changes the resource
    if (!s_etag.empty()) req.header("If-None-Match", s_etag);
    std::string url = fmt::format("https://api.github.com/repos/{}/releases/latest", up::kRepo);
    checkTask().spawn("gprl-update-check", req.get(url), [](web::WebResponse res) {
        int code = res.code();
        auto outcome = up::outcomeOfStatus(code);
        if (outcome == up::CheckOutcome::NotModified) return finish("no new release (304)", outcome, true);
        if (outcome == up::CheckOutcome::RateLimited)
            return finish(fmt::format("GitHub rate limit (HTTP {}) - next check in 15 minutes", code), outcome);
        if (code == 404) return finish("no release published yet", up::CheckOutcome::Failed, s_loggedUpToDate);
        if (!res.ok()) {
            std::string_view msg = res.errorMessage();
            return finish(fmt::format("update check failed (HTTP {}{}{})", code, msg.empty() ? "" : ": ", msg), up::CheckOutcome::Failed);
        }
        auto text = res.string();
        gprl::json::Value body;
        gprl::json::ParseError pe;
        if (!text.isOk() || !gprl::json::parse(text.unwrap(), body, &pe)) return finish("update check: unreadable answer from GitHub", up::CheckOutcome::Failed);
        up::Release rel;
        std::string why;
        if (!up::parseLatestRelease(body, rel, why)) return finish("latest release ignored: " + why, up::CheckOutcome::Failed);
        if (auto etag = res.header("ETag")) s_etag = std::string(std::string_view(*etag));
        if (!up::isNewer(installedVersion(), rel.version)) {
            bool quiet = s_loggedUpToDate;
            s_loggedUpToDate = true;
            return finish(fmt::format("up to date ({}; latest release {}) - checking again every minute", Mod::get()->getVersion().toVString(), rel.tag),
                          up::CheckOutcome::UpToDate, quiet);
        }
        download(std::move(rel));
    });
}

/// Every few seconds for the whole session (release builds): starts the minute's check when it is
/// due and nothing runs in front of unpaused gameplay (core/updater shouldCheckNow).
void tick() {
    if (s_state == State::Off) return;
    if (!Mod::get()->getSettingValue<bool>("auto-update")) {
        if (!s_loggedSettingOff) log::info("GPRL updater: off (setting Auto-update)");
        s_loggedSettingOff = true;
        return;
    }
    s_loggedSettingOff = false;
    if (up::shouldCheckNow(nowMs(), s_nextCheckAt, s_state == State::Idle, playingUnpaused())) check();
}

class UpdateTicker : public CCObject {
public:
    static UpdateTicker* get() {
        static UpdateTicker* t = [] {
            auto* x = new UpdateTicker();   // refcount 1, never released: lives for the whole game
            return x;
        }();
        return t;
    }
    void onTick(float) { tick(); }
};

void onMainMenu_() {
    if (s_state == State::Off) return;
    if (!kReleaseBuild) {
        s_state = State::Off;
        log::info("GPRL updater: development build - self-update is off (only release builds update themselves)");
        return;
    }
    if (!s_tickerStarted) {
        s_tickerStarted = true;
        CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(UpdateTicker::onTick), UpdateTicker::get(), 5.f, false);
    }
    if (s_state == State::Ready) return showReadyPopup();
    tick();   // the first main menu checks at once (s_nextCheckAt starts at 0)
}

}  // namespace

class $modify(GPRLUpdaterMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        s_popupShown = false;   // a downloaded update asks again on every visit to the main menu
        // next frame: the menu scene is running by then, so the popup has a scene to sit on
        Loader::get()->queueInMainThread([] { onMainMenu_(); });
        return true;
    }
};
