#include "Environment.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/Dirs.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <Psapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "../core/classify.hpp"
#include "../core/crypto.hpp"
#include "Settings.hpp"
#include "eclipse/config.hpp"

using namespace geode::prelude;

namespace gprl::env {

namespace {

constexpr char const* kEclipse = "eclipse.eclipse-menu";
constexpr char const* kMegahack = "absolllute.megahack";
constexpr char const* kOpenhack = "prevter.openhack";
constexpr char const* kCbf = "syzzi.click_between_frames";

// Mods whose presence the client marks as gameplay-affecting (the server re-classifies). Menus
// with an API we read are classified by state; the rest are "unknown gameplay mod" (SPEC §21:
// rated sigma/s disabled until verified server side).
struct KnownMod {
    char const* id;
    bool hasApi;
};
constexpr KnownMod kKnownGameplayMods[] = {
    {kEclipse, true},   {kMegahack, false},  {kOpenhack, false},   {"tobyadd.gdh", false},
    {"zilko.xdbot", false}, {"ninxout.crystal_client", false}, {"firee.prism", false}, {"elnexreal.gdhack", false},
};

bool isKnownGameplayMod(std::string_view id, bool& hasApi) {
    for (auto const& k : kKnownGameplayMods) {
        if (id == k.id) {
            hasApi = k.hasApi;
            return true;
        }
    }
    return false;
}

bool eclipseApiReady() {
    auto& vt = eclipse::__internal__::getVTable();
    return vt.Config_getBoolInternal != nullptr;
}

std::string hexOf(std::string const& s) { return crypto::toHex(crypto::sha256(s)); }

int64_t fileSize(std::filesystem::path const& p) {
    std::error_code ec;
    auto size = std::filesystem::file_size(p, ec);
    return ec ? -1 : static_cast<int64_t>(size);
}

// ---- modules (Windows: EnumProcessModules; name + size only, SPEC §22) ----

struct ModuleCache {
    std::vector<telemetry::EnvironmentModule> modules;
    std::string gdHash = "unknown";
    std::string geodeHash = "unknown";
    std::string gprlHash = "unknown";
    bool ok = false;
    std::chrono::steady_clock::time_point at{};
};
ModuleCache s_modules;

void refreshModules() {
    auto now = std::chrono::steady_clock::now();
    if (s_modules.ok && now - s_modules.at < std::chrono::seconds(60)) return;
    ModuleCache cache;
    cache.at = now;
#ifdef GEODE_IS_WINDOWS
    HMODULE handles[2048];
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (EnumProcessModules(proc, handles, sizeof handles, &needed)) {
        size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(handles));
        cache.modules.reserve(count);
        std::string geodeBinary = dirs::getGeodeDir().string();
        std::string ownBinary = Mod::get()->getBinaryPath().filename().string();
        for (size_t i = 0; i < count; ++i) {
            char path[MAX_PATH];
            DWORD len = GetModuleFileNameA(handles[i], path, MAX_PATH);
            if (!len) continue;
            std::filesystem::path p(std::string(path, len));
            telemetry::EnvironmentModule m;
            m.name = p.filename().string();
            m.size = fileSize(p);
            if (m.size < 0) {
                MODULEINFO info{};
                if (GetModuleInformation(proc, handles[i], &info, sizeof info)) m.size = static_cast<int64_t>(info.SizeOfImage);
                else m.size = 0;
            }
            m.hash = hexOf(m.name + "|" + std::to_string(m.size));   // Phase 1: no content hash (documented)
            std::string lower = m.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "geometrydash.exe") cache.gdHash = m.hash;
            else if (lower == "geode.dll") cache.geodeHash = hexOf(m.name + "|" + std::to_string(m.size) + "|" + Loader::get()->getVersion().toNonVString());
            else if (!ownBinary.empty() && m.name == ownBinary) cache.gprlHash = hexOf(m.name + "|" + std::to_string(m.size) + "|" + Mod::get()->getVersion().toNonVString());
            cache.modules.push_back(std::move(m));
            if (cache.modules.size() >= telemetry::kMaxModulesPerEnvironment) break;
        }
        cache.ok = true;
    }
#endif
    if (cache.gprlHash == "unknown") {
        // fallback: our own package on disk
        auto own = Mod::get()->getBinaryPath();
        int64_t size = fileSize(own);
        if (size >= 0) cache.gprlHash = hexOf(own.filename().string() + "|" + std::to_string(size) + "|" + Mod::get()->getVersion().toNonVString());
    }
    s_modules = std::move(cache);
}

double currentFps() {
    double interval = CCDirector::sharedDirector()->getAnimationInterval();
    return interval > 0.0 ? 1.0 / interval : 0.0;
}

/// The menu-derived fields of an environment report (shared by capture() and refreshed()).
/// Integrity: core/classify integrityFor (flagged for bot / physics, warnings for noclip / unknown
/// gameplay mods, unknown when the module list could not be read).
void applyMenus(telemetry::EnvironmentPayload& p, MenuState const& menus, bool modulesReadable) {
    TrustState trust = classify(menus);
    p.cbf = menus.cbfActive;
    p.tpsBypass = menus.tpsBypass;
    p.tps = menus.tpsBypass ? menus.tps : kTicksPerSecond;
    p.fps = currentFps();
    p.trust = trust;
    p.noclip = menus.noclip;
    p.bot = menus.botState == 2;
    p.integrity = gprl::classify::integrityFor(trust, modulesReadable);
}

}  // namespace

MenuState readMenus() {
    MenuState m;
    auto loader = Loader::get();
    m.eclipseLoaded = loader->isModLoaded(kEclipse);
    m.megahackLoaded = loader->isModLoaded(kMegahack);
    m.openhackLoaded = loader->isModLoaded(kOpenhack);
    m.cbfLoaded = loader->isModLoaded(kCbf);
    if (m.cbfLoaded) {
        if (auto cbf = loader->getLoadedMod(kCbf)) {
            // Same reading as frame-perfect-counter: soft-toggle off and not "click on steps" = sub-tick inputs live.
            bool soft = cbf->getSettingValue<bool>("soft-toggle");
            bool onSteps = cbf->getSettingValue<bool>("click-on-steps");
            m.cbfActive = !soft && !onSteps;
        }
    }
    if (m.eclipseLoaded && eclipseApiReady()) {
        m.eclipseApi = true;
        m.noclip = eclipse::config::getInternal<bool>("player.noclip", false);
        m.botState = eclipse::config::getInternal<int>("bot.state", 0);
        m.tpsBypass = eclipse::config::getInternal<bool>("global.tpsbypass.toggle", false);
        if (m.tpsBypass) m.tps = eclipse::config::getInternal<double>("global.tpsbypass", 240.0);
        m.speedhack = eclipse::config::getInternal<bool>("global.speedhack.toggle", false);
        if (m.speedhack) m.speedhackValue = eclipse::config::getInternal<double>("global.speedhack", 1.0);
    }
    for (auto* mod : loader->getAllMods()) {
        if (!mod->isLoaded()) continue;
        bool hasApi = false;
        std::string id(mod->getID());
        if (isKnownGameplayMod(id, hasApi) && !hasApi) m.unknownGameplayMods.push_back(id);
    }
    return m;
}

TrustState classify(MenuState const& m) {
    if (m.botState == 2) return TrustState::Botting;                       // Eclipse macro playback
    if ((m.tpsBypass && std::abs(m.tps - 240.0) > 0.5) || (m.speedhack && std::abs(m.speedhackValue - 1.0) > 1e-3)) return TrustState::PhysicsChanged;
    if (m.noclip) return TrustState::NoclipModified;
    if (!m.unknownGameplayMods.empty()) return TrustState::UnknownMod;
    return TrustState::Allowed;
}

std::string hashLevel(std::string const& levelString, std::string const& fallbackId) {
    if (!levelString.empty()) return hexOf(levelString);
    if (!fallbackId.empty()) return hexOf("level-id|" + fallbackId);
    return "unknown";
}

Snapshot capture(std::string const& levelHash) {
    Snapshot s;
    s.menus = readMenus();
    s.trust = classify(s.menus);
    s.gdVersion = Loader::get()->getGameVersion();
    refreshModules();

    auto& p = s.payload;
    for (auto* mod : Loader::get()->getAllMods()) {
        if (!mod->isLoaded()) continue;
        telemetry::EnvironmentMod m;
        m.id = std::string(mod->getID());
        m.version = mod->getVersion().toNonVString();
        bool hasApi = false;
        bool known = isKnownGameplayMod(m.id, hasApi);
        m.gameplayAffecting = known || m.id == kCbf;
        p.mods.push_back(m);
        s.modList.push_back(m);
        if (p.mods.size() >= telemetry::kMaxModsPerEnvironment) break;
    }
    if (s_modules.ok) p.modules = s_modules.modules;
    p.hashes.gd = s_modules.gdHash;
    p.hashes.geode = s_modules.geodeHash;
    p.hashes.gprl = s_modules.gprlHash;
    p.hashes.level = levelHash.empty() ? "unknown" : levelHash;
    p.gdVersion = s.gdVersion;
    applyMenus(p, s.menus, s_modules.ok);
    return s;
}

telemetry::EnvironmentPayload refreshed(telemetry::EnvironmentPayload const& last, MenuState const& menus) {
    telemetry::EnvironmentPayload p = last;
    // modules present in the last report = the module list was readable (capture() rule)
    applyMenus(p, menus, last.modules.has_value());
    return p;
}

}  // namespace gprl::env
