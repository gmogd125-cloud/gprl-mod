#pragma once
// Level families, the mod's glue between the analyzer's read-only extraction and the pure identity
// core (docs/LEVEL_FAMILY_DESIGN.md §3, §8, FA-D2, FA-D11; docs/contracts/level-family.md). PURE
// C++20, header-only, no Geode / cocos / GD include; host-tested in
// tests/analyzer_identity_glue_tests.cpp.
//
// What lives here (everything that decides WHAT the extraction read for the identity, so the mod
// file src/analyzer/Extract.cpp only reads binding members into plain numbers):
//   - the role of an object the physics ignores: decoration (type 7 and every non-gameplay object
//     that is not a trigger: collectibles) or a trigger (colour / pulse / alpha / shake / camera /
//     every other harmless trigger), from the extraction's own classification;
//   - the trigger id -> TriggerObject::Kind table (GD 2.2 object ids);
//   - DecoReading -> identity::DecoObject with the SAME rect rule as gameplay objects (GD's
//     getObjectRect formula on the level's static fields at m_startPosition, extract::gdObjectRect),
//     TriggerReading -> identity::TriggerObject;
//   - the upload body `{ sessionId?, identity }` (text assembly: the identity JSON is large and is
//     not parsed a second time) and its 1 MB guard (LEVEL_IDENTITY_LIMITS / the route's 413);
//   - the V1LevelIdentityResponse parser (FamilyAnswer), the Session tab's one-line summary, the
//     percent format of the contract (floored to 0.1, so a near match never reads 100.0%);
//   - the on-disk cache record `identity/<levelHash>.json` { identity, answer, timestamps } and its
//     24 h freshness rule (a revisit inside it sends nothing and re-shows the cached notice).
//
// READ-ONLY RULE (the whole analyzer, AN-D1 / AN-D11): nothing here touches the game; this file is
// plain data mapping and text.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "analyzer_extract.hpp"
#include "identity/identity.hpp"
#include "identity/presentation.hpp"
#include "json.hpp"

namespace gprl::analyzer::family {

// /2 (2026-10-02): the identity versions moved to gprl-gameplay-fp/2, gprl-section-fp/2,
// gprl-presentation-fp/2 (GD's exact rect formula on static fields): a /1 record (identity + the
// server's answer to it) is never reused
constexpr char const* kCacheVersion = "gprl-identity-cache/2";
constexpr size_t kMaxBodyBytes = 1024u * 1024u;            // the route's raw-body limit (413 above it)
constexpr int64_t kAnswerFreshMs = 24ll * 60 * 60 * 1000;   // a cached server answer is reused this long
constexpr size_t kMaxNoticeLines = 12;
constexpr size_t kMaxNoticeLineChars = 200;

// identity::DecoObject is kept at 40 bytes (100k decoration objects = 4 MB on the game thread
// while the walk runs); a wider struct here would silently double that.
static_assert(sizeof(gprl::identity::DecoObject) <= 40, "DecoObject grew past 40 bytes");

// ---- what an object the physics ignores is for the identity ----

enum class Role : uint8_t {
    None = 0,   // a gameplay object (in the World), a StartPos, or anything else the identity gets from the World
    Deco,       // type 7 decoration and every non-trigger object the physics ignores (collectibles)
    Trigger,    // a harmless trigger: colour / pulse / alpha / shake / camera / song / ... (TriggerObject)
};

/// From the extraction's own classification (core/analyzer_extract.hpp classify()).
inline Role roleOf(extract::Classified const& c) {
    switch (c.use) {
        case extract::Use::Decoration: return Role::Deco;
        case extract::Use::Skip: return c.trigger ? Role::Trigger : Role::Deco;
        case extract::Use::Gameplay:
        case extract::Use::StartPos: return Role::None;
    }
    return Role::None;
}

/// GD 2.2 object ids of the triggers the presentation fingerprint counts (docs §3 flashes /
/// cameraEffects). Colour: 899 (2.0 Color trigger) plus the 1.x channel triggers 29 BG, 30 Ground,
/// 105 Line, 221 Obj, 717 3DL, 718 Col1, 743 Col2, 744 Col3, 900 Col4, 915 Ground2. Everything else
/// is Other (counted nowhere, kept for the decoration count only).
inline gprl::identity::TriggerObject::Kind triggerKind(int objectId) {
    using Kind = gprl::identity::TriggerObject::Kind;
    switch (objectId) {
        case 899: case 29: case 30: case 105: case 221: case 717: case 718: case 743: case 744: case 900: case 915: return Kind::Color;
        case 1006: return Kind::Pulse;
        case 1007: return Kind::Alpha;
        case 1520: return Kind::Shake;
        case 1913: return Kind::CameraZoom;
        case 1914: return Kind::CameraStatic;
        case 1916: return Kind::CameraOffset;
        case 2015: return Kind::CameraRotate;
        case 2062: return Kind::CameraEdge;
        case 2925: return Kind::CameraMode;
        default: return Kind::Other;
    }
}

/// "Looks like something the player could collide with": the identity's own sprite table
/// (core/identity/presentation.cpp collidableSpriteKind, the 337 ids with a hitbox in gdclone's
/// object.json). In /1 the extraction has no separate decoration-only spike list, so the hint
/// equals the table (isFakeObject ORs the two; the result is the same).
inline bool looksCollidable(int objectId) { return gprl::identity::collidableSpriteKind(objectId) != gprl::identity::SpriteKind::None; }

/// Everything the mod READ from one decoration object (Extract.cpp names the binding member of each).
/// The geometry is the level's STATIC state, exactly like the gameplay objects' (core/analyzer_extract.hpp).
struct DecoReading {
    int objectId = 0;                  // m_objectID
    int gdType = 7;                    // m_objectType
    float x = 0.f, y = 0.f;            // getRealPosition(): (float)m_positionX / m_positionY (used without a start position)
    bool haveStart = false;            // startX / startY were read
    float startX = 0.f, startY = 0.f;  // m_startPosition (static: the rect is computed here)
    float rotation = 0.f;              // m_startRotationX
    float rotationY = 0.f;             // m_startRotationY
    bool haveRotationY = false;
    float scaleX = 1.f, scaleY = 1.f;  // m_startScaleX / m_startScaleY
    bool flipX = false, flipY = false; // m_startFlipX / m_startFlipY
    float width = 0.f, height = 0.f;   // m_width / m_height
    float spriteWidthScale = 1.f, spriteHeightScale = 1.f;   // m_spriteWidthScale / m_spriteHeightScale
    bool mirroredByScale = false;      // m_isMirroredByScale
    float boxOffsetX = 0.f, boxOffsetY = 0.f;   // m_customBoxOffset
    float objectRadius = 0.f;          // m_objectRadius
    int zLayer = 0;                    // m_zLayer (ZLayer as int: B4 -3 .. T3 9, 0 default)
    int zOrder = 0;                    // m_zOrder
    bool noGlow = false;               // m_hasNoGlow
    bool blending = false;             // m_baseOrDetailBlending (additive blending on either sprite)
    int opacity = -1;                  // 0..255 from a STATIC field; -1 = unknown (-> hint 1). Extract.cpp passes -1:
                                       // CCNodeRGBA::getOpacity() is the live, fading value (not hash-stable)
    bool noTouch = false;              // m_isNoTouch
    bool passable = false;             // m_isPassable
};

/// The identity's DecoObject. Rect rule = the gameplay objects' (WorldBuilder::add): GD's
/// getObjectRect formula (extract::gdObjectRect) on the static fields at m_startPosition (the
/// position as read when none was read), never GD's rect cache. `passable` = m_isPassable ||
/// m_isNoTouch || type Decoration (7).
inline gprl::identity::DecoObject makeDeco(DecoReading const& r) {
    gprl::identity::DecoObject d;
    d.objectId = r.objectId;
    extract::RectInputs in;
    in.x = r.haveStart ? r.startX : r.x;
    in.y = r.haveStart ? r.startY : r.y;
    in.width = r.width;
    in.height = r.height;
    in.spriteWidthScale = r.spriteWidthScale;
    in.spriteHeightScale = r.spriteHeightScale;
    in.scaleX = r.scaleX;
    in.scaleY = r.scaleY;
    in.rotationX = r.rotation;
    in.rotationY = r.haveRotationY ? r.rotationY : r.rotation;
    in.flipX = r.flipX;
    in.flipY = r.flipY;
    in.mirroredByScale = r.mirroredByScale;
    in.customBoxOffsetX = r.boxOffsetX;
    in.customBoxOffsetY = r.boxOffsetY;
    in.objectRadius = r.objectRadius;
    in.gdType = r.gdType;
    in.noTouch = r.noTouch;
    extract::Rect rect = extract::gdObjectRect(in).rect;
    extract::normalizeRect(rect);
    d.rx = rect.x;
    d.ry = rect.y;
    d.rw = rect.w;
    d.rh = rect.h;
    d.zLayer = r.zLayer;
    d.zOrder = r.zOrder;
    d.noGlow = r.noGlow;
    d.blending = r.blending;
    d.opacityHint = r.opacity < 0 ? 1.f : static_cast<float>(std::clamp(r.opacity, 0, 255)) / 255.f;
    d.hazardLook = looksCollidable(r.objectId);
    d.indicatorLook = false;   // /1: no arrow / marker id list in the extraction (documented in docs §3)
    d.passable = r.passable || r.noTouch || r.gdType == 7;
    return d;
}

struct TriggerReading {
    int objectId = 0;       // m_objectID
    float x = 0.f;          // getPosition().x
    float duration = 0.f;   // EffectGameObject::m_duration (0 when the object is no EffectGameObject)
};

inline gprl::identity::TriggerObject makeTrigger(TriggerReading const& r) {
    gprl::identity::TriggerObject t;
    t.objectId = r.objectId;
    t.x = r.x;
    t.kind = triggerKind(r.objectId);
    t.duration = std::isfinite(r.duration) && r.duration > 0.f ? r.duration : 0.f;
    return t;
}

// ---- the upload ----

/// `{ "sessionId": "<id>", "identity": <identityJson> }`; the sessionId member is omitted when
/// empty (the route takes it as optional). `identityJson` is identity::toJson's text as is.
inline std::string uploadBody(std::string const& sessionId, std::string const& identityJson) {
    std::string body = "{";
    if (!sessionId.empty()) body += "\"sessionId\":" + json::stringify(json::Value(sessionId)) + ",";
    body += "\"identity\":" + identityJson + "}";
    return body;
}

inline bool bodyWithinLimit(std::string const& body) { return body.size() <= kMaxBodyBytes; }

/// A rough size of the identity JSON from its section count (for the log before the text exists):
/// 64 minhash values of up to 10 digits + the 11 presentation features + the scalar fields
/// ~ 1.2 KB per section, plus ~400 bytes of level-wide fields and ~90 bytes per StartPos.
inline size_t estimateBodyBytes(int sections, int startPos = 0) {
    return 400u + static_cast<size_t>(std::max(0, sections)) * 1200u + static_cast<size_t>(std::max(0, startPos)) * 90u;
}

// ---- the answer (V1LevelIdentityResponse) ----

struct FamilyAnswer {
    std::string levelVersionId;
    std::string familyId;
    std::string name;
    int64_t canonicalGdLevelId = -1;          // -1 = null
    std::string relationship;                 // founder | exact_match | ... (FamilyMemberRelationship)
    std::string relationshipLabel;            // "Family Founder", "StartPos Derivative", ...
    double gameplaySimilarity = 0.0;          // 0..1
    double presentationSimilarity = 0.0;      // 0..1
    std::string confidence;                   // confirmed | probable | partial_derivative | uncertain | unrelated
    std::string confidenceLabel;
    bool hasStartposRange = false;
    double startposFrom = 0.0, startposTo = 0.0;
    std::string mechanicalTransfer;           // high | moderate | partial | none
    std::string visualTransfer;
    int64_t matchedGdLevelId = -1;            // -1 = null
    int64_t memberCount = 0;
    bool hasNotice = false;                   // notice was a non-empty array
    std::vector<std::string> notice;          // the lines to show (capped)
    double mechanical = 0.0, visual = 0.0;    // familiarity 0..1
    int64_t fullAttempts = 0, practiceAttempts = 0, startposAttempts = 0;
};

inline double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

/// Tolerant parse of the route's answer: `family.name` and `family.relationship` are required
/// (strings), everything else has a default. `notice` null / absent = no notice; strings only,
/// at most kMaxNoticeLines lines of kMaxNoticeLineChars.
inline bool parseAnswer(json::Value const& body, FamilyAnswer& out, std::string* err = nullptr) {
    out = FamilyAnswer{};
    if (!body.isObject()) {
        if (err) *err = "answer is not an object";
        return false;
    }
    auto const& fam = body["family"];
    if (!fam.isObject()) {
        if (err) *err = "answer without `family`";
        return false;
    }
    out.levelVersionId = body.getString("levelVersionId");
    out.familyId = fam.getString("id");
    out.name = fam.getString("name");
    out.relationship = fam.getString("relationship");
    if (out.relationship.empty()) {
        if (err) *err = "family without `relationship`";
        return false;
    }
    out.relationshipLabel = fam.getString("relationshipLabel");
    if (out.relationshipLabel.empty()) out.relationshipLabel = out.relationship;
    if (auto const* v = fam.find("canonicalGdLevelId"); v && v->isNumber()) out.canonicalGdLevelId = v->asInt();
    out.gameplaySimilarity = clamp01(fam.getNumber("gameplaySimilarity"));
    out.presentationSimilarity = clamp01(fam.getNumber("presentationSimilarity"));
    out.confidence = fam.getString("confidence");
    out.confidenceLabel = fam.getString("confidenceLabel", out.confidence);
    if (auto const* v = fam.find("startposRange"); v && v->isArray() && v->asArray().size() == 2 && (*v)[0].isNumber() && (*v)[1].isNumber()) {
        out.hasStartposRange = true;
        out.startposFrom = (*v)[0].asNumber();
        out.startposTo = (*v)[1].asNumber();
    }
    out.mechanicalTransfer = fam.getString("mechanicalTransfer", "none");
    out.visualTransfer = fam.getString("visualTransfer", "none");
    if (auto const* v = fam.find("matchedGdLevelId"); v && v->isNumber()) out.matchedGdLevelId = v->asInt();
    out.memberCount = fam.getInt("memberCount");
    if (auto const* n = body.find("notice"); n && n->isArray()) {
        for (auto const& line : n->asArray()) {
            if (!line.isString() || out.notice.size() >= kMaxNoticeLines) continue;
            std::string s = line.asString();
            if (s.size() > kMaxNoticeLineChars) s.resize(kMaxNoticeLineChars);
            if (!s.empty()) out.notice.push_back(std::move(s));
        }
        out.hasNotice = !out.notice.empty();
    }
    auto const& f = body["familiarity"];
    if (f.isObject()) {
        out.mechanical = clamp01(f.getNumber("mechanical"));
        out.visual = clamp01(f.getNumber("visual"));
        out.fullAttempts = f.getInt("fullAttempts");
        out.practiceAttempts = f.getInt("practiceAttempts");
        out.startposAttempts = f.getInt("startposAttempts");
    }
    return true;
}

/// The contract's formatMatchPercent: a 0..1 similarity as a percent floored to 0.1 ("99.7%"), so a
/// near match never reads 100.0%.
inline std::string matchPercent(double similarity) {
    double v = clamp01(similarity) * 1000.0;
    double floored = std::floor(v + 1e-6) / 10.0;
    char b[32];
    std::snprintf(b, sizeof b, "%.1f%%", floored);
    return b;
}

/// The Session tab's one line: "Family: <name> · gameplay 99.7% · presentation 68.1% · <label>"
/// (the status printer folds the middle dots to "-" for GD's ASCII fonts).
inline std::string familyLine(FamilyAnswer const& a) {
    std::string name = a.name.empty() ? std::string("(unnamed)") : a.name;
    return "Family: " + name + " \xC2\xB7 gameplay " + matchPercent(a.gameplaySimilarity) + " \xC2\xB7 presentation " + matchPercent(a.presentationSimilarity)
        + " \xC2\xB7 " + a.relationshipLabel;
}

// ---- the on-disk cache record (identity/<levelHash>.json) ----

struct CacheRecord {
    std::string version;
    int64_t savedAtMs = 0;
    std::string levelHash;
    int gdLevelId = 0;
    bool hasIdentity = false;
    bool hasAnswer = false;
    int64_t answeredAtMs = 0;
    FamilyAnswer answer;
    std::string state;         // the upload state text when it was written
};

/// Text assembly (the identity and the answer are already JSON text). `answerJson` "" = null.
inline std::string cacheRecordText(std::string const& levelHash, int gdLevelId, std::string const& identityJson, std::string const& answerJson,
                                   int64_t savedAtMs, int64_t answeredAtMs, std::string const& state) {
    std::string t = "{\"version\":" + json::stringify(json::Value(kCacheVersion));
    t += ",\"savedAt\":" + json::formatNumber(static_cast<double>(savedAtMs));
    t += ",\"levelHash\":" + json::stringify(json::Value(levelHash));
    t += ",\"gdLevelId\":" + json::formatNumber(static_cast<double>(gdLevelId));
    t += ",\"state\":" + json::stringify(json::Value(state));
    t += ",\"answeredAt\":" + json::formatNumber(static_cast<double>(answerJson.empty() ? 0 : answeredAtMs));
    t += ",\"answer\":" + (answerJson.empty() ? std::string("null") : answerJson);
    t += ",\"identity\":" + (identityJson.empty() ? std::string("null") : identityJson) + "}";
    return t;
}

inline bool parseCacheRecord(json::Value const& v, CacheRecord& out) {
    out = CacheRecord{};
    if (!v.isObject()) return false;
    out.version = v.getString("version");
    if (out.version != kCacheVersion) return false;
    out.savedAtMs = v.getInt("savedAt");
    out.levelHash = v.getString("levelHash");
    out.gdLevelId = static_cast<int>(v.getInt("gdLevelId"));
    out.state = v.getString("state");
    out.hasIdentity = v["identity"].isObject();
    out.answeredAtMs = v.getInt("answeredAt");
    auto const& a = v["answer"];
    if (a.isObject() && parseAnswer(a, out.answer)) out.hasAnswer = true;
    return true;
}

/// A revisit inside kAnswerFreshMs of the answer sends nothing (the notice is re-shown from it).
inline bool answerFresh(CacheRecord const& r, int64_t nowMs) {
    return r.hasAnswer && r.answeredAtMs > 0 && nowMs >= r.answeredAtMs && nowMs - r.answeredAtMs < kAnswerFreshMs;
}

}  // namespace gprl::analyzer::family
