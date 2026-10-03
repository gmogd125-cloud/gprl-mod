#include "sections.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

#include "gameplay_fingerprint.hpp"

namespace gprl::identity {

namespace {

constexpr double kCutMergeTolerance = 0.01;

bool unsupportedCut(SimObject const& o) {
    if (o.kind != ObjKind::Unsupported || !o.unsupportedMechanic) return false;
    char const* m = o.unsupportedMechanic;
    return std::strstr(m, "dual") != nullptr || std::strstr(m, "mirror") != nullptr || std::strstr(m, "teleport") != nullptr;
}

/// A deterministic order for the state walk: two portals at the same x apply in (x, id, y) order
/// whatever the extraction's object order was.
bool stateOrder(SimObject const* a, SimObject const* b) {
    return std::tie(a->x, a->objectId, a->y, a->rx) < std::tie(b->x, b->objectId, b->y, b->rx);
}

}  // namespace

float levelEnd(World const& world) {
    if (world.endX > 0.f && std::isfinite(world.endX)) return world.endX;
    if (world.lengthX > 0.f && std::isfinite(world.lengthX)) return world.lengthX;
    float last = 0.f;
    for (auto const& o : world.objects) last = std::max(last, o.right());
    return last > 0.f ? last : 1.f;
}

bool isCutObject(SimObject const& o) {
    switch (o.kind) {
        case ObjKind::GamemodePortal:
        case ObjKind::SizePortal:
        case ObjKind::SpeedChange: return true;
        case ObjKind::Unsupported: return unsupportedCut(o);
        default: return false;
    }
}

std::vector<SectionCut> cutSections(World const& world, IdentityConfig const& cfg) {
    double end = static_cast<double>(levelEnd(world));
    std::vector<double> xs;
    xs.push_back(0.0);
    for (auto const& o : world.objects) {
        if (!isCutObject(o)) continue;
        double x = static_cast<double>(o.x);
        if (!std::isfinite(x) || x <= 0.0 || x >= end) continue;
        xs.push_back(x);
    }
    std::sort(xs.begin(), xs.end());
    std::vector<double> bounds;
    for (double x : xs) {
        if (!bounds.empty() && x - bounds.back() < kCutMergeTolerance) continue;
        bounds.push_back(x);
    }
    if (end - bounds.back() < kCutMergeTolerance) bounds.pop_back();
    bounds.push_back(end);

    double maxLen = cfg.maxSectionLength > 0.f ? static_cast<double>(cfg.maxSectionLength) : 600.0;
    std::vector<SectionCut> cuts;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        double a = bounds[i], b = bounds[i + 1];
        double len = b - a;
        int n = std::max(1, static_cast<int>(std::ceil(len / maxLen - 1e-9)));
        for (int k = 0; k < n; ++k) {
            SectionCut c;
            c.xFrom = static_cast<float>(k == 0 ? a : a + len * k / n);
            c.xTo = static_cast<float>(k + 1 == n ? b : a + len * (k + 1) / n);
            c.forced = k == 0;
            cuts.push_back(c);
        }
    }
    if (cuts.empty()) {
        SectionCut c;
        c.xFrom = 0.f;
        c.xTo = static_cast<float>(end);
        cuts.push_back(c);
    }

    // the state entering each section: every mode / speed / size portal assigned to this or an
    // earlier section, applied in x order from the level start
    std::vector<SimObject const*> portals;
    for (auto const& o : world.objects) {
        if (o.kind == ObjKind::GamemodePortal || o.kind == ObjKind::SpeedChange || o.kind == ObjKind::SizePortal) portals.push_back(&o);
    }
    std::sort(portals.begin(), portals.end(), stateOrder);
    Gamemode mode = world.start.mode;
    Speed speed = world.start.speed;
    bool mini = world.start.mini;
    size_t p = 0;
    for (size_t s = 0; s < cuts.size(); ++s) {
        while (p < portals.size() && sectionIndexForX(cuts, static_cast<double>(portals[p]->x)) <= static_cast<int>(s)) {
            auto const& o = *portals[p];
            if (o.kind == ObjKind::GamemodePortal) mode = o.mode;
            else if (o.kind == ObjKind::SpeedChange) speed = o.speed;
            else mini = o.flag;
            ++p;
        }
        cuts[s].gamemode = mode;
        cuts[s].speed = speed;
        cuts[s].mini = mini;
    }
    return cuts;
}

int sectionIndexForX(std::vector<SectionCut> const& cuts, double x) {
    if (cuts.empty()) return 0;
    if (!std::isfinite(x)) return 0;
    int lo = 0, hi = static_cast<int>(cuts.size()) - 1;
    // last index whose xFrom <= x
    int best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (static_cast<double>(cuts[static_cast<size_t>(mid)].xFrom) <= x) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

std::vector<int> assignSections(World const& world, std::vector<SectionCut> const& cuts) {
    std::vector<int> out(world.objects.size(), -1);
    for (size_t i = 0; i < world.objects.size(); ++i) {
        auto const& o = world.objects[i];
        if (!hashedForIdentity(o)) continue;
        out[i] = sectionIndexForX(cuts, static_cast<double>(o.x));
    }
    return out;
}

std::vector<SectionIdentity> sectionsFor(World const& world, std::vector<SectionCut> const& cuts, std::vector<int> const& assignment,
                                         IdentityConfig const& cfg) {
    std::vector<std::vector<uint64_t>> tuples(cuts.size());
    for (size_t i = 0; i < world.objects.size() && i < assignment.size(); ++i) {
        int s = assignment[i];
        if (s < 0 || s >= static_cast<int>(cuts.size())) continue;
        tuples[static_cast<size_t>(s)].push_back(tupleHash(world.objects[i], static_cast<double>(cuts[static_cast<size_t>(s)].xFrom), cfg));
    }
    double pctEnd = static_cast<double>(levelEnd(world));
    std::vector<SectionIdentity> out;
    out.reserve(cuts.size());
    for (size_t s = 0; s < cuts.size(); ++s) {
        SectionIdentity sec;
        sec.index = static_cast<int>(s);
        sec.xFrom = cuts[s].xFrom;
        sec.xTo = cuts[s].xTo;
        sec.pctFrom = pctEnd > 0.0 ? static_cast<double>(cuts[s].xFrom) / pctEnd * 100.0 : 0.0;
        sec.pctTo = pctEnd > 0.0 ? static_cast<double>(cuts[s].xTo) / pctEnd * 100.0 : 0.0;
        sec.objects = static_cast<int>(tuples[s].size());
        sec.minhash = minhashOf(tuples[s]);
        sec.hash = hex16(hashSortedTuples(tuples[s]));
        sec.gamemode = cuts[s].gamemode;
        sec.speed = cuts[s].speed;
        sec.mini = cuts[s].mini;
        out.push_back(std::move(sec));
    }
    return out;
}

std::vector<SectionIdentity> sections(World const& world, IdentityConfig const& cfg) {
    auto cuts = cutSections(world, cfg);
    auto assignment = assignSections(world, cuts);
    return sectionsFor(world, cuts, assignment, cfg);
}

}  // namespace gprl::identity
