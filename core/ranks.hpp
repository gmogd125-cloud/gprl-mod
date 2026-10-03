#pragma once
// Pure rules of the in-game GPRL menu (v0.3.0): the rank ladder as GET /api/ranks serves it, the
// player profile (GET /api/players/<username>) and the leaderboard (GET /api/leaderboard) as the
// popup shows them. No Geode includes: compiled by the mod AND by the host tests
// (tests/menu_tests.cpp).
//
// NOTHING here hardcodes a threshold (docs/RANKS.md: thresholds are server configuration and
// "may change without rewriting the rank code"). The ladder is whatever the response lists, in
// the response order. Two response shapes are accepted (the API is moving from the first to the
// second):
//   current   RankDefinition[]  { id, name, order, divisions: 3, requirements{minVerifiedSigma,
//             minConfidence, minRatedGamemodes, minVerifiedRuns}, color '#rrggbb', iconKey,
//             description, divisionThresholds[{division, minVerifiedSigma, maxVerifiedSigma|null}],
//             playerCount, maxVerifiedSigma|null }
//   next      { ranks: [{ id, name, order, kind: 'divisions'|'single'|'ascendant', color, iconKey,
//             divisions?: [{division, minSigma, maxSigma|null}], minSigma, maxSigma|null,
//             tier?, reached?, requirements{minConfidence, ...} }], eligibility{minConfidence} }
// A missing `kind` means 'divisions' when the rank lists division thresholds, else 'single'; an
// id starting with "ascendant" (or kind 'ascendant') is an Ascendant tier (one shared badge with
// the tier numeral drawn on it). Every band is [lower, upper) on the real-valued sigma/s.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "vocab.hpp"

namespace gprl::ranks {

struct Color {
    uint8_t r = 255, g = 255, b = 255;
    bool operator==(Color const&) const = default;
};

/// '#rrggbb' / 'rrggbb' / '#rgb' (any case) -> Color. False (out untouched) for anything else.
bool parseHexColor(std::string_view text, Color& out);
/// Multiplies each channel by `k` (0..1+, clamped to 255): dimmed / brightened variants.
Color scaled(Color c, double k);

enum class Kind : uint8_t { Divisions, Single, Ascendant };
constexpr std::string_view name(Kind k) {
    switch (k) {
        case Kind::Divisions: return "divisions";
        case Kind::Single: return "single";
        case Kind::Ascendant: return "ascendant";
    }
    return "single";
}

/// One sigma/s band of a rank: a division (III/II/I) or the whole rank for single-band ranks.
struct Band {
    int division = 0;                // 3 = III (lowest) ... 1 = I; 0 = the rank has no divisions
    double minSigma = 0.0;           // inclusive
    std::optional<double> maxSigma;  // exclusive; nullopt = open-ended
};

struct Requirements {
    double minSigma = 0.0;           // requirements.minVerifiedSigma / rank.minSigma
    double minConfidence = 0.0;      // [0,1]
    int minRatedGamemodes = 0;
    int minVerifiedRuns = 0;
};

struct Rank {
    std::string id;                  // "bronze" ... "ascendant-4"
    std::string name;                // "Bronze", "Ascendant IV"
    int order = 0;
    Kind kind = Kind::Single;
    Color color;
    std::string colorText;           // as served ('#rrggbb'), for round trips / debugging
    std::string iconKey;             // badge key: the rank id, "ascendant" for every tier
    std::string description;
    std::vector<Band> bands;         // ascending sigma; Divisions: III, II, I; else exactly one band
    int tier = 0;                    // Ascendant tier number (1 = I); 0 otherwise
    bool reached = true;             // Ascendant: false for the "Unreached" tier above the best player
    Requirements requirements;
    int playerCount = 0;

    double minSigma() const { return bands.empty() ? requirements.minSigma : bands.front().minSigma; }
    std::optional<double> maxSigma() const { return bands.empty() ? std::nullopt : bands.back().maxSigma; }
    bool hasDivisions() const { return kind == Kind::Divisions && bands.size() > 1; }
};

struct RankList {
    std::vector<Rank> ranks;         // response order (the ladder, lowest first as the server lists it)
    std::optional<double> eligibilityMinConfidence;   // next shape: eligibility.minConfidence
};

/// Parses either response shape (an array, or an object with `ranks`). False with `err` when
/// the JSON is not a rank list at all; individual malformed entries are skipped.
bool parseRankList(json::Value const& v, RankList& out, std::string* err = nullptr);

/// Where a sigma/s sits on the ladder (by sigma alone; requirements are the server's business and
/// the profile carries the official rank). `rank` = index into RankList::ranks, -1 when below
/// every band; `division` = the band's division (0 for single-band ranks).
struct Placement {
    int rank = -1;
    int division = 0;
    bool valid() const { return rank >= 0; }
};
Placement placeSigma(RankList const& list, double sigma);

/// Index of the rank with this id (case-insensitive), -1 when absent.
int findRank(RankList const& list, std::string_view id);

/// Band of a rank for a division (0 / unknown division of a Divisions rank = the lowest band).
Band const* bandOf(Rank const& rank, int division);

/// Progress to the next band on the ladder (docs/RANKS.md "Progress"): the next band is the one
/// after the sigma's band in ladder order (the next division, or the next rank's first band);
/// percent = (sigma - band.min) / (next.min - band.min) * 100, clamped to [0, 100]. nullopt when
/// the sigma is below the ladder or the ladder ends (no next band: nothing to progress to).
struct Progress {
    std::string nextName;   // "Master I", "Grandmaster", "Ascendant V"
    double percent = 0.0;   // 0..100
};
std::optional<Progress> progressOf(RankList const& list, double sigma);

/// "Master II" / "Grandmaster" / "Ascendant IV": the rank name with the division numeral when the
/// rank has divisions (Ascendant names already carry the tier). Empty for an invalid placement.
std::string bandName(RankList const& list, Placement p);
std::string bandName(Rank const& rank, int division);

/// Roman numeral of n >= 1 ("I", "IV", "XIX", "XL", ... up to 3999); "" for n <= 0.
std::string numeral(int n);
/// Division numeral: 1 -> "I", 2 -> "II", 3 -> "III", 0 -> "".
std::string divisionNumeral(int division);
/// The numeral drawn on a badge: the division for Divisions ranks, the tier for Ascendant, else "".
std::string badgeNumeral(Rank const& rank, int division);

/// Sigma/s with at most one decimal, trailing ".0" dropped ("60", "68.3", "136.7").
std::string formatSigma(double sigma);
/// "60-85" or "300+" (open-ended). No unit (GD's bitmap fonts have no sigma glyph; the UI adds "sigma/s").
std::string formatRange(double minSigma, std::optional<double> maxSigma);
/// "III 0-20  II 20-40  I 40-60" for a Divisions rank, else the single range.
std::string formatBands(Rank const& rank);
/// "min confidence 90%, 4 gamemodes rated, 30 verified runs" (parts with a zero minimum left out;
/// "no extra requirements" when nothing is required).
std::string formatRequirements(Requirements const& r);

// ---- player profile (GET /api/players/<username>) ----

struct PlayerRank {
    std::string rankId;
    int division = 0;
};

struct GamemodeRating {
    Gamemode gamemode = Gamemode::Cube;
    bool locked = true;                    // availability != "available"
    std::optional<double> sigma;           // verified sigma/s
    double calibrationProgress = 0.0;      // [0,1]
};

struct Profile {
    std::string username;
    std::string displayName;
    bool identityVerified = false;
    std::optional<PlayerRank> rank;        // the official rank the server computed
    bool ratingsLocked = true;             // ratings.availability not "available" / "visible" (or missing)
    std::string rankHint;                  // optional server text while sigma/s is visible without a rank
    std::optional<double> verifiedSigma;
    std::optional<double> rawSigma;
    std::optional<double> practicalSigma;
    std::optional<double> confidence;      // [0,1]
    double calibrationPercent = 0.0;       // calibration.percent [0,1]
    int leaderboardPosition = 0;           // 0 = not on the board
    int verifiedRuns = 0;
    std::vector<GamemodeRating> gamemodes; // always 8, in Gamemode order
    // optional fields of the next API shape (docs/RANKS.md "UI"); absent today
    std::optional<Progress> rankProgress;             // rankProgress { nextName, percent }
    std::optional<PlayerRank> unverifiedRankEquivalent;
    std::optional<bool> competitiveVerified;          // verification status when the profile has one
};
bool parseProfile(json::Value const& v, Profile& out, std::string* err = nullptr);

/// Progress line for the Profile tab: the server's rankProgress when present, else computed from
/// the ladder and the verified sigma/s (nullopt while locked or without a ladder).
std::optional<Progress> profileProgress(Profile const& p, RankList const* list);

// ---- leaderboard (GET /api/leaderboard) ----

struct BoardRow {
    int position = 0;
    std::string username;
    std::string displayName;
    std::optional<PlayerRank> rank;
    std::optional<double> sigma;   // scopeSigma, else verifiedSigma
};

struct Leaderboard {
    std::vector<BoardRow> rows;
    int total = 0;
    std::string asOf;
};
bool parseLeaderboard(json::Value const& v, Leaderboard& out, std::string* err = nullptr);

/// "12%" of a [0,1] fraction (floor).
std::string percentText(double fraction);

}  // namespace gprl::ranks
