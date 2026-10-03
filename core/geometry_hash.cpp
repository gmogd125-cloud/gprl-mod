#include "geometry_hash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace gprl {

uint64_t fnv1a64(void const* data, size_t size, uint64_t seed) {
    uint64_t h = seed;
    auto const* p = static_cast<unsigned char const*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

namespace {

void mixInt(uint64_t& h, int64_t v) {
    unsigned char bytes[8];
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
    h = fnv1a64(bytes, 8, h);
}

}  // namespace

GeometryHash geometryHash(std::vector<GeometryObject> const& objects, double playerX, GeometryHashParams const& params) {
    GeometryHash out;
    if (!std::isfinite(playerX) || params.quantum <= 0.0 || params.blockSize <= 0.0) return out;
    struct Cand {
        double dist;
        double x, y;
        int id;
    };
    std::vector<Cand> cands;
    for (auto const& o : objects) {
        if (o.decoration) continue;
        if (!std::isfinite(o.x) || !std::isfinite(o.y)) continue;
        if (o.x < playerX - params.rangeBefore || o.x > playerX + params.rangeAfter) continue;
        cands.push_back({std::fabs(o.x - playerX), o.x, o.y, o.objectId});
    }
    if (cands.empty()) return out;
    // nearest by x first; ties broken by (x, y, id) so the selection is order independent
    std::sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b) {
        return std::tie(a.dist, a.x, a.y, a.id) < std::tie(b.dist, b.x, b.y, b.id);
    });
    if (params.maxObjects > 0 && cands.size() > static_cast<size_t>(params.maxObjects)) cands.resize(static_cast<size_t>(params.maxObjects));
    double ax = std::floor(playerX / params.blockSize) * params.blockSize;
    std::vector<std::tuple<int64_t, int64_t, int64_t>> tuples;
    tuples.reserve(cands.size());
    for (auto const& c : cands) {
        int64_t dx = static_cast<int64_t>(std::llround((c.x - ax) / params.quantum));
        int64_t dy = static_cast<int64_t>(std::llround(c.y / params.quantum));
        tuples.emplace_back(c.id, dx, dy);
    }
    std::sort(tuples.begin(), tuples.end());
    uint64_t h = 14695981039346656037ull;
    mixInt(h, static_cast<int64_t>(tuples.size()));
    for (auto const& [id, dx, dy] : tuples) {
        mixInt(h, id);
        mixInt(h, dx);
        mixInt(h, dy);
    }
    out.value = h;
    out.objects = static_cast<int>(tuples.size());
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    out.hex = buf;
    out.low32 = static_cast<uint32_t>(h & 0xffffffffull);
    if (out.low32 == 0) out.low32 = static_cast<uint32_t>((h >> 32) | 1u);   // 0 means "unknown" in the snapshot
    return out;
}

}  // namespace gprl
