#include "gameplay_hash.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../geometry_hash.hpp"

namespace gprl::sim {

namespace {

constexpr uint64_t kFnvBasis = 14695981039346656037ull;

void mixInt(uint64_t& h, int64_t v) {
    unsigned char bytes[8];
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
    h = fnv1a64(bytes, 8, h);
}

void mixString(uint64_t& h, std::string const& s) {
    mixInt(h, static_cast<int64_t>(s.size()));
    if (!s.empty()) h = fnv1a64(s.data(), s.size(), h);
}

int64_t q10(double v) { return std::isfinite(v) ? static_cast<int64_t>(std::llround(v * 10.0)) : 0; }
int64_t q100(double v) { return std::isfinite(v) ? static_cast<int64_t>(std::llround(v * 100.0)) : 0; }
int64_t q1(double v) { return std::isfinite(v) ? static_cast<int64_t>(std::llround(v)) : 0; }

/// The canonical tuple of one gameplay object. Position first so the sort order is
/// (round(x*10), round(y*10), objectId, ...): identical objects in another order hash the same.
using Tuple = std::array<int64_t, 24>;

Tuple tupleOf(SimObject const& o) {
    Tuple t{};
    size_t i = 0;
    t[i++] = q10(o.x);
    t[i++] = q10(o.y);
    t[i++] = o.objectId;
    t[i++] = o.gdType;
    t[i++] = static_cast<int64_t>(o.kind);
    t[i++] = q10(o.rx);
    t[i++] = q10(o.ry);
    t[i++] = q10(o.rw);
    t[i++] = q10(o.rh);
    t[i++] = q1(o.rotation);
    t[i++] = q100(o.scaleX);
    t[i++] = q100(o.scaleY);
    t[i++] = o.flipX ? 1 : 0;
    t[i++] = o.flipY ? 1 : 0;
    t[i++] = o.slope;
    t[i++] = static_cast<int64_t>(o.mode);
    t[i++] = static_cast<int64_t>(o.speed);
    t[i++] = o.flag ? 1 : 0;
    t[i++] = static_cast<int64_t>(o.orb);
    t[i++] = static_cast<int64_t>(o.pad);
    t[i++] = o.multiActivate ? 1 : 0;
    t[i++] = o.hidden ? 1 : 0;
    t[i++] = static_cast<int64_t>(o.groupsHash);
    t[i++] = o.groupCount;
    return t;
}

void mixStart(uint64_t& h, StartState const& s) {
    mixInt(h, q10(s.x));
    mixInt(h, q10(s.y));
    mixInt(h, static_cast<int64_t>(s.mode));
    mixInt(h, s.mini ? 1 : 0);
    mixInt(h, s.upsideDown ? 1 : 0);
    mixInt(h, static_cast<int64_t>(s.speed));
    mixInt(h, s.dual ? 1 : 0);
    mixInt(h, s.mirror ? 1 : 0);
    mixInt(h, s.reversed ? 1 : 0);
    mixInt(h, s.platformer ? 1 : 0);
    mixInt(h, s.twoPlayer ? 1 : 0);
}

}  // namespace

uint64_t gameplayHashValue(World const& world, GameplayHashOptions const& options) {
    std::vector<Tuple> tuples;
    tuples.reserve(world.objects.size());
    for (auto const& o : world.objects) {
        if (o.kind == ObjKind::Decoration) continue;   // never gameplay (the extraction keeps them out already)
        tuples.push_back(tupleOf(o));
    }
    std::sort(tuples.begin(), tuples.end());

    uint64_t h = kFnvBasis;
    mixString(h, std::string(kGameplayHashVersion));
    mixInt(h, static_cast<int64_t>(tuples.size()));
    for (auto const& t : tuples)
        for (int64_t v : t) mixInt(h, v);

    mixInt(h, 0x5354);   // "ST": start block marker
    mixStart(h, world.start);

    if (!options.ignoreStartPos) {
        mixInt(h, 0x5350);   // "SP"
        std::vector<StartState> sp = world.startPositions;
        std::stable_sort(sp.begin(), sp.end(), [](StartState const& a, StartState const& b) {
            if (q10(a.x) != q10(b.x)) return q10(a.x) < q10(b.x);
            return q10(a.y) < q10(b.y);
        });
        mixInt(h, static_cast<int64_t>(sp.size()));
        for (auto const& s : sp) mixStart(h, s);
    }
    else {
        mixInt(h, 0x4e53);   // "NS": no StartPos in this hash (keeps the two variants distinct by design)
    }

    mixInt(h, 0x5553);   // "US": unsupported spans
    std::vector<UnsupportedSpan> spans = world.unsupported;
    std::stable_sort(spans.begin(), spans.end(), [](UnsupportedSpan const& a, UnsupportedSpan const& b) {
        if (q10(a.x0) != q10(b.x0)) return q10(a.x0) < q10(b.x0);
        if (q10(a.x1) != q10(b.x1)) return q10(a.x1) < q10(b.x1);
        return a.mechanic < b.mechanic;
    });
    mixInt(h, static_cast<int64_t>(spans.size()));
    for (auto const& s : spans) {
        mixInt(h, q10(s.x0));
        mixInt(h, q10(s.x1));
        mixString(h, s.mechanic);
        mixInt(h, s.objectId);
    }

    mixInt(h, 0x4558);   // "EX": extent
    mixInt(h, q10(world.groundY));
    mixInt(h, q10(world.ceilingY));
    mixInt(h, q10(world.lengthX));
    mixInt(h, q10(world.endX));
    return h;
}

std::string hashHex(uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

std::string gameplayHash(World const& world, GameplayHashOptions const& options) {
    return hashHex(gameplayHashValue(world, options));
}

}  // namespace gprl::sim
