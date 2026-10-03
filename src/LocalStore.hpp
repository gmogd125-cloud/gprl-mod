#pragma once
// Bounded, rotating JSONL spool in the mod's save directory (<save dir>/telemetry/).
//
// Used for: local-only mode (every batch), batches produced while not connected or while the API is not
// configured ("unsent"), and batches the server rejected or that failed after retries ("failed",
// evidence is never silently dropped - ARCHITECTURE §4). One JSON object per line:
//   {"kind":"local"|"unsent"|"failed","at":<unix ms>,"status":<http or 0>,"signature":"<hex or empty>","batch":{...}}
// Files: batches-<n>.jsonl, rotated at maxFileBytes, at most maxFiles kept (oldest deleted).
// Thread: any (internal mutex); the telemetry worker is the normal caller. No per-click writes.
//
// STUB: re-uploading "unsent" spool files after a later link is not implemented (Phase 2); the
// files stay on disk for the playtest export tooling.
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace gprl::localstore {

struct Config {
    size_t maxFileBytes = 4u * 1024u * 1024u;
    int maxFiles = 8;
};

/// Main thread, once (needs Mod::get()->getSaveDir()).
void init(std::filesystem::path dir, Config cfg = {});
std::filesystem::path dir();
/// Account tab "Reset data": removes every spooled batches-*.jsonl and restarts the series at 0.
void clear();

/// Append one spool record. `batchJson` must already be a JSON object text.
void appendBatch(std::string_view kind, int status, std::string_view signatureHex, std::string const& batchJson);

/// Records written since init (for the popup).
int64_t recordsWritten();

}  // namespace gprl::localstore
