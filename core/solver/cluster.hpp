#pragma once
// Connected input clusters (docs/TIMING_SOLVER_V2.md §2.7, V2-D4; AUDIT §6): measured, not
// guessed. Two neighbouring inputs i, i+1 of one attempt are connected when shifting i still
// matters when i+1 is applied:
//
//   connectedNext(i) = yes  i+1 lies inside i's look-ahead and a passing shift of i had not
//                           re-joined the real run before t_{i+1} (re-join frame > t_{i+1}, or
//                           survived without re-joining), or a failing shift died downstream
//                    = no   a forced break between them (attempt boundary, gamemode / gravity /
//                           size / dual / teleport portal, a real or would-be death), i+1 beyond the
//                           look-ahead (`gap_beyond_horizon`), or every tested shift of i re-joined
//                           before t_{i+1} and no fail was downstream (i's timing cannot reach i+1)
//                    = unknown  i had no usable local outcomes (dropped, skipped, a miss)
//
// Cluster id `<attemptId>:<attemptInputIndex of the first member>`, index = 1-based position.
// The size is not known when the first results are emitted: each result carries connectedPrev /
// connectedNext and the server rebuilds clusters per attempt from them.
//
// chunkFor: the SA job chunking (§3.3): consecutive candidates of the same cluster, at most
// maxInputsPerJob, span at most maxJobSpanTicks, seeded by the narrowest local window.
//
// PURE C++20; host-tested in tests/cluster_tests.cpp. Every loop is bounded by its input size.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../vocab.hpp"
#include "pass_planner.hpp"

namespace gprl::solver::cluster {

enum class Tri : uint8_t { Unknown, No, Yes };
constexpr char const* name(Tri t) { return t == Tri::Yes ? "y" : t == Tri::No ? "n" : "?"; }

struct ConnectInput {
    bool haveOutcomes = false;                    // i has usable local outcomes (not dropped / skipped / a miss)
    double frame = 0.0;                           // t_i
    double horizonFrame = 0.0;                    // end of i's local look-ahead
    double nextFrame = kNaN;                      // t_{i+1} (NaN = no later input logged)
    bool forcedBreak = false;                     // attempt boundary / portal / death between i and i+1
    std::vector<ShiftOutcome> const* outcomes = nullptr;
};

struct Connect {
    Tri value = Tri::Unknown;
    char const* why = "unknown";
};

inline Connect connectedNext(ConnectInput const& in) {
    if (in.forcedBreak) return {Tri::No, "forced_break"};
    if (std::isnan(in.nextFrame)) return {Tri::No, "no_next_input"};
    if (in.nextFrame > in.horizonFrame + 1e-9) return {Tri::No, "gap_beyond_horizon"};
    if (!in.haveOutcomes || !in.outcomes) return {Tri::Unknown, "no_local_outcomes"};
    bool tested = false;
    for (auto const& o : *in.outcomes) {
        if (o.kind == ShiftKind::NotTested || o.kind == ShiftKind::Invalid) continue;
        tested = true;
        if (o.kind == ShiftKind::Survived) return {Tri::Yes, "not_rejoined"};
        if (o.kind == ShiftKind::Resynced) {
            double rejoin = std::isnan(o.rejoinAfterFrames) ? o.deathAfterFrames : o.rejoinAfterFrames;
            if (std::isnan(rejoin) || in.frame + rejoin > in.nextFrame + 1e-9) return {Tri::Yes, "not_rejoined"};
        }
        if (o.kind == ShiftKind::Died && o.laterFixed >= 1) return {Tri::Yes, "downstream_fail"};
    }
    if (!tested) return {Tri::Unknown, "no_tested_shift"};
    return {Tri::No, "rejoined_before_next"};
}

struct ClusterRef {
    std::string id;           // "<attemptId>:<attemptInputIndex of the first member>"
    int index = 1;            // 1-based position in the cluster
    Tri connectedPrev = Tri::Unknown;
    Tri connectedNext = Tri::Unknown;
};

/// Per-attempt connectivity of the engine's input log (attemptInputIndex, 1-based). Cleared at
/// every attempt start; grows by one entry per logged input.
class ClusterTracker {
public:
    void reset() { m_next.clear(); }
    void setConnectedNext(int i, Tri v) {
        if (i < 1 || i > 1000000) return;
        if (static_cast<int>(m_next.size()) < i) m_next.resize(static_cast<size_t>(i), Tri::Unknown);
        m_next[static_cast<size_t>(i - 1)] = v;
    }
    Tri connectedNext(int i) const {
        if (i < 1 || i > static_cast<int>(m_next.size())) return Tri::Unknown;
        return m_next[static_cast<size_t>(i - 1)];
    }
    Tri connectedPrev(int i) const { return i <= 1 ? Tri::No : connectedNext(i - 1); }

    /// Cluster of input i: walks back over `yes` links (index strictly decreases: bounded by i).
    ClusterRef ref(std::string const& attemptId, int i) const {
        ClusterRef r;
        int start = std::max(1, i);
        int idx = 1;
        while (start > 1 && connectedNext(start - 1) == Tri::Yes) {
            --start;
            ++idx;
        }
        r.id = attemptId + ":" + std::to_string(start);
        r.index = idx;
        r.connectedPrev = connectedPrev(i);
        r.connectedNext = connectedNext(i);
        return r;
    }

private:
    std::vector<Tri> m_next;
};

/// `cluster.id` shape (schema.ts TIMING_CLUSTER_ID_PATTERN): ^[A-Za-z0-9_-]{1,64}:[0-9]{1,6}$.
inline bool clusterIdOk(std::string const& id) {
    auto colon = id.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 64) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    size_t digits = id.size() - colon - 1;
    if (digits < 1 || digits > 6) return false;
    for (size_t i = colon + 1; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return true;
}

// ---- portals (forced cluster breaks, transition inputs; §2.7, §2.10) ----

/// GD 2.2081 portal object ids the real player's GJBaseGameLayer::playerTouchedTrigger reports.
enum class PortalKind : uint8_t { Other, Gamemode, Gravity, Size, Dual, Teleport, Speed, Mirror };
constexpr PortalKind portalKind(int objectId) {
    switch (objectId) {
        case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1933: return PortalKind::Gamemode;
        case 10: case 11: case 2926: return PortalKind::Gravity;
        case 99: case 101: return PortalKind::Size;
        case 286: case 287: return PortalKind::Dual;
        case 747: case 2902: return PortalKind::Teleport;
        case 200: case 201: case 202: case 203: case 1334: return PortalKind::Speed;
        case 45: case 46: return PortalKind::Mirror;
        default: return PortalKind::Other;
    }
}
/// A gamemode / gravity / size / dual / teleport portal between two inputs is a forced break:
/// the second is never moved as the first one's follower. Speed and mirror portals are not.
constexpr bool breaksCluster(PortalKind k) {
    return k == PortalKind::Gamemode || k == PortalKind::Gravity || k == PortalKind::Size || k == PortalKind::Dual || k == PortalKind::Teleport;
}
/// A gamemode / gravity / size portal within `transitionTicks` after an input makes it a
/// transition input (the level analysis labels its row with the mode after the portal).
constexpr bool transitionPortal(PortalKind k) { return k == PortalKind::Gamemode || k == PortalKind::Gravity || k == PortalKind::Size; }
/// The mode a gamemode portal switches to (`timing_result.gamemodeAfter`); false for other ids.
constexpr bool portalGamemode(int objectId, Gamemode& out) {
    switch (objectId) {
        case 12: out = Gamemode::Cube; return true;
        case 13: out = Gamemode::Ship; return true;
        case 47: out = Gamemode::Ball; return true;
        case 111: out = Gamemode::Ufo; return true;
        case 660: out = Gamemode::Wave; return true;
        case 745: out = Gamemode::Robot; return true;
        case 1331: out = Gamemode::Spider; return true;
        case 1933: out = Gamemode::Swing; return true;
        default: return false;
    }
}

// ---- SA job chunking (§3.3) ----

struct ChunkCandidate {
    int index = 0;            // attemptInputIndex
    double frame = 0.0;
    double widthFrames = 0.0; // local window width (the priority: narrowest first)
    Tri connectedNext = Tri::Unknown;
    int64_t order = 0;        // age (queue order): older first on equal widths
    bool positionMeasured = false;   // Fable D10: its position bucket already has saMaxPerPosition finished SA results this level visit
};

/// Fable review D10 (W10): the SA queue's order. A candidate whose position (x bucket) already has
/// `saMaxPerPosition` finished SA results this level visit ranks behind every unmeasured position -
/// it still runs when nothing else waits, it is never skipped - so on a dense wave the vertices the
/// ring could not hold in one attempt get their turn in the next instead of the same narrow ones
/// again. Inside a rank: the narrowest local window, then the oldest (Opus §3.3).
struct SAQueueRank {
    int measured = 0;          // 0 = unmeasured position (first), 1 = measured
    double narrowest = 0.0;    // local width, ticks
    int64_t age = 0;           // queue order (smaller = older)
    bool before(SAQueueRank const& o) const {
        if (measured != o.measured) return measured < o.measured;
        if (std::fabs(narrowest - o.narrowest) > 1e-9) return narrowest < o.narrowest;
        return age < o.age;
    }
};
inline SAQueueRank saQueueRank(bool positionMeasured, double narrowestLocalTicks, int64_t age) {
    return {positionMeasured ? 1 : 0, narrowestLocalTicks, age};
}

/// The candidate to start next (saQueueRank: unmeasured positions first, then the narrowest local
/// window, then the oldest). -1 when empty. Ties keep the earlier position in `ready` (stable).
inline int pickSeed(std::vector<ChunkCandidate> const& ready) {
    int best = -1;
    for (size_t i = 0; i < ready.size(); ++i) {
        if (best < 0) { best = static_cast<int>(i); continue; }
        auto const& a = ready[i];
        auto const& b = ready[static_cast<size_t>(best)];
        if (saQueueRank(a.positionMeasured, a.widthFrames, a.order).before(saQueueRank(b.positionMeasured, b.widthFrames, b.order))) best = static_cast<int>(i);
    }
    return best;
}

/// Positions (into `ready`, ascending index) of the chunk grown around `seed`: consecutive input
/// indices linked by connectedNext == yes, at most `maxInputs`, first-to-last span at most
/// `maxSpanFrames`. Forward first, then backward. Bounded by maxInputs iterations.
inline std::vector<size_t> chunkFor(std::vector<ChunkCandidate> const& ready, size_t seed, int maxInputs, double maxSpanFrames) {
    std::vector<size_t> out;
    if (seed >= ready.size()) return out;
    auto find = [&](int index) -> int {
        for (size_t i = 0; i < ready.size(); ++i) {
            if (ready[i].index == index) return static_cast<int>(i);
        }
        return -1;
    };
    int lo = static_cast<int>(seed), hi = static_cast<int>(seed);
    out.push_back(seed);
    for (int guard = 0; guard < maxInputs && static_cast<int>(out.size()) < maxInputs; ++guard) {
        bool grew = false;
        // forward: the next input index, linked from the current last member
        int f = find(ready[static_cast<size_t>(hi)].index + 1);
        if (f >= 0 && ready[static_cast<size_t>(hi)].connectedNext == Tri::Yes
            && ready[static_cast<size_t>(f)].frame - ready[static_cast<size_t>(lo)].frame <= maxSpanFrames + 1e-9) {
            hi = f;
            out.push_back(static_cast<size_t>(f));
            grew = true;
        }
        if (static_cast<int>(out.size()) >= maxInputs) break;
        int b = find(ready[static_cast<size_t>(lo)].index - 1);
        if (b >= 0 && ready[static_cast<size_t>(b)].connectedNext == Tri::Yes
            && ready[static_cast<size_t>(hi)].frame - ready[static_cast<size_t>(b)].frame <= maxSpanFrames + 1e-9) {
            lo = b;
            out.push_back(static_cast<size_t>(b));
            grew = true;
        }
        if (!grew) break;
    }
    std::sort(out.begin(), out.end(), [&](size_t a, size_t c) { return ready[a].index < ready[c].index; });
    return out;
}

}  // namespace gprl::solver::cluster
