// core/ranks host tests (the in-game GPRL menu, v0.3.0): both /api/ranks response shapes, the
// sigma -> rank / division lookup over a runtime ladder (Ascendant tiers included), progress to
// the next band, numerals, colours, threshold formatting, profile and leaderboard parsing
// (including the deployed API's LOCKED / empty answers of 2026-09-29).
#include "test_util.hpp"

#include "../core/json.hpp"
#include "../core/ranks.hpp"

#include <climits>
#include <cmath>
#include <string>

using namespace gprl;
using namespace gprl::ranks;

namespace {

json::Value parseJson(char const* text) {
    json::Value v;
    json::ParseError err;
    bool ok = json::parse(text, v, &err);
    CHECK_MSG(ok, "json parse: " + err.message);
    return v;
}

// The deployed API on 2026-09-29 (shape 1: RankDefinition[] with divisionThresholds), trimmed.
char const* kCurrentRanks = R"([
{"id":"bronze","name":"Bronze","order":0,"divisions":3,"requirements":{"minVerifiedSigma":0,"minConfidence":0.5,"minRatedGamemodes":1,"minVerifiedRuns":1},"color":"#c47a3a","iconKey":"bronze","description":"Calibrated players with their first verified runs.","divisionThresholds":[{"division":3,"minVerifiedSigma":0,"maxVerifiedSigma":20},{"division":2,"minVerifiedSigma":20,"maxVerifiedSigma":40},{"division":1,"minVerifiedSigma":40,"maxVerifiedSigma":60}],"playerCount":0,"maxVerifiedSigma":60},
{"id":"silver","name":"Silver","order":1,"divisions":3,"requirements":{"minVerifiedSigma":60,"minConfidence":0.6,"minRatedGamemodes":2,"minVerifiedRuns":3},"color":"#b8c2cc","iconKey":"silver","description":"Consistent timing on hard and insane demons.","divisionThresholds":[{"division":3,"minVerifiedSigma":60,"maxVerifiedSigma":68.33333333333333},{"division":2,"minVerifiedSigma":68.33333333333333,"maxVerifiedSigma":76.66666666666667},{"division":1,"minVerifiedSigma":76.66666666666667,"maxVerifiedSigma":85}],"playerCount":0,"maxVerifiedSigma":85},
{"id":"master","name":"Master","order":5,"divisions":3,"requirements":{"minVerifiedSigma":145,"minConfidence":0.85,"minRatedGamemodes":4,"minVerifiedRuns":20},"color":"#c084fc","iconKey":"master","description":"Elite timing precision with broad gamemode coverage.","divisionThresholds":[{"division":3,"minVerifiedSigma":145,"maxVerifiedSigma":155},{"division":2,"minVerifiedSigma":155,"maxVerifiedSigma":165},{"division":1,"minVerifiedSigma":165,"maxVerifiedSigma":175}],"playerCount":0,"maxVerifiedSigma":175},
{"id":"grandmaster","name":"Grandmaster","order":6,"divisions":1,"requirements":{"minVerifiedSigma":175,"minConfidence":0.9,"minRatedGamemodes":4,"minVerifiedRuns":30},"color":"#f43f5e","iconKey":"grandmaster","description":"The most precise verified players on GPRL.","divisionThresholds":[{"division":1,"minVerifiedSigma":175,"maxVerifiedSigma":null}],"playerCount":0,"maxVerifiedSigma":null}
])";

// The announced next shape (docs/RANKS.md): kinds, single bands, Ascendant tiers, eligibility.
char const* kNextRanks = R"({"ranks":[
{"id":"bronze","name":"Bronze","order":0,"kind":"divisions","color":"#c47a3a","iconKey":"bronze","divisions":[{"division":3,"minSigma":0,"maxSigma":20},{"division":2,"minSigma":20,"maxSigma":40},{"division":1,"minSigma":40,"maxSigma":60}],"minSigma":0,"maxSigma":60,"requirements":{"minConfidence":0.9,"minRatedGamemodes":1,"minVerifiedRuns":1}},
{"id":"master","name":"Master","order":5,"color":"#c084fc","iconKey":"master","divisions":[{"division":3,"minSigma":145,"maxSigma":155},{"division":2,"minSigma":155,"maxSigma":165},{"division":1,"minSigma":165,"maxSigma":175}],"minSigma":145,"maxSigma":175,"requirements":{"minConfidence":0.9}},
{"id":"grandmaster","name":"Grandmaster","order":6,"kind":"single","color":"#f43f5e","iconKey":"grandmaster","minSigma":175,"maxSigma":210,"requirements":{"minConfidence":0.9,"minVerifiedRuns":30}},
{"id":"elite","name":"Elite","order":7,"color":"#ff8c42","iconKey":"elite","minSigma":210,"maxSigma":250,"requirements":{"minConfidence":0.9}},
{"id":"apex","name":"Apex","order":8,"kind":"single","color":"#ffd166","iconKey":"apex","minSigma":250,"maxSigma":300,"requirements":{"minConfidence":0.95}},
{"id":"ascendant-1","name":"Ascendant I","order":9,"kind":"ascendant","color":"#e0f2fe","iconKey":"ascendant","minSigma":300,"maxSigma":330,"tier":1,"reached":true,"requirements":{"minConfidence":0.95}},
{"id":"ascendant-2","name":"Ascendant","order":10,"kind":"ascendant","color":"#e0f2fe","minSigma":330,"maxSigma":365,"tier":2,"reached":true,"requirements":{"minConfidence":0.95}},
{"id":"ascendant-3","name":"Ascendant III","order":11,"color":"#e0f2fe","minSigma":365,"maxSigma":null,"reached":false,"requirements":{"minConfidence":0.95}}
],"eligibility":{"minConfidence":0.9,"verification":"competitive"}})";

// The deployed API's answer for a fresh player on 2026-09-29 (everything locked, rank null).
char const* kLockedProfile = R"({"id":"7cae2c5c","username":"gmo12","displayName":"Gmo12","avatarUrl":null,"country":null,"rank":null,"leaderboardPosition":null,"verifiedSigma":null,"rawSigma":null,"practicalSigma":null,"confidence":null,"mainGamemode":null,"verifiedRuns":0,"bio":null,"joinedAt":"2026-09-30T00:57:02.51137+00:00","lastActiveAt":null,"ratings":{"availability":"locked","verified":null,"pending":null,"unverified":null,"raw":null,"practical":null},"gamemodes":[{"gamemode":"cube","availability":"locked","sigma":null,"confidence":null,"raw":null,"practical":null,"effectiveSamples":null,"calibrationProgress":0,"algorithmVersion":"0.1.0-experimental"},{"gamemode":"wave","availability":"locked","sigma":null,"calibrationProgress":0.25,"algorithmVersion":"0.1.0-experimental"}],"skills":null,"calibration":{"complete":false,"percent":0.12,"effectiveSamples":{"current":84,"required":700},"gamemodes":{"current":0,"required":3},"timingVariety":null,"patternVariety":null,"releaseData":"insufficient","difficultyCoverage":null,"overallLocked":true},"badges":[],"algorithmVersion":"0.1.0-experimental","stats":{"verifiedRuns":0},"identityVerified":true})";

char const* kRankedProfile = R"({"username":"pro","displayName":"Pro Player","identityVerified":true,"rank":{"rankId":"master","division":2},"leaderboardPosition":3,"verifiedSigma":159.7,"rawSigma":171.2,"practicalSigma":160.1,"confidence":0.94,"verifiedRuns":25,"ratings":{"availability":"available","verified":{"value":159.7,"confidence":0.94,"algorithmVersion":"x","updatedAt":"2026-09-29T00:00:00Z"},"raw":{"value":171.2,"confidence":0.94},"practical":{"value":160.1,"confidence":0.94}},"gamemodes":[{"gamemode":"cube","availability":"available","sigma":162.5,"calibrationProgress":1},{"gamemode":"ship","availability":"locked","sigma":null,"calibrationProgress":0.6}],"calibration":{"percent":1,"overallLocked":false},"rankProgress":{"nextName":"Master I","percent":47},"unverifiedRankEquivalent":{"rankId":"grandmaster","division":1},"verificationStatus":"verified"})";

char const* kEmptyBoard = R"({"items":[],"page":1,"pageSize":50,"total":0,"hasMore":false,"scope":"overall","period":"lifetime","algorithmVersion":"0.1.0-experimental","asOf":"2026-09-30T01:15:40.906Z"})";

char const* kBoard = R"({"items":[
{"position":1,"id":"a","username":"alpha","displayName":"Alpha","rank":{"rankId":"grandmaster","division":1},"verifiedSigma":181.2,"scopeSigma":181.2,"scopeConfidence":0.93},
{"position":2,"id":"b","username":"beta","displayName":"","rank":null,"verifiedSigma":150.55,"scopeSigma":150.55},
{"position":3,"player":{"username":"gamma","displayName":"Gamma","rank":{"id":"silver","division":3}},"verifiedSigma":61}
],"total":3,"asOf":"2026-09-30T01:15:40.906Z"})";

void testColors() {
    SECTION("parseHexColor / scaled");
    Color c;
    CHECK((parseHexColor("#c47a3a", c) && c == Color{0xc4, 0x7a, 0x3a}));
    CHECK((parseHexColor("B8C2CC", c) && c == Color{0xb8, 0xc2, 0xcc}));
    CHECK((parseHexColor("#fff", c) && c == Color{255, 255, 255}));
    CHECK((parseHexColor("#f43f5e", c) && c == Color{0xf4, 0x3f, 0x5e}));
    Color keep{1, 2, 3};
    CHECK((!parseHexColor("", keep) && keep == Color{1, 2, 3}));
    CHECK(!parseHexColor("#12345", keep));
    CHECK(!parseHexColor("#gggggg", keep));
    CHECK(!parseHexColor("red", keep));
    CHECK((scaled(Color{100, 200, 250}, 0.5) == Color{50, 100, 125}));
    CHECK((scaled(Color{100, 200, 250}, 2.0) == Color{200, 255, 255}));
}

void testNumerals() {
    SECTION("numeral / divisionNumeral");
    CHECK(numeral(1) == "I");
    CHECK(numeral(2) == "II");
    CHECK(numeral(3) == "III");
    CHECK(numeral(4) == "IV");
    CHECK(numeral(5) == "V");
    CHECK(numeral(9) == "IX");
    CHECK(numeral(14) == "XIV");
    CHECK(numeral(19) == "XIX");
    CHECK(numeral(20) == "XX");
    CHECK(numeral(40) == "XL");
    CHECK(numeral(49) == "XLIX");
    CHECK(numeral(99) == "XCIX");
    CHECK(numeral(1994) == "MCMXCIV");
    CHECK(numeral(3999) == "MMMCMXCIX");
    CHECK(numeral(0).empty());
    CHECK(numeral(-3).empty());
    CHECK(numeral(4000).empty());
    CHECK(divisionNumeral(1) == "I");
    CHECK(divisionNumeral(3) == "III");
    CHECK(divisionNumeral(0).empty());
    CHECK(divisionNumeral(4).empty());
}

void testFormatting() {
    SECTION("formatSigma / formatRange / percentText / formatRequirements");
    CHECK(formatSigma(60) == "60");
    CHECK(formatSigma(68.33333333333333) == "68.3");
    CHECK(formatSigma(76.66666666666667) == "76.7");
    CHECK(formatSigma(136.66666666666666) == "136.7");
    CHECK(formatSigma(159.97) == "160");
    CHECK(formatSigma(0) == "0");
    CHECK(formatRange(175, std::nullopt) == "175+");
    CHECK(formatRange(175, 210) == "175-210");
    CHECK(formatRange(68.33333333333333, 76.66666666666667) == "68.3-76.7");
    CHECK(percentText(0.12) == "12%");
    CHECK(percentText(0.999) == "99%");
    CHECK(percentText(1.0) == "100%");
    CHECK(percentText(0.0) == "0%");
    CHECK(percentText(-1.0) == "0%");
    CHECK(percentText(2.0) == "100%");
    CHECK((formatRequirements(Requirements{0, 0.9, 4, 30}) == "min confidence 90%, 4 gamemodes rated, 30 verified runs"));
    CHECK((formatRequirements(Requirements{0, 0.5, 1, 1}) == "min confidence 50%, 1 gamemode rated, 1 verified run"));
    CHECK(formatRequirements(Requirements{}) == "no extra requirements");
}

void testCurrentShape() {
    SECTION("parseRankList: current shape (RankDefinition[] with divisionThresholds)");
    RankList list;
    std::string err;
    CHECK_MSG(parseRankList(parseJson(kCurrentRanks), list, &err), err);
    CHECK(list.ranks.size() == 4);
    CHECK(!list.eligibilityMinConfidence);
    if (list.ranks.size() != 4) return;
    auto const& bronze = list.ranks[0];
    CHECK(bronze.id == "bronze");
    CHECK(bronze.name == "Bronze");
    CHECK(bronze.kind == Kind::Divisions);
    CHECK(bronze.hasDivisions());
    CHECK((bronze.color == Color{0xc4, 0x7a, 0x3a}));
    CHECK(bronze.iconKey == "bronze");
    CHECK(bronze.bands.size() == 3);
    CHECK(bronze.bands[0].division == 3 && bronze.bands[0].minSigma == 0 && bronze.bands[0].maxSigma == 20);
    CHECK(bronze.bands[1].division == 2 && bronze.bands[1].minSigma == 20);
    CHECK(bronze.bands[2].division == 1 && bronze.bands[2].minSigma == 40 && bronze.bands[2].maxSigma == 60);
    CHECK(bronze.requirements.minSigma == 0);
    CHECK(bronze.requirements.minConfidence == 0.5);
    CHECK(bronze.requirements.minRatedGamemodes == 1);
    CHECK(bronze.requirements.minVerifiedRuns == 1);
    CHECK(bronze.description == "Calibrated players with their first verified runs.");
    CHECK(formatBands(bronze) == "III 0-20  II 20-40  I 40-60");
    auto const& silver = list.ranks[1];
    CHECK(formatBands(silver) == "III 60-68.3  II 68.3-76.7  I 76.7-85");
    CHECK(silver.requirements.minSigma == 60);
    auto const& gm = list.ranks[3];
    CHECK(gm.kind == Kind::Single);   // "divisions": 1 with a single threshold = one band
    CHECK(!gm.hasDivisions());
    CHECK(gm.bands.size() == 1);
    CHECK(gm.bands[0].division == 0);
    CHECK(gm.bands[0].minSigma == 175);
    CHECK(!gm.bands[0].maxSigma);
    CHECK(formatBands(gm) == "175+");
    CHECK(gm.requirements.minSigma == 175);
    CHECK(badgeNumeral(gm, 1).empty());
    CHECK(badgeNumeral(bronze, 2) == "II");
    CHECK(bandName(gm, 1) == "Grandmaster");
    CHECK(bandName(bronze, 3) == "Bronze III");

    SECTION("placeSigma / progressOf over the current shape");
    auto p = placeSigma(list, 19.5);
    CHECK(p.rank == 0 && p.division == 3);
    p = placeSigma(list, 20.0);
    CHECK(p.rank == 0 && p.division == 2);
    p = placeSigma(list, 59.999);
    CHECK(p.rank == 0 && p.division == 1);
    p = placeSigma(list, 60.0);
    CHECK(p.rank == 1 && p.division == 3);
    p = placeSigma(list, 100.0);   // gold/platinum/diamond are not in this trimmed list: silver I holds
    CHECK(p.rank == 1 && p.division == 1);
    p = placeSigma(list, 145.0);
    CHECK(p.rank == 2 && p.division == 3);
    p = placeSigma(list, 500.0);
    CHECK(p.rank == 3 && p.division == 0);
    CHECK(bandName(list, p) == "Grandmaster");
    p = placeSigma(list, -1.0);
    CHECK(!p.valid());
    CHECK(bandName(list, p).empty());
    CHECK(!placeSigma(list, std::nan("")).valid());

    auto pr = progressOf(list, 10.0);
    CHECK(pr && pr->nextName == "Bronze II");
    if (pr) CHECK_NEAR(pr->percent, 50.0, 1e-9);
    pr = progressOf(list, 50.0);
    CHECK(pr && pr->nextName == "Silver III");
    if (pr) CHECK_NEAR(pr->percent, 50.0, 1e-9);
    pr = progressOf(list, 159.7);   // docs/RANKS.md example: 47% -> Master I
    CHECK(pr && pr->nextName == "Master I");
    if (pr) CHECK_NEAR(pr->percent, 47.0, 1e-9);
    pr = progressOf(list, 170.0);
    CHECK(pr && pr->nextName == "Grandmaster");
    if (pr) CHECK_NEAR(pr->percent, 50.0, 1e-9);
    CHECK(!progressOf(list, 200.0));   // top of this ladder: nothing above
    CHECK(!progressOf(list, -5.0));

    CHECK(findRank(list, "master") == 2);
    CHECK(findRank(list, "MASTER") == 2);
    CHECK(findRank(list, "elite") == -1);
    CHECK(bandOf(list.ranks[0], 1)->minSigma == 40);
    CHECK(bandOf(list.ranks[0], 0)->minSigma == 0);   // unknown division: lowest band
    CHECK(bandOf(list.ranks[3], 1)->minSigma == 175);
}

void testNextShape() {
    SECTION("parseRankList: next shape ({ ranks, eligibility }, kinds, Ascendant tiers)");
    RankList list;
    std::string err;
    CHECK_MSG(parseRankList(parseJson(kNextRanks), list, &err), err);
    CHECK(list.ranks.size() == 8);
    CHECK(list.eligibilityMinConfidence && *list.eligibilityMinConfidence == 0.9);
    if (list.ranks.size() != 8) return;
    CHECK(list.ranks[0].kind == Kind::Divisions && list.ranks[0].bands.size() == 3);
    CHECK(list.ranks[1].kind == Kind::Divisions);   // no kind, divisions listed
    CHECK(list.ranks[1].requirements.minSigma == 145);
    CHECK(list.ranks[2].kind == Kind::Single && list.ranks[2].bands.size() == 1);
    CHECK(list.ranks[2].bands[0].minSigma == 175 && list.ranks[2].bands[0].maxSigma == 210);
    CHECK(formatBands(list.ranks[2]) == "175-210");
    CHECK(list.ranks[3].kind == Kind::Single);   // no kind, no divisions -> single
    CHECK(list.ranks[3].iconKey == "elite");
    CHECK(formatBands(list.ranks[3]) == "210-250");
    CHECK(list.ranks[4].kind == Kind::Single && formatBands(list.ranks[4]) == "250-300");
    CHECK(list.ranks[4].requirements.minConfidence == 0.95);
    auto const& a1 = list.ranks[5];
    CHECK(a1.kind == Kind::Ascendant);
    CHECK(a1.tier == 1);
    CHECK(a1.name == "Ascendant I");
    CHECK(a1.iconKey == "ascendant");
    CHECK(a1.reached);
    CHECK(badgeNumeral(a1, 0) == "I");
    auto const& a2 = list.ranks[6];
    CHECK(a2.tier == 2);
    CHECK(a2.name == "Ascendant II");   // name "Ascendant" + tier -> numeral appended
    CHECK(a2.iconKey == "ascendant");   // missing iconKey on an ascendant tier -> the shared badge
    CHECK(badgeNumeral(a2, 0) == "II");
    CHECK(formatBands(a2) == "330-365");
    auto const& a3 = list.ranks[7];
    CHECK(a3.kind == Kind::Ascendant);   // no kind: id starts with "ascendant"
    CHECK(a3.tier == 3);                 // taken from the id "ascendant-3"
    CHECK(!a3.reached);
    CHECK(formatBands(a3) == "365+");
    CHECK(bandName(a3, 0) == "Ascendant III");

    auto p = placeSigma(list, 300.0);
    CHECK(p.rank == 5 && p.division == 0);
    p = placeSigma(list, 364.9);
    CHECK(p.rank == 6);
    p = placeSigma(list, 1000.0);
    CHECK(p.rank == 7);
    CHECK(bandName(list, p) == "Ascendant III");
    p = placeSigma(list, 209.99);
    CHECK(p.rank == 2);
    p = placeSigma(list, 100.0);   // gap in this trimmed ladder (no silver..diamond): the highest band below
    CHECK(p.rank == 0 && p.division == 1);

    auto pr = progressOf(list, 200.0);
    CHECK(pr && pr->nextName == "Elite");
    if (pr) CHECK_NEAR(pr->percent, 25.0 / 35.0 * 100.0, 1e-9);
    pr = progressOf(list, 340.0);
    CHECK(pr && pr->nextName == "Ascendant III");
    if (pr) CHECK_NEAR(pr->percent, 10.0 / 35.0 * 100.0, 1e-9);
    pr = progressOf(list, 174.9);
    CHECK(pr && pr->nextName == "Grandmaster");
    if (pr) CHECK_NEAR(pr->percent, 99.0, 1e-9);
    CHECK(!progressOf(list, 400.0));   // the unreached tier is the top: nothing above it
}

void testRankListTolerance() {
    SECTION("parseRankList: malformed input");
    RankList list;
    std::string err;
    CHECK(!parseRankList(parseJson(R"({"error":{"code":"x"}})"), list, &err));
    CHECK(!err.empty());
    CHECK(!parseRankList(json::Value(), list));
    CHECK(!parseRankList(json::Value("text"), list));
    // entries without id / name are skipped, bad colours fall back, unnumbered divisions are filled
    CHECK(parseRankList(parseJson(R"([{"foo":1},{"id":"x","color":"nope","divisionThresholds":[{"minVerifiedSigma":10},{"minVerifiedSigma":0},{"minVerifiedSigma":20,"maxVerifiedSigma":30}]}])"), list));
    CHECK(list.ranks.size() == 1);
    if (list.ranks.size() == 1) {
        auto const& x = list.ranks[0];
        CHECK(x.name == "x");
        CHECK((x.color == Color{200, 200, 200}));
        CHECK(x.kind == Kind::Divisions);
        CHECK(x.bands.size() == 3);
        CHECK(x.bands[0].minSigma == 0 && x.bands[0].division == 3);
        CHECK(x.bands[1].minSigma == 10 && x.bands[1].division == 2);
        CHECK(x.bands[2].minSigma == 20 && x.bands[2].division == 1);
        CHECK(x.requirements.minSigma == 0);
    }
    // ascendant tiers without tier numbers count up in ladder order
    CHECK(parseRankList(parseJson(R"({"ranks":[{"id":"apex","name":"Apex","minSigma":250},{"id":"ascendant","name":"Ascendant","kind":"ascendant","minSigma":300},{"id":"ascendant","name":"Ascendant","kind":"ascendant","minSigma":330}]})"), list));
    CHECK(list.ranks.size() == 3);
    if (list.ranks.size() == 3) {
        CHECK(list.ranks[1].tier == 1 && list.ranks[1].name == "Ascendant I");
        CHECK(list.ranks[2].tier == 2 && list.ranks[2].name == "Ascendant II");
        CHECK(list.ranks[0].kind == Kind::Single && formatBands(list.ranks[0]) == "250+");
    }
    CHECK(parseRankList(parseJson("[]"), list));
    CHECK(list.ranks.empty());
    CHECK(!placeSigma(list, 50).valid());
    CHECK(!progressOf(list, 50));
}

void testProfiles() {
    SECTION("parseProfile: deployed LOCKED profile");
    Profile p;
    std::string err;
    CHECK_MSG(parseProfile(parseJson(kLockedProfile), p, &err), err);
    CHECK(p.username == "gmo12");
    CHECK(p.displayName == "Gmo12");
    CHECK(p.identityVerified);
    CHECK(!p.rank);
    CHECK(p.ratingsLocked);
    CHECK(!p.verifiedSigma && !p.rawSigma && !p.practicalSigma && !p.confidence);
    CHECK_NEAR(p.calibrationPercent, 0.12, 1e-12);
    CHECK(p.leaderboardPosition == 0);
    CHECK(p.verifiedRuns == 0);
    CHECK(p.gamemodes.size() == 8);
    if (p.gamemodes.size() == 8) {
        CHECK(p.gamemodes[0].gamemode == Gamemode::Cube && p.gamemodes[0].locked && !p.gamemodes[0].sigma);
        CHECK(p.gamemodes[4].gamemode == Gamemode::Wave && p.gamemodes[4].calibrationProgress == 0.25);
        CHECK(p.gamemodes[7].gamemode == Gamemode::Swing && p.gamemodes[7].locked);   // not listed: locked
    }
    CHECK(!p.rankProgress);
    CHECK(!p.unverifiedRankEquivalent);
    CHECK(!p.competitiveVerified);
    RankList list;
    CHECK(parseRankList(parseJson(kCurrentRanks), list));
    CHECK(!profileProgress(p, &list));   // locked: no progress line
    CHECK(!profileProgress(p, nullptr));

    SECTION("parseProfile: ranked profile with the optional next-shape fields");
    CHECK_MSG(parseProfile(parseJson(kRankedProfile), p, &err), err);
    CHECK(!p.ratingsLocked);
    CHECK(p.rank && p.rank->rankId == "master" && p.rank->division == 2);
    CHECK(p.verifiedSigma && *p.verifiedSigma == 159.7);
    CHECK(p.rawSigma && *p.rawSigma == 171.2);
    CHECK(p.practicalSigma && *p.practicalSigma == 160.1);
    CHECK(p.confidence && *p.confidence == 0.94);
    CHECK(p.leaderboardPosition == 3);
    CHECK(p.verifiedRuns == 25);
    CHECK(p.gamemodes[0].sigma && *p.gamemodes[0].sigma == 162.5 && !p.gamemodes[0].locked);
    CHECK(p.gamemodes[1].locked && !p.gamemodes[1].sigma && p.gamemodes[1].calibrationProgress == 0.6);
    CHECK(p.rankProgress && p.rankProgress->nextName == "Master I" && p.rankProgress->percent == 47);
    CHECK(p.unverifiedRankEquivalent && p.unverifiedRankEquivalent->rankId == "grandmaster");
    CHECK(p.competitiveVerified && *p.competitiveVerified);
    auto pr = profileProgress(p, &list);
    CHECK(pr && pr->nextName == "Master I" && pr->percent == 47);   // the server's value wins
    // without the server's field the ladder gives the same answer
    Profile local = p;
    local.rankProgress.reset();
    pr = profileProgress(local, &list);
    CHECK(pr && pr->nextName == "Master I");
    if (pr) CHECK_NEAR(pr->percent, 47.0, 1e-9);
    CHECK(!profileProgress(local, nullptr));

    SECTION("parseProfile: tolerance");
    CHECK(!parseProfile(parseJson(R"({"displayName":"x"})"), p));   // no username
    CHECK(!parseProfile(json::Value(), p));
    // a summary sigma is dropped while ratings are locked; fraction percent; string rank; bool verification
    CHECK(parseProfile(parseJson(R"({"username":"u","verifiedSigma":99,"ratings":{"availability":"locked"},"rank":"gold","rankProgress":{"next":"Gold I","progress":0.5},"unverifiedEquivalent":"apex","competitiveVerified":false})"), p));
    CHECK(p.displayName == "u");
    CHECK(p.ratingsLocked && !p.verifiedSigma);
    CHECK(p.rank && p.rank->rankId == "gold" && p.rank->division == 0);
    CHECK(p.rankProgress && p.rankProgress->nextName == "Gold I" && p.rankProgress->percent == 50);
    CHECK(p.unverifiedRankEquivalent && p.unverifiedRankEquivalent->rankId == "apex");
    CHECK(p.competitiveVerified && !*p.competitiveVerified);
    CHECK(parseProfile(parseJson(R"({"username":"u","ratings":{"availability":"available","verified":{"value":70.5,"confidence":0.91}},"verification":{"status":"pending"}})"), p));
    CHECK(!p.ratingsLocked && p.verifiedSigma && *p.verifiedSigma == 70.5);
    CHECK(p.confidence && *p.confidence == 0.91);
    CHECK(p.competitiveVerified && !*p.competitiveVerified);
    pr = profileProgress(p, &list);
    CHECK(pr && pr->nextName == "Silver I");

    SECTION("parseProfile: sigma/s visible without a rank (docs/RANKS.md visibility thresholds)");
    // availability "visible": figures shown, rank withheld, the server's hint carried along
    CHECK(parseProfile(parseJson(R"({"username":"u","rank":null,"rankHint":"Rank at 90% confidence","ratings":{"availability":"visible","verified":{"value":123.4,"confidence":0.82},"raw":{"value":130},"practical":{"value":121}},"calibration":{"percent":0.886}})"), p));
    CHECK(!p.ratingsLocked && !p.rank);
    CHECK(p.verifiedSigma && *p.verifiedSigma == 123.4);
    CHECK(p.rawSigma && *p.rawSigma == 130.0 && p.practicalSigma && *p.practicalSigma == 121.0);
    CHECK(p.confidence && *p.confidence == 0.82);
    CHECK(p.rankHint == "Rank at 90% confidence");
    CHECK_NEAR(p.calibrationPercent, 0.886, 1e-12);
    // the hint may also sit under ratings; "available" with no rank is the same picture
    CHECK(parseProfile(parseJson(R"({"username":"u","ratings":{"availability":"available","rankHint":"Rank at 90% confidence","verified":{"value":80,"confidence":0.85}}})"), p));
    CHECK(!p.ratingsLocked && !p.rank && p.rankHint == "Rank at 90% confidence" && p.verifiedSigma && *p.verifiedSigma == 80.0);
    // "calibrating" / unknown: locked, figures redacted; a non-string hint is dropped
    CHECK(parseProfile(parseJson(R"({"username":"u","rankHint":"x","ratings":{"availability":"calibrating","verified":{"value":80,"confidence":0.85}}})"), p));
    CHECK(p.ratingsLocked && !p.verifiedSigma);
    CHECK(parseProfile(parseJson(R"({"username":"u","rankHint":7,"ratings":{"availability":"shown","verified":{"value":80}}})"), p));
    CHECK(p.ratingsLocked && !p.verifiedSigma && p.rankHint.empty());
}

void testLeaderboard() {
    SECTION("parseLeaderboard: empty (deployed today)");
    Leaderboard b;
    std::string err;
    CHECK_MSG(parseLeaderboard(parseJson(kEmptyBoard), b, &err), err);
    CHECK(b.rows.empty());
    CHECK(b.total == 0);
    CHECK(b.asOf == "2026-09-30T01:15:40.906Z");

    SECTION("parseLeaderboard: rows");
    CHECK_MSG(parseLeaderboard(parseJson(kBoard), b, &err), err);
    CHECK(b.rows.size() == 3);
    CHECK(b.total == 3);
    if (b.rows.size() == 3) {
        CHECK(b.rows[0].position == 1 && b.rows[0].username == "alpha" && b.rows[0].displayName == "Alpha");
        CHECK(b.rows[0].rank && b.rows[0].rank->rankId == "grandmaster" && b.rows[0].rank->division == 1);
        CHECK(b.rows[0].sigma && *b.rows[0].sigma == 181.2);
        CHECK(b.rows[1].displayName == "beta");   // empty displayName -> username
        CHECK(!b.rows[1].rank);
        CHECK(b.rows[1].sigma && *b.rows[1].sigma == 150.55);
        CHECK(b.rows[2].username == "gamma");     // nested player object
        CHECK(b.rows[2].rank && b.rows[2].rank->rankId == "silver" && b.rows[2].rank->division == 3);
        CHECK(b.rows[2].sigma && *b.rows[2].sigma == 61);
    }
    CHECK(!parseLeaderboard(parseJson(R"({"error":{}})"), b, &err));
    CHECK(!parseLeaderboard(json::Value(), b));
    CHECK(parseLeaderboard(parseJson(R"([{"username":"solo"},{"nothing":true}])"), b));
    CHECK(b.rows.size() == 1 && b.rows[0].position == 1 && b.total == 2);
}

// Hostile JSON: wrong types everywhere, huge / non-finite numbers, missing arrays, empty
// strings, repeated division numbers. Nothing here may crash, trap or wrap; the parsers keep what
// makes sense and skip the rest.
void testHostileJson() {
    SECTION("parseRankList: hostile input");
    RankList list;
    std::string err;
    CHECK(!parseRankList(parseJson(R"({"ranks":"x"})"), list, &err));
    CHECK(!parseRankList(parseJson(R"({"ranks":{"id":"bronze"}})"), list));
    CHECK(!parseRankList(parseJson("42"), list));
    CHECK(!parseRankList(parseJson("\"\""), list));
    CHECK(parseRankList(parseJson(R"([1,"a",null,true,[],{"id":5,"name":7},{"id":"","name":""}])"), list));
    CHECK(list.ranks.empty());
    // every member of the wrong type on one rank
    CHECK(parseRankList(parseJson(R"([{"id":"x","name":["n"],"order":"first","kind":42,"color":123,"iconKey":false,"description":{},
        "divisionThresholds":"nope","requirements":"nope","playerCount":"3","tier":{},"reached":"no","minVerifiedSigma":"9","maxVerifiedSigma":[1]}])"), list));
    CHECK(list.ranks.size() == 1);
    if (list.ranks.size() == 1) {
        auto const& x = list.ranks[0];
        CHECK(x.name == "x");
        CHECK(x.kind == Kind::Single);
        CHECK(x.bands.size() == 1 && x.bands[0].minSigma == 0 && !x.bands[0].maxSigma);
        CHECK(x.playerCount == 0 && x.tier == 0 && x.reached);
        CHECK(x.iconKey == "x");
        CHECK(formatBands(x) == "0+");
        CHECK(formatRequirements(x.requirements) == "no extra requirements");
    }
    // huge and non-finite numbers: saturate, never wrap; bands with garbage entries are skipped
    CHECK(parseRankList(parseJson(R"([{"id":"big","order":-1e300,"playerCount":1e300,"tier":-1e300,
        "requirements":{"minConfidence":1e400,"minRatedGamemodes":1e300,"minVerifiedRuns":-1e300},
        "divisionThresholds":[{"division":1e300,"minVerifiedSigma":1e308,"maxVerifiedSigma":-1e308},{"division":-5,"minVerifiedSigma":"str"},42,null,
                              {"division":2,"minVerifiedSigma":10,"maxVerifiedSigma":1e400},{"division":1,"minVerifiedSigma":20}]}])"), list));
    CHECK(list.ranks.size() == 1);
    if (list.ranks.size() == 1) {
        auto const& big = list.ranks[0];
        CHECK(big.playerCount == INT_MAX);
        CHECK(big.tier == 0);
        CHECK(big.requirements.minRatedGamemodes == INT_MAX && big.requirements.minVerifiedRuns == 0);
        CHECK(big.requirements.minConfidence == 0.0);   // 1e400 parses as +inf: not a finite number, ignored
        CHECK(big.bands.size() == 3);                   // 1e308 band, 10 band (max +inf -> open), 20 band
        if (big.bands.size() == 3) {
            CHECK(big.bands[0].minSigma == 10 && !big.bands[0].maxSigma && big.bands[0].division == 2);
            CHECK(big.bands[1].minSigma == 20 && big.bands[1].division == 1);
            CHECK(big.bands[2].minSigma == 1e308 && big.bands[2].division == INT_MAX);
        }
        std::string text = formatBands(big);
        CHECK(!text.empty());
        CHECK(text.find("1e+308") != std::string::npos);
        auto p = placeSigma(list, 15.0);
        CHECK(p.rank == 0 && p.division == 2);
        auto pr = progressOf(list, 15.0);
        CHECK(pr && pr->nextName == "big I");
        if (pr) CHECK_NEAR(pr->percent, 50.0, 1e-9);
        CHECK(placeSigma(list, 1e308).rank == 0);
        CHECK(!progressOf(list, 1e308));
        std::string name = bandName(big, INT_MAX);
        CHECK(name == "big");
    }
    // Ascendant tier numbers beyond the Roman range: no dangling "Ascendant " name, empty numeral
    CHECK(parseRankList(parseJson(R"({"ranks":[{"id":"ascendant-99999999999999999999","kind":"ascendant","name":"Ascendant","tier":1e300}]})"), list));
    CHECK(list.ranks.size() == 1);
    if (list.ranks.size() == 1) {
        CHECK(list.ranks[0].tier == INT_MAX);
        CHECK(list.ranks[0].name == "Ascendant");
        CHECK(badgeNumeral(list.ranks[0], 0).empty());
        CHECK(list.ranks[0].iconKey == "ascendant");
    }
    CHECK(parseRankList(parseJson(R"({"ranks":[{"id":"ascendant","kind":"ascendant","name":"Ascendant","tier":0}]})"), list));
    CHECK(list.ranks.size() == 1 && list.ranks[0].tier == 1 && list.ranks[0].name == "Ascendant I");
    // repeated division numbers: placement and progress still follow the band bounds
    CHECK(parseRankList(parseJson(R"({"ranks":[{"id":"a","name":"A","divisions":[{"division":2,"minSigma":0,"maxSigma":10},{"division":2,"minSigma":10,"maxSigma":20}]},
        {"id":"b","name":"B","minSigma":20}]})"), list));
    CHECK(list.ranks.size() == 2);
    if (list.ranks.size() == 2) {
        CHECK(list.ranks[0].kind == Kind::Divisions && list.ranks[0].bands.size() == 2);
        auto p = placeSigma(list, 15.0);
        CHECK(p.rank == 0 && p.division == 2);
        auto pr = progressOf(list, 15.0);
        CHECK(pr && pr->nextName == "B");
        if (pr) CHECK_NEAR(pr->percent, 50.0, 1e-9);
        pr = progressOf(list, 5.0);
        CHECK(pr && pr->nextName == "A II");
        if (pr) CHECK_NEAR(pr->percent, 50.0, 1e-9);
    }
    // eligibility of the wrong type
    CHECK(parseRankList(parseJson(R"({"ranks":[],"eligibility":"strict"})"), list));
    CHECK(list.ranks.empty() && !list.eligibilityMinConfidence);
    CHECK(parseRankList(parseJson(R"({"ranks":[],"eligibility":{"minConfidence":"0.9"}})"), list));
    CHECK(!list.eligibilityMinConfidence);

    SECTION("formatting: extreme values");
    CHECK(formatSigma(1e300) == "1e+300");
    CHECK(formatSigma(-1e300) == "-1e+300");
    CHECK(formatSigma(INFINITY) == "?");
    CHECK(formatSigma(-INFINITY) == "?");
    CHECK(formatSigma(std::nan("")) == "?");
    CHECK(formatSigma(-0.0) == "0");
    CHECK(formatSigma(999999999.0) == "999999999");
    CHECK(formatSigma(-3.25) == "-3.3" || formatSigma(-3.25) == "-3.2");   // rounding mode, no crash either way
    CHECK(formatRange(1e300, std::nullopt) == "1e+300+");
    CHECK(numeral(INT_MAX).empty());
    CHECK(numeral(INT_MIN).empty());
    CHECK(percentText(1e300) == "100%");
    CHECK(percentText(-1e300) == "0%");
    CHECK(formatRequirements(Requirements{0, 1e300, INT_MAX, INT_MIN}) == "min confidence 100%, " + std::to_string(INT_MAX) + " gamemodes rated");
    Color c{1, 2, 3};
    CHECK(!parseHexColor("#", c));
    CHECK(!parseHexColor("#ffffffff", c));
    CHECK(!parseHexColor("##fff", c));
    CHECK((scaled(Color{255, 255, 255}, 1e300) == Color{255, 255, 255}));
    CHECK((scaled(Color{255, 255, 255}, -1.0) == Color{0, 0, 0}));

    SECTION("parseProfile: hostile input");
    Profile p;
    CHECK(!parseProfile(parseJson(R"({"username":""})"), p));
    CHECK(!parseProfile(parseJson(R"({"username":42})"), p));
    CHECK(!parseProfile(parseJson("[]"), p));
    CHECK(!parseProfile(parseJson("null"), p));
    CHECK(parseProfile(parseJson(R"({"username":"u","displayName":123,"identityVerified":"yes","rank":42,"ratings":"locked",
        "gamemodes":[1,null,"cube",{"gamemode":"cube","sigma":"x","availability":"available","calibrationProgress":1e300},{"gamemode":"laser","sigma":5},
                     {"gamemode":"ship","availability":"available","sigma":1e400},{"gamemode":"ball","availability":7,"calibrationProgress":-1e300}],
        "calibration":[],"leaderboardPosition":-1e300,"verifiedRuns":1e300,"confidence":"high",
        "rankProgress":"x","unverifiedRankEquivalent":{"rankId":""},"unverifiedEquivalent":"","competitiveVerified":"yes","verification":42,"verificationStatus":""})"), p));
    CHECK(p.displayName == "u");
    CHECK(!p.identityVerified);
    CHECK(!p.rank);
    CHECK(p.ratingsLocked);
    CHECK(!p.confidence);
    CHECK(p.calibrationPercent == 0.0);
    CHECK(p.leaderboardPosition == 0);
    CHECK(p.verifiedRuns == INT_MAX);
    CHECK(p.gamemodes.size() == 8);
    if (p.gamemodes.size() == 8) {
        CHECK(!p.gamemodes[0].locked && !p.gamemodes[0].sigma && p.gamemodes[0].calibrationProgress == 1.0);   // "x" is not a sigma
        CHECK(!p.gamemodes[1].locked && !p.gamemodes[1].sigma);                                                // +inf is not a sigma
        CHECK(p.gamemodes[2].locked && p.gamemodes[2].calibrationProgress == 0.0);                            // availability 7 -> locked
        CHECK(p.gamemodes[7].locked);
    }
    CHECK(!p.rankProgress);
    CHECK(!p.unverifiedRankEquivalent);
    CHECK(!p.competitiveVerified);
    CHECK(!profileProgress(p, nullptr));
    // ratings.available with non-numeric values: unlocked but no figures; rankProgress without a name is dropped
    CHECK(parseProfile(parseJson(R"({"username":"u","ratings":{"availability":"AVAILABLE","verified":{"value":"n","confidence":null},"raw":"r","practical":[]},
        "verifiedSigma":"9","rankProgress":{"percent":50},"calibration":{"percent":"p"}})"), p));
    CHECK(!p.ratingsLocked && !p.verifiedSigma && !p.rawSigma && !p.practicalSigma && !p.confidence);
    CHECK(!p.rankProgress);
    RankList ladder;
    CHECK(parseRankList(parseJson(kCurrentRanks), ladder));
    CHECK(!profileProgress(p, &ladder));   // unlocked but no sigma: nothing to place
    // rankProgress percent as a huge number clamps; a negative one clamps to 0
    CHECK(parseProfile(parseJson(R"({"username":"u","rankProgress":{"nextName":"X","percent":1e300}})"), p));
    CHECK(p.rankProgress && p.rankProgress->percent == 100.0);
    CHECK(parseProfile(parseJson(R"({"username":"u","rankProgress":{"nextName":"X","percent":-7}})"), p));
    CHECK(p.rankProgress && p.rankProgress->percent == 0.0);
    // an unlocked profile with an absurd sigma still formats and places without crashing
    CHECK(parseProfile(parseJson(R"({"username":"u","ratings":{"availability":"available","verified":{"value":1e300}}})"), p));
    CHECK(p.verifiedSigma && *p.verifiedSigma == 1e300);
    CHECK(!profileProgress(p, &ladder));   // above every band: top of the ladder
    CHECK(formatSigma(*p.verifiedSigma) == "1e+300");

    SECTION("parseLeaderboard: hostile input");
    Leaderboard b;
    CHECK(!parseLeaderboard(parseJson(R"({"items":{}})"), b));
    CHECK(!parseLeaderboard(parseJson(R"({"items":"x"})"), b));
    CHECK(!parseLeaderboard(parseJson("\"\""), b));
    CHECK(!parseLeaderboard(parseJson("7"), b));
    CHECK(parseLeaderboard(parseJson(R"({"items":[{"position":"1","username":123,"displayName":""},{"player":"x","username":"ok","verifiedSigma":"n","scopeSigma":null,"rank":""},
        {"player":{"username":"nested","rank":{"rankId":"gold","division":99}},"position":1e300,"verifiedSigma":1e400},
        {"username":"neg","position":-1e300,"scopeSigma":-1e300},null,5,[]],"total":"x","asOf":42})"), b));
    CHECK(b.rows.size() == 3);
    CHECK(b.total == 7);   // "x" is not a count: the item count
    CHECK(b.asOf.empty());
    if (b.rows.size() == 3) {
        CHECK(b.rows[0].username == "ok" && b.rows[0].displayName == "ok" && b.rows[0].position == 2 && !b.rows[0].rank && !b.rows[0].sigma);
        CHECK(b.rows[1].username == "nested" && b.rows[1].position == INT_MAX && !b.rows[1].sigma);
        CHECK(b.rows[1].rank && b.rows[1].rank->rankId == "gold" && b.rows[1].rank->division == 0);
        CHECK(b.rows[2].username == "neg" && b.rows[2].position == 4);   // a non-positive position falls back to the row number
        CHECK(b.rows[2].sigma && *b.rows[2].sigma == -1e300);
        CHECK(formatSigma(*b.rows[2].sigma) == "-1e+300");
    }
    CHECK(parseLeaderboard(parseJson(R"({"items":[],"total":-1e300})"), b));
    CHECK(b.rows.empty() && b.total == 0);
}

}  // namespace

int main() {
    testColors();
    testNumerals();
    testFormatting();
    testCurrentShape();
    testNextShape();
    testRankListTolerance();
    testProfiles();
    testLeaderboard();
    testHostileJson();
    return test::finish("menu_tests");
}
