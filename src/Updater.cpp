// v0.12.1 self-update (owner request 2026-10-02): a copy handed to a friend once keeps itself up to
// date from the GitHub releases of core/updater.hpp kRepo (published by tools/release-mod.ps1).
//
// Only RELEASE builds do this (GPRL_RELEASE_BUILD, defined by the release script's configure): a
// development build from geode/build.ps1 never replaces itself, so a newer local build is never
// swapped for the last release.
//
// Flow, all on the main thread through Geode's async runtime (no own thread):
//   1. the first MenuLayer (setting "auto-update" on): GET api.github.com/repos/<repo>/releases/latest
//   2. core/updater decides: a plain vX.Y.Z release, strictly newer than this copy, with a sha256
//      digest and a download URL inside the repo's releases
//   3. download the asset, check size + SHA-256 + zip header, write it next to this package as
//      "<package>.part" (Geode only loads *.geode), check its mod.json (id gmo12.gprl, the release's
//      version), then rename it over this package - the same file Geode's own updater replaces
//   4. "GPRL vX downloaded" notification (not over unpaused gameplay) and, on the main menu, a
//      Later / Restart popup. The running copy stays loaded until the game restarts.
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

enum class State { Idle, Checking, Downloading, Ready, Finished };

// main thread only
State s_state = State::Idle;
std::string s_readyTag;
bool s_popupShown = false;

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

void finish(std::string const& line) {
    log::info("GPRL updater: {}", line);
    s_state = State::Finished;
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

void install(up::Release const& rel, web::WebResponse const& res) {
    if (!res.ok()) {
        std::string_view msg = res.errorMessage();
        return finish(fmt::format("download of {} failed (HTTP {}{}{}) - keeping {}", rel.tag, res.code(), msg.empty() ? "" : ": ", msg,
                                  Mod::get()->getVersion().toVString()));
    }
    auto const& data = res.data();
    std::string_view bytes(reinterpret_cast<char const*>(data.data()), data.size());
    std::string why;
    if (!up::verifyAsset(bytes, rel, why)) return finish(fmt::format("{} NOT installed: {}", rel.tag, why));

    std::filesystem::path const target = Mod::get()->getPackagePath();
    std::filesystem::path part = target;
    part += ".part";
    if (auto w = file::writeBinary(part, data); !w) return finish(fmt::format("{} NOT installed: cannot write {}: {}", rel.tag, utils::string::pathToString(part), w.unwrapErr()));

    std::error_code ec;
    auto meta = ModMetadata::createFromGeodeFile(part);
    auto mv = meta.getVersion();
    bool idOk = std::string_view(meta.getID()) == std::string_view(Mod::get()->getID());
    bool versionOk = static_cast<int>(mv.getMajor()) == rel.version.major && static_cast<int>(mv.getMinor()) == rel.version.minor &&
                     static_cast<int>(mv.getPatch()) == rel.version.patch;
    if (meta.hasErrors() || !idOk || !versionOk) {
        std::filesystem::remove(part, ec);
        return finish(fmt::format("{} NOT installed: the package's mod.json is {} {} (errors: {})", rel.tag, std::string_view(meta.getID()), mv.toVString(),
                                  meta.hasErrors() ? "yes" : "no"));
    }

    // replace this package; std::filesystem::rename replaces an existing file on Windows
    std::filesystem::rename(part, target, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::copy_file(part, target, std::filesystem::copy_options::overwrite_existing, ec2);
        std::filesystem::remove(part, ec);
        if (ec2) return finish(fmt::format("{} NOT installed: cannot replace {}: {}", rel.tag, utils::string::pathToString(target), ec2.message()));
    }

    s_state = State::Ready;
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
    std::string url = fmt::format("https://api.github.com/repos/{}/releases/latest", up::kRepo);
    checkTask().spawn("gprl-update-check", req.get(url), [](web::WebResponse res) {
        if (res.code() == 404) return finish("no release published yet");
        if (!res.ok()) {
            std::string_view msg = res.errorMessage();
            return finish(fmt::format("update check failed (HTTP {}{}{})", res.code(), msg.empty() ? "" : ": ", msg));
        }
        auto text = res.string();
        gprl::json::Value body;
        gprl::json::ParseError pe;
        if (!text.isOk() || !gprl::json::parse(text.unwrap(), body, &pe)) return finish("update check: unreadable answer from GitHub");
        up::Release rel;
        std::string why;
        if (!up::parseLatestRelease(body, rel, why)) return finish("latest release ignored: " + why);
        if (!up::isNewer(installedVersion(), rel.version))
            return finish(fmt::format("up to date ({}; latest release {})", Mod::get()->getVersion().toVString(), rel.tag));
        download(std::move(rel));
    });
}

void onMainMenu_() {
    switch (s_state) {
        case State::Idle:
            if (!kReleaseBuild) return finish("development build - self-update is off (only release builds update themselves)");
            if (!Mod::get()->getSettingValue<bool>("auto-update")) return finish("off (setting Auto-update)");
            check();
            break;
        case State::Ready: showReadyPopup(); break;
        default: break;
    }
}

}  // namespace

class $modify(GPRLUpdaterMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        // next frame: the menu scene is running by then, so the popup has a scene to sit on
        Loader::get()->queueInMainThread([] { onMainMenu_(); });
        return true;
    }
};
