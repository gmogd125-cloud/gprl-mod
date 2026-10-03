// analyzer_identity_glue_tests: the pure glue between the analyzer's read-only extraction and the
// level identity core (core/analyzer_identity.hpp; docs/LEVEL_FAMILY_DESIGN.md §3, §8, FA-D11;
// docs/contracts/level-family.md): object roles, the trigger table, DecoReading -> DecoObject with
// the gameplay rect rule, the upload body and its 1 MB guard, the answer parser, the percent
// format, the Session-tab line and the on-disk cache record with its 24 h rule.
#include "../core/analyzer_identity.hpp"
#include "test_util.hpp"

#include <string>

using namespace gprl;
using namespace gprl::analyzer;
using Kind = gprl::identity::TriggerObject::Kind;

namespace {

/// core/json's reader returns bool + out; the tests read a value or fail loudly.
json::Value P(std::string const& text) {
    json::Value v;
    bool ok = json::parse(text, v);
    CHECK_MSG(ok, "JSON parse failed: " + text.substr(0, 60));
    return v;
}

void testRoles() {
    SECTION("roleOf: decoration, harmless triggers and collectibles go to the identity; gameplay never");
    extract::Classified c;
    c.use = extract::Use::Decoration;
    CHECK(family::roleOf(c) == family::Role::Deco);
    c.use = extract::Use::Skip;
    c.trigger = true;
    CHECK(family::roleOf(c) == family::Role::Trigger);
    c.trigger = false;
    CHECK(family::roleOf(c) == family::Role::Deco);   // collectibles: decoration for the presentation
    c.use = extract::Use::Gameplay;
    CHECK(family::roleOf(c) == family::Role::None);
    c.use = extract::Use::StartPos;
    CHECK(family::roleOf(c) == family::Role::None);
}

void testTriggerKinds() {
    SECTION("triggerKind: colour (2.0 and 1.x channel triggers), pulse, alpha, shake, camera, other");
    for (int id : {899, 29, 30, 105, 221, 717, 718, 743, 744, 900, 915}) CHECK_MSG(family::triggerKind(id) == Kind::Color, std::to_string(id));
    CHECK(family::triggerKind(1006) == Kind::Pulse);
    CHECK(family::triggerKind(1007) == Kind::Alpha);
    CHECK(family::triggerKind(1520) == Kind::Shake);
    CHECK(family::triggerKind(1913) == Kind::CameraZoom);
    CHECK(family::triggerKind(1914) == Kind::CameraStatic);
    CHECK(family::triggerKind(1916) == Kind::CameraOffset);
    CHECK(family::triggerKind(2015) == Kind::CameraRotate);
    CHECK(family::triggerKind(2062) == Kind::CameraEdge);
    CHECK(family::triggerKind(2925) == Kind::CameraMode);
    CHECK(family::triggerKind(901) == Kind::Other);    // move: gameplay (an unsupported span), never presentation
    CHECK(family::triggerKind(1934) == Kind::Other);   // song
    CHECK(family::triggerKind(0) == Kind::Other);

    SECTION("makeTrigger: duration kept only when finite and positive");
    family::TriggerReading t;
    t.objectId = 1006;
    t.x = 450.f;
    t.duration = 0.25f;
    auto o = family::makeTrigger(t);
    CHECK(o.kind == Kind::Pulse && o.x == 450.f && o.duration == 0.25f);
    t.duration = -1.f;
    CHECK(family::makeTrigger(t).duration == 0.f);
    t.duration = std::nanf("");
    CHECK(family::makeTrigger(t).duration == 0.f);
}

family::DecoReading deco(int id, float x, float y) {
    family::DecoReading r;
    r.objectId = id;
    r.gdType = 7;
    r.x = x;
    r.y = y;
    r.width = 30.f;
    r.height = 30.f;
    return r;
}

void testMakeDeco() {
    SECTION("makeDeco: GD's rect formula on the static fields; z, glow, blending, opacity, passable");
    auto r = deco(1, 300.f, 200.f);
    r.zLayer = 7;
    r.zOrder = 12;
    r.noGlow = true;
    r.blending = true;
    r.opacity = 128;
    auto d = family::makeDeco(r);
    CHECK(d.objectId == 1);
    CHECK(d.rx == 285.f && d.ry == 185.f && d.rw == 30.f && d.rh == 30.f);
    CHECK(d.zLayer == 7 && d.zOrder == 12 && d.noGlow && d.blending);
    CHECK_NEAR(d.opacityHint, 128.0 / 255.0, 1e-6);
    CHECK(d.passable);   // type 7 is passable by definition

    SECTION("makeDeco: unknown opacity = 1, out-of-range opacity clamped");
    r.opacity = -1;
    CHECK(family::makeDeco(r).opacityHint == 1.f);
    r.opacity = 400;
    CHECK(family::makeDeco(r).opacityHint == 1.f);

    SECTION("makeDeco: a moved object is placed at m_startPosition (the gameplay rect rule)");
    auto m = deco(1, 900.f, 200.f);
    m.haveStart = true;
    m.startX = 300.f;
    m.startY = 200.f;
    auto dm = family::makeDeco(m);
    CHECK_MSG(dm.rx < 400.f, "moved deco must use the start position, got rx " + std::to_string(dm.rx));

    SECTION("makeDeco: the same rule as gameplay objects (gdObjectRect): sprite scale, static scale, box offset");
    auto e = deco(1, 300.f, 200.f);
    e.scaleX = 2.f;
    e.spriteHeightScale = 0.5f;
    e.boxOffsetY = 10.f;
    auto de = family::makeDeco(e);
    CHECK(de.rw == 60.f && de.rh == 15.f);
    CHECK(de.rx == 270.f && de.ry == 202.5f);   // centre (300, 210)
    SECTION("makeDeco: decoration is never oriented (updateIsOriented skips type 7): a rotated one keeps GD's axis rect");
    auto r45 = deco(1, 300.f, 200.f);
    r45.rotation = 45.f;
    auto d45 = family::makeDeco(r45);
    CHECK(d45.rw == 30.f && d45.rh == 30.f);
    auto r90 = deco(1, 300.f, 200.f);           // ... but a quarter turn swaps w / h (m_isRotationAligned)
    r90.width = 10.f;
    r90.rotation = 90.f;
    auto d90 = family::makeDeco(r90);
    CHECK(d90.rw == 30.f && d90.rh == 10.f);
    SECTION("makeDeco: a negative static scale is normalised (origin + |size|)");
    auto neg = deco(1, 300.f, 200.f);
    neg.scaleX = -1.f;
    auto dn = family::makeDeco(neg);
    CHECK(dn.rw == 30.f && dn.rx == 285.f);

    SECTION("makeDeco: hazard look comes from the identity's collidable sprite table");
    auto s = deco(8, 300.f, 105.f);   // id 8 = the spike sprite (a hazard look when it is decoration)
    CHECK(family::makeDeco(s).hazardLook == family::looksCollidable(8));
    CHECK(family::looksCollidable(8));
    CHECK(!family::makeDeco(s).indicatorLook);   // /1: no indicator list
    auto solid = deco(1, 0.f, 0.f);
    solid.gdType = 0;
    solid.passable = false;
    solid.noTouch = false;
    CHECK(!family::makeDeco(solid).passable);
    solid.noTouch = true;
    CHECK(family::makeDeco(solid).passable);
}

void testUploadBody() {
    SECTION("uploadBody: sessionId first when present, the identity text untouched");
    std::string idJson = "{\"levelHash\":\"ab\",\"sections\":[]}";
    std::string b = family::uploadBody("sess-1", idJson);
    CHECK(b == "{\"sessionId\":\"sess-1\",\"identity\":" + idJson + "}");
    auto parsed = P(b);
    CHECK(parsed["sessionId"].asString() == "sess-1" && parsed["identity"].getString("levelHash") == "ab");
    CHECK(family::uploadBody("", idJson) == "{\"identity\":" + idJson + "}");
    CHECK(family::uploadBody("q\"x", idJson).find("\"q\\\"x\"") != std::string::npos);   // escaped

    SECTION("bodyWithinLimit / estimateBodyBytes: 1 MB guard; 400 sections stay far below it");
    CHECK(family::bodyWithinLimit(std::string(family::kMaxBodyBytes, 'x')));
    CHECK(!family::bodyWithinLimit(std::string(family::kMaxBodyBytes + 1, 'x')));
    CHECK(family::estimateBodyBytes(400, 10) < family::kMaxBodyBytes);
    CHECK(family::estimateBodyBytes(-5) == 400u);
}

char const* kAnswer = R"({
  "levelVersionId": "v-1",
  "family": {
    "id": "f-1", "name": "Sonic Wave", "canonicalGdLevelId": 26681070,
    "relationship": "startpos_derivative", "relationshipLabel": "StartPos Derivative",
    "gameplaySimilarity": 0.9997, "presentationSimilarity": 0.6814,
    "confidence": "confirmed", "confidenceLabel": "Confirmed Family Match",
    "startposRange": [65, 100], "mechanicalTransfer": "high", "visualTransfer": "moderate",
    "matchedGdLevelId": 26681070, "memberCount": 4
  },
  "notice": ["RELATED GAMEPLAY DETECTED", "Family: Sonic Wave", "Gameplay match: 99.9%", "", 7],
  "familiarity": { "mechanical": 0.82, "visual": 1.7, "fullAttempts": 8000, "practiceAttempts": 120, "startposAttempts": 3 }
})";

void testParseAnswer() {
    SECTION("parseAnswer: the contract's V1LevelIdentityResponse");
    auto v = P(kAnswer);
    family::FamilyAnswer a;
    std::string err;
    CHECK(family::parseAnswer(v, a, &err));
    CHECK(a.levelVersionId == "v-1" && a.familyId == "f-1" && a.name == "Sonic Wave");
    CHECK(a.canonicalGdLevelId == 26681070 && a.matchedGdLevelId == 26681070 && a.memberCount == 4);
    CHECK(a.relationship == "startpos_derivative" && a.relationshipLabel == "StartPos Derivative");
    CHECK(a.confidence == "confirmed" && a.confidenceLabel == "Confirmed Family Match");
    CHECK(a.hasStartposRange && a.startposFrom == 65.0 && a.startposTo == 100.0);
    CHECK(a.mechanicalTransfer == "high" && a.visualTransfer == "moderate");
    CHECK_MSG(a.notice.size() == 3, "empty strings and non-strings are dropped");
    CHECK(a.hasNotice && a.notice[0] == "RELATED GAMEPLAY DETECTED");
    CHECK_NEAR(a.mechanical, 0.82, 1e-12);
    CHECK(a.visual == 1.0);   // clamped
    CHECK(a.fullAttempts == 8000 && a.practiceAttempts == 120 && a.startposAttempts == 3);

    SECTION("parseAnswer: null notice, missing optional members, refusals");
    auto minimal = P(R"({"family":{"name":"X","relationship":"founder"},"notice":null})");
    CHECK(family::parseAnswer(minimal, a));
    CHECK(!a.hasNotice && a.notice.empty() && a.canonicalGdLevelId == -1 && a.relationshipLabel == "founder");
    CHECK(a.mechanicalTransfer == "none" && !a.hasStartposRange);
    CHECK(!family::parseAnswer(P(R"({"notice":[]})"), a, &err) && !err.empty());
    CHECK(!family::parseAnswer(P(R"({"family":{"name":"X"}})"), a, &err));
    CHECK(!family::parseAnswer(P("[1,2]"), a, &err));

    SECTION("parseAnswer: notice lines capped in count and length");
    std::string many = "{\"family\":{\"name\":\"X\",\"relationship\":\"founder\"},\"notice\":[";
    for (int i = 0; i < 30; ++i) many += std::string(i ? "," : "") + "\"" + std::string(500, 'a') + "\"";
    many += "]}";
    CHECK(family::parseAnswer(P(many), a));
    CHECK(a.notice.size() == family::kMaxNoticeLines);
    CHECK(a.notice[0].size() == family::kMaxNoticeLineChars);
}

void testFormat() {
    SECTION("matchPercent: floored to 0.1, never rounds a near match up to 100.0%");
    CHECK(family::matchPercent(0.9997) == "99.9%");
    CHECK(family::matchPercent(0.6814) == "68.1%");
    CHECK(family::matchPercent(1.0) == "100.0%");
    CHECK(family::matchPercent(0.0) == "0.0%");
    CHECK(family::matchPercent(-3.0) == "0.0%");
    CHECK(family::matchPercent(std::nan("")) == "0.0%");

    SECTION("familyLine: the Session tab's one line");
    family::FamilyAnswer a;
    a.name = "Sonic Wave";
    a.gameplaySimilarity = 0.9997;
    a.presentationSimilarity = 0.6814;
    a.relationshipLabel = "StartPos Derivative";
    std::string line = family::familyLine(a);
    CHECK(line.find("Family: Sonic Wave") == 0);
    CHECK(line.find("gameplay 99.9%") != std::string::npos && line.find("presentation 68.1%") != std::string::npos);
    CHECK(line.find("StartPos Derivative") != std::string::npos);
    a.name.clear();
    CHECK(family::familyLine(a).find("(unnamed)") != std::string::npos);
}

void testCacheRecord() {
    SECTION("cache record: round trip with and without the server answer");
    std::string idJson = "{\"levelHash\":\"ab\"}";
    std::string text = family::cacheRecordText("ab", 42, idJson, kAnswer, 1000, 2000, "sent");
    auto v = P(text);
    family::CacheRecord r;
    CHECK(family::parseCacheRecord(v, r));
    CHECK(r.version == family::kCacheVersion && r.levelHash == "ab" && r.gdLevelId == 42 && r.state == "sent");
    CHECK(r.hasIdentity && r.hasAnswer && r.answeredAtMs == 2000 && r.answer.name == "Sonic Wave");

    std::string noAnswer = family::cacheRecordText("ab", 42, idJson, "", 1000, 2000, "waiting for the session");
    CHECK(family::parseCacheRecord(P(noAnswer), r));
    CHECK(!r.hasAnswer && r.answeredAtMs == 0 && r.hasIdentity);

    SECTION("cache record: an unknown version is refused (a future format is never misread)");
    CHECK(!family::parseCacheRecord(P(R"({"version":"gprl-identity-cache/9"})"), r));
    // /1 records hold gprl-gameplay-fp/1 identities and the server's answer to them: never reused
    CHECK(std::string(family::kCacheVersion) == "gprl-identity-cache/2");
    CHECK(!family::parseCacheRecord(P(R"({"version":"gprl-identity-cache/1","levelHash":"ab","answer":null,"identity":{}})"), r));
    CHECK(!family::parseCacheRecord(P("3"), r));

    SECTION("answerFresh: 24 h from the answer, never for a missing answer or a clock going backwards");
    CHECK(family::parseCacheRecord(P(text), r));
    CHECK(family::answerFresh(r, 2000));
    CHECK(family::answerFresh(r, 2000 + family::kAnswerFreshMs - 1));
    CHECK(!family::answerFresh(r, 2000 + family::kAnswerFreshMs));
    CHECK(!family::answerFresh(r, 1999));
    CHECK(family::parseCacheRecord(P(noAnswer), r));
    CHECK(!family::answerFresh(r, 2000));
}

}  // namespace

int main() {
    testRoles();
    testTriggerKinds();
    testMakeDeco();
    testUploadBody();
    testParseAnswer();
    testFormat();
    testCacheRecord();
    return gprl::test::finish("analyzer_identity_glue_tests");
}
