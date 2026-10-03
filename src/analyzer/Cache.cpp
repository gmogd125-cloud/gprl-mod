// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field. This file only reads / writes the
// analyzer's own result files in the mod's save dir (worker thread).
#include "Cache.hpp"

#include <Geode/Geode.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "../../core/analyzer_extract.hpp"
#include "../../core/analyzer_worker_rules.hpp"
#include "../../core/sim/world.hpp"

using namespace geode::prelude;

namespace gprl::analyzer::cache {

namespace fs = std::filesystem;

namespace {

std::mutex s_mx;
fs::path s_dir;
fs::path s_identityDir;
std::unordered_set<std::string> s_versionLogged;   // stored results of other versions, logged once each

bool safeHash(std::string const& h) {
    if (h.empty() || h.size() > 64) return false;
    for (char c : h) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

rules::StoreVersions currentVersions() {
    rules::StoreVersions v;
    v.analyzer = sim::kAnalyzerVersion;
    v.sim = sim::kSimVersion;
    v.gameplayHash = sim::kGameplayHashVersion;
    v.extract = extract::kVersion;
    return v;
}

fs::path fileFor(int gdLevelId, std::string const& gameplayHash) { return s_dir / rules::resultFileName(gdLevelId, gameplayHash, currentVersions()); }

int64_t unixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string nameOf(fs::path const& p) { return pathText(p.filename()); }

bool readText(fs::path const& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/// "Used now" for the LRU (a failure only makes the file look older).
void touch(fs::path const& path) {
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
}

/// Temp file + rename; the temp file never survives a failure.
bool writeAtomic(fs::path const& path, std::string const& text) {
    fs::path tmp = path;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            log::warn("GPRL analyzer: cannot write {}", pathText(tmp));
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            log::warn("GPRL analyzer: writing {} failed (disk full?)", pathText(tmp));
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm;
        fs::remove(path, rm);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        log::warn("GPRL analyzer: cannot replace {} ({})", nameOf(path), ec.message());
        std::error_code rm;
        fs::remove(tmp, rm);
        return false;
    }
    return true;
}

/// The LRU cap over both folders (s_mx held). `protect` = the file just written / read.
void enforceLocked(fs::path const* protect) {
    struct Found {
        fs::path path;
        rules::CacheFile file;
    };
    std::vector<Found> found;
    int tmpRemoved = 0;
    for (fs::path const* d : {&s_dir, &s_identityDir}) {
        if (d->empty()) continue;
        std::error_code ec;
        fs::directory_iterator it(*d, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code fe;
            if (!it->is_regular_file(fe)) continue;
            fs::path p = it->path();
            if (p.extension() == ".tmp") {
                // only this thread writes here and never while enforcing: a temp file is a leftover
                std::error_code rm;
                if (fs::remove(p, rm)) ++tmpRemoved;
                continue;
            }
            if (p.extension() != ".json") continue;
            Found f;
            f.path = p;
            auto size = fs::file_size(p, fe);
            f.file.bytes = fe ? 0 : static_cast<uint64_t>(size);
            auto t = fs::last_write_time(p, fe);
            f.file.lastUse = fe ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
            found.push_back(std::move(f));
        }
    }
    std::vector<rules::CacheFile> files;
    files.reserve(found.size());
    std::optional<size_t> keep;
    for (size_t i = 0; i < found.size(); ++i) {
        files.push_back(found[i].file);
        if (protect) {
            std::error_code ec;
            if (fs::equivalent(found[i].path, *protect, ec)) keep = i;
        }
    }
    auto drop = rules::lruEvict(files, rules::kCacheCapBytes, keep);
    uint64_t freed = 0;
    int removed = 0;
    for (size_t i : drop) {
        std::error_code ec;
        if (fs::remove(found[i].path, ec)) {
            ++removed;
            freed += found[i].file.bytes;
        }
    }
    if (removed > 0 || tmpRemoved > 0) {
        log::info("GPRL analyzer: local analysis store over its {} MB cap - removed {} least recently used file(s) ({:.1f} MB){}",
                  rules::kCacheCapBytes / (1024 * 1024), removed, static_cast<double>(freed) / (1024.0 * 1024.0),
                  tmpRemoved > 0 ? fmt::format(" and {} leftover temp file(s)", tmpRemoved) : std::string());
    }
}

void enforceSafely(fs::path const* protect) {
    try {
        enforceLocked(protect);
    } catch (std::exception const& e) {
        log::warn("GPRL analyzer: the local analysis store's size check failed ({})", e.what());
    } catch (...) {
        log::warn("GPRL analyzer: the local analysis store's size check failed");
    }
}

}  // namespace

std::string pathText(fs::path const& p) {
    try {
        auto u = p.u8string();
        return std::string(reinterpret_cast<char const*>(u.data()), u.size());
    } catch (...) {
        return "<path>";
    }
}

void init(fs::path dir) {
    std::lock_guard lock(s_mx);
    s_dir = std::move(dir);
    std::error_code ec;
    fs::create_directories(s_dir, ec);
    if (ec) log::warn("GPRL analyzer: cannot create {} ({}); finished analyses are not kept on this computer", pathText(s_dir), ec.message());
}

fs::path dir() {
    std::lock_guard lock(s_mx);
    return s_dir;
}

std::optional<Entry> load(int gdLevelId, std::string const& gameplayHash) {
    try {
        std::lock_guard lock(s_mx);
        if (s_dir.empty() || gdLevelId == 0 || !safeHash(gameplayHash)) return std::nullopt;
        auto path = fileFor(gdLevelId, gameplayHash);
        std::error_code ec;
        if (!fs::exists(path, ec)) return std::nullopt;
        std::string text;
        if (!readText(path, text)) return std::nullopt;
        json::Value v;
        json::ParseError pe;
        if (!json::parse(text, v, &pe) || !v.isObject() || !v["result"].isObject()) {
            log::warn("GPRL analyzer: ignoring the unreadable stored analysis {} ({})", nameOf(path), pe.message);
            return std::nullopt;
        }
        // review MEDIUM-9: a result of another analyzer / simulator / hash / extraction version is never used
        auto const& vs = v["versions"];
        rules::StoreVersions stored;
        if (vs.isObject()) {
            stored.analyzer = vs.getString("analyzer");
            stored.sim = vs.getString("sim");
            stored.gameplayHash = vs.getString("gameplayHash");
            stored.extract = vs.getString("extract");
        }
        auto cur = currentVersions();
        if (!rules::versionsMatch(stored, cur)) {
            if (s_versionLogged.insert(nameOf(path)).second) {
                log::info("GPRL analyzer: the stored analysis {} is of other versions ({} / {} / {} / {}, now {} / {} / {} / {}); it is not used", nameOf(path),
                          stored.analyzer.empty() ? "?" : stored.analyzer, stored.sim.empty() ? "?" : stored.sim,
                          stored.gameplayHash.empty() ? "?" : stored.gameplayHash, stored.extract.empty() ? "?" : stored.extract, cur.analyzer, cur.sim,
                          cur.gameplayHash, cur.extract);
            }
            return std::nullopt;
        }
        Entry e;
        e.uploaded = v.getBool("uploaded");
        e.uploadStatus = v.getString("uploadStatus");
        e.savedAtMs = v.getInt("savedAt");
        e.result = v["result"];
        e.resultText = json::stringify(e.result);
        touch(path);
        return e;
    } catch (std::exception const& ex) {
        log::warn("GPRL analyzer: reading a stored analysis failed ({})", ex.what());
    } catch (...) {
        log::warn("GPRL analyzer: reading a stored analysis failed");
    }
    return std::nullopt;
}

bool store(int gdLevelId, std::string const& gameplayHash, std::string const& resultText, bool uploaded, std::string const& uploadStatus) {
    try {
        std::lock_guard lock(s_mx);
        if (s_dir.empty() || gdLevelId == 0 || !safeHash(gameplayHash)) return false;
        auto path = fileFor(gdLevelId, gameplayHash);
        auto cur = currentVersions();
        std::string text;
        text.reserve(resultText.size() + 512);
        text += "{\"versions\":{\"analyzer\":" + json::stringify(json::Value(cur.analyzer));
        text += ",\"sim\":" + json::stringify(json::Value(cur.sim));
        text += ",\"gameplayHash\":" + json::stringify(json::Value(cur.gameplayHash));
        text += ",\"extract\":" + json::stringify(json::Value(cur.extract)) + "}";
        text += std::string(",\"uploaded\":") + (uploaded ? "true" : "false");
        text += ",\"uploadStatus\":" + json::stringify(json::Value(uploadStatus));
        text += ",\"savedAt\":" + std::to_string(unixMs());
        text += ",\"result\":";
        text += resultText;
        text += "}";
        if (!writeAtomic(path, text)) return false;
        enforceSafely(&path);
        return true;
    } catch (std::exception const& ex) {
        log::warn("GPRL analyzer: storing the analysis of level {} failed ({})", gdLevelId, ex.what());
    } catch (...) {
        log::warn("GPRL analyzer: storing the analysis of level {} failed", gdLevelId);
    }
    return false;
}

void enforceLimit() {
    try {
        std::lock_guard lock(s_mx);
        enforceSafely(nullptr);
    } catch (...) {
    }
}

// ---- level families ----

void initIdentity(fs::path dir) {
    std::lock_guard lock(s_mx);
    s_identityDir = std::move(dir);
    std::error_code ec;
    fs::create_directories(s_identityDir, ec);
    if (ec) log::warn("GPRL analyzer: cannot create {} ({}); level identities are not kept on this computer", pathText(s_identityDir), ec.message());
}

fs::path identityDir() {
    std::lock_guard lock(s_mx);
    return s_identityDir;
}

std::optional<json::Value> loadIdentity(std::string const& levelHash) {
    try {
        std::lock_guard lock(s_mx);
        if (s_identityDir.empty() || !safeHash(levelHash)) return std::nullopt;
        auto path = s_identityDir / (levelHash + ".json");
        std::error_code ec;
        if (!fs::exists(path, ec)) return std::nullopt;
        std::string text;
        if (!readText(path, text)) return std::nullopt;
        json::Value v;
        json::ParseError pe;
        if (!json::parse(text, v, &pe) || !v.isObject()) {
            log::warn("GPRL analyzer: ignoring the unreadable stored level identity {} ({})", nameOf(path), pe.message);
            return std::nullopt;
        }
        touch(path);
        return v;
    } catch (std::exception const& ex) {
        log::warn("GPRL analyzer: reading a stored level identity failed ({})", ex.what());
    } catch (...) {
        log::warn("GPRL analyzer: reading a stored level identity failed");
    }
    return std::nullopt;
}

bool storeIdentity(std::string const& levelHash, std::string const& recordText) {
    try {
        std::lock_guard lock(s_mx);
        if (s_identityDir.empty() || !safeHash(levelHash)) return false;
        auto path = s_identityDir / (levelHash + ".json");
        if (!writeAtomic(path, recordText)) return false;
        enforceSafely(&path);
        return true;
    } catch (std::exception const& ex) {
        log::warn("GPRL analyzer: storing a level identity failed ({})", ex.what());
    } catch (...) {
        log::warn("GPRL analyzer: storing a level identity failed");
    }
    return false;
}

}  // namespace gprl::analyzer::cache
