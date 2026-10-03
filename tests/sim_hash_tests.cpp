// gprl-gameplay-hash/1 (core/sim/gameplay_hash): order independence, decoration ignored, a moved
// gameplay object changes it, StartPos is gameplay (and the ignoreStartPos variant for the
// level-family stream), hex format, start state / spans / extent sensitivity.
#include "test_util.hpp"

#include <algorithm>
#include <cctype>
#include <random>

#include "../core/sim/gameplay_hash.hpp"
#include "sim_analysis_world.hpp"

using namespace gprl::sim;

namespace {

World sample() {
    World w = simworld::base(1500.f);
    w.objects.push_back(simworld::spike(300.f));
    w.objects.push_back(simworld::spike(330.f));
    w.objects.push_back(simworld::block(450.f, 0.f));
    w.objects.push_back(simworld::block(450.f, 30.f));
    w.objects.push_back(simworld::orb(600.f, 75.f));
    w.objects.push_back(simworld::portal(800.f, Gamemode::Ship));
    w.objects.push_back(simworld::speedChange(1000.f, Speed::Double));
    simworld::finalize(w);
    return w;
}

void testFormatAndOrder() {
    SECTION("16 lowercase hex digits; the same objects in any order hash the same");
    World w = sample();
    std::string h = gameplayHash(w);
    CHECK(h.size() == 16);
    for (char c : h) CHECK(std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)));
    CHECK(hashHex(gameplayHashValue(w)) == h);
    std::mt19937 rng(11);
    for (int i = 0; i < 10; ++i) {
        World s = w;
        std::shuffle(s.objects.begin(), s.objects.end(), rng);
        CHECK(gameplayHash(s) == h);
    }
    SECTION("unique ids, names and metadata never enter");
    World ids = w;
    for (auto& o : ids.objects) o.uniqueId += 1000;
    ids.levelHash = std::string(64, 'b');
    ids.gdLevelId = 99;
    ids.decorationObjects = 500;
    CHECK(gameplayHash(ids) == h);
}

void testDecorationAndMoves() {
    SECTION("decoration added = same hash; one spike moved 1 unit = different");
    World w = sample();
    std::string h = gameplayHash(w);
    World deco = w;
    deco.objects.push_back(simworld::decoration(320.f, 60.f));
    deco.objects.push_back(simworld::decoration(700.f, 90.f));
    deco.decoration.objects = 2;
    CHECK(gameplayHash(deco) == h);
    World moved = w;
    for (auto& o : moved.objects)
        if (o.kind == ObjKind::Hazard) {
            o.x += 1.f;
            o.rx += 1.f;
            break;
        }
    CHECK(gameplayHash(moved) != h);
    SECTION("sub-quantum jitter (< 0.05 units) keeps the hash");
    World jitter = w;
    jitter.objects[0].x += 0.02f;
    jitter.objects[0].rx += 0.02f;
    CHECK(gameplayHash(jitter) == h);
    SECTION("gameplay settings of an object change it: portal target, speed value, orb kind, hidden, groups");
    World p = w;
    for (auto& o : p.objects)
        if (o.kind == ObjKind::GamemodePortal) o.mode = Gamemode::Ufo;
    CHECK(gameplayHash(p) != h);
    World s = w;
    for (auto& o : s.objects)
        if (o.kind == ObjKind::SpeedChange) o.speed = Speed::Triple;
    CHECK(gameplayHash(s) != h);
    World o2 = w;
    for (auto& o : o2.objects)
        if (o.kind == ObjKind::Orb) o.orb = OrbKind::Pink;
    CHECK(gameplayHash(o2) != h);
    World hid = w;
    hid.objects[0].hidden = true;
    CHECK(gameplayHash(hid) != h);
    World grp = w;
    grp.objects[0].groupsHash = 12345;
    CHECK(gameplayHash(grp) != h);
}

void testStartPos() {
    SECTION("a StartPos is gameplay for the simulator: added = different; ignoreStartPos = same as without");
    World w = sample();
    std::string h = gameplayHash(w);
    World sp = w;
    StartState s;
    s.x = 700.f;
    s.y = 15.f;
    s.mode = Gamemode::Cube;
    sp.startPositions.push_back(s);
    CHECK(gameplayHash(sp) != h);
    GameplayHashOptions ignore;
    ignore.ignoreStartPos = true;
    CHECK(gameplayHash(sp, ignore) == gameplayHash(w, ignore));
    CHECK(gameplayHash(w, ignore) != h);   // the two variants are distinct by design
    SECTION("start state, unsupported spans and extent are part of the gameplay");
    World st = w;
    st.start.mode = Gamemode::Ship;
    CHECK(gameplayHash(st) != h);
    World un = w;
    simworld::addUnsupported(un, 500.f, 800.f, "move_trigger");
    CHECK(gameplayHash(un) != h);
    World ex = w;
    ex.endX += 30.f;
    CHECK(gameplayHash(ex) != h);
    SECTION("an empty world hashes deterministically");
    World e = simworld::emptyWorld();
    CHECK(gameplayHash(e) == gameplayHash(simworld::emptyWorld()));
    CHECK(gameplayHash(e) != h);
}

}  // namespace

int main() {
    testFormatAndOrder();
    testDecorationAndMoves();
    testStartPos();
    return gprl::test::finish("sim_hash_tests");
}
