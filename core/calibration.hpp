#pragma once
// Local calibration progress (SPEC §10, §11). DISPLAY ONLY.
//
// REFERENCE IMPLEMENTATION: shared/src/rating/calibration.ts (computeCalibration) with the
// parameters of shared/src/rating/params.ts (RATING_PARAMS_0_1_0.calibration). This file mirrors
// it so the HUD can show "calibration 12% | samples 84/700 | sigma/s LOCKED" without a round trip.
// The server is authoritative: it recomputes calibration from stored samples and its answer
// (GET /v1/me/calibration, overrideFromServer) replaces the local picture whenever it arrives.
// This tracker never produces a sigma/s value and never unlocks anything by itself: locally there
// is no fitted L, so neither the difficulty-coverage nor the informative-miss requirement (model
// calibration/0.2.0: an absolute count of misses at timings the player makes at least 30 % of the
// time) can be met and `overallLocked` stays true (SPEC §10: never reveal even a rough sigma/s
// early). Tests may pass a fitted L to check the mirror against the TypeScript fixture.
//
// The words of the calibration screen ("You need about 7 more misses at timings you usually
// make ...") come from calibrationHint, the exact mirror of calibration.ts calibrationHint; the
// server sends the same text in GET /v1/me/calibration `requirements`
// (calibrationRequirementsFromJson). geode/tests/calibration_tests.cpp checks the parity on
// tests/fixtures/api/me-calibration-*.json.
//
// Both sides must reproduce the familiarity weights / effective sample counts of
// tests/fixtures/rating/synthetic-player-*.json and tests/fixtures/familiarity/*.json (generated
// FROM the TypeScript by `npm run fixtures:write -w @gprl/shared`; geode/tests/calibration_tests.cpp
// and fingerprint_tests.cpp check them). Effective samples use core/fingerprint's familiarity
// weighting: 1000 near-identical timings are not 1000 observations.
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fingerprint.hpp"
#include "json.hpp"
#include "vocab.hpp"

namespace gprl {

/// Mirror of params.ts CalibrationParams (SPEC §10 numbers).
struct CalibrationParams {
    std::string version = "0.1.0-experimental";
    /// Requirement model (params.ts `modelVersion`). calibration/0.2.0 (2026-09-30): the miss
    /// requirement is an absolute count of informative misses, no longer a 3 % share of the
    /// effective samples (docs/RATING.md §6.1). calibration/0.3.0 (2026-09-30, owner decision,
    /// docs/RATING.md §6.3): boundary samples 60 -> 30, informative misses 21 -> 10.
    /// calibration/0.4.0 (2026-10-01, owner decision, §6.4): 700 effective samples with a fit
    /// (and at least one effective miss) complete the calibration outright.
    std::string modelVersion = "calibration/0.4.0";
    bool completeAtEffectiveSamples = true;  // params.ts completeAtEffectiveSamples
    double overallEffectiveSamples = 700.0;
    int minGamemodes = 3;
    int minLevels = 3;
    double minReleaseShare = 0.15;
    double partialReleaseShare = 0.05;
    int minSpeeds = 3;
    double boundarySamples = 30.0;   // effective samples with fitted P(hit) in [bandLow, bandHigh] (60 before 0.3.0)
    double bandLow = 0.3;
    double bandHigh = 0.95;
    /// Effective informative misses needed: misses at windows with fitted P(hit) >=
    /// informativeMissMinPHit. Only misses bound the fit from above.
    double minInformativeMisses = 10.0;  // 21 before calibration/0.3.0
    double informativeMissMinPHit = 0.3;
    double gamemodeCountsAt = 25.0;  // effective samples before a gamemode counts
    double perGamemodeEffectiveSamples = 200.0;
    double perGamemodeBoundaryShare = 0.5;  // boundary samples a gamemode needs = boundarySamples * this (params.ts)
    int patternsForFullVariety = 40;
    double timingBandMinMs = 0.5;
    double timingBandMaxMs = 250.0;
    int timingBands = 9;
    struct PercentWeights {
        double effectiveSamples = 3.0;
        double gamemodes = 1.0;
        double levels = 1.0;
        double releases = 1.0;
        double speeds = 1.0;
        double difficultyCoverage = 2.0;
        double misses = 1.0;
        double patterns = 1.0;
    } percentWeights;
};

/// One rated timing as the calibration sees it (the subset of rating/sample.ts TimingSample the
/// requirement checks read). `weight` is the familiarity weight (0 excludes the sample).
struct CalibrationSample {
    std::string id;
    std::string levelId;
    double windowMs = 0.0;
    bool hit = true;
    InputKind kind = InputKind::Press;
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    TimingFingerprint fingerprint;
    double weight = 1.0;
    double recordedAtMs = 0.0;   // unix ms (sample.ts recordedAt); 0 = unknown -> no memory-days filtering
    // SampleFlags: bot / physicsModified exclude the sample (isRatable), wouldBeDeath makes it a miss.
    bool bot = false;
    bool wouldBeDeath = false;
    bool physicsModified = false;

    bool ratable() const { return !bot && !physicsModified && windowMs > 0.0; }
    bool effectiveHit() const { return hit && !wouldBeDeath; }
};

enum class ReleaseData : uint8_t { Sufficient, Partial, Insufficient };
constexpr std::string_view name(ReleaseData r) {
    switch (r) {
        case ReleaseData::Sufficient: return "sufficient";
        case ReleaseData::Partial: return "partial";
        case ReleaseData::Insufficient: return "insufficient";
    }
    return "insufficient";
}

struct CalibrationRequirement {
    double current = 0.0;
    double required = 0.0;
};

/// Mirror of domain/rating.ts CalibrationState: the SPEC §10 screen. Never includes a sigma/s.
struct CalibrationState {
    bool complete = false;
    double percent = 0.0;                 // overall progress [0,1]
    CalibrationRequirement effectiveSamples;
    CalibrationRequirement gamemodes;
    double timingVariety = 0.0;           // [0,1]
    double patternVariety = 0.0;          // [0,1]
    ReleaseData releaseData = ReleaseData::Insufficient;
    double difficultyCoverage = 0.0;      // [0,1]
    bool overallLocked = true;
};

struct RequirementCheck {
    std::string id;       // effectiveSamples | gamemodes | levels | releases | speeds | difficultyCoverage | misses | patterns
    std::string label;
    double current = 0.0;
    double required = 0.0;
    double ratio = 0.0;   // min(1, current/required)
    bool satisfied = false;
    std::string note;     // caveat on the measurement (coarse pattern keys) / "counted as met: ..." (calibration/0.4.0); empty = none (TS `note?`)
};

struct GamemodeCalibration {
    Gamemode gamemode = Gamemode::Cube;
    double effectiveSamples = 0.0;
    int sampleCount = 0;
    double releaseShare = 0.0;
    double boundarySamples = 0.0;
    double informativeMisses = 0.0;   // at the gamemode's fitted L; not part of the per-gamemode requirement
    double progress = 0.0;   // [0,1]
    bool complete = false;
};

struct CalibrationReport {
    std::string version;
    std::string model;       // CalibrationParams::modelVersion the requirements were checked under
    CalibrationState state;
    std::vector<RequirementCheck> requirements;
    std::map<Gamemode, GamemodeCalibration> perGamemode;
    int levels = 0;
    int speeds = 0;
    int patterns = 0;
    double missShare = 0.0;   // informational: no requirement reads it any more
    double releaseShare = 0.0;
    std::optional<double> boundarySamples;     // null when no fit was available
    std::optional<double> informativeMisses;   // null when no fit was available
    std::vector<std::string> debug;
};

/// P(hit | window, L) = erf(w L / (2 sqrt 2)), w in seconds (rating/difficulty.ts hitProbability).
double hitProbability(double windowMs, double L);

/// Inverse error function (rating/erf.ts erfinv: Giles' approximation + Newton steps on std::erf).
double erfinv(double p);

/// The window width in ms that a player of precision L hits with probability `pHit`
/// (rating/difficulty.ts windowMsForDifficulty(L, pHit)); +inf for L <= 0.
double windowMsForHitProbability(double L, double pHit);

/// A miss (effective: noclip would-be deaths included) at a window the player makes at least
/// `informativeMissMinPHit` of the time at the fitted L (calibration.ts isInformativeMiss). Never
/// without a fit.
bool isInformativeMiss(CalibrationSample const& s, std::optional<double> L, CalibrationParams const& p);

// ---- what to play next (calibration.ts calibrationHint; GET /v1/me/calibration `requirements`) ----

/// One requirement as the calibration screen shows it (shared CalibrationRequirementStatus).
struct CalibrationRequirementStatus {
    std::string id;
    std::string label;
    double current = 0.0;
    double required = 0.0;
    double progress = 0.0;    // min(1, current / required)
    bool satisfied = false;
    double remaining = 0.0;   // whole units still missing (the missing share for `releases`); 0 once met
    std::optional<std::string> hint;   // the screen's words; absent once met
};

/// The screen's words for one requirement (exactly calibration.ts calibrationHint). `visibleL` is
/// the player's fitted L ONLY while the server shows the sigma/s anyway: the boundary hints then
/// also name window widths, which would otherwise give the fitted L away (SPEC §10).
std::optional<std::string> calibrationHint(std::string const& id, double current, double required, bool satisfied,
                                           CalibrationParams const& p = {}, std::optional<double> visibleL = std::nullopt);

/// A requirement check with its progress, what is missing and the hint (calibrationRequirementStatus).
CalibrationRequirementStatus calibrationRequirementStatus(RequirementCheck const& check, CalibrationParams const& p = {},
                                                          std::optional<double> visibleL = std::nullopt);

/// Every requirement of a (local) report with its hint, in the report's order.
std::vector<CalibrationRequirementStatus> calibrationRequirementStatuses(CalibrationReport const& report, CalibrationParams const& p = {},
                                                                        std::optional<double> visibleL = std::nullopt);

/// Reads `requirements` of a GET /v1/me/calibration response (the server's list, hints included).
/// False when the member is absent or null (a row not yet recalculated under the current model) or
/// malformed; `out` is then empty.
bool calibrationRequirementsFromJson(json::Value const& response, std::vector<CalibrationRequirementStatus>& out);

/// Log-spaced timing band index of a window width (calibration.ts timingBand).
int timingBand(double windowMs, CalibrationParams const& p);

/// computeCalibration() of calibration.ts. `fittedL` is the internally fitted overall L (never
/// displayed); std::nullopt when no fit exists (always the case locally).
CalibrationReport computeCalibration(std::vector<CalibrationSample> const& samples, std::optional<double> fittedL,
                                     std::map<Gamemode, double> const& fittedLByGamemode = {}, CalibrationParams const& params = {});

/// Incremental local tracker: keeps this session's samples, assigns familiarity weights
/// chronologically (each sample sees only earlier ratable ones, like familiarityWeights in
/// familiarity.ts) and produces the report on demand.
class CalibrationTracker {
public:
    explicit CalibrationTracker(CalibrationParams params = {}, FamiliarityParams fam = {}, FingerprintParams fp = {});

    /// Adds a sample; its weight is computed here from the history. Returns the familiarity result.
    FamiliarityResult add(CalibrationSample sample, int attemptsAtCluster);
    void reset();

    /// Local report (server override applied to `state` when one was received).
    CalibrationReport report(std::optional<double> fittedL = std::nullopt) const;

    /// Replace the displayed state with the server's authoritative one (GET /v1/me/calibration).
    void overrideFromServer(CalibrationState const& state);
    bool hasServerOverride() const { return m_serverOverride.has_value(); }
    void clearServerOverride() { m_serverOverride.reset(); }

    size_t sampleCount() const { return m_samples.size(); }
    CalibrationParams const& params() const { return m_params; }
    /// History cap: beyond this many fingerprints the oldest stop contributing to S (cost control).
    size_t maxHistory = 5000;

private:
    CalibrationParams m_params;
    FamiliarityParams m_fam;
    FingerprintParams m_fp;
    std::vector<CalibrationSample> m_samples;
    std::vector<FingerprintAt> m_history;   // ratable fingerprints with their times, chronological
    std::optional<CalibrationState> m_serverOverride;
};

/// CalibrationState <-> JSON in the shape of domain/rating.ts CalibrationState (what
/// GET /v1/me/calibration returns under `overall`). Nullable fields (`timingVariety`,
/// `patternVariety`, `difficultyCoverage`) read as 0 when null.
json::Value calibrationStateToJson(CalibrationState const& s);
bool calibrationStateFromJson(json::Value const& v, CalibrationState& out, std::string* err = nullptr);

// ---- rating display (docs/RANKS.md "Visibility thresholds", v0.4.3) ----
//
// The server decides what the mod may show (both thresholds are server configuration, never a
// number in this mod): GET /v1/me/calibration carries `displayState`:
//   locked   below the visibility confidence (or calibration too early): sigma/s LOCKED with
//            the calibration progress, exactly as before;
//   visible  sigma/s shown (verified / raw / practical + confidence) but no official rank yet:
//            "Unranked" with the server's `rankHint` ("Rank at 90% confidence");
//   ranked   sigma/s shown and the official rank awarded.
// The sigma fields are only ever read when the state is not locked (SPEC §10 stays: a locked
// answer never yields a figure, whatever else the JSON says).

enum class RatingDisplay : uint8_t { Locked, Visible, Ranked };
constexpr std::string_view name(RatingDisplay d) {
    switch (d) {
        case RatingDisplay::Locked: return "locked";
        case RatingDisplay::Visible: return "visible";
        case RatingDisplay::Ranked: return "ranked";
    }
    return "locked";
}
/// "locked" | "visible" | "ranked" (case-insensitive); false for anything else.
bool parseRatingDisplay(std::string_view text, RatingDisplay& out);

struct CalibrationDisplay {
    RatingDisplay state = RatingDisplay::Locked;
    std::optional<double> verifiedSigma;    // verified rating (the headline, practical L)
    std::optional<double> rawSigma;
    std::optional<double> practicalSigma;
    std::optional<double> confidence;       // Rating Confidence [0,1]
    std::string rankHint;                   // server text between the thresholds ("Rank at 90% confidence")
    // Optional rank the server names with a `ranked` state: { rankId | id, division, name } or a
    // string. The site profile / ladder stays the official source; this only labels the HUD.
    std::string rankId;
    int rankDivision = 0;
    std::string rankName;
    // The thresholds the server answered with (`sigmaMinConfidence` / `rankMinConfidence` at the
    // root): configuration, never a number of this mod's own. Absent when not sent.
    std::optional<double> sigmaMinConfidence;
    std::optional<double> rankMinConfidence;
    // Owner decision 2026-10-02: the gamemodes the overall sigma/s still waits for (root
    // `missingGamemodes` of GET /v1/me/calibration: required by visibility.requiredGamemodes, not
    // calibrated yet), in the server's order. Read in every state; absent / malformed = empty.
    std::vector<std::string> missingGamemodes;

    // PRIVATE sigma/s (owner decision 2026-10-01; docs/contracts/calibration.md "Private sigma/s"):
    // the player's OWN estimate from `privateSigma` { raw, practical, confidence,
    // calibrationPercent, minCalibration, label }, sent from 50 % calibration on (since the owner
    // decision of 2026-10-02 only while the public gate holds: 80 % confidence, cube / ship /
    // wave / ball calibrated). It is not a rating: shown only to this player (HUD, Profile tab), always as
    // "private" / "Private sigma/s (estimate, only you)". Read in EVERY state (that is the point);
    // a locked state still clears the public figures above.
    bool privatePresent = false;
    std::optional<double> privateRaw;
    std::optional<double> privatePractical;
    std::optional<double> privateConfidence;          // [0,1]
    std::optional<double> privateCalibrationPercent;  // [0,1]
    std::optional<double> privateMinCalibration;      // [0,1], the live threshold the server used

    // Verification status (owner decision 2026-10-02, docs/SECURITY.md §10.8): the server's
    // `verificationStatus` { status, label, provisional, provisionalLabel, reason }, read in every
    // state. Separate from the Rating Confidence: a normal clean rating is "Auto-Verified" (with
    // "Provisional / Unranked ..." below the official minimum), never "Unverified". Empty status =
    // an older server that did not send it.
    std::string verificationStatus;     // auto_verified | verified | pending_manual_verification | unverified | invalid | calibrating
    std::string verificationLabel;      // "Auto-Verified", "Pending Manual Verification", ...
    bool provisional = false;
    std::string provisionalLabel;       // "Provisional / Unranked (Rating Confidence 77% < 90%)"
    std::string verificationReason;     // one public sentence

    bool locked() const { return state == RatingDisplay::Locked; }
    /// The sigma/s a one-line display shows: verified, else practical, else raw.
    std::optional<double> headlineSigma() const;
    /// The private figure a one-line display shows: practical, else raw; nullopt without one.
    std::optional<double> privateHeadline() const;
};

/// Reads the display fields of a GET /v1/me/calibration response (`response` is the whole body;
/// `displayState` / `rankHint` are read at the root, or under a `display` / `rating` object when
/// the root has no `displayState`). The figures come from the `sigma` object the server sends
/// (shared V1CalibrationSigma: `verified` + `verifiedConfidence`, `raw`, `practical`, `sigma`,
/// `confidence`, `rankBandId`; pinned in tests/fixtures/api/me-calibration-*.json) or, when there
/// is no such object, from flat fields next to `displayState` (`verifiedSigma` / `verified`, ...).
/// Tolerant: a missing or unknown `displayState` falls back to the calibration lock of
/// `overall.overallLocked` (missing => locked); a locked state clears every sigma; non-finite /
/// non-number sigma fields are absent; confidence is clamped to [0,1]; the rank is only kept with
/// a `ranked` state (`rank` object / string, else `rankBandId` through parseRankBandId).
/// `privateSigma` (root object; owner decision 2026-10-01) is read in every state: present when it
/// carries a positive raw or practical figure (and, when both are sent, calibrationPercent >=
/// minCalibration); null / malformed / absent = no private figure. `sigmaMinConfidence` /
/// `rankMinConfidence` are kept when they are finite numbers in [0,1].
/// Returns whether a `displayState` was present.
bool calibrationDisplayFromJson(json::Value const& response, CalibrationDisplay& out);

/// Splits an official band id (docs/RANKS.md "Band identifiers") into the ladder's rank id and a
/// division: "master-1" -> ("master", 1), "grandmaster" -> ("grandmaster", 0), "ascendant-4" ->
/// ("ascendant-4", 0) (one Rank per Ascendant tier in core/ranks). Lower-cased. False when empty.
bool parseRankBandId(std::string_view bandId, std::string& rankId, int& division);

/// The HUD's sigma/s field (the text after "sigma/s "):
///   locked   "LOCKED (calibrating 42%)"
///            "LOCKED (calibrating 62%) | private ~123.4"   with the player's own private figure
///            (practical, else raw; always one decimal)
///   visible  "123.4 (82% conf, unranked)"   ("-" without a figure, no "(.. conf" without one)
///   ranked   "123.4 | Master II"              (`rankName`, else the display's rank name, else "ranked")
/// Visible / ranked never add the private figure: the public one is shown already.
std::string sigmaHudText(CalibrationDisplay const& d, double calibrationPercent, std::string const& rankName = {});

/// "What to play next" for the gamemodes the sigma/s still waits for (owner request 2026-10-02),
/// one line per `missingGamemodes` entry in the server's order:
///   "Calibrate Ship: play ship levels (needed before your sigma/s shows)."
std::vector<std::string> missingGamemodeSteps(CalibrationDisplay const& d);
/// "sigma/s LOCKED until ship is calibrated" / "... until ship and ball are calibrated";
/// empty when nothing is missing.
std::string missingGamemodesLockText(CalibrationDisplay const& d);

/// v0.12.0 top-right panel (owner request 2026-10-02): the sigma/s big, one small line under it.
///   locked   big "LOCKED"   small "calibrating 62%"   (+ "  private ~123.4" when `showPrivate`)
///   visible  big "170.1"    small "sigma/s  77% conf  unranked"
///   ranked   big "171.0"    small "sigma/s  Grandmaster"
/// `showPrivate` false while the clip buffer records (the private estimate never reaches a clip).
struct TopRightSigma {
    std::string big;
    std::string detail;
};
TopRightSigma topRightSigma(CalibrationDisplay const& d, double calibrationPercent, std::string const& rankName = {}, bool showPrivate = true);

/// The Profile tab's private line (owner decision 2026-10-01), empty without a private figure:
///   "Private sigma/s (estimate, only you): raw 117.3   practical 123.4   confidence 45%"
/// ("-" for a figure the server sent as null; one decimal; the confidence floored like the HUD).
std::string privateSigmaProfileText(CalibrationDisplay const& d);

/// The one-line note under it, built from the configuration the server sent (never a number of
/// this mod's own); empty without a private figure. With every threshold known:
///   "shown to you from 50% calibration; public from 80% confidence, rank at 90%"
/// The rank threshold comes from the ladder (`/api/ranks` eligibility.minConfidence) when known,
/// else from the calibration answer's `rankMinConfidence`; a part whose threshold is unknown is
/// left out.
std::string privateSigmaNoteText(CalibrationDisplay const& d, std::optional<double> ladderMinConfidence);

/// The Profile tab's hint under "Unranked" while sigma/s is visible: the server's `rankHint`, else
/// "Rank at 90% confidence" from the ladder's eligibility minimum (`/api/ranks` eligibility.minConfidence)
/// when known. Empty when locked, ranked, or nothing to say.
std::string rankHintText(CalibrationDisplay const& d, std::optional<double> ladderMinConfidence);

}  // namespace gprl
