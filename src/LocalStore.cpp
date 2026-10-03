#include "LocalStore.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <vector>

using namespace geode::prelude;

namespace gprl::localstore {

namespace {

std::mutex s_mx;
std::filesystem::path s_dir;
Config s_cfg;
int s_index = 0;
size_t s_currentBytes = 0;
int64_t s_records = 0;
bool s_ready = false;

std::filesystem::path fileFor(int index) { return s_dir / fmt::format("batches-{}.jsonl", index); }

// Highest existing index and its size, so restarts continue the series.
void scanExisting() {
    std::error_code ec;
    int best = -1;
    for (auto const& e : std::filesystem::directory_iterator(s_dir, ec)) {
        auto name = e.path().filename().string();
        int idx = 0;
        if (std::sscanf(name.c_str(), "batches-%d.jsonl", &idx) == 1) best = std::max(best, idx);
    }
    s_index = best < 0 ? 0 : best;
    auto size = std::filesystem::file_size(fileFor(s_index), ec);
    s_currentBytes = ec ? 0 : static_cast<size_t>(size);
}

void pruneOld() {
    std::error_code ec;
    std::vector<int> indices;
    for (auto const& e : std::filesystem::directory_iterator(s_dir, ec)) {
        int idx = 0;
        if (std::sscanf(e.path().filename().string().c_str(), "batches-%d.jsonl", &idx) == 1) indices.push_back(idx);
    }
    std::sort(indices.begin(), indices.end());
    while (static_cast<int>(indices.size()) > s_cfg.maxFiles) {
        std::filesystem::remove(fileFor(indices.front()), ec);
        indices.erase(indices.begin());
    }
}

}  // namespace

void init(std::filesystem::path dir, Config cfg) {
    std::lock_guard lock(s_mx);
    s_dir = std::move(dir);
    s_cfg = cfg;
    std::error_code ec;
    std::filesystem::create_directories(s_dir, ec);
    if (ec) {
        log::warn("GPRL local store: cannot create {}: {}", s_dir.string(), ec.message());
        s_ready = false;
        return;
    }
    scanExisting();
    s_ready = true;
}

std::filesystem::path dir() {
    std::lock_guard lock(s_mx);
    return s_dir;
}

void clear() {
    std::lock_guard lock(s_mx);
    if (!s_ready) return;
    std::error_code ec;
    int removed = 0;
    for (auto const& e : std::filesystem::directory_iterator(s_dir, ec)) {
        int idx = 0;
        if (std::sscanf(e.path().filename().string().c_str(), "batches-%d.jsonl", &idx) != 1) continue;
        std::error_code rec;
        if (std::filesystem::remove(e.path(), rec)) ++removed;
    }
    s_index = 0;
    s_currentBytes = 0;
    s_records = 0;
    log::info("GPRL local store: reset removed {} spool file(s) in {}", removed, s_dir.string());
}

void appendBatch(std::string_view kind, int status, std::string_view signatureHex, std::string const& batchJson) {
    std::lock_guard lock(s_mx);
    if (!s_ready) return;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string line = fmt::format("{{\"kind\":\"{}\",\"at\":{},\"status\":{},\"signature\":\"{}\",\"batch\":{}}}\n", kind, now, status,
                                   signatureHex, batchJson);
    if (s_currentBytes + line.size() > s_cfg.maxFileBytes && s_currentBytes > 0) {
        ++s_index;
        s_currentBytes = 0;
        pruneOld();
    }
    std::ofstream f(fileFor(s_index), std::ios::binary | std::ios::app);
    if (!f) return;
    f << line;
    s_currentBytes += line.size();
    ++s_records;
}

int64_t recordsWritten() {
    std::lock_guard lock(s_mx);
    return s_records;
}

}  // namespace gprl::localstore
