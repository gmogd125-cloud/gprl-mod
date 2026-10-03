#pragma once
// Background level analyzer: the local result store (docs/BACKGROUND_ANALYZER_DESIGN.md AN-D7).
// One file per gameplay version AND analyzer version under the mod's save dir:
//   analysis/<gdLevelId>-<gameplayHash>-<versionTag>.json  =
//     { "versions": { "analyzer", "sim", "gameplayHash", "extract" }, "uploaded": bool,
//       "uploadStatus": "...", "savedAt": unix ms, "result": <LevelSimResult> }
// so a level visited again (offline too) is not simulated again, and an analysis that could not be
// sent is sent on a later visit. Review MEDIUM-9 (2026-10-02): the key carries the versions -
// `versionTag` (core/analyzer_worker_rules.hpp, FNV-1a 32 over sim::kAnalyzerVersion,
// sim::kSimVersion, sim::kGameplayHashVersion and extract::kVersion) is part of the file name, and
// load() also compares the stored `versions` with the current constants: a result of any other
// analyzer / simulator / hash / extraction version is never used (it is left on disk for the LRU,
// not deleted on sight). Files of v0.12.0 before this rule (no tag) are never found again.
//
// Review MEDIUM-10: analysis/ + identity/ together are capped at rules::kCacheCapBytes (200 MB),
// least recently used first (a load touches the file's write time); enforced after every store and
// once when the worker thread starts (enforceLimit). A temp file is removed on every failure path.
//
// Written by the worker thread only (temp file + rename); the paths are fixed on the main thread at
// init. Every function catches its own exceptions (filesystem_error, bad_alloc): a broken disk never
// reaches the worker loop. Paths are logged through pathText (UTF-8: path::string() throws on Windows
// for a user name outside the ANSI code page).
//
// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field; this file touches only its own
// files in the mod's save dir.
#include <filesystem>
#include <optional>
#include <string>

#include "../../core/json.hpp"

namespace gprl::analyzer::cache {

/// UTF-8 text of a path for logs (never throws).
std::string pathText(std::filesystem::path const& p);

/// Main thread, once (Mod::get()->getSaveDir() / "analysis").
void init(std::filesystem::path dir);
std::filesystem::path dir();

struct Entry {
    bool uploaded = false;
    std::string uploadStatus;
    int64_t savedAtMs = 0;
    json::Value result;            // the LevelSimResult JSON object
    std::string resultText;        // the same, as stored (sent as is)
};

/// Worker thread. nullopt = no file / unreadable / other versions (a broken file is ignored, never deleted).
std::optional<Entry> load(int gdLevelId, std::string const& gameplayHash);
/// Worker thread. `resultText` = the LevelSimResult JSON object text. False on an I/O error (logged).
bool store(int gdLevelId, std::string const& gameplayHash, std::string const& resultText, bool uploaded, std::string const& uploadStatus);

/// Worker thread: deletes least recently used files of analysis/ + identity/ until both together
/// fit in rules::kCacheCapBytes (stale *.tmp files go first). Logs what it removed.
void enforceLimit();

// ---- level families (docs/LEVEL_FAMILY_DESIGN.md §8): identity/<levelHash>.json ----
// One file per EXACT level version (the sha256 level hash), holding the identity JSON, the server's
// V1LevelIdentityResponse and timestamps (core/analyzer_identity.hpp cacheRecordText /
// parseCacheRecord, which also carries its own record version). A revisit within 24 h of the answer
// sends nothing. Same thread / write / cap rules.

/// Main thread, once (Mod::get()->getSaveDir() / "identity").
void initIdentity(std::filesystem::path dir);
std::filesystem::path identityDir();
/// Worker thread. The parsed record object, nullopt = no file / unreadable.
std::optional<json::Value> loadIdentity(std::string const& levelHash);
/// Worker thread. `recordText` = the whole record as JSON text. False on an I/O error (logged).
bool storeIdentity(std::string const& levelHash, std::string const& recordText);

}  // namespace gprl::analyzer::cache
