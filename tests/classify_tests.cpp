// core/classify host tests (SPEC §45 "input classification" and "gamemode detection"): the rules
// src/Tracker.cpp applies to what the game reports, over plain values, table-driven.
//   - all 8 gamemodes from the PlayerObject flag tuple (+ precedence when several flags are set)
//   - m_playerSpeed -> Speed with the GD 2.2 constants 0.7 / 0.9 / 1.1 / 1.3 / 1.6 and every
//     class boundary (exact boundary, just below, just above, float-stored GD values)
//   - handleButton ints -> player 1/2, jump/left/right, press/release; unknown buttons dropped
//   - sub-tick clock: whole ticks, GD half ticks, Click Between Frames splits, TPS bypass, clamps
//   - would-be death rule, integrity summary, environment-change rule (mid-session re-emit)
//   - the cross-package goldens tests/fixtures/classification/*.json (gamemode, speed, input,
//     death, sub-tick): every case must be reproduced by core/classify (read-only here; the
//     TypeScript side re-checks the same files in tests/fixture-checks/classification.mjs)
// argv[1] = repository root (D:\GPRL).
#include "test_util.hpp"

#include "../core/classify.hpp"
#include "../core/json.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::classify;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

std::string g_root;

json::Value fixture(char const* rel) {
    std::string path = g_root + "/tests/fixtures/" + rel;
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    json::Value v;
    json::ParseError pe;
    CHECK_MSG(json::parse(text, v, &pe), path + ": " + pe.message);
    return v;
}

// ---- gamemodes ----

void testGamemodes() {
    SECTION("gamemode from the PlayerObject flag tuple: all 8 gamemodes");
    struct Row {
        ModeFlags flags;
        Gamemode expected;
        char const* wire;
    };
    auto only = [](int which) {
        ModeFlags f;
        switch (which) {
            case 1: f.ship = true; break;
            case 2: f.ball = true; break;
            case 3: f.ufo = true; break;
            case 4: f.wave = true; break;
            case 5: f.robot = true; break;
            case 6: f.spider = true; break;
            case 7: f.swing = true; break;
            default: break;
        }
        return f;
    };
    Row const rows[] = {
        {only(0), Gamemode::Cube, "cube"},     {only(1), Gamemode::Ship, "ship"},   {only(2), Gamemode::Ball, "ball"},
        {only(3), Gamemode::Ufo, "ufo"},       {only(4), Gamemode::Wave, "wave"},   {only(5), Gamemode::Robot, "robot"},
        {only(6), Gamemode::Spider, "spider"}, {only(7), Gamemode::Swing, "swing"},
    };
    int seen = 0;
    for (auto const& r : rows) {
        Gamemode g = gamemodeFromFlags(r.flags);
        CHECK_MSG(g == r.expected, std::string("expected ") + r.wire);
        CHECK_MSG(name(g) == r.wire, std::string("wire name ") + r.wire);   // = shared GAMEMODES
        ++seen;
    }
    CHECK(seen == kGamemodeCount);

    SECTION("gamemode precedence when GD reports several flags (mid-toggle)");
    struct Pair {
        int a, b;
        Gamemode expected;
    };
    Pair const pairs[] = {
        {1, 2, Gamemode::Ship}, {2, 3, Gamemode::Ball},   {3, 4, Gamemode::Ufo},   {4, 5, Gamemode::Wave},
        {5, 6, Gamemode::Robot}, {6, 7, Gamemode::Spider}, {1, 7, Gamemode::Ship}, {4, 7, Gamemode::Wave},
    };
    for (auto const& p : pairs) {
        ModeFlags f = only(p.a);
        ModeFlags g = only(p.b);
        f.ship |= g.ship; f.ball |= g.ball; f.ufo |= g.ufo; f.wave |= g.wave; f.robot |= g.robot; f.spider |= g.spider; f.swing |= g.swing;
        CHECK_MSG(gamemodeFromFlags(f) == p.expected, "flags " + std::to_string(p.a) + "+" + std::to_string(p.b));
    }
    ModeFlags all{true, true, true, true, true, true, true};
    CHECK(gamemodeFromFlags(all) == Gamemode::Ship);
}

// ---- speeds ----

void testSpeeds() {
    SECTION("speed from m_playerSpeed: the GD 2.2 constants map to their class");
    struct Row {
        double v;
        Speed expected;
    };
    Row const constants[] = {
        {0.7, Speed::Slow}, {0.9, Speed::Normal}, {1.1, Speed::Fast}, {1.3, Speed::Faster}, {1.6, Speed::Fastest},
        // PlayerObject stores m_playerSpeed as a float: the widened float values classify identically
        {static_cast<double>(0.7f), Speed::Slow},   {static_cast<double>(0.9f), Speed::Normal},  {static_cast<double>(1.1f), Speed::Fast},
        {static_cast<double>(1.3f), Speed::Faster}, {static_cast<double>(1.6f), Speed::Fastest},
    };
    for (auto const& r : constants) CHECK_MSG(speedFromMultiplier(r.v) == r.expected, "v=" + std::to_string(r.v));
    for (int i = 0; i < kSpeedCount; ++i) {
        Speed s = static_cast<Speed>(i);
        CHECK_MSG(speedFromMultiplier(multiplierOf(s)) == s, std::string(name(s)));
    }
    CHECK(kParams.speedSlow == 0.7 && kParams.speedNormal == 0.9 && kParams.speedFast == 1.1 && kParams.speedFaster == 1.3 &&
          kParams.speedFastest == 1.6);

    SECTION("speed class boundaries: at the boundary = faster class, just below = slower");
    struct Boundary {
        double at;
        Speed below;
        Speed above;
        double lowConstant;
        double highConstant;
    };
    Boundary const boundaries[] = {
        {kParams.boundarySlowNormal, Speed::Slow, Speed::Normal, kParams.speedSlow, kParams.speedNormal},
        {kParams.boundaryNormalFast, Speed::Normal, Speed::Fast, kParams.speedNormal, kParams.speedFast},
        {kParams.boundaryFastFaster, Speed::Fast, Speed::Faster, kParams.speedFast, kParams.speedFaster},
        {kParams.boundaryFasterFastest, Speed::Faster, Speed::Fastest, kParams.speedFaster, kParams.speedFastest},
    };
    for (auto const& b : boundaries) {
        std::string at = "boundary " + std::to_string(b.at);
        CHECK_MSG(speedFromMultiplier(b.at) == b.above, at);
        CHECK_MSG(speedFromMultiplier(std::nextafter(b.at, 0.0)) == b.below, at + " - 1 ulp");
        CHECK_MSG(speedFromMultiplier(b.at - 1e-6) == b.below, at + " - 1e-6");
        CHECK_MSG(speedFromMultiplier(b.at + 1e-6) == b.above, at + " + 1e-6");
        // each boundary is the midpoint of the neighbouring GD constants
        CHECK_MSG(b.lowConstant < b.at && b.at < b.highConstant, at);
        CHECK_NEAR(b.at, (b.lowConstant + b.highConstant) / 2.0, 1e-12);
    }
    CHECK(kParams.boundarySlowNormal == 0.8 && kParams.boundaryNormalFast == 1.0 && kParams.boundaryFastFaster == 1.2 &&
          kParams.boundaryFasterFastest == 1.45);

    SECTION("speed outside the GD range and junk values");
    struct Row2 {
        double v;
        Speed expected;
    };
    Row2 const edges[] = {
        {0.1, Speed::Slow}, {3.0, Speed::Fastest}, {100.0, Speed::Fastest},
        // never produced by GD: below the first boundary is Slow, above the last is Fastest
        {0.0, Speed::Slow}, {-0.7, Speed::Slow}, {-kInf, Speed::Slow}, {kInf, Speed::Fastest},
        // NaN has no order: Normal (the vocabulary default), not Fastest by fall-through
        {kNaN, Speed::Normal},
    };
    for (auto const& r : edges) CHECK_MSG(speedFromMultiplier(r.v) == r.expected, "v=" + std::to_string(r.v));

    SECTION("speed boundaries come from the params (one place)");
    ClassifyParams p;
    p.boundarySlowNormal = 0.75;
    CHECK(speedFromMultiplier(0.78, p) == Speed::Normal);
    CHECK(speedFromMultiplier(0.78) == Speed::Slow);
}

void testMini() {
    SECTION("mini from m_vehicleSize");
    struct Row {
        double size;
        bool mini;
    };
    Row const rows[] = {{0.6, true}, {static_cast<double>(0.6f), true}, {1.0, false}, {0.9, false}, {0.8999, true}, {1.2, false}};
    for (auto const& r : rows) CHECK_MSG(isMini(r.size) == r.mini, "size " + std::to_string(r.size));
}

// ---- inputs ----

void testButtons() {
    SECTION("handleButton(down, button, isPlayer1): player 1/2 x jump/left/right x press/release");
    struct Row {
        bool down;
        int gd;
        bool isPlayer1;
        int player;
        Button button;
        InputKind kind;
        char const* wire;
    };
    Row const rows[] = {
        {true, 1, true, 1, Button::Jump, InputKind::Press, "jump"},     {false, 1, true, 1, Button::Jump, InputKind::Release, "jump"},
        {true, 2, true, 1, Button::Left, InputKind::Press, "left"},     {false, 2, true, 1, Button::Left, InputKind::Release, "left"},
        {true, 3, true, 1, Button::Right, InputKind::Press, "right"},   {false, 3, true, 1, Button::Right, InputKind::Release, "right"},
        {true, 1, false, 2, Button::Jump, InputKind::Press, "jump"},    {false, 1, false, 2, Button::Jump, InputKind::Release, "jump"},
        {true, 2, false, 2, Button::Left, InputKind::Press, "left"},    {false, 2, false, 2, Button::Left, InputKind::Release, "left"},
        {true, 3, false, 2, Button::Right, InputKind::Press, "right"},  {false, 3, false, 2, Button::Right, InputKind::Release, "right"},
    };
    for (auto const& r : rows) {
        auto in = classifyButton(r.down, r.gd, r.isPlayer1);
        std::string what = std::string(r.wire) + (r.down ? " down" : " up") + (r.isPlayer1 ? " p1" : " p2");
        CHECK_MSG(in.has_value(), what);
        if (!in) continue;
        CHECK_MSG(in->player == r.player, what);
        CHECK_MSG(in->button == r.button && name(in->button) == r.wire, what);
        CHECK_MSG(in->kind == r.kind && in->down == r.down, what);
        CHECK_MSG(name(in->kind) == (r.down ? "press" : "release"), what);
    }
    SECTION("buttons GPRL does not record are dropped");
    for (int gd : {0, -1, 4, 5, 255, 1000}) {
        CHECK_MSG(!classifyButton(true, gd, true).has_value(), "button " + std::to_string(gd));
        CHECK_MSG(!classifyButton(false, gd, false).has_value(), "button " + std::to_string(gd));
    }
}

// ---- sub-tick clock ----

/// Feeds `deltas` (PlayerObject::update dt values of one tick) and returns the fraction an input
/// would read BEFORE each delta (GD processes the step's inputs before its update) plus the one
/// after the last delta.
std::vector<double> fractionsThrough(SubTickClock& clock, std::vector<double> const& deltas) {
    std::vector<double> out;
    for (double d : deltas) {
        out.push_back(clock.fraction());
        clock.onPlayerUpdate(d);
    }
    out.push_back(clock.fraction());
    return out;
}

void testTickLength() {
    SECTION("tick length in PlayerObject::update units for the TPS in effect");
    struct Row {
        bool bypass;
        double tps;
        double expected;
    };
    Row const rows[] = {
        {false, 240.0, 0.25}, {false, 480.0, 0.25},   // no bypass: GD's 240 TPS regardless of the value
        {true, 240.0, 0.25},  {true, 360.0, 60.0 / 360.0}, {true, 120.0, 0.5}, {true, 1000.0, 0.06},
        {true, 1.0, 0.25},    {true, 0.0, 0.25},      {true, -60.0, 0.25}, {true, kNaN, 0.25}, {true, kInf, 0.25},
    };
    for (auto const& r : rows) {
        CHECK_MSG(std::fabs(tickDtFor(r.bypass, r.tps) - r.expected) < 1e-12,
                  "bypass " + std::to_string(r.bypass) + " tps " + std::to_string(r.tps) + " -> " + std::to_string(tickDtFor(r.bypass, r.tps)));
    }
}

void testSubTickClock() {
    SECTION("sub-tick: whole 240 TPS ticks always read 0");
    SubTickClock clock;
    CHECK(clock.tickDt() == 0.25);
    for (int i = 0; i < 20; ++i) {
        CHECK(clock.fraction() == 0.0);
        clock.onPlayerUpdate(static_cast<double>(0.25f));
    }
    CHECK(clock.fraction() == 0.0);

    SECTION("sub-tick: GD half ticks (input in the second half reads 0.5) and CBF splits");
    struct Row {
        char const* what;
        std::vector<double> deltas;
        std::vector<double> expected;   // fraction before each delta, then after the last
    };
    Row const rows[] = {
        {"GD half tick", {0.125, 0.125}, {0.0, 0.5, 0.0}},
        {"CBF 40/60 split", {0.1, 0.15}, {0.0, 0.4, 0.0}},
        {"CBF three splits", {0.0625, 0.0625, 0.125}, {0.0, 0.25, 0.5, 0.0}},
        {"CBF early + late click", {0.03, 0.2, 0.02}, {0.0, 0.12, 0.92, 0.0}},
        {"CBF split stored as floats", {static_cast<double>(0.1f), static_cast<double>(0.15f)}, {0.0, static_cast<double>(0.1f) / 0.25, 0.0}},
        {"CBF tiny first slice", {0.0025, 0.2475}, {0.0, 0.01, 0.0}},
        {"two ticks, second split", {0.25, 0.05, 0.2}, {0.0, 0.0, 0.2, 0.0}},
    };
    for (auto const& r : rows) {
        SubTickClock c;
        auto got = fractionsThrough(c, r.deltas);
        CHECK_MSG(got.size() == r.expected.size(), r.what);
        for (size_t i = 0; i < got.size() && i < r.expected.size(); ++i) {
            CHECK_MSG(std::fabs(got[i] - r.expected[i]) < 1e-9,
                      std::string(r.what) + " step " + std::to_string(i) + ": got " + std::to_string(got[i]) + " expected " + std::to_string(r.expected[i]));
        }
    }

    SECTION("sub-tick: the tick closes within the float epsilon, the fraction stays < 1");
    {
        SubTickClock c;
        c.onPlayerUpdate(0.25 - kParams.tickBoundaryEpsilon);   // within epsilon: tick boundary
        CHECK(c.fraction() == 0.0);
        c.onPlayerUpdate(0.2498);                               // just outside: 0.9992 clamps to 0.999
        CHECK(c.fraction() == kParams.maxSubTick);
        CHECK(c.fraction() < 1.0);
    }

    SECTION("sub-tick: Eclipse TPS bypass changes the tick length");
    {
        SubTickClock c;
        c.reset(tickDtFor(true, 360.0));
        auto got = fractionsThrough(c, {60.0 / 360.0 / 2.0, 60.0 / 360.0 / 2.0, static_cast<double>(static_cast<float>(60.0 / 360.0))});
        CHECK(std::fabs(got[1] - 0.5) < 1e-9);
        CHECK(got[2] == 0.0 && got[3] == 0.0);
    }

    SECTION("sub-tick: reset clears the accumulator; junk tick lengths and deltas are ignored");
    {
        SubTickClock c;
        c.onPlayerUpdate(0.1);
        CHECK(c.fraction() > 0.0);
        c.reset(0.25);
        CHECK(c.fraction() == 0.0 && c.elapsed() == 0.0);
        for (double bad : {0.0, -1.0, kNaN, kInf}) {
            c.reset(bad);
            CHECK_MSG(c.tickDt() == 0.25, "reset(" + std::to_string(bad) + ")");
        }
        for (double bad : {0.0, -0.1, kNaN, kInf}) {
            c.onPlayerUpdate(bad);
            CHECK_MSG(c.elapsed() == 0.0, "update(" + std::to_string(bad) + ")");
        }
    }
}

// ---- deaths ----

void testDestroy() {
    SECTION("destroyPlayer: death, would-be death (noclip), ignored cases");
    struct Row {
        char const* what;
        DestroyFacts facts;
        DestroyVerdict expected;
    };
    Row const rows[] = {
        {"player 1 dies", {true, false, 1, false, true}, DestroyVerdict::Death},
        {"player 2 dies (dual)", {true, false, 2, false, true}, DestroyVerdict::Death},
        {"noclip swallowed it (p1)", {true, false, 1, false, false}, DestroyVerdict::WouldBeDeath},
        {"noclip swallowed it (p2)", {true, false, 2, false, false}, DestroyVerdict::WouldBeDeath},
        {"already dead", {true, true, 1, false, true}, DestroyVerdict::Ignore},
        {"already dead, alive after", {true, true, 1, false, false}, DestroyVerdict::Ignore},
        {"no open attempt", {false, false, 1, false, true}, DestroyVerdict::Ignore},
        {"no open attempt, noclip", {false, false, 1, false, false}, DestroyVerdict::Ignore},
        {"another mod's clone", {true, false, 0, false, true}, DestroyVerdict::Ignore},
        {"clone under noclip", {true, false, 0, false, false}, DestroyVerdict::Ignore},
        {"bad slot", {true, false, 3, false, true}, DestroyVerdict::Ignore},
        {"GD anti-cheat spike", {true, false, 1, true, false}, DestroyVerdict::Ignore},
        {"GD anti-cheat spike, dead flag", {true, false, 1, true, true}, DestroyVerdict::Ignore},
    };
    for (auto const& r : rows) CHECK_MSG(classifyDestroy(r.facts) == r.expected, r.what);
}

void testWouldBeStreak() {
    SECTION("v0.10.0: noclip would-be deaths are counted like Eclipse - one per contiguous run of frames");
    WouldBeDeathStreak s;
    CHECK(startsWouldBeDeath(s, 100));      // first swallowed destroy: a death
    CHECK(!startsWouldBeDeath(s, 100));     // a second hazard in the same frame: the same death
    CHECK(!startsWouldBeDeath(s, 101));     // the next frame inside the hazard: the same death
    CHECK(!startsWouldBeDeath(s, 102));
    CHECK(startsWouldBeDeath(s, 104));      // a frame without a destroy in between: a new death
    CHECK(startsWouldBeDeath(s, 300));
    CHECK(!startsWouldBeDeath(s, 299));     // an older frame (clamped clocks) never starts a death
    CHECK(!startsWouldBeDeath(s, 301));
    // 60 frames inside one spike = one death, not sixty
    WouldBeDeathStreak spike;
    int deaths = 0;
    for (int f = 0; f < 60; ++f) deaths += startsWouldBeDeath(spike, 1000 + f) ? 1 : 0;
    CHECK(deaths == 1);
    // a fresh attempt starts a fresh streak
    WouldBeDeathStreak fresh;
    CHECK(startsWouldBeDeath(fresh, 0));
}

// ---- environment ----

void testIntegrity() {
    SECTION("integrity summary of a trust state");
    struct Row {
        TrustState trust;
        bool readable;
        Integrity expected;
    };
    Row const rows[] = {
        {TrustState::Allowed, true, Integrity::Clean},         {TrustState::NoclipModified, true, Integrity::Warnings},
        {TrustState::UnknownMod, true, Integrity::Warnings},   {TrustState::Botting, true, Integrity::Flagged},
        {TrustState::PhysicsChanged, true, Integrity::Flagged}, {TrustState::Allowed, false, Integrity::Unknown},
        {TrustState::Botting, false, Integrity::Unknown},
    };
    for (auto const& r : rows) CHECK_MSG(integrityFor(r.trust, r.readable) == r.expected, std::string(name(r.trust)));
}

telemetry::EnvironmentPayload cleanEnvironment() {
    telemetry::EnvironmentPayload p;
    p.mods = {{"geode.loader", "5.6.1", false}};
    p.modules = std::vector<telemetry::EnvironmentModule>{{"GeometryDash.exe", 1, "h"}};
    p.hashes = {"g", "e", "p", "l"};
    p.cbf = false;
    p.tpsBypass = false;
    p.tps = 240.0;
    p.fps = 240.0;
    p.integrity = Integrity::Clean;
    p.trust = TrustState::Allowed;
    p.noclip = false;
    p.bot = false;
    p.gdVersion = "2.2081";
    p.droppedEvents = 0;
    return p;
}

void testEnvironmentChanged() {
    SECTION("environment re-emit rule: trust-relevant fields change it, the rest does not");
    auto base = cleanEnvironment();
    CHECK(!environmentChanged(base, base));
    struct Row {
        char const* what;
        void (*mutate)(telemetry::EnvironmentPayload&);
        bool changed;
    };
    Row const rows[] = {
        {"noclip toggled on", [](telemetry::EnvironmentPayload& p) { p.noclip = true; p.trust = TrustState::NoclipModified; p.integrity = Integrity::Warnings; }, true},
        {"noclip flag only", [](telemetry::EnvironmentPayload& p) { p.noclip = true; }, true},
        {"bot playback", [](telemetry::EnvironmentPayload& p) { p.bot = true; }, true},
        {"trust only", [](telemetry::EnvironmentPayload& p) { p.trust = TrustState::PhysicsChanged; }, true},
        {"trust absent", [](telemetry::EnvironmentPayload& p) { p.trust.reset(); }, true},
        {"TPS bypass on at 240", [](telemetry::EnvironmentPayload& p) { p.tpsBypass = true; }, true},
        {"tps changed", [](telemetry::EnvironmentPayload& p) { p.tps = 360.0; }, true},
        {"CBF toggled", [](telemetry::EnvironmentPayload& p) { p.cbf = true; }, true},
        {"integrity only", [](telemetry::EnvironmentPayload& p) { p.integrity = Integrity::Unknown; }, true},
        {"fps changed", [](telemetry::EnvironmentPayload& p) { p.fps = 60.0; }, false},
        {"dropped events", [](telemetry::EnvironmentPayload& p) { p.droppedEvents = 12; }, false},
        {"mods changed", [](telemetry::EnvironmentPayload& p) { p.mods.push_back({"x.y", "1.0.0", std::nullopt}); }, false},
        {"modules dropped", [](telemetry::EnvironmentPayload& p) { p.modules.reset(); }, false},
        {"hashes changed", [](telemetry::EnvironmentPayload& p) { p.hashes.level = "other"; }, false},
        {"tps float noise", [](telemetry::EnvironmentPayload& p) { p.tps = 240.0 + 1e-9; }, false},
        {"noclip absent vs false", [](telemetry::EnvironmentPayload& p) { p.noclip.reset(); }, false},
        {"bot absent vs false", [](telemetry::EnvironmentPayload& p) { p.bot.reset(); }, false},
        {"tpsBypass absent vs false", [](telemetry::EnvironmentPayload& p) { p.tpsBypass.reset(); }, false},
    };
    for (auto const& r : rows) {
        auto now = base;
        r.mutate(now);
        CHECK_MSG(environmentChanged(base, now) == r.changed, r.what);
        CHECK_MSG(environmentChanged(now, base) == r.changed, std::string(r.what) + " (reversed)");
    }
}

// ---- cross-package goldens: tests/fixtures/classification/*.json ----

ModeFlags flagsFrom(json::Value const& list) {
    ModeFlags f;
    for (auto const& v : list.asArray()) {
        std::string s = v.asString();
        if (s == "ship") f.ship = true;
        else if (s == "ball") f.ball = true;
        else if (s == "ufo") f.ufo = true;
        else if (s == "wave") f.wave = true;
        else if (s == "robot") f.robot = true;
        else if (s == "spider") f.spider = true;
        else if (s == "swing") f.swing = true;
        else CHECK_MSG(false, "unknown flag " + s);
    }
    return f;
}

void testGamemodeFixture() {
    SECTION("fixture classification/gamemode-from-flags.json");
    auto fx = fixture("classification/gamemode-from-flags.json");
    CHECK(fx.getString("kind") == "gprl.classification/gamemode");
    // the documented priority is the precedence gamemodeFromFlags implements
    std::vector<std::string> priority;
    for (auto const& p : fx["priority"].asArray()) priority.push_back(p.asString());
    CHECK((priority == std::vector<std::string>{"ship", "ball", "ufo", "wave", "robot", "spider", "swing"}));
    int n = 0;
    for (auto const& c : fx["cases"].asArray()) {
        Gamemode g = gamemodeFromFlags(flagsFrom(c["flags"]));
        CHECK_MSG(name(g) == c.getString("expected"), c.getString("name") + ": got " + std::string(name(g)));
        ++n;
    }
    CHECK(n >= kGamemodeCount);
}

void testSpeedFixture() {
    SECTION("fixture classification/speed-from-player-speed.json (values cast to float like GD stores them)");
    auto fx = fixture("classification/speed-from-player-speed.json");
    CHECK(fx.getString("kind") == "gprl.classification/speed");
    // the fixture's thresholds and constants are ClassifyParams' (one source of truth per side)
    auto const& th = fx["thresholds"].asArray();
    double const boundaries[] = {kParams.boundarySlowNormal, kParams.boundaryNormalFast, kParams.boundaryFastFaster, kParams.boundaryFasterFastest};
    CHECK(th.size() == 4);
    for (size_t i = 0; i < th.size() && i < 4; ++i) CHECK_MSG(th[i].getNumber("below") == boundaries[i], "threshold " + std::to_string(i));
    CHECK(fx.getString("otherwise") == "fastest");
    for (auto const& k : fx["gdConstants"].asArray()) {
        Speed s;
        CHECK_MSG(parse(k.getString("speed"), s), k.getString("speed"));
        CHECK_MSG(multiplierOf(s) == k.getNumber("playerSpeed"), k.getString("speed") + " playerSpeed");
        CHECK_MSG(unitsPerSecond(s) == k.getNumber("unitsPerSecond"), k.getString("speed") + " unitsPerSecond");
    }
    for (auto const& c : fx["cases"].asArray()) {
        double stored = static_cast<double>(static_cast<float>(c.getNumber("playerSpeed")));
        Speed s = speedFromMultiplier(stored);
        CHECK_MSG(name(s) == c.getString("expected"), c.getString("name") + ": got " + std::string(name(s)));
    }
}

void testInputFixture() {
    SECTION("fixture classification/input-classification.json");
    auto fx = fixture("classification/input-classification.json");
    CHECK(fx.getString("kind") == "gprl.classification/input");
    for (auto const& b : fx["buttons"].asArray()) {
        auto in = classifyButton(true, static_cast<int>(b.getInt("gdButton")), true);
        CHECK_MSG(in && name(in->button) == b.getString("button"), b.getString("button"));
    }
    for (auto const& c : fx["cases"].asArray()) {
        auto in = classifyButton(c.getBool("down"), static_cast<int>(c.getInt("gdButton")), c.getBool("isPlayer1"));
        auto const& x = c["expected"];
        std::string what = c.getString("name");
        CHECK_MSG(in.has_value() == x.getBool("accepted"), what);
        if (!in || !x.getBool("accepted")) continue;
        CHECK_MSG(in->player == x.getInt("player"), what);
        CHECK_MSG(name(in->button) == x.getString("button"), what);
        CHECK_MSG(name(in->kind) == x.getString("kind"), what);
    }
}

void testDeathFixture() {
    SECTION("fixture classification/death-classification.json");
    auto fx = fixture("classification/death-classification.json");
    CHECK(fx.getString("kind") == "gprl.classification/death");
    for (auto const& c : fx["cases"].asArray()) {
        DestroyFacts f;
        f.attemptOpen = true;
        f.wasDeadBefore = c.getBool("wasDead");
        f.deadAfter = c.getBool("isDead");
        f.anticheatSpike = c.getBool("isAnticheatSpike");
        f.playerSlot = static_cast<int>(c.getInt("slot"));
        DestroyVerdict v = classifyDestroy(f);
        auto const& x = c["expected"];
        std::string what = c.getString("name");
        CHECK_MSG((v != DestroyVerdict::Ignore) == x.getBool("event"), what);
        CHECK_MSG((v == DestroyVerdict::WouldBeDeath) == x.getBool("wouldBe"), what);
        CHECK_MSG((v == DestroyVerdict::Death) == x.getBool("endsAttempt"), what);
        CHECK_MSG((v == DestroyVerdict::WouldBeDeath) == x.getBool("marksNoclipSeen"), what);
    }
}

void testSubTickFixture() {
    SECTION("fixture classification/sub-tick.json (dt fed as the float GD passes)");
    auto fx = fixture("classification/sub-tick.json");
    CHECK(fx.getString("kind") == "gprl.classification/subtick");
    auto const& td = fx["tickDt"];
    CHECK(td.getNumber("default") == tickDtFor(false, 0.0));
    CHECK(td.getNumber("epsilon") == kParams.tickBoundaryEpsilon);
    CHECK(td.getNumber("maxSubTick") == kParams.maxSubTick);
    double tol = fx.getNumber("tolerance", 1e-6);
    int reads = 0;
    for (auto const& seq : fx["sequences"].asArray()) {
        std::string what = seq.getString("name");
        double tickDt = tickDtFor(seq.getBool("tpsBypass"), seq.getNumber("tps"));
        SubTickClock clock;
        clock.reset(tickDt);
        int step = 0;
        for (auto const& s : seq["steps"].asArray()) {
            if (auto* u = s.find("update")) clock.onPlayerUpdate(static_cast<double>(static_cast<float>(u->asNumber())));
            else if (auto* r = s.find("read")) {
                ++reads;
                CHECK_MSG(std::fabs(clock.fraction() - r->asNumber()) <= tol,
                          what + " step " + std::to_string(step) + ": got " + std::to_string(clock.fraction()) + " expected " + std::to_string(r->asNumber()));
            }
            else if (s.find("reset")) clock.reset(tickDt);
            ++step;
        }
    }
    CHECK(reads > 0);
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "D:/GPRL";
    testGamemodes();
    testSpeeds();
    testMini();
    testButtons();
    testTickLength();
    testSubTickClock();
    testDestroy();
    testWouldBeStreak();
    testIntegrity();
    testEnvironmentChanged();
    testGamemodeFixture();
    testSpeedFixture();
    testInputFixture();
    testDeathFixture();
    testSubTickFixture();
    return gprl::test::finish("classify_tests");
}
