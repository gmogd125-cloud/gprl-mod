// core/calibration host tests (SPEC §10-§11).
//   1. Familiarity weights + effective sample count of tests/fixtures/rating/synthetic-player-{a,b,c}.json
//      (shared golden fixtures) and synthetic-player-{90,150,300}.json (tools/fixture-gen) - the
//      TypeScript engine's chronological familiarityWeights - reproduced by CalibrationTracker, plus
//      the calibration percent at the fixture's fitted raw L.
//   2. computeCalibration behaviour: empty -> locked 0 %, 1000 near-identical wave clicks never
//      unlock, a varied history with a fitted L completes, percent weights, server override.
// argv[1] = repository root.
#include "test_util.hpp"
#include "fixture_util.hpp"

#include "../core/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <map>

using namespace gprl;

namespace {

std::string g_root;

struct FixtureSample {
    CalibrationSample sample;
    int attemptsAtCluster = 0;
    double timeMs = 0.0;
};

bool loadPlayer(char const* rel, json::Value& out) {
    std::string path = g_root + "/tests/fixtures/rating/" + rel;
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    if (text.empty()) return false;
    json::ParseError err;
    bool ok = json::parse(text, out, &err);
    CHECK_MSG(ok, "fixture parse error: " + err.message);
    return ok;
}

bool sampleFromJson(json::Value const& v, FixtureSample& out) {
    CalibrationSample s;
    s.id = v.getString("id");
    s.levelId = v.getString("levelId");
    s.windowMs = v.getNumber("windowMs");
    s.hit = v.getBool("hit");
    if (!parse(v.getString("kind"), s.kind)) return false;
    if (!parse(v.getString("gamemode"), s.gamemode)) return false;
    if (!parse(v.getString("speed"), s.speed)) return false;
    std::string err;
    if (!telemetry::fingerprintFromJson(v["fingerprint"], s.fingerprint, &err)) return false;
    s.bot = v["flags"].getBool("bot");
    s.wouldBeDeath = v["flags"].getBool("wouldBeDeath");
    s.physicsModified = v["flags"].getBool("physicsModified");
    s.recordedAtMs = test::isoToUnixMs(v.getString("recordedAt"));
    out.sample = s;
    out.attemptsAtCluster = static_cast<int>(v["attemptContext"].getInt("attemptsAtCluster"));
    out.timeMs = s.recordedAtMs;
    return true;
}

void testSyntheticPlayer(char const* rel) {
    SECTION(rel);
    json::Value fx;
    if (!loadPlayer(rel, fx)) return;
    std::vector<FixtureSample> samples;
    for (auto const& v : fx["samples"].asArray()) {
        FixtureSample s;
        CHECK_MSG(sampleFromJson(v, s), "bad sample " + v.getString("id"));
        samples.push_back(std::move(s));
    }
    CHECK(samples.size() == 150);
    // familiarityWeights() sorts chronologically (time, then id) so each sample only sees earlier ones
    std::stable_sort(samples.begin(), samples.end(), [](FixtureSample const& a, FixtureSample const& b) {
        if (a.timeMs != b.timeMs) return a.timeMs < b.timeMs;
        return a.sample.id < b.sample.id;
    });
    std::map<std::string, double> expected;
    for (auto const& w : fx["familiarityWeights"].asArray()) expected[w.getString("id")] = w.getNumber("weight");
    CHECK(expected.size() == samples.size());

    CalibrationTracker tracker;
    double total = 0.0;
    int mismatches = 0;
    for (auto const& s : samples) {
        auto r = tracker.add(s.sample, s.attemptsAtCluster);
        total += r.weight;
        auto it = expected.find(s.sample.id);
        if (it == expected.end() || !test::near(r.weight, it->second, 1e-9)) {
            if (mismatches < 5) CHECK_MSG(false, s.sample.id + ": weight " + std::to_string(r.weight) + " expected " + (it == expected.end() ? "?" : std::to_string(it->second)));
            ++mismatches;
        }
    }
    CHECK_MSG(mismatches == 0, std::to_string(mismatches) + " weight mismatches");
    double expectedEffective = fx["expected"].getNumber("effectiveSamples");
    double relTol = fx["expected"].getNumber("toleranceRelative", 1e-6);
    CHECK_MSG(test::near(total, expectedEffective, expectedEffective * relTol), "effective samples " + std::to_string(total) + " expected " + std::to_string(expectedEffective));

    // The local report never unlocks: no fitted L locally -> difficulty coverage 0 -> locked.
    auto report = tracker.report();
    CHECK(report.state.overallLocked);
    CHECK(!report.state.complete);
    CHECK_NEAR(report.state.effectiveSamples.current, std::floor(total + 0.5), 1e-9);
    CHECK(report.state.difficultyCoverage == 0.0);
    CHECK(!report.boundarySamples.has_value());
    CHECK(report.version == "0.1.0-experimental");
    // With the fixture's fitted raw L the calibration percent of the TypeScript engine is reproduced.
    double rawL = fx["expected"].getNumber("rawL");
    double expectedPercent = fx["expected"].getNumber("calibrationPercent");
    auto fitted = tracker.report(rawL);
    CHECK_MSG(test::near(fitted.state.percent, expectedPercent, std::max(1e-6, expectedPercent * relTol)),
              "calibration percent " + std::to_string(fitted.state.percent) + " expected " + std::to_string(expectedPercent));
    CHECK(fitted.state.overallLocked == fx["expected"].getBool("locked"));
}

CalibrationSample makeSample(int i, Gamemode gm, Speed speed, InputKind kind, double windowMs, bool hit, std::string level, std::string geo) {
    CalibrationSample s;
    s.id = "s" + std::to_string(i);
    s.levelId = std::move(level);
    s.windowMs = windowMs;
    s.hit = hit;
    s.kind = kind;
    s.gamemode = gm;
    s.speed = speed;
    s.fingerprint.gamemode = gm;
    s.fingerprint.speed = speed;
    s.fingerprint.kind = kind;
    s.fingerprint.windowMs = windowMs;
    s.fingerprint.geometryHash = std::move(geo);
    s.recordedAtMs = 1'800'000'000'000.0 + i * 1000.0;
    return s;
}

void testBehaviour() {
    SECTION("empty calibration is locked at 0 %");
    auto empty = computeCalibration({}, std::nullopt);
    CHECK(empty.state.overallLocked && !empty.state.complete);
    CHECK(empty.state.percent == 0.0);
    CHECK(empty.state.effectiveSamples.required == 700.0 && empty.state.gamemodes.required == 3.0);
    CHECK(empty.state.releaseData == ReleaseData::Insufficient);
    CHECK(empty.requirements.size() == 8);
    CHECK(!empty.debug.empty());

    SECTION("1000 near-identical wave clicks are ~340 effective samples (saturating familiarity, MASTER §9 / C3) and never unlock");
    CalibrationTracker wave;
    for (int i = 0; i < 1000; ++i) wave.add(makeSample(i, Gamemode::Wave, Speed::Fast, InputKind::Press, 8.0, true, "lvl", "geo-1"), i);
    auto r = wave.report(150.0);   // even with a fit supplied
    CHECK(r.state.effectiveSamples.current < 400.0 && r.state.effectiveSamples.current > 300.0);
    CHECK(r.state.effectiveSamples.current < r.state.effectiveSamples.required);
    CHECK(r.state.overallLocked);
    CHECK(r.state.gamemodes.current == 1.0);
    CHECK(r.state.releaseData == ReleaseData::Insufficient);
    CHECK(r.state.patternVariety < 0.05);
    CHECK(r.levels == 1 && r.speeds == 1 && r.patterns == 1);
    CHECK(r.state.percent < 0.6);

    SECTION("varied evidence with a fitted L completes; percent is the weighted requirement ratio");
    std::vector<CalibrationSample> varied;
    Gamemode gms[] = {Gamemode::Cube, Gamemode::Ship, Gamemode::Wave, Gamemode::Ball};
    Speed speeds[] = {Speed::Normal, Speed::Fast, Speed::Faster};
    int n = 0;
    for (int i = 0; i < 900; ++i) {
        // windows spread from 2 ms to 40 ms so a fit at L = 150 sees plenty of boundary samples
        double w = 2.0 + (i % 50) * 0.8;
        bool hit = (i % 10) != 0;   // 10 % misses
        auto s = makeSample(n++, gms[i % 4], speeds[i % 3], (i % 5 == 0) ? InputKind::Release : InputKind::Press, w, hit,
                            "lvl" + std::to_string(i % 4), "geo-" + std::to_string(i % 60));
        s.fingerprint.yVelocity = (i % 7) * 40.0;
        s.weight = 1.0;
        varied.push_back(s);
    }
    auto v = computeCalibration(varied, 150.0);
    for (auto const& req : v.requirements) CHECK_MSG(req.satisfied, req.id + " not satisfied: " + std::to_string(req.current) + "/" + std::to_string(req.required));
    CHECK(v.state.complete && !v.state.overallLocked);
    CHECK_NEAR(v.state.percent, 1.0, 1e-12);
    CHECK(v.state.releaseData == ReleaseData::Sufficient);
    CHECK(v.state.gamemodes.current == 4.0);
    CHECK(v.perGamemode.size() == 4);
    // without the fit the same evidence stays locked (difficulty coverage unknown)
    auto nofit = computeCalibration(varied, std::nullopt);
    CHECK(nofit.state.overallLocked && nofit.state.difficultyCoverage == 0.0);
    double sumW = 0.0, sumWr = 0.0;
    CalibrationParams p;
    double const weights[] = {p.percentWeights.effectiveSamples, p.percentWeights.gamemodes, p.percentWeights.levels, p.percentWeights.releases,
                              p.percentWeights.speeds, p.percentWeights.difficultyCoverage, p.percentWeights.misses, p.percentWeights.patterns};
    for (size_t i = 0; i < nofit.requirements.size(); ++i) {
        sumW += weights[i];
        sumWr += weights[i] * nofit.requirements[i].ratio;
    }
    CHECK_NEAR(nofit.state.percent, sumWr / sumW, 1e-12);
    // calibration/0.2.0: without a fit neither the boundary samples nor the informative misses count
    CHECK_NEAR(nofit.state.percent, 1.0 - (p.percentWeights.difficultyCoverage + p.percentWeights.misses) / sumW, 1e-12);
    CHECK(!nofit.informativeMisses.has_value() && v.informativeMisses.has_value());

    SECTION("calibration/0.4.0: 700 effective samples with a fit complete the calibration outright");
    CalibrationParams strict;        // the 0.3.0 rule: every requirement on its own
    strict.completeAtEffectiveSamples = false;
    CalibrationParams tall = strict; // a boundary requirement the varied set cannot meet
    tall.boundarySamples = 1e6;
    CHECK(!computeCalibration(varied, 150.0, {}, tall).state.complete);
    CalibrationParams tallWaived = tall;
    tallWaived.completeAtEffectiveSamples = true;
    auto waived = computeCalibration(varied, 150.0, {}, tallWaived);
    CHECK(waived.state.complete && !waived.state.overallLocked);
    CHECK_NEAR(waived.state.percent, 1.0, 1e-12);
    CHECK(waived.requirements[5].satisfied && waived.requirements[5].note.find("counted as met") != std::string::npos);
    // never without a fit, never without a miss, never below the count
    CHECK(!computeCalibration(varied, std::nullopt, {}, tallWaived).state.complete);
    std::vector<CalibrationSample> hitsOnly;
    for (auto s : varied) { s.hit = true; hitsOnly.push_back(s); }
    CHECK(!computeCalibration(hitsOnly, 150.0, {}, tallWaived).state.complete);
    std::vector<CalibrationSample> few(varied.begin(), varied.begin() + 600);
    CHECK(!computeCalibration(few, 150.0, {}, tallWaived).state.complete);
    CHECK(v.model == "calibration/0.4.0" && nofit.model == v.model);

    SECTION("timing bands and hit probability");
    CHECK(timingBand(0.1, p) == 0);
    CHECK(timingBand(250.0, p) == p.timingBands - 1);
    CHECK(timingBand(1000.0, p) == p.timingBands - 1);
    CHECK(timingBand(0.5, p) < timingBand(5.0, p) && timingBand(5.0, p) < timingBand(50.0, p));
    CHECK_NEAR(hitProbability(4.1667, 323.8), 0.5, 0.002);   // one frame at 240 is ~324 sigma/s (difficulty.ts)
    CHECK(hitProbability(0.0, 100.0) == 0.0);
    CHECK(hitProbability(100.0, 0.0) == 0.0);

    SECTION("server override replaces the displayed state and round-trips JSON");
    CalibrationTracker t;
    CalibrationState server;
    server.percent = 0.42;
    server.effectiveSamples = {294.0, 700.0};
    server.gamemodes = {2.0, 3.0};
    server.timingVariety = 0.5;
    server.patternVariety = 0.25;
    server.releaseData = ReleaseData::Partial;
    server.difficultyCoverage = 0.1;
    server.overallLocked = true;
    CalibrationState parsed;
    std::string err;
    CHECK_MSG(calibrationStateFromJson(calibrationStateToJson(server), parsed, &err), err);
    CHECK(parsed.percent == 0.42 && parsed.effectiveSamples.current == 294.0 && parsed.releaseData == ReleaseData::Partial && parsed.overallLocked);
    t.overrideFromServer(parsed);
    CHECK(t.hasServerOverride());
    auto shown = t.report();
    CHECK(shown.state.percent == 0.42 && shown.state.effectiveSamples.current == 294.0);
    t.clearServerOverride();
    CHECK(t.report().state.percent == 0.0);
    json::Value bad;
    json::parse("{\"complete\":false}", bad);
    CHECK(!calibrationStateFromJson(bad, parsed, &err));
}


// docs/RANKS.md "Visibility thresholds" (v0.4.3): the display fields of GET /v1/me/calibration.
void testDisplay() {
    auto parse = [](char const* text) {
        json::Value v;
        json::ParseError err;
        CHECK_MSG(json::parse(text, v, &err), "json: " + err.message);
        return v;
    };
    CalibrationDisplay d;

    SECTION("display: absent displayState => the calibration lock decides (locked as today)");
    // the live answer of 2026-09-30 (before the field existed): locked calibration, no figures
    CHECK(!calibrationDisplayFromJson(parse(R"({"algorithmVersion":"0.1.0-experimental","overall":{"complete":false,"percent":0.886,"effectiveSamples":{"current":404,"required":700},"gamemodes":{"current":3,"required":3},"timingVariety":0.8,"patternVariety":0.9,"releaseData":"sufficient","difficultyCoverage":0.6,"overallLocked":true},"perGamemode":{},"updatedAt":"2026-09-30T00:00:00Z","ratedDemonLevels":4,"note":null})"), d));
    CHECK(d.state == RatingDisplay::Locked && d.locked());
    CHECK(!d.verifiedSigma && !d.rawSigma && !d.practicalSigma && !d.confidence && !d.headlineSigma());
    CHECK(sigmaHudText(d, 0.886) == "LOCKED (calibrating 88%)");
    CHECK(rankHintText(d, 0.9).empty());
    // no overall at all: locked
    CHECK(!calibrationDisplayFromJson(parse(R"({})"), d));
    CHECK(d.locked());
    CHECK(!calibrationDisplayFromJson(json::Value(), d));
    CHECK(d.locked());
    // an old server that already unlocked the calibration keeps showing an unlocked line
    CHECK(!calibrationDisplayFromJson(parse(R"({"overall":{"overallLocked":false,"percent":1}})"), d));
    CHECK(d.state == RatingDisplay::Visible && !d.headlineSigma());
    CHECK(sigmaHudText(d, 1.0) == "- (unranked)");

    SECTION("display: locked never yields a figure, whatever else is sent");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","verifiedSigma":123.4,"rawSigma":130,"practicalSigma":120,"confidence":0.79,"rankHint":"x","overall":{"overallLocked":false}})"), d));
    CHECK(d.locked() && !d.verifiedSigma && !d.rawSigma && !d.practicalSigma && !d.confidence);
    CHECK(d.rankHint == "x");   // harmless: rankHintText ignores it while locked
    CHECK(rankHintText(d, 0.9).empty());
    CHECK(sigmaHudText(d, 0.382) == "LOCKED (calibrating 38%)");
    CHECK(sigmaHudText(d, 0.0) == "LOCKED (calibrating 0%)");
    CHECK(sigmaHudText(d, 1.0) == "LOCKED (calibrating 100%)");

    SECTION("display: visible between the thresholds");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","verifiedSigma":123.4,"rawSigma":130.25,"practicalSigma":121,"confidence":0.824,"rankHint":"Rank at 90% confidence","overall":{"overallLocked":true,"percent":0.886}})"), d));
    CHECK(d.state == RatingDisplay::Visible && !d.locked());
    CHECK(d.verifiedSigma && *d.verifiedSigma == 123.4);
    CHECK(d.rawSigma && *d.rawSigma == 130.25);
    CHECK(d.practicalSigma && *d.practicalSigma == 121.0);
    CHECK(d.confidence && *d.confidence == 0.824);
    CHECK(d.headlineSigma() && *d.headlineSigma() == 123.4);
    CHECK(d.rankHint == "Rank at 90% confidence");
    CHECK(sigmaHudText(d, 0.886) == "123.4 (82% conf, unranked)");
    CHECK(rankHintText(d, 0.9) == "Rank at 90% confidence");
    CHECK(rankHintText(d, std::nullopt) == "Rank at 90% confidence");
    // the rank can never leak in through the visible state
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","practicalSigma":88,"confidence":0.85,"rank":{"rankId":"gold","division":1,"name":"Gold I"}})"), d));
    CHECK(d.rankId.empty() && d.rankName.empty() && d.rankDivision == 0);
    CHECK(d.headlineSigma() && *d.headlineSigma() == 88.0);   // practical when there is no verified figure
    CHECK(sigmaHudText(d, 0.5) == "88 (85% conf, unranked)");
    // no server hint: the ladder's eligibility minimum words it; nothing known: no line
    CHECK(rankHintText(d, 0.9) == "Rank at 90% confidence");
    CHECK(rankHintText(d, 0.95) == "Rank at 95% confidence");
    CHECK(rankHintText(d, std::nullopt).empty());
    CHECK(rankHintText(d, 0.0).empty());
    // raw only, no confidence
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"VISIBLE","rawSigma":70.04})"), d));
    CHECK(d.state == RatingDisplay::Visible);
    CHECK(sigmaHudText(d, 0.5) == "70 (unranked)");

    SECTION("display: ranked");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked","verifiedSigma":159.7,"rawSigma":171.2,"practicalSigma":160.1,"confidence":0.94,"rankHint":"stale","rank":{"rankId":"master","division":2,"name":"Master II"}})"), d));
    CHECK(d.state == RatingDisplay::Ranked && !d.locked());
    CHECK(d.rankId == "master" && d.rankDivision == 2 && d.rankName == "Master II");
    CHECK(d.rankHint.empty());   // nothing to hint at once ranked
    CHECK(rankHintText(d, 0.9).empty());
    CHECK(sigmaHudText(d, 1.0) == "159.7 | Master II");
    CHECK(sigmaHudText(d, 1.0, "Master II (site)") == "159.7 | Master II (site)");   // the caller's ladder name wins
    // rank as a string / id only / absent: the HUD still says ranked
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked","practicalSigma":200,"confidence":0.97,"rank":"apex"})"), d));
    CHECK(d.rankId == "apex" && d.rankName.empty());
    CHECK(sigmaHudText(d, 1.0) == "200 | ranked");
    CHECK(sigmaHudText(d, 1.0, "Apex") == "200 | Apex");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked","practicalSigma":200,"rankName":"Apex"})"), d));
    CHECK(d.rankName == "Apex" && sigmaHudText(d, 1.0) == "200 | Apex");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked"})"), d));
    CHECK(sigmaHudText(d, 1.0) == "- | ranked");

    SECTION("display: nested / tolerant / hostile");
    // fields under a `display` object
    CHECK(calibrationDisplayFromJson(parse(R"({"overall":{"overallLocked":true},"display":{"displayState":"visible","verifiedSigma":101.5,"confidence":0.81,"rankHint":"Rank at 90% confidence"}})"), d));
    CHECK(d.state == RatingDisplay::Visible && d.verifiedSigma && *d.verifiedSigma == 101.5 && d.rankHint == "Rank at 90% confidence");
    // under `rating`, with the domain's short keys
    CHECK(calibrationDisplayFromJson(parse(R"({"rating":{"displayState":"ranked","verified":150,"raw":160,"practical":151,"ratingConfidence":0.93}})"), d));
    CHECK(d.state == RatingDisplay::Ranked && d.verifiedSigma && *d.verifiedSigma == 150.0 && d.rawSigma && *d.rawSigma == 160.0);
    CHECK(d.practicalSigma && *d.practicalSigma == 151.0 && d.confidence && *d.confidence == 0.93);
    // unknown state word / wrong type => the calibration lock decides
    CHECK(!calibrationDisplayFromJson(parse(R"({"displayState":"unlocked","verifiedSigma":5,"overall":{"overallLocked":true}})"), d));
    CHECK(d.locked() && !d.verifiedSigma);
    CHECK(!calibrationDisplayFromJson(parse(R"({"displayState":7,"overall":{"overallLocked":false}})"), d));
    CHECK(d.state == RatingDisplay::Visible);
    // null / string / negative sigma: no figure; confidence clamped; huge division clamped
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","verifiedSigma":null,"rawSigma":"90","practicalSigma":-3,"confidence":7})"), d));
    CHECK(!d.verifiedSigma && !d.rawSigma && !d.practicalSigma && !d.headlineSigma());
    CHECK(d.confidence && *d.confidence == 1.0);
    CHECK(sigmaHudText(d, 0.9) == "- (100% conf, unranked)");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","practicalSigma":1e300,"confidence":-2,"rankHint":12})"), d));
    CHECK(d.practicalSigma && d.confidence && *d.confidence == 0.0 && d.rankHint.empty());
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked","practicalSigma":50,"rank":{"id":"bronze","division":1e300,"bandName":"Bronze I"}})"), d));
    CHECK(d.rankId == "bronze" && d.rankDivision == 9 && d.rankName == "Bronze I");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked","rank":[1,2]})"), d));
    CHECK(d.rankId.empty() && d.rankName.empty());
    // the figures under a `sigma` object never carry a rank into a visible state
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","sigma":{"practical":88,"confidence":0.85,"rankBandId":"gold-1"}})"), d));
    CHECK(d.state == RatingDisplay::Visible && d.practicalSigma && *d.practicalSigma == 88.0 && d.rankId.empty() && d.rankDivision == 0);
    // flat fields win over a `sigma` NUMBER (an old / other shape); the headline `sigma` is used when nothing else is
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","sigma":77,"practicalSigma":90})"), d));
    CHECK(d.practicalSigma && *d.practicalSigma == 90.0);
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","sigma":{"sigma":77}})"), d));
    CHECK(d.practicalSigma && *d.practicalSigma == 77.0 && !d.rawSigma && !d.verifiedSigma);

    SECTION("display: the real GET /v1/me/calibration bodies (tests/fixtures/api, written by api/src/test/display.test.ts)");
    auto loadApi = [&](char const* name, json::Value& out) {
        std::string path = g_root + "/tests/fixtures/api/" + name;
        std::string text = test::readFile(path);
        CHECK_MSG(!text.empty(), "fixture missing: " + path);
        if (text.empty()) return false;
        json::ParseError err;
        bool ok = json::parse(text, out, &err);
        CHECK_MSG(ok, "fixture parse error: " + err.message);
        return ok;
    };
    json::Value fx;
    if (loadApi("me-calibration-locked.json", fx)) {
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(d.locked() && !d.headlineSigma() && !d.confidence && d.rankHint.empty() && d.rankId.empty());
        CHECK(sigmaHudText(d, fx["overall"].getNumber("percent")) == "LOCKED (calibrating 82%)");  // pinned body: a 500/20/6 player at 82.5 % (calibration/0.4.0 fixtures)
        CHECK(rankHintText(d, 0.9).empty());
    }
    if (loadApi("me-calibration-visible.json", fx)) {
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(d.state == RatingDisplay::Visible);
        CHECK(!d.verifiedSigma);   // no verified rating yet: the personal (unverified) analysis shows
        CHECK(d.practicalSigma && *d.practicalSigma == 118.4);
        CHECK(d.rawSigma && *d.rawSigma == 112.48);
        CHECK(d.confidence && *d.confidence == 0.85);
        CHECK(d.rankHint == "rank at 90% confidence");
        CHECK(d.rankId.empty() && d.rankName.empty() && d.rankDivision == 0);
        CHECK(d.headlineSigma() && *d.headlineSigma() == 118.4);
        CHECK(sigmaHudText(d, fx["overall"].getNumber("percent")) == "118.4 (85% conf, unranked)");
        CHECK(rankHintText(d, 0.9) == "Rank at 90% confidence");
        CHECK(rankHintText(d, std::nullopt) == "Rank at 90% confidence");
    }
    {
        // the owner's example (2026-10-02): 170.1 at 78 % confidence, integrity passed
        json::Value v;
        std::string const ownerText =
            R"j({"displayState":"visible","sigma":{"sigma":170.1,"practical":170.1,"verified":170.1,"verifiedConfidence":0.776},)j"
            R"j("verificationStatus":{"status":"auto_verified","label":"Auto-Verified","provisional":true,)j"
            R"j("provisionalLabel":"Provisional / Unranked (Rating Confidence 77% < 90%)","reason":"Integrity checks passed: verified automatically."}})j";
        bool const ownerParsed = json::parse(ownerText, v);
        CHECK(ownerParsed);
        CalibrationDisplay p;
        CHECK(calibrationDisplayFromJson(v, p));
        CHECK(p.verificationStatus == "auto_verified" && p.provisional);
        CHECK(p.provisionalLabel == "Provisional / Unranked (Rating Confidence 77% < 90%)");
        CHECK(p.verifiedSigma && *p.verifiedSigma == 170.1);
        // an older server without the block: empty status (the Profile tab falls back to the badge)
        json::Value old;
        std::string const oldText = R"({"displayState":"visible","sigma":{"sigma":120}})";
        bool const oldParsed = json::parse(oldText, old);
        CHECK(oldParsed);
        CalibrationDisplay q;
        CHECK(calibrationDisplayFromJson(old, q));
        CHECK(q.verificationStatus.empty() && !q.provisional);
    }
    {
        // v0.12.0 top-right panel (owner request 2026-10-02)
        CalibrationDisplay d;
        d.state = RatingDisplay::Locked;
        auto t = topRightSigma(d, 0.62);
        CHECK(t.big == "LOCKED" && t.detail == "calibrating 62%");
        d.privatePresent = true;
        d.privatePractical = 123.44;
        CHECK(topRightSigma(d, 0.62).detail == "calibrating 62%  private ~123.4");
        CHECK(topRightSigma(d, 0.62, {}, false).detail == "calibrating 62%");   // the clip buffer records
        CalibrationDisplay v;
        v.state = RatingDisplay::Visible;
        v.verifiedSigma = 170.13;
        v.confidence = 0.776;
        auto tv = topRightSigma(v, 1.0);
        CHECK(tv.big == "170.1" && tv.detail == "sigma/s  77% conf  unranked");
        CalibrationDisplay r;
        r.state = RatingDisplay::Ranked;
        r.verifiedSigma = 171.0;
        CHECK(topRightSigma(r, 1.0, "Grandmaster").detail == "sigma/s  Grandmaster");
        CHECK(topRightSigma(r, 1.0).detail == "sigma/s  ranked");
        for (auto const& x : {t, tv})
            for (char ch : x.big + x.detail) CHECK_MSG(static_cast<unsigned char>(ch) < 0x80, "ASCII only (GD fonts)");
    }
    if (loadApi("me-calibration-ranked.json", fx)) {
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(d.state == RatingDisplay::Ranked);
        CHECK(d.verifiedSigma && *d.verifiedSigma == 171.0);
        CHECK(d.practicalSigma && *d.practicalSigma == 171.0);
        // 2026-10-02 verification status: a ranked automatic rating is Auto-Verified, not provisional
        CHECK(d.verificationStatus == "auto_verified" && d.verificationLabel == "Auto-Verified");
        CHECK(!d.provisional && d.provisionalLabel.empty() && !d.verificationReason.empty());
        CHECK(d.rawSigma && *d.rawSigma == 162.45);
        CHECK(d.confidence && *d.confidence == 0.95);   // verifiedConfidence, the figure's own
        // 171 sigma/s is Grandmaster (no divisions) on the 2026-10-02 ladder: the fixture's rankBandId
        // "grandmaster" parses to the rank id alone, division 0.
        CHECK(d.rankId == "grandmaster" && d.rankDivision == 0 && d.rankName.empty());
        CHECK(d.rankHint.empty());
        CHECK(sigmaHudText(d, 1.0) == "171 | ranked");
        CHECK(sigmaHudText(d, 1.0, "Grandmaster") == "171 | Grandmaster");
        CHECK(rankHintText(d, 0.9).empty());
        // owner decision 2026-10-02: the private figure follows the public gate (80 % confidence,
        // cube / ship / wave / ball calibrated); the fixture player's recomputed analysis meets
        // neither, so the ranked body carries only the verified figure
        CHECK(!d.privatePresent && !d.privateHeadline() && privateSigmaProfileText(d).empty());
    }
    // the three older bodies: no private figure in the locked / visible ones, the HUD text unchanged
    if (loadApi("me-calibration-locked.json", fx)) {
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(!d.privatePresent && !d.privateHeadline() && privateSigmaProfileText(d).empty() && privateSigmaNoteText(d, 0.9).empty());
        CHECK(d.sigmaMinConfidence && *d.sigmaMinConfidence == 0.8 && d.rankMinConfidence && *d.rankMinConfidence == 0.9);
    }
    if (loadApi("me-calibration-visible.json", fx)) {
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(!d.privatePresent && sigmaHudText(d, fx["overall"].getNumber("percent")) == "118.4 (85% conf, unranked)");
    }

    SECTION("display: the private sigma/s (owner decisions 2026-10-01 / 2026-10-02; tests/fixtures/api/me-calibration-private.json)");
    if (loadApi("me-calibration-private.json", fx)) {
        // since 2026-10-02 the private figure needs the public gate too, so the pinned body is a
        // VISIBLE one (123.4 at 85 %, the four gamemodes calibrated) with the private figure next to it
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(d.state == RatingDisplay::Visible && d.headlineSigma() && *d.headlineSigma() == 123.4);
        CHECK(d.privatePresent);
        CHECK(d.privateRaw && *d.privateRaw == 117.3);
        CHECK(d.privatePractical && *d.privatePractical == 123.4);
        CHECK(d.privateConfidence && *d.privateConfidence == 0.85);
        CHECK(d.privateCalibrationPercent && *d.privateCalibrationPercent == 0.825);
        CHECK(d.privateMinCalibration && *d.privateMinCalibration == 0.5);
        CHECK(d.privateHeadline() && *d.privateHeadline() == 123.4);
        CHECK(sigmaHudText(d, fx["overall"].getNumber("percent")) == "123.4 (85% conf, unranked)");   // visible keeps the public text
        CHECK(privateSigmaProfileText(d) == "Private sigma/s (estimate, only you): raw 117.3   practical 123.4   confidence 85%");
        CHECK(privateSigmaNoteText(d, 0.9) == "shown to you from 50% calibration; public from 80% confidence, rank at 90%");
        CHECK(privateSigmaNoteText(d, std::nullopt) == "shown to you from 50% calibration; public from 80% confidence, rank at 90%");   // the answer's rankMinConfidence
        CHECK(privateSigmaNoteText(d, 0.95) == "shown to you from 50% calibration; public from 80% confidence, rank at 95%");     // the ladder wins
        CHECK(rankHintText(d, 0.9) == "Rank at 90% confidence");
    }
    // the HUD's private figure: practical with one decimal, raw when practical is missing, the
    // text unchanged without one; visible / ranked keep the public text whatever the private part says
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","privateSigma":{"raw":101.24,"practical":null,"confidence":0.3,"calibrationPercent":0.62,"minCalibration":0.5,"label":"x"}})"), d));
    CHECK(sigmaHudText(d, 0.62) == "LOCKED (calibrating 62%) | private ~101.2");   // raw when practical is missing
    CHECK(privateSigmaProfileText(d) == "Private sigma/s (estimate, only you): raw 101.2   practical -   confidence 30%");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","privateSigma":{"raw":99,"practical":118,"confidence":0.3,"calibrationPercent":0.62,"minCalibration":0.5}})"), d));
    CHECK(sigmaHudText(d, 0.62) == "LOCKED (calibrating 62%) | private ~118.0");
    CHECK(privateSigmaNoteText(d, std::nullopt) == "shown to you from 50% calibration");   // no public thresholds sent: left out, never invented
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","practicalSigma":121,"confidence":0.824,"privateSigma":{"raw":115,"practical":121,"confidence":0.824,"calibrationPercent":0.7,"minCalibration":0.5}})"), d));
    CHECK(d.privatePresent && sigmaHudText(d, 0.7) == "121 (82% conf, unranked)");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"ranked","practicalSigma":171,"privateSigma":{"raw":160,"practical":170,"confidence":0.95,"calibrationPercent":1,"minCalibration":0.5}})"), d));
    CHECK(d.privatePresent && sigmaHudText(d, 1.0) == "171 | ranked");
    // absent / null / malformed / below its own threshold: no private figure, the old text
    for (char const* body : {R"({"displayState":"locked"})", R"({"displayState":"locked","privateSigma":null})",
                             R"({"displayState":"locked","privateSigma":7})", R"({"displayState":"locked","privateSigma":[1,2]})",
                             R"({"displayState":"locked","privateSigma":{"raw":null,"practical":null,"confidence":0.4}})",
                             R"({"displayState":"locked","privateSigma":{"raw":"120","practical":-3,"confidence":0.4}})",
                             R"({"displayState":"locked","privateSigma":{"practical":120,"confidence":0.4,"calibrationPercent":0.4,"minCalibration":0.5}})"}) {
        CHECK(calibrationDisplayFromJson(parse(body), d));
        CHECK_MSG(!d.privatePresent && sigmaHudText(d, 0.42) == "LOCKED (calibrating 42%)", std::string("private figure from ") + body);
        CHECK(privateSigmaProfileText(d).empty() && privateSigmaNoteText(d, 0.9).empty());
    }
    // hostile numbers: confidence clamped, absurd figure short, percent outside [0,1] ignored
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","privateSigma":{"practical":1e300,"confidence":7,"calibrationPercent":3,"minCalibration":-1}})"), d));
    CHECK(d.privatePresent && d.privateConfidence && *d.privateConfidence == 1.0 && !d.privateCalibrationPercent && !d.privateMinCalibration);
    CHECK(sigmaHudText(d, 0.5) == "LOCKED (calibrating 50%) | private ~1e+300");
    CHECK(privateSigmaNoteText(d, 0.9) == "rank at 90%");

    SECTION("display: official band ids -> ladder rank id + division");
    std::string rid;
    int div = 0;
    CHECK(parseRankBandId("master-1", rid, div) && rid == "master" && div == 1);
    CHECK(parseRankBandId("Bronze-3", rid, div) && rid == "bronze" && div == 3);
    CHECK(parseRankBandId("grandmaster", rid, div) && rid == "grandmaster" && div == 0);
    CHECK(parseRankBandId("apex", rid, div) && rid == "apex" && div == 0);
    CHECK(parseRankBandId("ascendant-4", rid, div) && rid == "ascendant-4" && div == 0);
    CHECK(parseRankBandId("ascendant-2", rid, div) && rid == "ascendant-2" && div == 0);
    CHECK(parseRankBandId("master-9", rid, div) && rid == "master-9" && div == 0);   // not a division: kept whole
    CHECK(parseRankBandId("master-", rid, div) && rid == "master-" && div == 0);
    CHECK(!parseRankBandId("", rid, div) && rid.empty() && div == 0);

    RatingDisplay rd;
    CHECK(parseRatingDisplay("Ranked", rd) && rd == RatingDisplay::Ranked);
    CHECK(!parseRatingDisplay("", rd) && !parseRatingDisplay("rank", rd));
    CHECK(name(RatingDisplay::Visible) == "visible" && name(RatingDisplay::Ranked) == "ranked" && name(RatingDisplay::Locked) == "locked");
}

// calibration/0.2.0 (docs/RATING.md §6.1-§6.2): informative misses and the screen's words.
void testRequirements() {
    CalibrationParams p;

    SECTION("requirements: the model and its miss parameters mirror params.ts");
    CHECK(p.modelVersion == "calibration/0.4.0");
    CHECK(p.minInformativeMisses == 10.0 && p.boundarySamples == 30.0 && p.informativeMissMinPHit == 0.3);  // calibration/0.3.0

    SECTION("requirements: erfinv / window widths (difficulty.ts windowMsForDifficulty)");
    CHECK_NEAR(erfinv(0.5), 0.4769362762044699, 1e-14);
    CHECK_NEAR(erfinv(-0.3), -0.27246271472675443, 1e-14);
    CHECK(erfinv(0.0) == 0.0 && std::isinf(erfinv(1.0)) && std::isnan(erfinv(2.0)));
    for (double pr : {0.05, 0.3, 0.5, 0.9, 0.95, 0.999}) CHECK_NEAR(hitProbability(windowMsForHitProbability(130.0, pr), 130.0), pr, 1e-12);
    CHECK_NEAR(windowMsForHitProbability(130.0, 0.3), 5.928, 0.001);
    CHECK_NEAR(windowMsForHitProbability(130.0, 0.95), 30.154, 0.001);

    SECTION("requirements: a miss is informative at P(hit) >= 0.3, never without a fit");
    auto miss = [&](double pHit) {
        return makeSample(1, Gamemode::Wave, Speed::Normal, InputKind::Press, windowMsForHitProbability(130.0, pHit), false, "lvl", "geo");
    };
    CHECK(isInformativeMiss(miss(0.31), 130.0, p));
    CHECK(isInformativeMiss(miss(0.99), 130.0, p));   // above the band: the strongest upper-bound evidence
    CHECK(!isInformativeMiss(miss(0.29), 130.0, p));
    CHECK(!isInformativeMiss(miss(0.6), std::nullopt, p));
    auto hit = miss(0.6);
    hit.hit = true;
    CHECK(!isInformativeMiss(hit, 130.0, p));
    hit.wouldBeDeath = true;   // noclip would-be death = a miss (SPEC §19)
    CHECK(isInformativeMiss(hit, 130.0, p));
    // the count, not a share: 3 informative misses among 1000 comfortable hits stay 3
    std::vector<CalibrationSample> mix;
    for (int i = 0; i < 3; ++i) mix.push_back(miss(0.6));
    for (int i = 0; i < 1000; ++i)
        mix.push_back(makeSample(10 + i, Gamemode::Ship, Speed::Fast, InputKind::Press, 80.0, true, "lvl2", "geo-" + std::to_string(i)));
    auto rep = computeCalibration(mix, 130.0);
    CHECK(rep.informativeMisses && *rep.informativeMisses == 3.0);
    CHECK(rep.requirements[6].id == "misses" && rep.requirements[6].current == 3.0 && rep.requirements[6].required == p.minInformativeMisses);

    SECTION("requirements: the screen's words (calibration.ts calibrationHint)");
    RequirementCheck m{"misses", "Informative misses", 14.0, 21.0, 14.0 / 21.0, false};
    auto st = calibrationRequirementStatus(m, p);
    CHECK(st.remaining == 7.0 && !st.satisfied && st.hint);
    CHECK(st.hint && st.hint->find("You need about 7 more misses at timings you usually make (hit at least 30% of the time)") == 0);
    CHECK(calibrationHint("misses", 20.2, 21.0, false, p)->find("about 1 more miss at") != std::string::npos);
    CHECK(!calibrationHint("misses", 25.0, 21.0, true, p));
    CHECK(*calibrationHint("gamemodes", 2.0, 3.0, false, p) == "Play 1 more gamemode until each has 25 effective samples.");
    CHECK(*calibrationHint("levels", 1.0, 3.0, false, p) == "Play 2 more different rated demons.");
    CHECK(*calibrationHint("releases", 0.08, 0.15, false, p) ==
          "Play more held inputs (ship, wave, robot): releases are 8% of your samples, 15% needed.");
    auto rel = calibrationRequirementStatus(RequirementCheck{"releases", "Release share", 0.08, 0.15, 0.08 / 0.15, false}, p);
    CHECK_NEAR(rel.remaining, 0.07, 1e-12);
    // unmet never reads "about 0 more"; an unmet share is rounded down (calibration.ts)
    auto hair = calibrationRequirementStatus(RequirementCheck{"misses", "Informative misses", 21.0 - 1e-10, 21.0, 1.0, false}, p);
    CHECK(hair.remaining == 1.0 && hair.hint && hair.hint->find("You need about 1 more miss at") == 0);
    CHECK(calibrationHint("effectiveSamples", 699.5, 700.0, false, p)->find("about 1 more effective sample. New timings") != std::string::npos);
    CHECK(calibrationHint("effectiveSamples", 149.0, 700.0, false, p)->find("about 551 more effective samples.") != std::string::npos);
    CHECK(calibrationHint("releases", 0.146, 0.15, false, p)->find("releases are 14% of your samples, 15% needed.") != std::string::npos);
    CHECK(calibrationHint("releases", 0.29, 0.3, false, p)->find("releases are 29% of your samples, 30% needed.") != std::string::npos);
    // never a window width without a visible sigma/s (SPEC §10), the band's widths with one
    auto dc = calibrationHint("difficultyCoverage", 39.0, 60.0, false, p);
    CHECK(dc && dc->find(" ms") == std::string::npos && dc->find("between 30% and 95% of the time.") != std::string::npos);
    CHECK(calibrationHint("difficultyCoverage", 39.0, 60.0, false, p, 130.0)->find("(for you now: windows of about 5.9-30 ms)") != std::string::npos);
    CHECK(calibrationHint("misses", 14.0, 21.0, false, p, 130.0)->find("; for you now: windows of about 5.9 ms or wider)") != std::string::npos);
    auto local = calibrationRequirementStatuses(computeCalibration({}, std::nullopt));
    CHECK(local.size() == 8);
    for (auto const& s : local) CHECK_MSG(s.hint.has_value(), s.id + " has no hint");

    SECTION("requirements: parity with the server's words (tests/fixtures/api/me-calibration-*.json)");
    // The route passes the raw sigma/s only while it is visible (`sigma.raw`), never while locked.
    int compared = 0;
    for (char const* name : {"me-calibration-locked.json", "me-calibration-visible.json", "me-calibration-ranked.json",
                             "me-calibration-private.json"}) {
        std::string path = g_root + "/tests/fixtures/api/" + name;
        std::string text = test::readFile(path);
        CHECK_MSG(!text.empty(), "fixture missing: " + path);
        json::Value fx;
        json::ParseError err;
        if (text.empty() || !json::parse(text, fx, &err)) continue;
        CHECK(fx.getString("calibrationModel") == p.modelVersion);
        std::vector<CalibrationRequirementStatus> all;
        CHECK_MSG(calibrationRequirementsFromJson(fx, all), std::string(name) + ": no requirements");
        // Owner request 2026-10-02: the server puts one unmet `requiredGamemode` line per
        // `missingGamemodes` entry FIRST. Its words are the server's own (not the calibration
        // model this file mirrors): checked for order, shape and no width, then skipped.
        std::vector<std::string> missing;
        if (auto* mg = fx.find("missingGamemodes"); mg && mg->isArray())
            for (auto const& g : mg->asArray()) if (g.isString()) missing.push_back(g.asString());
        std::vector<CalibrationRequirementStatus> reqs;
        size_t gamemodeLines = 0;
        for (auto const& r : all) {
            if (r.id != "requiredGamemode") {
                reqs.push_back(r);
                continue;
            }
            CHECK_MSG(reqs.empty(), std::string(name) + ": a requiredGamemode line after the model's checks");
            CHECK(!r.satisfied && r.required == 1.0 && r.hint.has_value());
            CHECK(gamemodeLines < missing.size() && r.hint && r.hint->rfind("Calibrate " + missing[gamemodeLines] + ": play ", 0) == 0);
            CHECK(r.hint && r.hint->find(" ms") == std::string::npos);
            ++gamemodeLines;
        }
        CHECK(gamemodeLines == missing.size());
        CHECK(reqs.size() == 8);
        std::optional<double> visibleL;
        if (fx.getString("displayState") != "locked") {
            if (auto* raw = fx["sigma"].find("raw"); raw && raw->isNumber()) visibleL = raw->asNumber();
            CHECK_MSG(visibleL.has_value(), std::string(name) + ": visible without a raw figure");
        }
        for (auto const& r : reqs) {
            auto mine = calibrationHint(r.id, r.current, r.required, r.satisfied, p, visibleL);
            CHECK_MSG(mine == r.hint, std::string(name) + " " + r.id + ": C++ \"" + mine.value_or("<none>") + "\" vs server \"" + r.hint.value_or("<none>") + "\"");
            auto status = calibrationRequirementStatus(RequirementCheck{r.id, r.label, r.current, r.required, 0.0, r.satisfied}, p, visibleL);
            CHECK_NEAR(status.remaining, r.remaining, 1e-12);
            CHECK_NEAR(status.progress, r.progress, 1e-12);
            if (!visibleL && r.hint) CHECK_MSG(r.hint->find(" ms") == std::string::npos, std::string(name) + " " + r.id + " names a width while locked");
            ++compared;
        }
    }
    CHECK(compared == 32);   // 4 bodies x 8 requirements; the private body is locked: its hints name no width either
    // absent / null / malformed requirements: nothing (a row not yet recalculated under the model)
    std::vector<CalibrationRequirementStatus> none;
    json::Value v;
    json::parse(R"({"requirements":null})", v);
    CHECK(!calibrationRequirementsFromJson(v, none) && none.empty());
    json::parse(R"({})", v);
    CHECK(!calibrationRequirementsFromJson(v, none));
    json::parse(R"({"requirements":[{"id":"misses","current":"14","required":21,"satisfied":false}]})", v);
    CHECK(!calibrationRequirementsFromJson(v, none) && none.empty());
    CHECK(!calibrationRequirementsFromJson(json::Value(), none));
}

}  // namespace

// Owner request 2026-10-02: the gamemodes the sigma/s still waits for (root `missingGamemodes` of
// GET /v1/me/calibration) lead "what to play next" and name the lock.
void testMissingGamemodes() {
    auto parse = [](std::string const& text) {
        json::Value v;
        json::ParseError err;
        CHECK_MSG(json::parse(text, v, &err), "json: " + err.message);
        return v;
    };
    CalibrationDisplay d;

    SECTION("missingGamemodes: parsed from the root in a locked answer, server order kept");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","requiredGamemodes":["cube","ship","wave","ball"],"missingGamemodes":["ship","ball"],"overall":{"overallLocked":true,"complete":true}})"), d));
    CHECK(d.locked());
    CHECK((d.missingGamemodes == std::vector<std::string>{"ship", "ball"}));
    auto steps = missingGamemodeSteps(d);
    CHECK(steps.size() == 2);
    CHECK(steps[0] == "Calibrate Ship: play ship levels (needed before your sigma/s shows).");
    CHECK(steps[1] == "Calibrate Ball: play ball levels (needed before your sigma/s shows).");
    CHECK(missingGamemodesLockText(d) == "sigma/s LOCKED until ship and ball are calibrated");

    SECTION("missingGamemodes: one gamemode, UFO label, list wording");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","missingGamemodes":["ship"]})"), d));
    CHECK(missingGamemodesLockText(d) == "sigma/s LOCKED until ship is calibrated");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","missingGamemodes":["ufo","cube","wave"]})"), d));
    CHECK(missingGamemodeSteps(d)[0] == "Calibrate UFO: play ufo levels (needed before your sigma/s shows).");
    CHECK(missingGamemodesLockText(d) == "sigma/s LOCKED until ufo, cube and wave are calibrated");

    SECTION("missingGamemodes: tolerant - absent, malformed, unknown and repeated entries");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked"})"), d));
    CHECK(d.missingGamemodes.empty() && missingGamemodeSteps(d).empty() && missingGamemodesLockText(d).empty());
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","missingGamemodes":"ship"})"), d));
    CHECK(d.missingGamemodes.empty());
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","missingGamemodes":["ship",3,null,"jetpack","ship","SHIP","ball"]})"), d));
    CHECK((d.missingGamemodes == std::vector<std::string>{"ship", "ball"}));

    SECTION("missingGamemodes: read in every state (a visible answer with nothing missing)");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"visible","practicalSigma":121,"missingGamemodes":[]})"), d));
    CHECK(!d.locked() && d.missingGamemodes.empty());

    // v0.14.6: the server's `note` (why the last session gave nothing), ASCII for GD's fonts
    SECTION("sessionNote: the root `note`, every state, ASCII, bounded; null / absent / non-string = empty");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","note":"Your last session on Bloodbath (10565740) did not count: a mod menu without a GPRL state adapter was loaded (mod menu without a state adapter: Mega Hack (absolllute.megahack)). Sessions played without bots or changed physics feed calibration and ratings, on any level."})"), d));
    CHECK(d.sessionNote.rfind("Your last session on Bloodbath (10565740) did not count: a mod menu without a GPRL state adapter was loaded", 0) == 0);
    CHECK(calibrationDisplayFromJson(parse("{\"displayState\":\"visible\",\"practicalSigma\":121,\"note\":\"Noclip was on: no Practical \xCF\x83/s \xE2\x80\x93 reduced weight\xE2\x80\xA6\"}"), d));
    CHECK(d.sessionNote == "Noclip was on: no Practical sigma/s - reduced weight...");
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","note":null})"), d));
    CHECK(d.sessionNote.empty());
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked","note":42})"), d));
    CHECK(d.sessionNote.empty());
    CHECK(calibrationDisplayFromJson(parse(R"({"displayState":"locked"})"), d));
    CHECK(d.sessionNote.empty());
    CHECK(calibrationDisplayFromJson(parse("{\"displayState\":\"locked\",\"note\":\"" + std::string(900, 'n') + "\"}"), d));
    CHECK(d.sessionNote.size() == 320);
}

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "D:/GPRL";
    // shared/test/golden-fixtures.test.ts players
    testSyntheticPlayer("synthetic-player-a.json");
    testSyntheticPlayer("synthetic-player-b.json");
    testSyntheticPlayer("synthetic-player-c.json");
    // tools/fixture-gen players (a second, independent generator over the same TS engines)
    testSyntheticPlayer("synthetic-player-90.json");
    testSyntheticPlayer("synthetic-player-150.json");
    testSyntheticPlayer("synthetic-player-300.json");
    testBehaviour();
    testDisplay();
    testRequirements();
    testMissingGamemodes();
    return gprl::test::finish("calibration_tests");
}
