#pragma once
// Background level analyzer: the PURE rules behind the worker thread and its local result store
// (docs/BACKGROUND_ANALYZER_DESIGN.md §2 "Worker", AN-D7, AN-D12; review fixes 2026-10-02).
// PURE C++20, no Geode / cocos / GD include; host-tested in tests/analyzer_worker_tests.cpp.
// src/analyzer/Worker.cpp and src/analyzer/Cache.cpp only apply these:
//
//   SessionMap        level hash -> the telemetry session of THIS device on that exact version,
//                     filled only from the (sessionLevelHash, sessionId, sessionGen) triple the
//                     telemetry worker publishes together (review MEDIUM-5); an older generation
//                     never overwrites a newer one; the bounded map evicts the oldest entry.
//   LevelBacklog<T>   the ordered backlog of finished extractions that wait while the simulator may
//                     not run (Record-Safe during an attempt, frame pressure: review MEDIUM-8);
//                     attempts / the level exit of a waiting visit attach to its item.
//   versionTag / versionsMatch   the local result store's version key (review MEDIUM-9).
//   lruEvict          which cached files go when the store is over its cap (review MEDIUM-10).
//   rateLimitDelaySeconds   a 429's Retry-After, clamped (review LOW).
//   levelSimUploadBody / levelSimUploadBodyBytes   the exact POST /v1/me/level-sim body and its
//                     size, so the 2 MB check covers the {sessionId, result} envelope (review LOW).
//   rearmOnRevisit    which "kept on this computer" upload states a revisit sends again (review LOW).
//
// READ-ONLY RULE (the whole analyzer, AN-D1 / AN-D11): nothing here touches the game.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json.hpp"

namespace gprl::analyzer::rules {

// ---- the session per exact level version ----

class SessionMap {
public:
    explicit SessionMap(size_t capacity = 64) : m_cap(capacity < 1 ? 1 : capacity) {}

    /// The telemetry worker's published triple. Empty hash / id = nothing to map. A generation older
    /// than the one stored for the same hash is ignored (a late publish of a previous session).
    /// True when the map changed.
    bool note(std::string const& levelHash, std::string const& sessionId, uint32_t gen) {
        if (levelHash.empty() || sessionId.empty()) return false;
        auto it = m_map.find(levelHash);
        if (it != m_map.end()) {
            if (gen < it->second.gen) return false;
            bool changed = it->second.sessionId != sessionId || it->second.gen != gen;
            it->second.sessionId = sessionId;
            it->second.gen = gen;
            it->second.order = ++m_seq;   // used again: the newest
            return changed;
        }
        if (m_map.size() >= m_cap) evictOldest();
        m_map.emplace(levelHash, Slot{sessionId, gen, ++m_seq});
        return true;
    }

    /// "" = no session of this device seen on that version this run.
    std::string find(std::string const& levelHash) const {
        auto it = m_map.find(levelHash);
        return it == m_map.end() ? std::string() : it->second.sessionId;
    }

    size_t size() const { return m_map.size(); }
    bool contains(std::string const& levelHash) const { return m_map.count(levelHash) != 0; }

private:
    struct Slot {
        std::string sessionId;
        uint32_t gen = 0;
        uint64_t order = 0;   // insertion / last update sequence
    };
    void evictOldest() {
        auto oldest = m_map.end();
        for (auto it = m_map.begin(); it != m_map.end(); ++it) {
            if (oldest == m_map.end() || it->second.order < oldest->second.order) oldest = it;
        }
        if (oldest != m_map.end()) m_map.erase(oldest);
    }
    size_t m_cap;
    uint64_t m_seq = 0;
    std::unordered_map<std::string, Slot> m_map;
};

// ---- the backlog of extractions waiting for the simulator to be allowed ----

template <class T>
class LevelBacklog {
public:
    struct Item {
        uint64_t visitId = 0;
        bool closed = false;   // the level was left while the item waited
        T payload;
    };

    explicit LevelBacklog(size_t capacity = 4) : m_cap(capacity < 1 ? 1 : capacity) {}

    /// A finished extraction. Returns the visit id of the item dropped to stay within the capacity
    /// (the oldest), 0 = none.
    uint64_t push(uint64_t visitId, T payload) {
        uint64_t dropped = 0;
        if (m_items.size() >= m_cap) {
            dropped = m_items.front().visitId;
            m_items.pop_front();
        }
        m_items.push_back(Item{visitId, false, std::move(payload)});
        return dropped;
    }

    /// The waiting item of a visit (nullptr = not waiting): attempts attach to it.
    Item* find(uint64_t visitId) {
        for (auto& i : m_items) {
            if (i.visitId == visitId) return &i;
        }
        return nullptr;
    }

    /// The level of a waiting visit was left: it is handled later as a closed visit. False = not waiting.
    bool markClosed(uint64_t visitId) {
        if (auto* i = find(visitId)) {
            i->closed = true;
            return true;
        }
        return false;
    }

    bool empty() const { return m_items.empty(); }
    size_t size() const { return m_items.size(); }
    Item take() {
        Item i = std::move(m_items.front());
        m_items.pop_front();
        return i;
    }
    void clear() { m_items.clear(); }

private:
    size_t m_cap;
    std::deque<Item> m_items;
};

// ---- the local result store's version key ----

struct StoreVersions {
    std::string analyzer;       // sim::kAnalyzerVersion
    std::string sim;            // sim::kSimVersion
    std::string gameplayHash;   // sim::kGameplayHashVersion
    std::string extract;        // analyzer::extract::kVersion
};

inline bool versionsMatch(StoreVersions const& a, StoreVersions const& b) {
    return a.analyzer == b.analyzer && a.sim == b.sim && a.gameplayHash == b.gameplayHash && a.extract == b.extract;
}

/// 8 lowercase hex digits (FNV-1a 32 over the four version strings, each ended by a 0 byte) - the
/// file-name part that keeps a result of other versions from ever being found.
inline std::string versionTag(StoreVersions const& v) {
    uint32_t h = 2166136261u;
    auto mix = [&](std::string const& s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        h ^= 0u;
        h *= 16777619u;
    };
    mix(v.analyzer);
    mix(v.sim);
    mix(v.gameplayHash);
    mix(v.extract);
    char b[16];
    std::snprintf(b, sizeof b, "%08x", h);
    return b;
}

/// analysis/<gdLevelId>-<gameplayHash>-<versionTag>.json
inline std::string resultFileName(int gdLevelId, std::string const& gameplayHash, StoreVersions const& v) {
    return std::to_string(gdLevelId) + "-" + gameplayHash + "-" + versionTag(v) + ".json";
}

// ---- the store's size cap ----

constexpr uint64_t kCacheCapBytes = 200ull * 1024ull * 1024ull;   // analysis/ + identity/ together

struct CacheFile {
    uint64_t bytes = 0;
    int64_t lastUse = 0;   // any monotonic stamp (the file's last write time; a load touches it)
};

/// Indices of the files to delete, least recently used first, until the rest fit in `capBytes`.
/// `protect` (the file just written or read) is never chosen. Ties: the lower index goes first.
inline std::vector<size_t> lruEvict(std::vector<CacheFile> const& files, uint64_t capBytes, std::optional<size_t> protect = std::nullopt) {
    uint64_t total = 0;
    for (auto const& f : files) total += f.bytes;
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < files.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return files[a].lastUse < files[b].lastUse; });
    std::vector<size_t> out;
    for (size_t i : order) {
        if (total <= capBytes) break;
        if (protect && *protect == i) continue;
        out.push_back(i);
        total -= files[i].bytes;
    }
    return out;
}

// ---- uploads ----

constexpr int kRateLimitDefaultSeconds = 600;   // the server's rule: one stored upload per device + version per 10 min
constexpr int kRateLimitMinSeconds = 60;
constexpr int kRateLimitMaxSeconds = 3600;

/// A 429's wait: the Retry-After header's seconds when the server sent one (clamped to
/// [60 s, 60 min]), else 10 minutes. `retryAfterSeconds` < 0 = no header.
inline int rateLimitDelaySeconds(int retryAfterSeconds) {
    if (retryAfterSeconds < 0) return kRateLimitDefaultSeconds;
    return std::clamp(retryAfterSeconds, kRateLimitMinSeconds, kRateLimitMaxSeconds);
}

/// Retry-After as delta-seconds ("120"); an HTTP date or junk = -1 (the caller uses the default).
inline int parseRetryAfter(std::string const& header) {
    size_t i = 0;
    while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
    if (i >= header.size()) return -1;
    long long v = 0;
    size_t digits = 0;
    for (; i < header.size() && header[i] >= '0' && header[i] <= '9'; ++i, ++digits) {
        v = v * 10 + (header[i] - '0');
        if (v > 86400) v = 86400;
    }
    while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
    if (digits == 0 || i != header.size()) return -1;
    return static_cast<int>(v);
}

/// The exact POST /v1/me/level-sim body: { "sessionId": "<id>", "result": <resultJson> } (sessionId
/// omitted when empty). src/Api.cpp sends exactly this text.
inline std::string levelSimUploadBody(std::string const& sessionId, std::string const& resultJson) {
    std::string body = "{";
    if (!sessionId.empty()) body += "\"sessionId\":" + json::stringify(json::Value(sessionId)) + ",";
    body += "\"result\":" + resultJson + "}";
    return body;
}

/// Its size without building it (the 2 MB check runs before every attempt to send).
inline size_t levelSimUploadBodyBytes(std::string const& sessionId, std::string const& resultJson) {
    size_t n = 1 + 9 + resultJson.size() + 1;   // { "result": ... }
    if (!sessionId.empty()) n += 12 + json::stringify(json::Value(sessionId)).size() + 1;   // "sessionId": <str> ,
    return n;
}

/// Upload states (Worker.cpp texts) that a revisit within the 10-minute keep sends again: the ones
/// that waited for a session or promised "a later visit". Never the sent / refused / too-large /
/// switched-off ones.
inline bool rearmOnRevisit(std::string const& state) {
    auto starts = [&](char const* p) { return state.rfind(p, 0) == 0; };
    return starts("kept on this computer (no session") || starts("waiting for the session") || starts("not sent: the server does not know")
        || starts("not sent: rate limited") || starts("send failed");
}

}  // namespace gprl::analyzer::rules
