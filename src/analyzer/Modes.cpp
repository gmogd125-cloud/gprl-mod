// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. Reads here: the mod settings, CCDirector::getAnimationInterval(), the display refresh rate,
// GD's own Vertical Sync option (GameManager::getGameVariable(GameVar::VerticalSync) = "0030", a
// lookup in GD's option store).
#include "Modes.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <chrono>

#include "../../core/analyzer_recorder.hpp"
#include "../Settings.hpp"

// last: <Windows.h> (EnumDisplaySettingsW) must not see the std::min / std::max uses above
#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace gprl::analyzer::modes {

namespace {

namespace sm = sim::modes;

// config packed into one word: enabled | mode << 1 | recordSafe << 3 | debug << 5 | requested cpu tier << 6
// (bit 4 is unused since v0.12.2: lowCpu follows the EFFECTIVE tier, resolved in config())
std::atomic<uint32_t> s_cfgBits{1u | (static_cast<uint32_t>(sm::AnalysisMode::Full) << 1)};
// v0.12.2: what the server's entitlement allows (core/entitlements.hpp, written by the telemetry
// worker after every GET /v1/me/entitlements; Normal = Free / Supporter / unknown). Memory only.
std::atomic<uint8_t> s_allowance{static_cast<uint8_t>(sm::SpeedAllowance::Normal)};
std::atomic<bool> s_inLevel{false};
std::atomic<bool> s_attempt{false};
std::atomic<bool> s_deathPause{false};
std::atomic<bool> s_paused{false};
std::atomic<bool> s_pressure{false};
std::atomic<double> s_avgMs{0.0};
std::atomic<double> s_targetMs{1000.0 / 60.0};

FramePressure s_sampler;   // main thread only
std::chrono::steady_clock::time_point s_lastFrame{};
bool s_haveLastFrame = false;
double s_refreshHz = 0.0;
bool s_refreshRead = false;

sm::CpuTier requestedTier(uint32_t b) { return static_cast<sm::CpuTier>((b >> 6) & 3u); }

sm::SpeedAllowance allowance() { return static_cast<sm::SpeedAllowance>(s_allowance.load(std::memory_order_relaxed)); }

sm::ModeConfig unpack(uint32_t b, sm::SpeedAllowance allowed) {
    sm::ModeConfig c;
    c.enabled = (b & 1u) != 0;
    c.mode = static_cast<sm::AnalysisMode>((b >> 1) & 3u);
    c.recordSafe = ((b >> 3) & 1u) != 0;
    c.cpu = sm::resolveCpu(requestedTier(b), allowed).effective;
    c.lowCpu = c.cpu == sm::CpuTier::Low;
    return c;
}

double displayRefreshHz() {
#ifdef GEODE_IS_WINDOWS
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return static_cast<double>(dm.dmDisplayFrequency);
#endif
    return 0.0;
}

}  // namespace

void applySettings() {
    auto const& s = settings::get();
    auto mode = sm::parse(s.analysisMode.c_str());
    auto tier = sm::parseCpu(s.analysisCpu.c_str());
    uint32_t b = (s.enabled ? 1u : 0u) | (static_cast<uint32_t>(mode) << 1) | (s.recordSafe ? 1u << 3 : 0u) | (s.analysisDebug ? 1u << 5 : 0u)
                 | (static_cast<uint32_t>(tier) << 6);
    uint32_t old = s_cfgBits.exchange(b);
    if (old != b) {
        auto c = unpack(b, allowance());
        log::info("GPRL analyzer: {} (analysis-mode {}, record-safe {}, {}){}", sm::summary(c), sm::name(c.mode), c.recordSafe ? "on" : "off",
                  speedLine(), sm::clonesAllowed(c) ? "" : " - the live clone solver is off");
    }
}

void setSpeedAllowance(sm::SpeedAllowance allowed) {
    auto old = static_cast<sm::SpeedAllowance>(s_allowance.exchange(static_cast<uint8_t>(allowed)));
    if (old == allowed) return;
    // only when the effective tier changes is it worth a line (a Plus plan with `analysis-cpu` low changes nothing)
    uint32_t b = s_cfgBits.load(std::memory_order_relaxed);
    auto before = sm::resolveCpu(requestedTier(b), old);
    auto after = sm::resolveCpu(requestedTier(b), allowed);
    if (before.effective != after.effective || before.limited != after.limited)
        log::info("GPRL analyzer: plan allows {} analysis -> {}", sm::name(allowed), sm::speedLine(after));
}

sm::CpuChoice cpuChoice() { return sm::resolveCpu(requestedTier(s_cfgBits.load(std::memory_order_relaxed)), allowance()); }

std::string speedLine() { return sm::speedLine(cpuChoice()); }

sm::ModeConfig config() { return unpack(s_cfgBits.load(std::memory_order_relaxed), allowance()); }
bool clonesAllowed() { return sm::clonesAllowed(config()); }
bool botPlaybackEvidenceAllowed() { return sm::botPlaybackEvidenceAllowed(config()); }
bool simulatorEnabled() { return sm::simulatorEnabled(config()); }
char const* summary() { return sm::summary(config()); }
bool analysisDebug() { return ((s_cfgBits.load(std::memory_order_relaxed) >> 5) & 1u) != 0 || settings::debugEnabled(); }

std::string liveSolverOffText() {
    auto c = config();
    if (!c.enabled) return "live solver off (mod disabled)";
    if (c.recordSafe) return "live solver off (Record-Safe)";
    return "live solver on";
}

void setInLevel(bool v) { s_inLevel.store(v); }
void setAttemptActive(bool v) { s_attempt.store(v); }
void setDeathPause(bool v) { s_deathPause.store(v); }
void setPaused(bool v) { s_paused.store(v); }

sm::RuntimeFacts facts() {
    sm::RuntimeFacts f;
    f.inLevel = s_inLevel.load();
    f.attemptActive = f.inLevel && (s_attempt.load() || s_deathPause.load());
    f.paused = s_paused.load();
    f.framePressure = f.inLevel && !f.paused && s_pressure.load();
    return f;
}

bool simAllowedNow() { return sm::simAllowedNow(config(), facts()); }

int extractionSliceUs() {
    sm::RuntimeFacts f;
    f.inLevel = s_inLevel.load();
    f.attemptActive = s_attempt.load() && !s_deathPause.load();
    f.paused = s_paused.load();
    f.framePressure = f.inLevel && !f.paused && s_pressure.load();
    return sm::extractionSliceUs(config(), f, s_targetMs.load());
}

void refreshTarget() {
    if (!s_refreshRead) {
        s_refreshHz = displayRefreshHz();
        s_refreshRead = true;
    }
    double interval = 0.0;
    if (auto* d = CCDirector::sharedDirector()) interval = d->getAnimationInterval();
    bool vsync = false;
    if (auto* gm = GameManager::get()) vsync = gm->getGameVariable(GameVar::VerticalSync);
    double t = targetFrameMs(interval, s_refreshHz, vsync);
    s_sampler.setTargetMs(t);
    s_targetMs.store(t);
}

void frameSample() {
    auto now = std::chrono::steady_clock::now();
    if (s_haveLastFrame) {
        double ms = std::chrono::duration<double, std::milli>(now - s_lastFrame).count();
        s_sampler.frame(ms);
        s_pressure.store(s_sampler.pressure());
        s_avgMs.store(s_sampler.averageMs());
    }
    s_lastFrame = now;
    s_haveLastFrame = true;
}

void resetPressure() {
    s_sampler.reset();
    s_haveLastFrame = false;
    s_pressure.store(false);
    s_avgMs.store(0.0);
}

bool framePressure() { return s_pressure.load(); }
double frameAverageMs() { return s_avgMs.load(); }
double frameTargetMs() { return s_targetMs.load(); }

}  // namespace gprl::analyzer::modes
