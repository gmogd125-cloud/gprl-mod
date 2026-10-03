#pragma once
// Background level analyzer: analysis modes, Record-Safe Mode and the frame-pressure sampler
// (docs/BACKGROUND_ANALYZER_DESIGN.md §5, AN-D4, AN-D12). The RULES are pure and host-tested
// (core/sim/modes.hpp in sim_modes_tests, core/analyzer_recorder.hpp FramePressure in
// analyzer_recorder_tests); this file maps the settings onto them and publishes the runtime facts
// the worker thread polls.
//
// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. This file reads the settings, CCDirector's animation interval, the display's refresh
// rate and GD's Vertical Sync option, nothing else.
//
// Threads: the setters and the sampler run on the main thread; config(), facts(),
// simAllowedNow() and framePressure() are lock-free reads any thread may make (the worker).
#include <string>

#include "../../core/sim/modes.hpp"

namespace gprl::analyzer::modes {

/// settings::load() (main thread): `enabled`, `analysis-mode`, `record-safe`, `analysis-cpu`.
void applySettings();

/// v0.12.2 (any thread; the telemetry worker after every GET /v1/me/entitlements, and Normal on
/// Disconnect): what the server's plan allows for `analysis-cpu` (core/entitlements.hpp
/// backgroundAnalysis). Never read from a setting or a file. config() resolves the setting against
/// it (core/sim/modes.hpp resolveCpu): fast needs Plus, fastest Pro, anything above falls back.
void setSpeedAllowance(sim::modes::SpeedAllowance allowed);
/// The setting vs what runs (Session tab / log).
sim::modes::CpuChoice cpuChoice();
/// "Analysis speed: Normal (Fast needs GPRL Plus)" (core/sim/modes.hpp speedLine).
std::string speedLine();

/// Lock-free (any thread). `cpu` / `lowCpu` are the EFFECTIVE tier: the worker's burst
/// (sim::modes::workerBurst) and the extraction's 0.5 ms (low) / 1 ms (every other tier) slice.
sim::modes::ModeConfig config();
/// The live clone solver's one gate (CloneEngine via oracle::setup / refreshGate / applySettings).
bool clonesAllowed();
/// Bot / replay playback measured as level-only evidence (oracle::refreshGate).
bool botPlaybackEvidenceAllowed();
/// offline / full: the isolated simulator is part of this configuration (passive: no simulation,
/// no level identity / family notice either).
bool simulatorEnabled();
/// "Analysis: offline simulation (Record-Safe)" (Account tab, Session tab).
char const* summary();
/// "live solver off (Record-Safe)" / "live solver off (mod disabled)" / "live solver on": the
/// solver's state line (every analysis mode keeps the live solver; only Record-Safe stops it).
std::string liveSolverOffText();
bool analysisDebug();

// ---- runtime facts (main thread writes, the worker reads) ----
void setInLevel(bool inLevel);
/// An attempt is open: from resetLevel until the death / completion / exit.
void setAttemptActive(bool active);
/// The death pause: from a real death until the next attempt starts. Record-Safe counts it as part
/// of the attempt (§5 "death pause excluded"), the extraction counts it as a pause (8 ms slices).
void setDeathPause(bool deathPause);
/// The pause menu is open (PlayLayer::pauseGame / resume).
void setPaused(bool paused);

/// The facts the worker's may-run rule sees (attemptActive includes the death pause).
sim::modes::RuntimeFacts facts();
bool simAllowedNow();
/// Main-thread extraction slice budget in microseconds for one PlayLayer::postUpdate
/// (core/sim/modes.hpp extractionSliceUs: 0 under frame pressure, else min(8 ms, 25 % of the frame
/// target), <= 0.5 / 1 ms while an attempt runs; the death pause counts as not playing). The 8 ms
/// slice of setupHasCompleted is passed by Analyzer::onLevelEnter itself (kSliceLimits.setupUs).
int extractionSliceUs();

// ---- frame pressure (PlayLayer::postUpdate, main thread) ----
/// Re-reads the target frame time: CCDirector's animation interval (GD's FPS cap), the display
/// refresh rate and GD's Vertical Sync option (the refresh rate only bounds the target with VSync on).
void refreshTarget();
/// One rendered level frame (wall time since the previous call is measured here).
void frameSample();
/// Level enter / pause / exit: the window restarts (a paused game is no pressure).
void resetPressure();
bool framePressure();
double frameAverageMs();
double frameTargetMs();

}  // namespace gprl::analyzer::modes
