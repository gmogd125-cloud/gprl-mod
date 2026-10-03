#pragma once
// Mod settings (mod.json "settings") cached in a plain struct on the main thread, plus the
// client build string and the "is the API configured" rule. The telemetry worker never reads
// these directly (it gets a copy through client::applySettings, see Telemetry.cpp).
#include <Geode/Geode.hpp>

#include "../core/config.hpp"
#include "../core/identity.hpp"

#include <filesystem>
#include <string>

namespace gprl::settings {

/// PLACEHOLDER API base URL shipped in mod.json until the GPRL Worker is deployed. While the
/// setting still equals this (or points at any *.example.* host) the mod behaves as if local-only
/// mode were on: nothing is sent anywhere and the popup says so.
constexpr char const* kPlaceholderApi = config::kPlaceholderApi;

struct Settings {
    bool enabled = true;
    std::string apiBaseUrl = kPlaceholderApi;
    std::string siteOrigin = identity::kDefaultSite;   // normalised `site-url` (core/identity)
    bool localOnly = false;
    // live updates (owner decision 2026-10-01, core/live_recalc.hpp): POST /v1/me/recalc every 45 s
    // while a counting level sends timing windows, so the HUD, the menu and the website follow the game
    bool liveRecalc = true;
    bool showHud = true;
    bool showLastWindow = true;   // middle-right readout of the last measured window
    bool debugLog = false;
    // noclip-death-detector/2 (docs/NOCLIP_DEATH_DETECTOR.md §6): one Geode log line per death /
    // would-be-death candidate (accepted or rejected, with the reason) and the hitbox overlay in the
    // level. Off by default: nothing is read or drawn for it then.
    bool deathDebug = false;
    // solver (docs/SOLVER_DESIGN.md): timing windows are measured with hidden clones in every
    // analysis mode (owner decision 2026-10-02: `measure-windows` was removed, only Record-Safe
    // Mode stops the live solver)
    std::string solverSubtick = "1/8 tick";   // "off" | "1/8 tick" | "1/64 tick" (only with CBF active)
    std::string solverDebug = "windows";      // "off" | "windows" | "verbose" (debug-log = verbose)
    int maxShiftTicks = 10;
    double horizonSeconds = 0.5;
    // v0.11.0 (core/solver/settle.hpp): a shifted copy must be SETTLED (on the ground after the
    // input, or flying long enough) before it counts as passing; alive-in-the-air is not a pass
    bool settleLookahead = true;
    double settleMaxSeconds = 8.0;
    int settleGroundTicks = 24;
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §3.4 item 9): `measure-joint-share` replaces v0.6.x
    // `measure-sequences` for the M4 joint jobs (superseded, default off, only while no
    // sequence-adjusted job waits); `measure-sequence-adjusted` = the sequence-adjusted windows
    bool measureJointShare = false;
    bool measureSequenceAdjusted = true;
    // v0.7.0 debug view (AUDIT §11, docs/TIMING_SOLVER_V2.md §2.12): `solver-trace-max-ticks`
    // traces every local window at most this many ticks wide (0 = off, 0..40, at most 8 per
    // attempt); `solver-trace-overlay` draws the last traced input in the level (src/Hud)
    int traceMaxTicks = 0;
    bool traceOverlay = false;
    // v0.8.2 (docs/LIVE_ISOLATION_DESIGN.md §3.2): `isolation-check` = "every block" (release
    // default) | "every clone step" (pinpoints the trial; the debug log forces it)
    std::string isolationCheck = "every block";
    // v0.8.3 (docs/LIVE_ISOLATION_DESIGN.md §2.7): `solver-dual` = "shadow only" (default: the P1 + P2
    // pair shadow in dual sections, nothing measured) | "off" (no clone steps while dual)
    std::string solverDual = "shadow only";
    // v0.12.0 background level analyzer (docs/BACKGROUND_ANALYZER_DESIGN.md §5, src/analyzer/Modes
    // turns these into core/sim/modes.hpp ModeConfig): passive (no background level simulation) |
    // offline = full (the background simulation runs; default full). The live clone solver runs in
    // every mode; Record-Safe Mode is the one switch that stops it (no hidden clones, no bot
    // evidence, the simulator only while no attempt runs)
    std::string analysisMode = "full";
    bool recordSafe = false;
    // "low" | "normal" | v0.12.2 "fast" (GPRL Plus) | "fastest" (GPRL Pro): what the player WANTS;
    // the server's plan caps what runs (src/analyzer/Modes resolves it, core/sim/modes.hpp resolveCpu)
    std::string analysisCpu = "low";
    bool analysisCache = true;                // ask the server for a trusted cached analysis first
    bool analysisUpload = true;               // send the finished analysis (level evidence only)
    bool analysisDebug = false;               // analyzer steps in the Geode log
    // level families (docs/LEVEL_FAMILY_DESIGN.md FA-D11): the top-left notice on level enter when
    // the server links this exact level version to a known family (the identity upload itself
    // follows `analysis-upload`)
    bool familyNotice = true;
    // clipping buffer (v0.6.0, docs/CLIPPING.md; src/Clipper turns these into core/clip ClipConfig)
    bool clipping = false;                    // OFF by default: nothing is recorded until the player turns it on
    int clipBufferSeconds = 120;              // 10..600
    std::string clipQuality = "720p";         // "480p" | "720p" | "1080p"
    int clipDiskCapMb = 1024;                 // 128..8192
    bool clipGameAudio = true;
    bool clipMic = false;                     // separate opt-in (SPEC §29)
    bool clipDesktopAudio = false;            // v0.9.0: the computer's own sound as a third track
    std::filesystem::path clipFolder;         // empty = the mod save folder
    std::filesystem::path ffmpegPath;         // empty = looked up (src/clip/ClipUtil resolvePaths)
};

/// Main thread: (re)reads every setting from Geode. Called on load and on every setting change.
void load();
Settings const& get();

/// "gprl-geode <version>+win" - the clientBuild string of every batch / connect / session request.
std::string clientBuild();
/// True while the API base URL is still the placeholder (or empty / an example host) or is not a
/// usable http(s) URL after normalisation (core/config.hpp). Either way nothing is sent.
bool apiIsPlaceholder();
/// local-only setting OR placeholder API: never network.
bool effectiveLocalOnly();

/// Debug log gate (setting "debug-log"): every event, batch and network step.
bool debugEnabled();

}  // namespace gprl::settings

/// GPRL_DEBUG("...", args) - Geode debug log line only while the debug-log setting is on.
#define GPRL_DEBUG(...)                                                       \
    do {                                                                      \
        if (::gprl::settings::debugEnabled()) geode::log::debug(__VA_ARGS__); \
    } while (0)
