#include "Settings.hpp"

#include "Clipper.hpp"
#include "Telemetry.hpp"
#include "analyzer/Analyzer.hpp"
#include "solver/GdOracle.hpp"
#include "../core/config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>

using namespace geode::prelude;

namespace gprl::settings {

namespace {

Settings s_settings;
std::atomic<bool> s_debug{false};

}  // namespace

void load() {
    auto m = Mod::get();
    Settings s;
    s.enabled = m->getSettingValue<bool>("enabled");
    s.apiBaseUrl = m->getSettingValue<std::string>("api-base-url");
    // "Open my profile" only opens links on this origin (core/identity profileUrlAllowed).
    s.siteOrigin = identity::normalizeSiteOrigin(m->getSettingValue<std::string>("site-url"));
    s.localOnly = m->getSettingValue<bool>("local-only");
    s.liveRecalc = m->getSettingValue<bool>("live-recalc");
    s.showHud = m->getSettingValue<bool>("show-hud");
    s.showLastWindow = m->getSettingValue<bool>("show-last-window");
    s.debugLog = m->getSettingValue<bool>("debug-log");
    // Owner decision 2026-10-02: measuring cannot be turned off (`measure-windows` was removed; a
    // saved `false` from v0.11 and older is never read). Only Record-Safe Mode stops the live solver.
    s.solverSubtick = m->getSettingValue<std::string>("solver-subtick");
    s.solverDebug = m->getSettingValue<std::string>("solver-debug");
    s.maxShiftTicks = static_cast<int>(m->getSettingValue<int64_t>("max-shift-ticks"));
    s.horizonSeconds = m->getSettingValue<double>("horizon-seconds");
    s.settleLookahead = m->getSettingValue<bool>("settle-lookahead");
    s.settleMaxSeconds = m->getSettingValue<double>("settle-max-seconds");
    s.settleGroundTicks = static_cast<int>(m->getSettingValue<int64_t>("settle-ground-ticks"));
    s.measureJointShare = m->getSettingValue<bool>("measure-joint-share");
    s.measureSequenceAdjusted = m->getSettingValue<bool>("measure-sequence-adjusted");
    s.traceMaxTicks = std::clamp(static_cast<int>(m->getSettingValue<int64_t>("solver-trace-max-ticks")), 0, 40);
    s.traceOverlay = m->getSettingValue<bool>("solver-trace-overlay");
    s.isolationCheck = m->getSettingValue<std::string>("isolation-check");
    s.solverDual = m->getSettingValue<std::string>("solver-dual");
    s.analysisMode = m->getSettingValue<std::string>("analysis-mode");
    s.recordSafe = m->getSettingValue<bool>("record-safe");
    s.analysisCpu = m->getSettingValue<std::string>("analysis-cpu");
    s.analysisCache = m->getSettingValue<bool>("analysis-cache");
    s.analysisUpload = m->getSettingValue<bool>("analysis-upload");
    s.analysisDebug = m->getSettingValue<bool>("analysis-debug");
    s.familyNotice = m->getSettingValue<bool>("family-notice");
    s.clipping = m->getSettingValue<bool>("clipping");
    s.clipBufferSeconds = static_cast<int>(m->getSettingValue<int64_t>("clip-buffer-seconds"));
    s.clipQuality = m->getSettingValue<std::string>("clip-quality");
    s.clipDiskCapMb = static_cast<int>(m->getSettingValue<int64_t>("clip-disk-cap-mb"));
    s.clipGameAudio = m->getSettingValue<bool>("clip-game-audio");
    s.clipMic = m->getSettingValue<bool>("clip-mic");
    s.clipDesktopAudio = m->getSettingValue<bool>("clip-desktop-audio");
    s.clipFolder = m->getSettingValue<std::filesystem::path>("clip-folder");
    s.ffmpegPath = m->getSettingValue<std::filesystem::path>("ffmpeg-path");
    // "gprl-api.me.workers.dev", "https://.../v1/", "http://...workers.dev" -> "https://...workers.dev"
    // (core/config.cpp, host-tested); path joins append "/v1/..." to this. An empty value or the old
    // example placeholder (saved by v0.1.0-v0.1.1) means the deployed Worker, config::kDefaultApi.
    s.apiBaseUrl = config::effectiveApiBaseUrl(s.apiBaseUrl);
    s_settings = std::move(s);
    s_debug.store(s_settings.debugLog, std::memory_order_relaxed);
    client::applySettings();
    // the analyzer's modes first: the solver's gate (oracle::applySettings) asks them
    analyzer::applySettings();
    solver::oracle::applySettings();
    clipper::applySettings();
}

Settings const& get() { return s_settings; }

std::string clientBuild() {
    return fmt::format("gprl-geode {}+win", Mod::get()->getVersion().toNonVString());
}

bool apiIsPlaceholder() {
    return config::apiBaseUrlIsPlaceholder(s_settings.apiBaseUrl) || !config::apiBaseUrlIsValid(s_settings.apiBaseUrl);
}

bool effectiveLocalOnly() { return s_settings.localOnly || apiIsPlaceholder(); }

bool debugEnabled() { return s_debug.load(std::memory_order_relaxed); }

}  // namespace gprl::settings
