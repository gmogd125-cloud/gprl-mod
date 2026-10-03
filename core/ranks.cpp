#include "ranks.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>

namespace gprl::ranks {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix; }

/// A number member that may be null / missing.
std::optional<double> optNumber(json::Value const& v, std::string_view key) {
    auto* m = v.find(key);
    if (!m || !m->isNumber()) return std::nullopt;
    double d = m->asNumber();
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

/// An int member clamped to the int range (a hostile 1e300 must not wrap or trap).
int intMember(json::Value const& v, std::string_view key, int def = 0) {
    int64_t n = v.getInt(key, def);
    return static_cast<int>(std::clamp<int64_t>(n, INT_MIN, INT_MAX));
}

/// The first member of `keys` that is a finite number.
std::optional<double> firstNumber(json::Value const& v, std::initializer_list<std::string_view> keys) {
    for (auto k : keys) {
        if (auto n = optNumber(v, k)) return n;
    }
    return std::nullopt;
}

bool parseBand(json::Value const& v, Band& out) {
    if (!v.isObject()) return false;
    auto min = firstNumber(v, {"minSigma", "minVerifiedSigma", "min"});
    if (!min) return false;
    out.minSigma = *min;
    out.maxSigma = firstNumber(v, {"maxSigma", "maxVerifiedSigma", "max"});
    out.division = intMember(v, "division", 0);
    if (out.division < 0) out.division = 0;
    return true;
}

std::optional<PlayerRank> parsePlayerRank(json::Value const& v) {
    if (v.isString()) {
        if (v.asString().empty()) return std::nullopt;
        return PlayerRank{v.asString(), 0};
    }
    if (!v.isObject()) return std::nullopt;
    std::string id = v.getString("rankId");
    if (id.empty()) id = v.getString("id");
    if (id.empty()) id = v.getString("rank");
    if (id.empty()) return std::nullopt;
    PlayerRank r;
    r.rankId = std::move(id);
    r.division = intMember(v, "division", 0);
    if (r.division < 0 || r.division > 3) r.division = 0;
    return r;
}

/// A percent that may be served as a fraction (0..1) or as a percentage (0..100).
double percentValue(double v) {
    if (!std::isfinite(v)) return 0.0;
    if (v <= 1.0) v *= 100.0;
    return std::clamp(v, 0.0, 100.0);
}

}  // namespace

bool parseHexColor(std::string_view text, Color& out) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() == 3) {
        int d[3];
        for (int i = 0; i < 3; ++i) {
            d[i] = hexDigit(text[static_cast<size_t>(i)]);
            if (d[i] < 0) return false;
        }
        out = Color{static_cast<uint8_t>(d[0] * 17), static_cast<uint8_t>(d[1] * 17), static_cast<uint8_t>(d[2] * 17)};
        return true;
    }
    if (text.size() != 6) return false;
    int d[6];
    for (int i = 0; i < 6; ++i) {
        d[i] = hexDigit(text[static_cast<size_t>(i)]);
        if (d[i] < 0) return false;
    }
    out = Color{static_cast<uint8_t>(d[0] * 16 + d[1]), static_cast<uint8_t>(d[2] * 16 + d[3]), static_cast<uint8_t>(d[4] * 16 + d[5])};
    return true;
}

Color scaled(Color c, double k) {
    if (!std::isfinite(k)) k = k > 0 ? 1.0 : 0.0;
    // clamp BEFORE rounding: lround of a huge double is undefined (and `long` is 32-bit on Windows)
    auto f = [k](uint8_t v) { return static_cast<uint8_t>(std::lround(std::clamp(v * k, 0.0, 255.0))); };
    return Color{f(c.r), f(c.g), f(c.b)};
}

bool parseRankList(json::Value const& v, RankList& out, std::string* err) {
    json::Value const* arr = nullptr;
    if (v.isArray()) arr = &v;
    else if (v.isObject() && v["ranks"].isArray()) arr = &v["ranks"];
    if (!arr) {
        if (err) *err = "rank list: expected an array or { ranks: [] }";
        return false;
    }
    RankList list;
    if (v.isObject()) list.eligibilityMinConfidence = optNumber(v["eligibility"], "minConfidence");
    for (auto const& item : arr->asArray()) {
        if (!item.isObject()) continue;
        Rank r;
        r.id = item.getString("id");
        r.name = item.getString("name");
        if (r.id.empty() && r.name.empty()) continue;
        if (r.id.empty()) r.id = lower(r.name);
        if (r.name.empty()) r.name = r.id;
        r.order = intMember(item, "order", static_cast<int>(list.ranks.size()));
        r.colorText = item.getString("color");
        if (!parseHexColor(r.colorText, r.color)) r.color = Color{200, 200, 200};
        r.iconKey = item.getString("iconKey");
        r.description = item.getString("description");
        r.playerCount = std::max(0, intMember(item, "playerCount", 0));
        r.tier = std::max(0, intMember(item, "tier", 0));
        r.reached = item.getBool("reached", true);

        auto const& req = item["requirements"];
        r.requirements.minConfidence = optNumber(req, "minConfidence").value_or(0.0);
        r.requirements.minRatedGamemodes = std::max(0, intMember(req, "minRatedGamemodes", 0));
        r.requirements.minVerifiedRuns = std::max(0, intMember(req, "minVerifiedRuns", 0));
        auto minSigma = firstNumber(item, {"minSigma", "minVerifiedSigma"});
        if (!minSigma) minSigma = firstNumber(req, {"minVerifiedSigma", "minSigma"});
        r.requirements.minSigma = minSigma.value_or(0.0);
        auto maxSigma = firstNumber(item, {"maxSigma", "maxVerifiedSigma"});

        // bands: `divisions` (next shape, an array) or `divisionThresholds` (current shape)
        json::Value const* thresholds = nullptr;
        if (item["divisions"].isArray()) thresholds = &item["divisions"];
        else if (item["divisionThresholds"].isArray()) thresholds = &item["divisionThresholds"];
        if (thresholds) {
            for (auto const& t : thresholds->asArray()) {
                Band b;
                if (parseBand(t, b)) r.bands.push_back(b);
            }
            std::stable_sort(r.bands.begin(), r.bands.end(), [](Band const& a, Band const& b) { return a.minSigma < b.minSigma; });
        }

        std::string kind = lower(item.getString("kind"));
        bool ascendantId = startsWith(lower(r.id), "ascendant") || startsWith(lower(r.iconKey), "ascendant");
        if (kind == "ascendant" || (kind.empty() && ascendantId)) r.kind = Kind::Ascendant;
        else if (kind == "divisions" || (kind.empty() && !r.bands.empty())) r.kind = Kind::Divisions;
        else r.kind = Kind::Single;
        // a "divisions" rank with a single (or no) band behaves like a single band
        if (r.kind == Kind::Divisions && r.bands.size() < 2) r.kind = Kind::Single;
        if (r.kind != Kind::Divisions) {
            // one band: the rank's own range (a listed single band is honoured first)
            Band b;
            if (r.bands.size() == 1) b = r.bands.front();
            else {
                b.minSigma = minSigma.value_or(r.bands.empty() ? 0.0 : r.bands.front().minSigma);
                b.maxSigma = maxSigma;
            }
            b.division = 0;
            r.bands.assign(1, b);
        }
        else {
            // fill divisions that were served without numbers: III, II, I from the lowest band up
            int n = static_cast<int>(r.bands.size());
            for (int i = 0; i < n; ++i) {
                if (r.bands[static_cast<size_t>(i)].division <= 0) r.bands[static_cast<size_t>(i)].division = std::max(1, n - i);
            }
            if (!minSigma) r.requirements.minSigma = r.bands.front().minSigma;
        }
        if (r.kind == Kind::Ascendant) {
            if (r.iconKey.empty() || startsWith(lower(r.iconKey), "ascendant")) r.iconKey = "ascendant";
            if (r.tier <= 0) {
                // "ascendant-4" / "ascendant4" / "ascendant_iv": take a trailing number when there is one
                std::string tail;
                for (char c : r.id) {
                    if (std::isdigit(static_cast<unsigned char>(c))) tail.push_back(c);
                }
                if (!tail.empty() && tail.size() < 6) r.tier = std::atoi(tail.c_str());
            }
            // numeral() is empty above 3999: never leave a dangling "Ascendant " then
            if (r.tier > 0 && lower(r.name) == "ascendant" && !numeral(r.tier).empty()) r.name = "Ascendant " + numeral(r.tier);
        }
        if (r.iconKey.empty()) r.iconKey = r.id;
        list.ranks.push_back(std::move(r));
    }
    // Ascendant tiers without an explicit number: count them in ladder order
    int tier = 0;
    for (auto& r : list.ranks) {
        if (r.kind != Kind::Ascendant) continue;
        if (r.tier > 0) tier = r.tier;
        else {
            r.tier = ++tier;
            if (lower(r.name) == "ascendant" && !numeral(r.tier).empty()) r.name = "Ascendant " + numeral(r.tier);
        }
    }
    out = std::move(list);
    return true;
}

Placement placeSigma(RankList const& list, double sigma) {
    Placement best;
    if (!std::isfinite(sigma)) return best;
    // the highest band whose lower bound the sigma reaches; later bands win ties (ladder order).
    // Tracked by pointer, not through bandOf(): a hostile ladder may repeat division numbers.
    Band const* bestBand = nullptr;
    for (size_t i = 0; i < list.ranks.size(); ++i) {
        for (auto const& b : list.ranks[i].bands) {
            if (sigma >= b.minSigma && (!bestBand || b.minSigma >= bestBand->minSigma)) {
                best.rank = static_cast<int>(i);
                best.division = b.division;
                bestBand = &b;
            }
        }
    }
    return best;
}

int findRank(RankList const& list, std::string_view id) {
    std::string want = lower(id);
    for (size_t i = 0; i < list.ranks.size(); ++i) {
        if (lower(list.ranks[i].id) == want) return static_cast<int>(i);
    }
    return -1;
}

Band const* bandOf(Rank const& rank, int division) {
    if (rank.bands.empty()) return nullptr;
    for (auto const& b : rank.bands) {
        if (b.division == division) return &b;
    }
    return &rank.bands.front();
}

std::optional<Progress> progressOf(RankList const& list, double sigma) {
    Placement p = placeSigma(list, sigma);
    if (!p.valid()) return std::nullopt;
    // flatten the ladder into bands in ladder order and find the one after ours
    struct Flat {
        size_t rank;
        Band const* band;
    };
    std::vector<Flat> flat;
    for (size_t i = 0; i < list.ranks.size(); ++i) {
        for (auto const& b : list.ranks[i].bands) flat.push_back({i, &b});
    }
    // our band: the placed rank's band that placeSigma chose (the last band of that rank whose
    // lower bound the sigma reaches - the same rule, so repeated division numbers cannot mislead)
    Band const* mine = nullptr;
    for (auto const& b : list.ranks[static_cast<size_t>(p.rank)].bands) {
        if (sigma >= b.minSigma && (!mine || b.minSigma >= mine->minSigma)) mine = &b;
    }
    if (!mine) return std::nullopt;
    for (size_t i = 0; i < flat.size(); ++i) {
        if (flat[i].band != mine) continue;
        if (i + 1 >= flat.size()) return std::nullopt;   // top of the ladder
        auto const& next = flat[i + 1];
        double span = next.band->minSigma - mine->minSigma;
        Progress pr;
        pr.nextName = bandName(list.ranks[next.rank], next.band->division);
        pr.percent = span > 0.0 ? std::clamp((sigma - mine->minSigma) / span * 100.0, 0.0, 100.0) : 100.0;
        return pr;
    }
    return std::nullopt;
}

std::string bandName(Rank const& rank, int division) {
    // no trailing space when the division has no numeral (0, or a hostile 99)
    std::string n = rank.hasDivisions() ? divisionNumeral(division) : std::string();
    return n.empty() ? rank.name : rank.name + " " + n;
}

std::string bandName(RankList const& list, Placement p) {
    if (!p.valid() || static_cast<size_t>(p.rank) >= list.ranks.size()) return {};
    return bandName(list.ranks[static_cast<size_t>(p.rank)], p.division);
}

std::string numeral(int n) {
    if (n <= 0 || n > 3999) return {};
    struct Pair {
        int value;
        char const* text;
    };
    static constexpr Pair kPairs[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
                                      {40, "XL"},  {10, "X"},   {9, "IX"},  {5, "V"},    {4, "IV"},  {1, "I"}};
    std::string out;
    for (auto const& p : kPairs) {
        while (n >= p.value) {
            out += p.text;
            n -= p.value;
        }
    }
    return out;
}

std::string divisionNumeral(int division) { return division >= 1 && division <= 3 ? numeral(division) : std::string(); }

std::string badgeNumeral(Rank const& rank, int division) {
    if (rank.kind == Kind::Ascendant) return numeral(rank.tier);
    if (rank.hasDivisions()) return divisionNumeral(division);
    return {};
}

std::string formatSigma(double sigma) {
    if (!std::isfinite(sigma)) return "?";
    char buf[64];
    // absurd magnitudes (a hostile 1e300) never go through llround (undefined above 2^63)
    if (std::fabs(sigma) >= 1e9) {
        std::snprintf(buf, sizeof buf, "%.3g", sigma);
        return buf;
    }
    double rounded = std::round(sigma * 10.0) / 10.0;
    if (std::fabs(rounded - std::round(rounded)) < 1e-9) std::snprintf(buf, sizeof buf, "%.0f", rounded == 0.0 ? 0.0 : rounded);
    else std::snprintf(buf, sizeof buf, "%.1f", rounded);
    return buf;
}

std::string formatRange(double minSigma, std::optional<double> maxSigma) {
    if (!maxSigma) return formatSigma(minSigma) + "+";
    return formatSigma(minSigma) + "-" + formatSigma(*maxSigma);
}

std::string formatBands(Rank const& rank) {
    if (rank.bands.empty()) return formatRange(rank.requirements.minSigma, std::nullopt);
    if (!rank.hasDivisions()) return formatRange(rank.bands.front().minSigma, rank.bands.front().maxSigma);
    std::string out;
    for (auto const& b : rank.bands) {
        if (!out.empty()) out += "  ";
        out += divisionNumeral(b.division) + " " + formatRange(b.minSigma, b.maxSigma);
    }
    return out;
}

std::string formatRequirements(Requirements const& r) {
    std::string out;
    auto add = [&](std::string part) {
        if (!out.empty()) out += ", ";
        out += std::move(part);
    };
    if (r.minConfidence > 0.0) add("min confidence " + percentText(r.minConfidence));
    if (r.minRatedGamemodes > 0) add(std::to_string(r.minRatedGamemodes) + (r.minRatedGamemodes == 1 ? " gamemode rated" : " gamemodes rated"));
    if (r.minVerifiedRuns > 0) add(std::to_string(r.minVerifiedRuns) + (r.minVerifiedRuns == 1 ? " verified run" : " verified runs"));
    if (out.empty()) out = "no extra requirements";
    return out;
}

bool parseProfile(json::Value const& v, Profile& out, std::string* err) {
    if (!v.isObject()) {
        if (err) *err = "profile: expected an object";
        return false;
    }
    Profile p;
    p.username = v.getString("username");
    p.displayName = v.getString("displayName");
    if (p.displayName.empty()) p.displayName = p.username;
    if (p.username.empty()) {
        if (err) *err = "profile: no username";
        return false;
    }
    p.identityVerified = v.getBool("identityVerified", false);
    p.rank = parsePlayerRank(v["rank"]);
    auto const& ratings = v["ratings"];
    // docs/RANKS.md "Visibility thresholds": "available" (ranked or not) and "visible" (sigma/s
    // shown at the visibility confidence, rank withheld) both show figures; anything else locks.
    std::string availability = lower(ratings.getString("availability", "locked"));
    p.ratingsLocked = availability != "available" && availability != "visible";
    p.rankHint = v.getString("rankHint", ratings.getString("rankHint"));
    auto sigmaOf = [&](std::string_view ratingKey, std::string_view summaryKey) -> std::optional<double> {
        auto const& r = ratings[ratingKey];
        if (r.isObject()) {
            if (auto n = optNumber(r, "value")) return n;
        }
        else if (r.isNumber()) return r.asNumber();
        return optNumber(v, summaryKey);
    };
    p.verifiedSigma = sigmaOf("verified", "verifiedSigma");
    p.rawSigma = sigmaOf("raw", "rawSigma");
    p.practicalSigma = sigmaOf("practical", "practicalSigma");
    p.confidence = optNumber(v, "confidence");
    if (!p.confidence && ratings["verified"].isObject()) p.confidence = optNumber(ratings["verified"], "confidence");
    if (p.ratingsLocked) {
        // SPEC §10: never a sigma/s figure while locked, whatever a summary field says
        p.verifiedSigma.reset();
        p.rawSigma.reset();
        p.practicalSigma.reset();
    }
    p.calibrationPercent = std::clamp(optNumber(v["calibration"], "percent").value_or(0.0), 0.0, 1.0);
    p.leaderboardPosition = std::max(0, intMember(v, "leaderboardPosition", 0));
    p.verifiedRuns = std::max(0, intMember(v, "verifiedRuns", 0));
    p.gamemodes.clear();
    for (int i = 0; i < kGamemodeCount; ++i) {
        GamemodeRating g;
        g.gamemode = static_cast<Gamemode>(i);
        p.gamemodes.push_back(g);
    }
    if (v["gamemodes"].isArray()) {
        for (auto const& item : v["gamemodes"].asArray()) {
            Gamemode gm;
            if (!parse(item.getString("gamemode"), gm)) continue;
            auto& g = p.gamemodes[static_cast<size_t>(gm)];
            g.locked = lower(item.getString("availability", "locked")) != "available";
            g.sigma = g.locked ? std::nullopt : optNumber(item, "sigma");
            g.calibrationProgress = std::clamp(optNumber(item, "calibrationProgress").value_or(0.0), 0.0, 1.0);
        }
    }
    // ---- optional next-shape fields (docs/RANKS.md "UI") ----
    auto const& rp = v["rankProgress"];
    if (rp.isObject()) {
        auto pct = firstNumber(rp, {"percent", "progress"});
        std::string next = rp.getString("nextName");
        if (next.empty()) next = rp.getString("next");
        if (pct && !next.empty()) p.rankProgress = Progress{next, percentValue(*pct)};
    }
    p.unverifiedRankEquivalent = parsePlayerRank(v["unverifiedRankEquivalent"]);
    if (!p.unverifiedRankEquivalent) p.unverifiedRankEquivalent = parsePlayerRank(v["unverifiedEquivalent"]);
    if (auto* cv = v.find("competitiveVerified"); cv && cv->isBool()) p.competitiveVerified = cv->asBool();
    else {
        std::string status = lower(v.getString("verificationStatus"));
        if (status.empty()) status = lower(v["verification"].getString("status"));
        if (status.empty() && v["verification"].isObject()) {
            if (auto* b = v["verification"].find("verified"); b && b->isBool()) p.competitiveVerified = b->asBool();
        }
        if (!status.empty()) p.competitiveVerified = status == "verified" || status == "competitive_verified" || status == "competitive-verified";
    }
    out = std::move(p);
    return true;
}

std::optional<Progress> profileProgress(Profile const& p, RankList const* list) {
    if (p.rankProgress) return p.rankProgress;
    if (p.ratingsLocked || !p.verifiedSigma || !list) return std::nullopt;
    return progressOf(*list, *p.verifiedSigma);
}

bool parseLeaderboard(json::Value const& v, Leaderboard& out, std::string* err) {
    json::Value const* items = nullptr;
    if (v.isArray()) items = &v;
    else if (v.isObject() && v["items"].isArray()) items = &v["items"];
    if (!items) {
        if (err) *err = "leaderboard: expected { items: [] }";
        return false;
    }
    Leaderboard b;
    b.total = std::max(0, intMember(v, "total", static_cast<int>(items->asArray().size())));
    b.asOf = v.getString("asOf");
    int position = 0;
    for (auto const& item : items->asArray()) {
        if (!item.isObject()) continue;
        BoardRow row;
        ++position;
        row.position = intMember(item, "position", position);
        if (row.position <= 0) row.position = position;
        // rows are PlayerSummary + position; tolerate a nested { player: {...} } too
        json::Value const& player = item["player"].isObject() ? item["player"] : item;
        row.username = player.getString("username");
        row.displayName = player.getString("displayName");
        if (row.displayName.empty()) row.displayName = row.username;
        if (row.username.empty() && row.displayName.empty()) continue;
        row.rank = parsePlayerRank(player["rank"]);
        row.sigma = firstNumber(item, {"scopeSigma", "verifiedSigma"});
        if (!row.sigma) row.sigma = firstNumber(player, {"verifiedSigma", "scopeSigma"});
        b.rows.push_back(std::move(row));
    }
    out = std::move(b);
    return true;
}

std::string percentText(double fraction) {
    if (!std::isfinite(fraction)) fraction = 0.0;
    return std::to_string(static_cast<int>(std::floor(std::clamp(fraction, 0.0, 1.0) * 100.0 + 1e-9))) + "%";
}

}  // namespace gprl::ranks
