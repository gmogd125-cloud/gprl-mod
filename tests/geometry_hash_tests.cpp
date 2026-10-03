// geometry_hash host tests (docs/SOLVER_DESIGN.md §9.4): order independence, quantisation, the
// 64-object cap, hex format, anchor behaviour at block boundaries, uint32 low bits, decorations
// ignored, range window.
#include "test_util.hpp"

#include "../core/geometry_hash.hpp"

#include <algorithm>
#include <cctype>
#include <random>

using namespace gprl;

namespace {

std::vector<GeometryObject> sample() {
    return {
        {1, 100.0, 105.0, false}, {8, 130.0, 105.0, false}, {1717, 160.0, 135.0, false}, {36, 190.0, 105.0, false},
        {35, 220.0, 150.0, false}, {1, 250.0, 105.0, false}, {8, 280.0, 105.0, false}, {2, 310.0, 120.0, false},
    };
}

void testBasicsAndOrder() {
    SECTION("deterministic, order independent, 16 hex digits, non-zero low bits");
    auto objs = sample();
    auto h1 = geometryHash(objs, 120.0);
    CHECK(h1.objects == 8);
    CHECK(h1.hex.size() == 16);
    for (char c : h1.hex) CHECK(std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)));
    CHECK(h1.low32 != 0);
    CHECK(h1.low32 == static_cast<uint32_t>(h1.value & 0xffffffffull) || (h1.value & 0xffffffffull) == 0);
    std::mt19937 rng(7);
    for (int i = 0; i < 20; ++i) {
        std::shuffle(objs.begin(), objs.end(), rng);
        auto h = geometryHash(objs, 120.0);
        CHECK(h.hex == h1.hex && h.low32 == h1.low32 && h.value == h1.value);
    }
    // a different object id changes it
    auto changed = sample();
    changed[2].objectId = 1718;
    CHECK(geometryHash(changed, 120.0).hex != h1.hex);
    // FNV-1a reference: empty input = offset basis, "a" = 0xaf63dc4c8601ec8c
    CHECK(fnv1a64("", 0) == 14695981039346656037ull);
    CHECK(fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
}

void testQuantisationAndAnchor() {
    SECTION("15-unit quantisation and the 30-unit block anchor");
    auto objs = sample();
    auto base = geometryHash(objs, 120.0);
    // moving an object by less than half a quantum keeps the hash
    auto near = sample();
    near[1].x += 4.0;
    near[1].y -= 5.0;
    CHECK(geometryHash(near, 120.0).hex == base.hex);
    // moving by a whole quantum changes it
    auto far = sample();
    far[1].x += 15.0;
    CHECK(geometryHash(far, 120.0).hex != base.hex);
    // player positions inside the same 30-unit block agree; crossing the block changes the anchor
    CHECK(geometryHash(objs, 125.0).hex == base.hex);
    CHECK(geometryHash(objs, 149.9).hex == base.hex);
    auto next = geometryHash(objs, 150.0);
    // objects at 100 leave the [px - 60, px + 300] window only below 90, so the set is the same and
    // the anchor moved by exactly 30 = 2 quanta -> every dx shifts by 2 -> a different hash
    CHECK(next.objects == 8);
    CHECK(next.hex != base.hex);
    // the same geometry translated by a whole block with the player translated too hashes equal
    auto shifted = sample();
    for (auto& o : shifted) o.x += 30.0;
    CHECK(geometryHash(shifted, 150.0).hex == base.hex);
}

void testRangeCapAndDecoration() {
    SECTION("range window, decoration objects ignored, nearest 64 kept");
    auto objs = sample();
    objs.push_back({9, 50.0, 100.0, false});     // 70 before the player: outside [-60, +300]
    objs.push_back({9, 430.0, 100.0, false});    // 310 after: outside
    objs.push_back({9, 200.0, 100.0, true});     // decoration: ignored
    auto h = geometryHash(objs, 120.0);
    CHECK(h.objects == 8);
    CHECK(h.hex == geometryHash(sample(), 120.0).hex);
    // exactly on the edges counts
    std::vector<GeometryObject> edge = {{1, 60.0, 0.0, false}, {1, 420.0, 0.0, false}};
    CHECK(geometryHash(edge, 120.0).objects == 2);
    // more than 64: the nearest by x survive, farther ones do not influence the hash
    std::vector<GeometryObject> many;
    for (int i = 0; i < 100; ++i) many.push_back({1, 120.0 + i * 2.5, 100.0, false});
    auto capped = geometryHash(many, 120.0);
    CHECK(capped.objects == 64);
    std::vector<GeometryObject> first64(many.begin(), many.begin() + 64);
    CHECK(geometryHash(first64, 120.0).hex == capped.hex);
    many[90].objectId = 999;   // beyond the cap
    CHECK(geometryHash(many, 120.0).hex == capped.hex);
    // nothing in range: empty hash (unknown)
    auto none = geometryHash({{1, 1000.0, 0.0, false}}, 120.0);
    CHECK(none.objects == 0 && none.hex.empty() && none.low32 == 0);
    GeometryHashParams p;
    p.maxObjects = 0;   // no cap
    CHECK(geometryHash(many, 120.0, p).objects == 100);
}

}  // namespace

int main() {
    testBasicsAndOrder();
    testQuantisationAndAnchor();
    testRangeCapAndDecoration();
    return gprl::test::finish("geometry_hash_tests");
}
