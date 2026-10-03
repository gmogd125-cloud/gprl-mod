#pragma once
// Level identities of the level-family system (docs/LEVEL_FAMILY_DESIGN.md §1-§3, FA-D1..FA-D4):
//
//   gameplay fingerprint   gprl-gameplay-fp/2    FNV-1a 64 over the sorted tuple hashes of EVERY
//                                                gameplay object with absolute positions; StartPos
//                                                objects and decoration excluded (a StartPos version
//                                                and a re-decoration keep the fingerprint)
//   sections               gprl-section-fp/2     the level cut at gamemode portals, speed changes,
//                                                size portals, dual / mirror / teleport portals and
//                                                every 600 units at most; per section a hash over the
//                                                tuples with x RELATIVE to the section start, a
//                                                64-value MinHash signature, gamemode / speed / mini
//                                                and the presentation feature vector
//   presentation           gprl-presentation-fp/2 the per-section feature vector of §3, a readability
//                                                score and a digest of the quantized vectors
//
// PURE C++20: compiled by the mod (worker thread, after the analyzer's extraction) and by the host
// tests (geode/tests/level_identity_tests.cpp, goldens in tests/fixtures/identity/). The
// TypeScript matcher (shared/src/level-family/match.ts) READS the JSON of `toJson`; it never
// recomputes a hash. `jaccardEstimate` and `presentationSimilarity` are the reference for its port.
//
// Namespace note: core/identity.hpp (the GD-account connection rules) shares `gprl::identity`;
// the two headers define disjoint names.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../json.hpp"
#include "../sim/world.hpp"

namespace gprl::identity {

using sim::Gamemode;
using sim::ObjKind;
using sim::SimObject;
using sim::Speed;
using sim::World;

// /2 (2026-10-02): the algorithms are unchanged, their INPUTS changed - the mod's extraction
// (gprl-extract/2) now gives every object GD's exact getObjectRect on the level's static fields
// (rotation / scale / flips / position at load), skips GD's anti-cheat spike and no longer reads
// the live opacity; identities of /1 and /2 are never compared
constexpr char const* kGameplayFingerprintVersion = "gprl-gameplay-fp/2";
constexpr char const* kSectionVersion = "gprl-section-fp/2";
constexpr char const* kPresentationVersion = "gprl-presentation-fp/2";

constexpr int kMinhashSize = 64;
using MinHash = std::array<uint32_t, kMinhashSize>;

/// One decoration object as the extraction keeps it (type 7 and every object the physics ignores).
struct DecoObject {
    int objectId = 0;
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;   // getObjectRect(): origin (bottom-left) and size
    int zLayer = 0;                                 // GameObject::m_zLayer (B4 -3, B3 -1, B2 1, B1 3, T1 5, T2 7, T3 9; 0 = default -> T1)
    int zOrder = 0;                                 // GameObject::m_zOrder
    bool noGlow = false;                            // m_hasNoGlow
    bool blending = false;                          // additive blending
    float opacityHint = 1.f;                        // 0..1, 1 = unknown / opaque
    bool hazardLook = false;                        // the extraction's "looks like a hazard" hint
    bool indicatorLook = false;                     // arrow / marker hint
    bool passable = true;                           // no collision (always true for type 7)
};

/// One trigger object: its kind and duration where the field exists.
struct TriggerObject {
    int objectId = 0;
    float x = 0.f;
    enum Kind : uint8_t { Color = 0, Pulse, Alpha, Shake, CameraZoom, CameraOffset, CameraRotate, CameraStatic, CameraEdge, CameraMode, Other } kind = Other;
    float duration = 0.f;                           // seconds (0 when the trigger has no duration)
};

struct DecoSet {
    std::vector<DecoObject> deco;
    std::vector<TriggerObject> triggers;
};

struct IdentityInput {
    std::string levelHash;      // sha256 of the level string (env::hashLevel); World::levelHash when empty
    int gdLevelId = 0;          // World::gdLevelId when 0
    int copiedFromGdId = 0;     // GD level key 30 (a HINT: never a fingerprint input)
    std::string nameHint;       // never a fingerprint input
};

/// §3 per-section presentation feature vector (units in the table of docs §3).
struct PresentationVector {
    double decoDensity = 0.0;      // decoration objects per 100 units
    double obstruction = 0.0;      // share of gameplay rect area overlapped by decoration drawn in front
    double clutter = 0.0;          // share of the section's bounding area covered by decoration (15-unit grid)
    double fakeObjects = 0.0;      // collidable-looking decoration without collision, per 100 units
    double hiddenGameplay = 0.0;   // hidden / >= 50 % obstructed gameplay objects, per 100 units
    double flashes = 0.0;          // short colour / pulse / alpha triggers + shakes, per 100 units
    double cameraEffects = 0.0;    // camera triggers, per 100 units
    double glowShare = 0.0;        // share of decoration with glow
    double opacityLow = 0.0;       // share of decoration with an opacity hint < 0.5
    double previewSeconds = 0.0;   // seconds from the section start to its first hazard / orb (capped 3)
    double indicators = 0.0;       // arrow / marker objects, per 100 units
};
constexpr int kPresentationFeatures = 11;

struct SectionIdentity {
    int index = 0;
    float xFrom = 0.f, xTo = 0.f;
    double pctFrom = 0.0, pctTo = 0.0;
    std::string hash;              // 16 lowercase hex digits
    MinHash minhash{};
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool mini = false;
    int objects = 0;               // gameplay objects hashed into the section
    PresentationVector presentation;
    double readability = 0.0;      // §3 weighted penalty: 0 = perfectly sight-readable, 1 = worst
};

struct StartPosEntry {
    float x = 0.f;
    double percent = 0.0;
    Gamemode mode = Gamemode::Cube;
    bool mini = false;
    Speed speed = Speed::Normal;
    bool upsideDown = false;
    bool dual = false;
};

struct LevelIdentity {
    std::string gameplayFingerprintVersion = kGameplayFingerprintVersion;
    std::string sectionVersion = kSectionVersion;
    std::string presentationVersion = kPresentationVersion;
    std::string levelHash;
    std::string gameplayFingerprint;       // 16 hex, StartPos ignored
    std::string presentationFingerprint;   // 16 hex digest of the quantized presentation vectors
    int gdLevelId = 0;
    int copiedFromGdId = 0;
    std::string nameHint;
    std::vector<SectionIdentity> sections;
    std::vector<StartPosEntry> startPos;
    int gameplayObjects = 0;
    int decorationObjects = 0;
    float lengthX = 0.f;
};

/// Every constant of the three algorithms (reported in tests/fixtures/identity/cases.json for the
/// TypeScript port of `readability` / `presentationSimilarity`).
struct IdentityConfig {
    // §2 cut rule
    float maxSectionLength = 600.f;      // a span between forced cuts longer than this is split into
                                         // n = ceil(len / max) EQUAL pieces (each >= max / 2 >= min)
    float minSectionLength = 150.f;      // a forced cut (portal) may still produce a shorter section
    double positionQuantum = 0.1;        // q(v) = round(v / quantum) = round(v * 10)
    // §3 normalisation scales: norm_f = clamp(value / scale, 0, 1)
    double decoDensityScale = 500.0;
    double obstructionScale = 1.0;
    double clutterScale = 1.0;
    double fakeObjectsScale = 5.0;
    double hiddenGameplayScale = 5.0;
    double flashesScale = 5.0;
    double cameraEffectsScale = 2.0;
    double glowShareScale = 1.0;
    double opacityLowScale = 1.0;
    double previewCapSeconds = 3.0;      // previewSeconds cap; norm = previewSeconds / cap; lowPreview = 1 - norm
    double indicatorsScale = 2.0;
    // §3 readability weights (sum 1)
    double wObstruction = 0.3;
    double wFlashes = 0.15;
    double wFakeObjects = 0.15;
    double wHiddenGameplay = 0.15;
    double wCameraEffects = 0.1;
    double wClutter = 0.05;
    double wLowPreview = 0.1;
    // §3 feature rules
    double flashMaxDuration = 0.3;       // colour / pulse / alpha triggers at most this long are flashes (shakes always)
    double opacityLowThreshold = 0.5;    // below: "low opacity"; such decoration does not obstruct
    double hiddenObstructionShare = 0.5; // a gameplay object obstructed by at least this share counts as hidden
    float clutterGrid = 15.f;            // the union approximation grid
    float minSectionHeight = 320.f;      // the clutter bounding box is at least one screen (320 units) tall
    double speedUnitsPerSecond[5] = {251.16, 311.58, 387.42, 468.0, 576.0};   // GD player speeds by Speed index
    double digestQuantum = 1000.0;       // presentation digest: round(feature * quantum)
    int featureDecimals = 6;             // features are rounded to this many decimals before use
    std::vector<int> indicatorIds;       // arrow / marker object ids counted as indicators (empty in /1)
};

inline const IdentityConfig kIdentityConfig{};

/// The whole identity from the analyzer's world + the extraction's decoration set.
LevelIdentity compute(World const& world, DecoSet const& deco, IdentityInput const& input, IdentityConfig const& cfg = kIdentityConfig);

/// camelCase keys named exactly like the fields; minhash = 64 numbers; gamemode as its name,
/// speed as the GD index 0..4. The TypeScript side reads this object as-is.
json::Value toJson(LevelIdentity const& id);

/// Share of equal MinHash positions = the Jaccard estimate of the two tuple sets.
double jaccardEstimate(MinHash const& a, MinHash const& b);

/// The 11 normalised features in PresentationVector order (previewSeconds as previewSeconds / cap).
std::array<double, kPresentationFeatures> normalise(PresentationVector const& v, IdentityConfig const& cfg = kIdentityConfig);

/// §3: 1 - mean over the 11 features of |norm_f(a) - norm_f(b)|.
double presentationSimilarity(PresentationVector const& a, PresentationVector const& b, IdentityConfig const& cfg = kIdentityConfig);

/// §3 readability penalty of one vector (SectionIdentity::readability).
double readability(PresentationVector const& v, IdentityConfig const& cfg = kIdentityConfig);

/// Every constant of a config as JSON (tests/fixtures/identity/cases.json `config`).
json::Value configJson(IdentityConfig const& cfg = kIdentityConfig);

}  // namespace gprl::identity
