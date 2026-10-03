// core/fingerprint host tests against the golden fixtures both implementations must reproduce:
//   tests/fixtures/familiarity/basic.json            similarity(a, b) cases
//   tests/fixtures/familiarity/effective-weight.json  familiarityWeight cases + identical-series sums
// The TypeScript engine (shared/src/rating/fingerprint.ts, familiarity.ts) is the reference.
// argv[1] = repository root.
#include "test_util.hpp"
#include "fixture_util.hpp"

#include "../core/fingerprint.hpp"

using namespace gprl;

namespace {

std::string g_root;

bool loadFixture(char const* rel, json::Value& out) {
    std::string path = g_root + "/tests/fixtures/" + rel;
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    if (text.empty()) return false;
    json::ParseError err;
    bool ok = json::parse(text, out, &err);
    CHECK_MSG(ok, "fixture parse error: " + err.message);
    return ok;
}

void testSimilarityFixture() {
    SECTION("familiarity/basic.json similarity cases");
    json::Value fx;
    if (!loadFixture("familiarity/basic.json", fx)) return;
    CHECK(fx.getString("algorithmVersion") == "0.1.0-experimental");
    FingerprintParams params = test::fingerprintParamsFromJson(fx["params"]);
    // the defaults in fingerprint.hpp must equal the fixture's parameter set
    FingerprintParams defaults;
    CHECK(defaults.hardMismatch == params.hardMismatch && defaults.nullMismatch == params.nullMismatch);
    CHECK(defaults.softMismatch.geometryHash == params.softMismatch.geometryHash && defaults.numericScales.logWindow == params.numericScales.logWindow);
    int cases = 0;
    for (auto const& c : fx["cases"].asArray()) {
        TimingFingerprint a, b;
        std::string err;
        CHECK_MSG(telemetry::fingerprintFromJson(c["a"], a, &err), err);
        CHECK_MSG(telemetry::fingerprintFromJson(c["b"], b, &err), err);
        double expected = c.getNumber("expected");
        double tol = c.getNumber("tolerance", 1e-9);
        double got = similarity(a, b, params);
        CHECK_MSG(test::near(got, expected, tol), c.getString("name") + ": got " + std::to_string(got) + " expected " + std::to_string(expected));
        // symmetric, and the breakdown multiplies to the same total
        CHECK_NEAR(similarity(b, a, params), got, 1e-12);
        auto breakdown = explainSimilarity(a, b, params);
        double product = 1.0;
        for (auto const& f : breakdown.factors) product *= f.second;
        CHECK_NEAR(std::min(1.0, std::max(0.0, product)), got, 1e-12);
        CHECK(breakdown.factors.size() == 15);
        ++cases;
    }
    CHECK_MSG(cases >= 15, "expected the fixture to hold many cases, got " + std::to_string(cases));
}

void testEffectiveWeightFixture() {
    SECTION("familiarity/effective-weight.json weight cases");
    json::Value fx;
    if (!loadFixture("familiarity/effective-weight.json", fx)) return;
    FamiliarityParams fam = test::familiarityParamsFromJson(fx["params"]);
    FamiliarityParams defaults;
    CHECK(defaults.alpha == fam.alpha && defaults.beta == fam.beta && defaults.attemptAlpha == fam.attemptAlpha
          && defaults.timingHistoryOverride == fam.timingHistoryOverride && defaults.memoryDays == fam.memoryDays
          && defaults.form == fam.form && defaults.saturationK == fam.saturationK);
    for (auto const& c : fx["weightCases"].asArray()) {
        SimilarityMass mass;
        mass.mass = c.getNumber("similarityMass");
        auto r = familiarityWeight(mass, static_cast<int>(c.getInt("attemptsAtCluster")), fam);
        CHECK_MSG(test::near(r.weight, c.getNumber("expectedWeight"), c.getNumber("tolerance", 1e-9)),
                  c.getString("name") + ": got " + std::to_string(r.weight));
        CHECK_MSG(name(r.source) == c.getString("source"), c.getString("name") + ": source " + std::string(name(r.source)));
    }

    SECTION("identical series: n identical timings one minute apart, attemptsAtCluster = index");
    TimingFingerprint fp;
    fp.gamemode = Gamemode::Wave;
    fp.speed = Speed::Fast;
    fp.kind = InputKind::Press;
    fp.windowMs = 8.0;
    fp.yVelocity = 120.0;
    fp.trajectory = Trajectory::Rising;
    fp.horizontalState = HorizontalState::Air;
    fp.prevInputGapMs = 120.0;
    fp.nextInputGapMs = 130.0;
    fp.holdMs = 60.0;
    fp.inputDirection = InputDirection::Up;
    fp.geometryHash = "geo-0001";
    for (auto const& c : fx["identicalSeries"]["cases"].asArray()) {
        int n = static_cast<int>(c.getInt("count"));
        std::vector<FingerprintAt> history;
        double total = 0.0;
        for (int i = 0; i < n; ++i) {
            double t = 1'700'000'000'000.0 + i * 60'000.0;
            auto r = familiarityWeight(similarityMass(fp, t, history, fam), i, fam);
            total += r.weight;
            history.push_back({fp, t});
        }
        CHECK_MSG(test::near(total, c.getNumber("expectedEffectiveSamples"), c.getNumber("tolerance", 1e-6)),
                  "n=" + std::to_string(n) + ": got " + std::to_string(total) + " expected " + std::to_string(c.getNumber("expectedEffectiveSamples")));
    }
}

void testMemoryAndPatternKey() {
    SECTION("memoryDays filter, minSimilarity floor, patternKey");
    FamiliarityParams fam;
    TimingFingerprint fp;
    fp.geometryHash = "g";
    double now = 1'800'000'000'000.0;
    std::vector<FingerprintAt> history;
    history.push_back({fp, now - (fam.memoryDays + 1) * 86'400'000.0});   // too old
    history.push_back({fp, now - 86'400'000.0});                          // yesterday
    auto m = similarityMass(fp, now, history, fam);
    CHECK(m.neighbours == 1);
    CHECK_NEAR(m.mass, 1.0, 1e-12);
    // unknown times (0) are never filtered
    std::vector<FingerprintAt> untimed = {{fp, 0.0}, {fp, 0.0}};
    CHECK(similarityMass(fp, 0.0, untimed, fam).neighbours == 2);
    // a hard mismatch contributes nothing
    TimingFingerprint other = fp;
    other.kind = InputKind::Release;
    CHECK(similarityMass(other, now, history, fam).neighbours == 0);
    CHECK(patternKey(fp) == "cube|press|g");
    TimingFingerprint unknownGeo = fp;
    unknownGeo.geometryHash.clear();
    CHECK_NEAR(similarity(fp, unknownGeo), 1.0, 1e-12);   // unknown geometry is neutral
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "D:/GPRL";
    testSimilarityFixture();
    testEffectiveWeightFixture();
    testMemoryAndPatternKey();
    return gprl::test::finish("fingerprint_tests");
}
