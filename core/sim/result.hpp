#pragma once
// LevelSimResult: what one background analysis of one gameplay version produces
// (docs/BACKGROUND_ANALYZER_DESIGN.md §4.6). PURE C++20. Serialised to JSON by
// core/sim/analysis.cpp (`toJson`) with exactly the keys of shared/src/level-sim/types.ts
// (`LevelSimResult`); the server validates the same shape (shared/src/level-sim/validate.ts).
#include <cstdint>
#include <string>
#include <vector>

#include "world.hpp"

namespace gprl::sim {

enum class WindowSource : uint8_t { Reference = 0, Recorded = 1 };
constexpr char const* name(WindowSource s) { return s == WindowSource::Reference ? "reference" : "recorded"; }

/// One measured timing window (the same meaning as the live solver's timing_window event).
struct SimWindow {
    WindowSource source = WindowSource::Reference;
    int tick = 0;                 // level tick of the input (from the level start, 240 TPS)
    double frame = 0.0;           // tick + subTick
    double tSeconds = 0.0;        // tick / 240
    double percent = 0.0;         // x / endX * 100
    bool down = true;             // press / release
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    double windowMs = 0.0;        // latestMs - earliestMs
    double earliestMs = 0.0;      // relative to the performed input (negative = earlier still passes)
    double latestMs = 0.0;
    double resolutionMs = 1000.0 / kTicksPerSecond;   // 4.1667 ticks, or the sub-tick refinement
    bool boundedEarly = false;    // the early edge was found inside the shift range
    bool boundedLate = false;
    bool verified = false;        // the section's verification verdict (§4.5); false without a recorded attempt
    bool supported = true;        // outside every UnsupportedSpan
    uint32_t geometryHash = 0;    // core/geometry_hash low32 at the input x
    std::string geometryHashHex;
    int trials = 0;               // shifted simulations run for this window
};

struct VerifyBin {
    double from = 0.0, to = 0.0;  // percent
    int ticks = 0;                // recorded ticks that fell in the bin
    bool verified = false;
    bool unsupported = false;     // the bin overlaps an UnsupportedSpan (not counted either way)
    float maxError = 0.f;         // max |dx| + |dy| in the bin
    int firstDivergenceStep = 0;  // 0 = none
    std::string reason;           // "" or e.g. "y_velocity", "gamemode", "dead", "position"
};

struct VerifyResult {
    int attempts = 0;             // recorded attempts replayed
    int ticksCompared = 0;
    std::vector<VerifyBin> bins;  // 2 % bins
    double verifiedShare = 0.0;   // verified bins / countedBins (0 when countedBins == 0)
    double tolerancePosition = 0.5;
    // ---- gprl-sim/2 (additive; extra JSON keys the server validator ignores) ----
    int countedBins = 0;          // bins with recorded ticks and not unsupported: the share's denominator
    int reseeds = 0;              // engine re-seeds at unsupported-span exits (verify.hpp "spans")
};

struct SimSection {
    double from = 0.0, to = 0.0;  // percent; cut at gamemode / speed / size portals and unsupported spans
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool mini = false;
    int inputs = 0;
    double narrowestMs = 0.0;     // 0 = no bounded window
    double medianMs = 0.0;
    bool supported = true;
    bool solved = false;          // the reference run crosses it
    bool verified = false;        // every verify bin inside is verified (false without recording)
};

struct CoverageSpan {
    double percentFrom = 0.0, percentTo = 0.0;
    float x0 = 0.f, x1 = 0.f;
    std::string mechanic;         // unsupported: §4.7 name; unsolved: "search_exhausted" / "budget"
};

struct SimCoverage {
    double physicsPercent = 0.0;  // share of [0, endX) outside unsupported spans
    double solvedPercent = 0.0;   // share crossed by the reference run
    bool hasVerification = false;
    double verifiedPercent = 0.0; // VerifyResult::verifiedShare * 100
    std::vector<CoverageSpan> unsupported;
    std::vector<CoverageSpan> unsolved;
};

struct SimDensity {
    double avgCps = 0.0, p90Cps = 0.0, peak1sCps = 0.0, peak5sCps = 0.0;
};

struct SimBudget {
    double cpuMs = 0.0, wallMs = 0.0, pausedMs = 0.0;
    uint64_t ticksSimulated = 0;
    int trials = 0;
    int searchRestarts = 0;
    std::string mode;             // modes::name
    bool recordSafe = false;
    bool budgetExhausted = false; // the job stopped on the wall-time cap (Partial)
};

struct LevelSimResult {
    std::string analyzerVersion = kAnalyzerVersion;
    std::string simVersion = kSimVersion;
    std::string gameplayHashVersion = kGameplayHashVersion;
    int gdLevelId = 0;
    std::string levelHash;
    std::string gameplayHash;
    std::string computedAt;       // ISO-8601 UTC, set by the mod at hand-over
    std::string build;            // "gprl-geode 0.12.0+win"
    // world
    int objects = 0, gameplayObjects = 0, decorationObjects = 0;
    float lengthX = 0.f;
    double lengthSeconds = 0.0;   // reference run ticks / 240, or the speed integral when unsolved
    int startPositions = 0;
    bool tooLarge = false;
    // results
    SimCoverage coverage;
    int referenceInputs = 0;
    int referenceTicks = 0;
    uint64_t trajectoryDigest = 0;   // FNV over the reference trajectory (determinism golden)
    std::vector<SimWindow> windows;
    bool hasVerification = false;
    VerifyResult verification;
    SimDensity density;
    std::vector<SimSection> sections;
    SimBudget budget;
    std::vector<std::string> debug;  // human lines, capped (never player data)
};

}  // namespace gprl::sim
