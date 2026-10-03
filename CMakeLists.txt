cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)

project(GPRL VERSION 0.14.11)

# core/ is pure C++20 (no Geode or cocos includes) and is compiled a second time by
# tests/run_tests.ps1 with cl.exe on the host. Keep it that way.
set(GPRL_CORE_SOURCES
    core/json.cpp
    core/updater.cpp
    core/telemetry.cpp
    core/crypto.cpp
    core/calibration.cpp
    core/fingerprint.cpp
    core/classify.cpp
    # noclip-death-detector/2 (docs/NOCLIP_DEATH_DETECTOR.md): the live death / would-be-death rules, pure
    core/death_detector.cpp
    core/config.cpp
    core/identity.cpp
    core/ranks.cpp
    core/geometry_hash.cpp
    core/fingerprint_build.cpp
    core/display.cpp
    core/clip.cpp
    core/clip_flow.cpp
    core/live_recalc.cpp
    # v0.12.2 Patreon plans (docs/contracts/patreon.md): the GET /v1/me/entitlements parser + texts
    core/entitlements.cpp
    core/solver/boundary_search.cpp
    core/solver/local_window.cpp
    core/solver/pass_planner.cpp
    core/solver/timeline.cpp
    core/solver/window_event.cpp
    core/solver/sequence.cpp
    core/solver/sequence_adjusted.cpp
    core/solver/compensation.cpp
    core/solver/timing_result_event.cpp
    # v0.12.0 background level analyzer, analysis side (docs/BACKGROUND_ANALYZER_DESIGN.md §4.3-§4.6):
    # gameplay hash, reference-run search, windows, verification, result assembly + JSON, the job
    core/sim/gameplay_hash.cpp
    core/sim/search.cpp
    core/sim/windows.cpp
    core/sim/verify.cpp
    core/sim/analysis.cpp
    core/sim/job.cpp
    # v0.12.0 background level analyzer, physics side (docs/BACKGROUND_ANALYZER_DESIGN.md §4.2):
    # constants + per-mode kinematics, rects / slopes / hazards, the tick engine
    core/sim/physics.cpp
    core/sim/collision.cpp
    core/sim/engine.cpp
    # level families, identity side (docs/LEVEL_FAMILY_DESIGN.md §1-§3): gameplay / section /
    # presentation fingerprints computed on the analyzer worker after extraction
    core/identity/gameplay_fingerprint.cpp
    core/identity/sections.cpp
    core/identity/presentation.cpp
    core/identity/level_identity.cpp
)

set(GPRL_MOD_SOURCES
    src/main.cpp
    src/Updater.cpp
    src/Settings.cpp
    src/Hooks.cpp
    src/Tracker.cpp
    # noclip-death-detector/2: live death path glue (identity, GD physics-step scope, debug log + overlay)
    src/DeathPath.cpp
    src/Environment.cpp
    src/Telemetry.cpp
    src/Api.cpp
    src/LocalStore.cpp
    src/Hud.cpp
    # v0.13.0 redesigned menu (src/ui): the sidebar popup that replaced the v0.3.0 Popup.cpp tabs
    src/ui/Widgets.cpp
    src/ui/Menu.cpp
    src/ui/PageHome.cpp
    src/ui/PageRanks.cpp
    src/ui/PageBoard.cpp
    src/ui/PageLevel.cpp
    src/ui/PageAccount.cpp
    # v0.12.2: "Enter Patreon code" (the Patreon link confirmation)
    src/PatreonCodePopup.cpp
    src/Connect.cpp
    src/Clipper.cpp
    src/ClipHooks.cpp
    src/clip/ClipUtil.cpp
    src/clip/Process.cpp
    src/clip/Capture.cpp
    src/clip/AudioTap.cpp
    src/clip/MicCapture.cpp
    src/clip/ClipPopup.cpp
    src/clip/ClipLinkPopup.cpp
    src/solver/CloneEngine.cpp
    src/solver/CloneHooks.cpp
    src/solver/IsolationHooks.cpp
    src/solver/LiveCapture.cpp
    src/solver/GdOracle.cpp
    # v0.12.0 background level analyzer (docs/BACKGROUND_ANALYZER_DESIGN.md §2 "mod"): read-only
    # extraction, passive recorder, worker thread, modes, cache, status
    src/analyzer/Analyzer.cpp
    src/analyzer/Modes.cpp
    src/analyzer/Recorder.cpp
    src/analyzer/Extract.cpp
    src/analyzer/Worker.cpp
    src/analyzer/Cache.cpp
    src/analyzer/Status.cpp
)

add_library(${PROJECT_NAME} SHARED ${GPRL_CORE_SOURCES} ${GPRL_MOD_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# v0.12.1 self-update (src/Updater.cpp): only builds configured by tools/release-mod.ps1 with
# -DGPRL_RELEASE_BUILD=ON replace themselves with newer GitHub releases. Never set it in a build
# folder that is also used for development builds (CMake caches the option).
option(GPRL_RELEASE_BUILD "Release build: self-update from GitHub releases" OFF)
if (GPRL_RELEASE_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GPRL_RELEASE_BUILD=1)
    message(STATUS "GPRL: RELEASE build (self-update on)")
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})

# Clipping buffer (v0.6.0, src/clip): wglGetCurrentDC (frame read-back), ShellExecuteW (open the
# clips folder), COM + MMCSS (WASAPI microphone capture, opt-in). Same set as the In-Game Clipper.
target_link_libraries(${PROJECT_NAME} opengl32 shell32 ole32 avrt)

# Argon (GlobedGD/argon): proves Geometry Dash account ownership for "Connect" (v0.2.0). The client
# gets an authtoken; the GPRL Worker validates it with argon.globed.dev (never the GD password/GJP).
CPMAddPackage("gh:GlobedGD/argon@1.5.1")
target_link_libraries(${PROJECT_NAME} argon)
# MSVC 14.44 (portable toolchain) hits an internal compiler error (C1001 at arc
# future/Pollable.hpp:174, backend access violation) when a file that instantiates Argon's
# coroutines is compiled WITH Geode's precompiled header (/Yu); the same files compile cleanly
# without it. So: no PCH for Argon's own sources, nor for the one GPRL file that includes
# <argon/argon.hpp>. Keep every other Argon use inside src/Connect.cpp.
set_target_properties(argon PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
set_source_files_properties(src/Connect.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# v0.12.1: src/Updater.cpp spawns Geode web futures (arc) on a TaskHolder: same C1001 with the PCH
set_source_files_properties(src/Updater.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
