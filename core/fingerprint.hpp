#pragma once
// Timing fingerprint + similarity + familiarity weighting (SPEC §10, §12, §13).
//
// REFERENCE IMPLEMENTATION: shared/src/rating/fingerprint.ts (similarity) and
// shared/src/rating/familiarity.ts (effective weights), parameters in shared/src/rating/params.ts
// (RATING_PARAMS_0_1_0.fingerprint / .familiarity). This file mirrors them so the mod can show
// local calibration progress; the server's numbers are authoritative. Both sides must reproduce
// tests/fixtures/familiarity/*.json (generated FROM the TypeScript by
// `npm run fixtures:write -w @gprl/shared`; geode/tests/fingerprint_tests.cpp checks them). If
// the two implementations disagree, the TypeScript wins and this file is fixed.
//
// similarity(a, b) in [0,1] is a PRODUCT of per-field kernels:
//   - hard categorical fields (gamemode, press/release): mismatch -> hardMismatch (0);
//   - soft categorical fields: mismatch -> the field's softMismatch factor;
//   - numeric fields: Gaussian kernel exp(-0.5 (d/scale)^2); window width, input gaps and hold
//     duration compared in log space; a null on exactly one side -> nullMismatch;
//   - an unknown geometry hash ("" on either side) is neutral: missing data, not a mismatch.
// Every factor comes from FingerprintParams; explainSimilarity() reports each kernel (debug output).
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vocab.hpp"

namespace gprl {

/// Mirror of schema.ts TimingFingerprint (SPEC §12): everything the familiarity model needs to
/// tell two timings apart. Computed by the client from the state snapshot and nearby inputs.
struct TimingFingerprint {
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    Gravity gravity = Gravity::Normal;
    InputKind kind = InputKind::Press;
    bool mini = false;
    double windowMs = 0.0;                              // width of the valid window (ms)
    double yVelocity = 0.0;                             // GD units/s, sign follows gravity-normalised "up"
    Trajectory trajectory = Trajectory::Grounded;
    HorizontalState horizontalState = HorizontalState::Ground;
    std::optional<double> prevInputGapMs;               // null = none within the analysis horizon
    std::optional<double> nextInputGapMs;
    std::optional<double> holdMs;                       // releases (and presses whose release is known)
    PortalTransition portalTransition = PortalTransition::None;
    InputDirection inputDirection = InputDirection::None;
    std::string geometryHash;                           // hash of nearby geometry (pattern identity)

    bool operator==(TimingFingerprint const&) const = default;
};

/// Mirror of params.ts FingerprintParams (one object per algorithm version).
struct FingerprintParams {
    std::string version = "0.1.0-experimental";
    double hardMismatch = 0.0;
    struct Soft {
        double gravity = 0.5;
        double speed = 0.6;
        double mini = 0.6;
        double trajectory = 0.5;
        double horizontalState = 0.5;
        double portalTransition = 0.7;
        double inputDirection = 0.5;
        double geometryHash = 0.6;
    } softMismatch;
    struct Scales {
        double logWindow = 0.35;     // natural-log units of window width
        double yVelocity = 120.0;    // GD units/s
        double logGapMs = 0.5;       // log-ms units for the previous/next input gap
        double logHoldMs = 0.5;      // log-ms units for the hold duration
    } numericScales;
    double nullMismatch = 0.7;       // kernel value when one side has a null gap/hold and the other does not
};

struct SimilarityBreakdown {
    double total = 1.0;
    std::vector<std::pair<std::string, double>> factors;   // field -> kernel value, in the reference order
};

/// Every kernel of similarity(a, b) (explainSimilarity in fingerprint.ts).
SimilarityBreakdown explainSimilarity(TimingFingerprint const& a, TimingFingerprint const& b, FingerprintParams const& params = {});

/// Similarity in [0,1]; 1 = identical timing pattern. Hard mismatches short-circuit to 0.
double similarity(TimingFingerprint const& a, TimingFingerprint const& b, FingerprintParams const& params = {});

/// Stable key of the "pattern" a fingerprint belongs to: "<gamemode>|<kind>|<geometryHash>".
std::string patternKey(TimingFingerprint const& fp);

/// Timing-history weight form (params.ts FamiliarityParams.form; MASTER §9, decision C3).
enum class FamiliarityForm : uint8_t { Saturating, Reciprocal };

/// Mirror of params.ts FamiliarityParams (SPEC §10, §12, §13; MASTER §9).
struct FamiliarityParams {
    std::string version = "0.1.0-experimental";
    FamiliarityForm form = FamiliarityForm::Saturating;  // saturating: weight = e^(-S / saturationK); 1000 identical -> ~340 effective
    double saturationK = 362.0;          // k of the saturating form (S_sat = 1 / (1 - e^(-1/k)) ~ k + 1/2)
    double alpha = 0.15;                 // reciprocal form only: weight = 1 / (1 + alpha * S); 1000 identical -> ~34 effective
    double beta = 2.0;                   // S = sum of similarity^beta
    double minSimilarity = 0.05;         // below this a history sample contributes nothing
    double memoryDays = 180.0;           // only samples within this many days count (needs timestamps; see similarityMass)
    double attemptAlpha = 0.35;          // attempt familiarity: 1 / (1 + attemptAlpha * ln(1 + attemptsAtCluster))
    double timingHistoryOverride = 3.0;  // S at which the timing history fully overrides the attempt prior
};

enum class FamiliaritySource : uint8_t { Timing, Attempt, Blend };
constexpr std::string_view name(FamiliaritySource s) {
    switch (s) {
        case FamiliaritySource::Timing: return "timing";
        case FamiliaritySource::Attempt: return "attempt";
        case FamiliaritySource::Blend: return "blend";
    }
    return "timing";
}

struct FamiliarityResult {
    double similarityMass = 0.0;   // S
    int similarNeighbours = 0;     // history samples with similarity >= minSimilarity
    double timingWeight = 1.0;
    double attemptWeight = 1.0;
    double weight = 1.0;           // final effective weight in (0, 1]
    FamiliaritySource source = FamiliaritySource::Timing;
};

/// A history entry: an earlier ratable sample's fingerprint and its time (unix ms; 0 = unknown).
struct FingerprintAt {
    TimingFingerprint fp;
    double timeMs = 0.0;
};

/// S and neighbour count of `fp` against `history` (earlier ratable samples), like the loop in
/// familiarity.ts effectiveWeight: history older than `memoryDays` before `sampleTimeMs` is
/// skipped (timing memory fades). With unknown times (0) nothing is skipped.
struct SimilarityMass {
    double mass = 0.0;
    int neighbours = 0;
};
SimilarityMass similarityMass(TimingFingerprint const& fp, double sampleTimeMs, std::vector<FingerprintAt> const& history,
                              FamiliarityParams const& fam = {}, FingerprintParams const& fp_params = {});

/// Effective weight from the similarity mass and the attempt count (effectiveWeight in familiarity.ts).
FamiliarityResult familiarityWeight(SimilarityMass const& mass, int attemptsAtCluster, FamiliarityParams const& params = {});

}  // namespace gprl
