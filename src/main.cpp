// GPRL - Geometry Precision Ranking List client (gmo12.gprl). See README.md for what this build
// does and does not do. The server is authoritative for every rating (ARCHITECTURE §3).
#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>

#include "Clipper.hpp"
#include "LocalStore.hpp"
#include "Settings.hpp"
#include "Telemetry.hpp"
#include "Tracker.hpp"
#include "analyzer/Analyzer.hpp"

using namespace geode::prelude;

$on_mod(Loaded) {
    gprl::localstore::init(Mod::get()->getSaveDir() / "telemetry");
    gprl::analyzer::init();   // v0.12.0: the background analyzer's result folder (its worker starts with the first analysed level)
    gprl::client::init();
    gprl::settings::load();   // also applies the clipping settings (clipper::applySettings)
    gprl::clipper::init();
    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) { gprl::settings::load(); });
    log::info("GPRL {} loaded ({}; API {}; local-only {}; clipping {})", Mod::get()->getVersion().toVString(), gprl::settings::clientBuild(),
              gprl::settings::apiIsPlaceholder() ? "PLACEHOLDER - not configured" : gprl::settings::get().apiBaseUrl,
              gprl::settings::effectiveLocalOnly() ? "yes" : "no", gprl::settings::get().clipping ? "ON" : "off (enable it in the mod settings)");
}

// Game exit (SPEC §46, ARCHITECTURE §4). Geode sends GameEvent Exiting from CCDirector::purgeDirector
// on the main thread BEFORE static destructors run and before its own async runtime shuts down
// (Geode's listener has priority 100, this one the default 0, lower runs first). Windows'
// ExitProcess kills every other thread before DLL static destructors, so a destructor-based flush
// never reached the worker. Here the open attempt is ended (reason exit, the layer is not touched),
// then the worker spools everything pending to the local JSONL store synchronously - no network on
// the exit path - and the remote session is left for the server to close (Telemetry.hpp "Game exit").
$on_game(Exiting) {
    // first: stops feeding the encoder and saves the clip index (never blocks); the attempt end the
    // tracker reports next is then only recorded, no clip is cut while the game is closing
    gprl::clipper::shutdown();
    gprl::tracker::onGameExit();
    gprl::client::shutdown();
    // v0.12.0: the analyzer's worker stops (an unfinished job is simply dropped; finished results
    // are already on disk)
    gprl::analyzer::shutdown();
}
