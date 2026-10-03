#pragma once
// Geometry hash of the objects around an input (docs/SOLVER_DESIGN.md §3.6, SPEC §12 pattern
// identity): the "which obstacle" part of a timing fingerprint, so two timings at the same place
// cluster and 1000 attempts at one orb are not 1000 independent observations.
//
//   objects   every non-decoration level object with x in [px - rangeBefore, px + rangeAfter],
//             at most `maxObjects` nearest by x
//   anchor    ax = floor(px / blockSize) * blockSize (the 30-unit block the player is in), so the
//             hash does not change with sub-block progress inside the same block
//   tuples    (objectId, round((x - ax) / quantum), round(y / quantum)), sorted
//   hash      FNV-1a 64 over the tuple stream (each integer as 8 little-endian bytes, object count
//             first); hex = 16 lowercase hex digits; low32 = the low 32 bits for
//             PlayerStateSnapshot.geometryHash (uint32, 0 = unknown -> never 0 for a real hash)
//
// Pure C++20 (host-tested in tests/geometry_hash_tests.cpp). The game side (src/solver) only
// collects the objects.
#include <cstdint>
#include <string>
#include <vector>

namespace gprl {

struct GeometryObject {
    int objectId = 0;
    double x = 0.0;
    double y = 0.0;
    bool decoration = false;   // GameObject::m_objectType == GameObjectType::Decoration (7)
};

struct GeometryHashParams {
    double rangeBefore = 60.0;
    double rangeAfter = 300.0;
    int maxObjects = 64;
    double blockSize = 30.0;
    double quantum = 15.0;
};

struct GeometryHash {
    uint64_t value = 0;
    std::string hex;     // 16 lowercase hex digits ("" when no object qualified)
    uint32_t low32 = 0;  // never 0 when `objects` > 0
    int objects = 0;     // objects that went into the hash
};

/// FNV-1a 64 over a byte range (offset basis 14695981039346656037, prime 1099511628211).
uint64_t fnv1a64(void const* data, size_t size, uint64_t seed = 14695981039346656037ull);

/// Objects in any order; `playerX` = the player's x at the input.
GeometryHash geometryHash(std::vector<GeometryObject> const& objects, double playerX, GeometryHashParams const& params = {});

}  // namespace gprl
