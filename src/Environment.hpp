#pragma once
// Environment inspection (SPEC §21-§23, ARCHITECTURE §3): the loaded Geode mods, the known mod
// menus and what their APIs say (Eclipse: noclip, bot state, TPS bypass, speedhack), the modules
// loaded in the GD process, reference hashes, and the resulting client-side TrustState.
//
// SCOPE (SPEC §22): only the GD environment. Phase 1 inspects
//   - the GD install dir: GeometryDash.exe name + size,
//   - the Geode loader binary name + size + version,
//   - the modules loaded in the GD process: FILE NAME + SIZE ONLY (EnumProcessModules), hashed as
//     sha256("name|size"). Module CONTENTS are NOT read in Phase 1 (documented; the server can ask
//     for content hashes of specific modules in a later phase),
//   - GPRL's own binary name + size + version,
//   - the level string (hashed for the session).
// Nothing outside the game process / install is ever touched.
//
// The TrustState here is INFORMATIONAL: the server re-classifies from the mod list and holds the
// policy (SPEC §21). It drives the HUD / popup and the `attempt_end.legit` flag only.
#include <string>
#include <vector>

#include "../core/telemetry.hpp"
#include "../core/vocab.hpp"

namespace gprl::env {

/// Live state of the known mod menus (cheap to read: a few Eclipse config lookups).
struct MenuState {
    bool eclipseLoaded = false;
    bool eclipseApi = false;      // Eclipse's vtable answered (API available)
    bool noclip = false;          // eclipse "player.noclip"
    int botState = 0;             // eclipse "bot.state": 0 off, 1 recording, 2 playback
    bool tpsBypass = false;       // eclipse "global.tpsbypass.toggle"
    double tps = 240.0;           // eclipse "global.tpsbypass" when the toggle is on
    bool speedhack = false;       // eclipse "global.speedhack.toggle"
    double speedhackValue = 1.0;
    bool megahackLoaded = false;  // absolllute.megahack (no API used yet)
    bool openhackLoaded = false;  // prevter.openhack (no API used yet)
    bool cbfLoaded = false;       // syzzi.click_between_frames
    bool cbfActive = false;       // loaded and not soft-toggled off / not "click on steps"
    std::vector<std::string> unknownGameplayMods;   // known gameplay-affecting mods without a readable API
};

MenuState readMenus();

/// Client-side trust classification of a MenuState (SPEC §21 precedence: botting > physics
/// changed > noclip > unknown mod > allowed).
TrustState classify(MenuState const& m);

/// Everything the session start and the environment event need.
struct Snapshot {
    telemetry::EnvironmentPayload payload;   // hashes.level filled by the caller
    std::vector<telemetry::EnvironmentMod> modList;
    MenuState menus;
    TrustState trust = TrustState::Allowed;
    std::string gdVersion;
};

/// Main thread. Enumerates mods + modules (modules cached for a minute) and reads the menus.
Snapshot capture(std::string const& levelHash);

/// Main thread, cheap (no mod / module enumeration): the last emitted report with its
/// menu-derived fields (cbf, tpsBypass, tps, fps, trust, noclip, bot, integrity) replaced by
/// `menus`. Mods, modules and hashes are kept: they cannot change while the game runs. The tracker
/// compares the result with the last report (core/classify environmentChanged) and emits a new
/// `environment` event when it differs (SPEC §19-§21, TELEMETRY.md §6).
telemetry::EnvironmentPayload refreshed(telemetry::EnvironmentPayload const& last, MenuState const& menus);

/// Hex SHA-256 of a level string (or "unknown" when empty).
std::string hashLevel(std::string const& levelString, std::string const& fallbackId);

}  // namespace gprl::env
