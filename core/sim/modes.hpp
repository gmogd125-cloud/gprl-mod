#pragma once
// Analysis modes and Record-Safe Mode (docs/BACKGROUND_ANALYZER_DESIGN.md §5, AN-D4). PURE rules,
// host-tested in tests/sim_modes_tests.cpp. The mod (src/analyzer/Modes.cpp) maps the settings
// `analysis-mode` / `record-safe` / `analysis-cpu` onto `ModeConfig` and asks these functions;
// CloneEngine asks `clonesAllowed` once at level enter and once at every attempt start.
//
// v0.12.2 analysis speed (`analysis-cpu` low | normal | fast | fastest, docs/contracts/patreon.md):
// a NON-competitive service. The plan the SERVER hands out (core/entitlements.hpp,
// features.backgroundAnalysis) caps what the player may pick; the setting is what the player wants.
// Free / Supporter keep exactly the pre-0.12.2 choices (low, normal); Plus adds fast, Pro fastest.
// A tier only changes the analyzer WORKER's burst length: the simulator still never runs on the
// game thread, the main-thread extraction slices are those of `normal`, and every tier still pauses
// under frame pressure and (Record-Safe) during attempts. Nothing here touches timing windows,
// sigma/s, telemetry or the content of an upload.
#include <cstdint>
#include <string>

namespace gprl::sim::modes {

enum class AnalysisMode : uint8_t {
    Passive = 0,   // telemetry only: no clones, no simulator
    Offline = 1,   // + the isolated background simulator
    Full = 2,      // + the live clone solver at the real player's inputs (player timing windows)
};

constexpr char const* name(AnalysisMode m) {
    switch (m) {
        case AnalysisMode::Passive: return "passive";
        case AnalysisMode::Offline: return "offline";
        case AnalysisMode::Full: return "full";
    }
    return "?";
}

/// Parses the setting string; unknown -> Full (the pre-0.12.0 behaviour; the first-run popup
/// writes an explicit value).
constexpr AnalysisMode parse(char const* s) {
    if (!s) return AnalysisMode::Full;
    auto eq = [](char const* a, char const* b) {
        while (*a && *b && *a == *b) { ++a; ++b; }
        return *a == 0 && *b == 0;
    };
    if (eq(s, "passive")) return AnalysisMode::Passive;
    if (eq(s, "offline")) return AnalysisMode::Offline;
    return AnalysisMode::Full;
}

// ---- analysis speed (v0.12.2) ----

/// `analysis-cpu`, in order of speed.
enum class CpuTier : uint8_t { Low = 0, Normal = 1, Fast = 2, Fastest = 3 };

constexpr char const* name(CpuTier t) {
    switch (t) {
        case CpuTier::Low: return "low";
        case CpuTier::Normal: return "normal";
        case CpuTier::Fast: return "fast";
        case CpuTier::Fastest: return "fastest";
    }
    return "low";
}

/// "Low" / "Normal" / "Fast" / "Fastest" (popup text).
constexpr char const* label(CpuTier t) {
    switch (t) {
        case CpuTier::Low: return "Low";
        case CpuTier::Normal: return "Normal";
        case CpuTier::Fast: return "Fast";
        case CpuTier::Fastest: return "Fastest";
    }
    return "Low";
}

/// Parses the setting string; unknown -> Low (the default, and what every value other than
/// "normal" meant before v0.12.2).
constexpr CpuTier parseCpu(char const* s) {
    if (!s) return CpuTier::Low;
    auto eq = [](char const* a, char const* b) {
        while (*a && *b && *a == *b) { ++a; ++b; }
        return *a == 0 && *b == 0;
    };
    if (eq(s, "normal")) return CpuTier::Normal;
    if (eq(s, "fast")) return CpuTier::Fast;
    if (eq(s, "fastest")) return CpuTier::Fastest;
    return CpuTier::Low;
}

/// What the server's entitlement allows (Entitlement.features.backgroundAnalysis): `normal` = Free
/// and Supporter (low, normal), `faster` = Plus (+ fast), `fastest` = Pro (+ fastest). Unknown,
/// not connected or not fetched yet = Normal: Free is never worse than before v0.12.2.
enum class SpeedAllowance : uint8_t { Normal = 0, Faster = 1, Fastest = 2 };

constexpr char const* name(SpeedAllowance a) {
    switch (a) {
        case SpeedAllowance::Normal: return "normal";
        case SpeedAllowance::Faster: return "faster";
        case SpeedAllowance::Fastest: return "fastest";
    }
    return "normal";
}

/// The fastest tier an allowance lets the player pick.
constexpr CpuTier maxTier(SpeedAllowance a) {
    switch (a) {
        case SpeedAllowance::Normal: return CpuTier::Normal;
        case SpeedAllowance::Faster: return CpuTier::Fast;
        case SpeedAllowance::Fastest: return CpuTier::Fastest;
    }
    return CpuTier::Normal;
}

/// The lowest plan a tier needs ("" = every plan).
constexpr char const* requiredPlan(CpuTier t) {
    switch (t) {
        case CpuTier::Fast: return "GPRL Plus";
        case CpuTier::Fastest: return "GPRL Pro";
        default: return "";
    }
}

struct CpuChoice {
    CpuTier requested = CpuTier::Low;   // the setting
    CpuTier effective = CpuTier::Low;   // what runs
    bool limited = false;               // the setting is above the plan: effective = the best the plan allows
};

/// The setting capped by the plan. Above the allowance the speed falls back to the best tier the
/// plan allows: `normal` for Free / Supporter (and whenever the entitlement is unknown), `fast` for
/// Plus asking for `fastest`. Never above the allowance; low / normal are never changed.
constexpr CpuChoice resolveCpu(CpuTier requested, SpeedAllowance allowed) {
    CpuChoice c;
    c.requested = requested;
    CpuTier cap = maxTier(allowed);
    if (static_cast<int>(requested) > static_cast<int>(cap)) {
        c.effective = cap;
        c.limited = true;
    }
    else c.effective = requested;
    return c;
}

/// The analyzer worker's work burst and rest (AN-D12) per tier. low 8 / 8 ms (at most half a core)
/// and normal 16 / 1 ms are the pre-0.12.2 numbers; fast 32 / 1, fastest 64 / 1. Every tier runs at
/// below-normal thread priority and every burst ends early when simAllowedNow says no.
struct WorkerBurst {
    double burstMs = 8.0;
    double restMs = 8.0;
};
constexpr WorkerBurst workerBurst(CpuTier t) {
    switch (t) {
        case CpuTier::Low: return WorkerBurst{8.0, 8.0};
        case CpuTier::Normal: return WorkerBurst{16.0, 1.0};
        case CpuTier::Fast: return WorkerBurst{32.0, 1.0};
        case CpuTier::Fastest: return WorkerBurst{64.0, 1.0};
    }
    return WorkerBurst{8.0, 8.0};
}

/// The Session tab line: "Analysis speed: Fast (GPRL Plus)", "Analysis speed: Normal",
/// "Analysis speed: Normal (Fast needs GPRL Plus)", "Analysis speed: Fast (Fastest needs GPRL Pro)".
inline std::string speedLine(CpuChoice const& c) {
    std::string s = std::string("Analysis speed: ") + label(c.effective);
    if (c.limited) s += std::string(" (") + label(c.requested) + " needs " + requiredPlan(c.requested) + ")";
    else if (*requiredPlan(c.effective)) s += std::string(" (") + requiredPlan(c.effective) + ")";
    return s;
}

struct ModeConfig {
    bool enabled = true;          // the mod's `enabled`
    AnalysisMode mode = AnalysisMode::Full;
    bool recordSafe = false;
    bool lowCpu = true;           // the effective `analysis-cpu` tier is low (0.5 ms extraction slices while playing)
    CpuTier cpu = CpuTier::Low;   // v0.12.2: the effective tier (resolveCpu), the worker's burst (workerBurst)
};

/// May hidden clones be stepped in the live physics loop? Owner decision 2026-10-02: measuring
/// the player's own timing windows cannot be turned off - every analysis mode keeps the live
/// solver; Record-Safe Mode (list submissions) is the one switch that stops it.
constexpr bool clonesAllowed(ModeConfig const& c) {
    return c.enabled && !c.recordSafe;
}

/// May bot / replay playback be used as level-only evidence? (Record-Safe: no automation at all.)
constexpr bool botPlaybackEvidenceAllowed(ModeConfig const& c) {
    return c.enabled && !c.recordSafe;
}

/// Is the isolated simulator part of this configuration at all?
constexpr bool simulatorEnabled(ModeConfig const& c) {
    return c.enabled && c.mode != AnalysisMode::Passive;
}

struct RuntimeFacts {
    bool attemptActive = false;   // an attempt is running (between resetLevel and death / complete / quit)
    bool inLevel = false;         // a PlayLayer exists
    bool paused = false;          // PauseLayer open
    bool framePressure = false;   // the frame-time sampler says the game is struggling (§0 AN-D12)
};

/// May the worker thread run NOW? Record-Safe: only while no attempt is active (menus, pause,
/// between attempts). Otherwise: whenever the game is not under frame pressure.
constexpr bool simAllowedNow(ModeConfig const& c, RuntimeFacts const& f) {
    if (!simulatorEnabled(c)) return false;
    if (f.framePressure) return false;
    if (c.recordSafe && f.attemptActive && !f.paused) return false;
    return true;
}

/// The extraction's slice caps (AN-D12; review fix 2026-10-02: 8 ms only at setupHasCompleted).
struct SliceLimits {
    int setupUs = 8000;            // PlayLayer::setupHasCompleted: the level is still loading (the caller passes it directly)
    int maxUs = 8000;              // never more than this per rendered frame
    double frameShare = 0.25;      // ... and never more than this share of the target frame time
    int playingLowUs = 500;        // an attempt runs, `analysis-cpu` low
    int playingNormalUs = 1000;    // an attempt runs, `analysis-cpu` normal / fast / fastest (<= 1 ms while playing, every tier)
    double fallbackTargetMs = 1000.0 / 60.0;   // an unusable frame target (<= 0, NaN, absurd) counts as 60 FPS
};
inline constexpr SliceLimits kSliceLimits{};

/// Main-thread extraction slice budget in microseconds for one rendered level frame
/// (PlayLayer::postUpdate). 0 = no slice this frame: no simulator, or the game is under frame
/// pressure. Otherwise min(8 ms, 25 % of the target frame time), and while an attempt is active
/// (not paused) also at most 0.5 ms (`analysis-cpu` low) / 1 ms (normal). The death pause counts as
/// "not playing" for the caller (src/analyzer/Modes.cpp) but still gets only the frame-share cap.
/// `frameTargetMs` = the frame-pressure sampler's target (core/analyzer_recorder.hpp targetFrameMs).
constexpr int extractionSliceUs(ModeConfig const& c, RuntimeFacts const& f, double frameTargetMs, SliceLimits const& k = kSliceLimits) {
    if (!simulatorEnabled(c)) return 0;
    if (f.framePressure) return 0;   // the game is struggling: skip the slice (the walk resumes next frame)
    double target = frameTargetMs > 0.0 && frameTargetMs < 1000.0 ? frameTargetMs : k.fallbackTargetMs;   // NaN fails both compares
    double shareUs = target * 1000.0 * k.frameShare;
    int cap = shareUs < static_cast<double>(k.maxUs) ? static_cast<int>(shareUs) : k.maxUs;
    if (f.attemptActive && !f.paused) {
        int playing = c.lowCpu ? k.playingLowUs : k.playingNormalUs;
        if (playing < cap) cap = playing;
    }
    return cap > 0 ? cap : 1;
}

/// The one-line status for the Account tab / HUD.
constexpr char const* summary(ModeConfig const& c) {
    if (!c.enabled) return "Analysis: off";
    switch (c.mode) {
        case AnalysisMode::Passive: return c.recordSafe ? "Analysis: passive telemetry (Record-Safe: live solver off)" : "Analysis: live solver, no level simulation";
        case AnalysisMode::Offline:
        case AnalysisMode::Full: return c.recordSafe ? "Analysis: offline simulation (Record-Safe: live solver off)" : "Analysis: full (live solver + level simulation)";
    }
    return "Analysis: ?";
}

}  // namespace gprl::sim::modes
