#include "gameplay_fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../geometry_hash.hpp"

namespace gprl::identity {

namespace {

constexpr uint64_t kFnvBasis = 14695981039346656037ull;

void mixInt(uint64_t& h, int64_t v) {
    unsigned char bytes[8];
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
    h = fnv1a64(bytes, 8, h);
}

void mixU64(uint64_t& h, uint64_t u) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
    h = fnv1a64(bytes, 8, h);
}

}  // namespace

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::string hex16(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

int64_t quantize(double v, double quantum) {
    if (!std::isfinite(v) || !(quantum > 0.0)) return 0;
    double q = v / quantum;
    if (!std::isfinite(q)) return 0;
    return static_cast<int64_t>(std::llround(q));
}

bool hashedForIdentity(SimObject const& o) {
    return o.kind != ObjKind::StartPos && o.objectId != 31;
}

uint64_t tupleHash(SimObject const& o, double xFrom, IdentityConfig const& cfg) {
    int64_t t[21] = {
        static_cast<int64_t>(o.objectId),
        static_cast<int64_t>(o.gdType),
        static_cast<int64_t>(o.kind),
        quantize(static_cast<double>(o.rx) - xFrom, cfg.positionQuantum),
        quantize(static_cast<double>(o.ry), cfg.positionQuantum),
        quantize(static_cast<double>(o.rw), cfg.positionQuantum),
        quantize(static_cast<double>(o.rh), cfg.positionQuantum),
        quantize(static_cast<double>(o.rotation), 1.0),
        quantize(static_cast<double>(o.scaleX) * 100.0, 1.0),
        quantize(static_cast<double>(o.scaleY) * 100.0, 1.0),
        o.flipX ? 1 : 0,
        o.flipY ? 1 : 0,
        static_cast<int64_t>(o.slope),
        static_cast<int64_t>(o.mode),
        static_cast<int64_t>(o.speed),
        o.flag ? 1 : 0,
        static_cast<int64_t>(o.orb),
        static_cast<int64_t>(o.pad),
        o.multiActivate ? 1 : 0,
        o.hidden ? 1 : 0,
        static_cast<int64_t>(o.groupsHash),
    };
    uint64_t h = kFnvBasis;
    for (int64_t v : t) mixInt(h, v);
    return h;
}

uint64_t hashSortedTuples(std::vector<uint64_t>& hashes) {
    std::sort(hashes.begin(), hashes.end());
    uint64_t h = kFnvBasis;
    for (uint64_t v : hashes) mixU64(h, v);
    return h;
}

MinHash minhashOf(std::vector<uint64_t> const& tupleHashes) {
    MinHash m{};
    for (int k = 0; k < kMinhashSize; ++k) {
        uint64_t seed = splitmix64(static_cast<uint64_t>(k) + 1u);
        uint32_t best = 0xffffffffu;
        for (uint64_t th : tupleHashes) {
            uint32_t v = static_cast<uint32_t>(splitmix64(th ^ seed) >> 32);
            if (v < best) best = v;
        }
        m[static_cast<size_t>(k)] = best;
    }
    return m;
}

uint64_t gameplayFingerprintValue(World const& world, IdentityConfig const& cfg) {
    std::vector<uint64_t> hashes;
    hashes.reserve(world.objects.size());
    for (auto const& o : world.objects) {
        if (!hashedForIdentity(o)) continue;
        hashes.push_back(tupleHash(o, 0.0, cfg));
    }
    return hashSortedTuples(hashes);
}

std::string gameplayFingerprint(World const& world, IdentityConfig const& cfg) {
    return hex16(gameplayFingerprintValue(world, cfg));
}

}  // namespace gprl::identity
