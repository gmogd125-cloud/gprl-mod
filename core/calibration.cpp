#include "calibration.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "ranks.hpp"

namespace gprl {

namespace {

constexpr double kTwoSqrtTwo = 2.0 * 1.4142135623730951;

// JavaScript Math.round for non-negative values (half rounds up).
double jsRound(double x) { return std::floor(x + 0.5); }

bool inBoundaryBand(CalibrationSample const& s, std::optional<double> L, CalibrationParams const& p) {
    if (!L) return false;
    double prob = hitProbability(s.windowMs, *L);
    return prob >= p.bandLow && prob <= p.bandHigh;
}

RequirementCheck req(char const* id, char const* label, double current, double required) {
    RequirementCheck r;
    r.id = id;
    r.label = label;
    r.current = current;
    r.required = required;
    r.ratio = required > 0.0 ? std::min(1.0, current / required) : 1.0;
    r.satisfied = current >= required;
    return r;
}

}  // namespace

double hitProbability(double windowMs, double L) {
    if (!(windowMs > 0.0) || !(L > 0.0)) return 0.0;
    return std::erf(((windowMs / 1000.0) * L) / kTwoSqrtTwo);
}

double erfinv(double p) {
    if (std::isnan(p) || p <= -1.0 || p >= 1.0) {
        if (p == 1.0) return INFINITY;
        if (p == -1.0) return -INFINITY;
        return NAN;
    }
    if (p == 0.0) return 0.0;
    // Giles (2010) single-precision approximation, refined by Newton steps (rating/erf.ts erfinv).
    double w = -std::log((1.0 - p) * (1.0 + p));
    double x;
    if (w < 5.0) {
        w -= 2.5;
        x = 2.81022636e-8;
        x = 3.43273939e-7 + x * w;
        x = -3.5233877e-6 + x * w;
        x = -4.39150654e-6 + x * w;
        x = 0.00021858087 + x * w;
        x = -0.00125372503 + x * w;
        x = -0.00417768164 + x * w;
        x = 0.246640727 + x * w;
        x = 1.50140941 + x * w;
    } else {
        w = std::sqrt(w) - 3.0;
        x = -0.000200214257;
        x = 0.000100950558 + x * w;
        x = 0.00134934322 + x * w;
        x = -0.00367342844 + x * w;
        x = 0.00573950773 + x * w;
        x = -0.0076224613 + x * w;
        x = 0.00943887047 + x * w;
        x = 1.00167406 + x * w;
        x = 2.83297682 + x * w;
    }
    x *= p;
    constexpr double kTwoOverSqrtPi = 1.1283791670955126;
    for (int i = 0; i < 4; ++i) {
        double step = (std::erf(x) - p) / (kTwoOverSqrtPi * std::exp(-x * x));
        x -= step;
        if (std::fabs(step) <= 1e-16 * std::max(1.0, std::fabs(x))) break;
    }
    return x;
}

double windowMsForHitProbability(double L, double pHit) {
    if (!(L > 0.0)) return INFINITY;
    return ((kTwoSqrtTwo * erfinv(pHit)) / L) * 1000.0;
}

bool isInformativeMiss(CalibrationSample const& s, std::optional<double> L, CalibrationParams const& p) {
    if (!L || s.effectiveHit()) return false;
    return hitProbability(s.windowMs, *L) >= p.informativeMissMinPHit;
}

int timingBand(double windowMs, CalibrationParams const& p) {
    double lo = std::log(p.timingBandMinMs);
    double hi = std::log(p.timingBandMaxMs);
    double clamped = std::max(p.timingBandMinMs, std::min(p.timingBandMaxMs, windowMs));
    double x = (std::log(clamped) - lo) / (hi - lo);
    return std::min(p.timingBands - 1, static_cast<int>(std::floor(x * static_cast<double>(p.timingBands))));
}

CalibrationReport computeCalibration(std::vector<CalibrationSample> const& samples, std::optional<double> fittedL,
                                     std::map<Gamemode, double> const& fittedLByGamemode, CalibrationParams const& p) {
    CalibrationReport report;
    report.version = p.version;
    report.model = p.modelVersion;

    double effective = 0.0, effectiveReleases = 0.0, effectiveMisses = 0.0, boundary = 0.0, informativeMisses = 0.0;
    std::set<std::string> levels, patterns;
    std::set<int> speeds, bands;
    std::map<Gamemode, GamemodeCalibration> acc;

    for (auto const& s : samples) {
        if (!s.ratable()) continue;
        double w = s.weight;
        if (w <= 0.0) continue;
        effective += w;
        if (s.kind == InputKind::Release) effectiveReleases += w;
        if (!s.effectiveHit()) effectiveMisses += w;
        levels.insert(s.levelId);
        speeds.insert(static_cast<int>(s.speed));
        patterns.insert(patternKey(s.fingerprint));
        bands.insert(timingBand(s.windowMs, p));
        std::optional<double> gmL = fittedL;
        if (auto it = fittedLByGamemode.find(s.gamemode); it != fittedLByGamemode.end()) gmL = it->second;
        if (inBoundaryBand(s, fittedL, p)) boundary += w;
        if (isInformativeMiss(s, fittedL, p)) informativeMisses += w;
        auto& g = acc[s.gamemode];
        g.gamemode = s.gamemode;
        g.effectiveSamples += w;
        g.sampleCount += 1;
        g.releaseShare += s.kind == InputKind::Release ? w : 0.0;   // converted to a share below
        if (inBoundaryBand(s, gmL, p)) g.boundarySamples += w;
        if (isInformativeMiss(s, gmL, p)) g.informativeMisses += w;
    }

    double releaseShare = effective > 0.0 ? effectiveReleases / effective : 0.0;
    double missShare = effective > 0.0 ? effectiveMisses / effective : 0.0;
    int gamemodesCounted = 0;
    for (auto const& [gm, g] : acc) if (g.effectiveSamples >= p.gamemodeCountsAt) ++gamemodesCounted;

    for (auto& [gm, g] : acc) {
        g.releaseShare = g.effectiveSamples > 0.0 ? g.releaseShare / g.effectiveSamples : 0.0;
        double sampleRatio = std::min(1.0, g.effectiveSamples / p.perGamemodeEffectiveSamples);
        double releaseRatio = std::min(1.0, g.releaseShare / p.minReleaseShare);
        double boundaryRatio = std::min(1.0, g.boundarySamples / (p.boundarySamples * p.perGamemodeBoundaryShare));
        g.progress = (sampleRatio * 3.0 + releaseRatio + boundaryRatio) / 5.0;
        g.complete = sampleRatio >= 1.0 && releaseRatio >= 1.0 && boundaryRatio >= 1.0;
        report.perGamemode[gm] = g;
    }

    report.requirements = {
        req("effectiveSamples", "Effective samples", effective, p.overallEffectiveSamples),
        req("gamemodes", "Gamemodes with data", gamemodesCounted, p.minGamemodes),
        req("levels", "Levels", static_cast<double>(levels.size()), p.minLevels),
        req("releases", "Release share", releaseShare, p.minReleaseShare),
        req("speeds", "Speeds", static_cast<double>(speeds.size()), p.minSpeeds),
        req("difficultyCoverage", "Samples near the failure boundary", boundary, p.boundarySamples),
        req("misses", "Informative misses", fittedL ? informativeMisses : 0.0, p.minInformativeMisses),
        req("patterns", "Distinct patterns", static_cast<double>(patterns.size()), p.patternsForFullVariety),
    };
    double const weights[] = {p.percentWeights.effectiveSamples, p.percentWeights.gamemodes, p.percentWeights.levels,
                              p.percentWeights.releases,         p.percentWeights.speeds,    p.percentWeights.difficultyCoverage,
                              p.percentWeights.misses,           p.percentWeights.patterns};
    // calibration/0.4.0 (calibration.ts countWaiver): the effective-sample count alone completes
    // the calibration once a fit with at least one effective miss exists.
    bool const countWaiver = p.completeAtEffectiveSamples && effective >= p.overallEffectiveSamples && fittedL.has_value() && effectiveMisses > 0.0;
    if (countWaiver) {
        for (auto& r : report.requirements) {
            if (r.satisfied) continue;
            r.satisfied = true;
            r.ratio = 1.0;
            char note[256];
            std::snprintf(note, sizeof note, "counted as met: %d effective samples reach the %g that complete the calibration (%s)",
                          static_cast<int>(std::floor(effective)), p.overallEffectiveSamples, p.modelVersion.c_str());
            r.note = r.note.empty() ? std::string(note) : r.note + "; " + note;
        }
    }
    double weightSum = 0.0, percent = 0.0;
    bool complete = true;
    for (size_t i = 0; i < report.requirements.size(); ++i) {
        weightSum += weights[i];
        percent += weights[i] * report.requirements[i].ratio;
        complete = complete && report.requirements[i].satisfied;
    }
    percent = weightSum > 0.0 ? percent / weightSum : 0.0;

    double timingVariety = std::min(1.0, 0.5 * (static_cast<double>(bands.size()) / static_cast<double>(p.timingBands))
                                             + 0.5 * std::min(1.0, static_cast<double>(speeds.size()) / static_cast<double>(p.minSpeeds)));
    double patternVariety = std::min(1.0, static_cast<double>(patterns.size()) / static_cast<double>(p.patternsForFullVariety));
    ReleaseData releaseData = releaseShare >= p.minReleaseShare ? ReleaseData::Sufficient
                            : releaseShare >= p.partialReleaseShare ? ReleaseData::Partial : ReleaseData::Insufficient;
    double difficultyCoverage = fittedL ? std::min(1.0, boundary / p.boundarySamples) : 0.0;

    auto& st = report.state;
    st.complete = complete;
    st.percent = percent;
    st.effectiveSamples = {jsRound(effective), p.overallEffectiveSamples};
    st.gamemodes = {static_cast<double>(gamemodesCounted), static_cast<double>(p.minGamemodes)};
    st.timingVariety = timingVariety;
    st.patternVariety = patternVariety;
    st.releaseData = releaseData;
    st.difficultyCoverage = difficultyCoverage;
    st.overallLocked = !complete;

    report.levels = static_cast<int>(levels.size());
    report.speeds = static_cast<int>(speeds.size());
    report.patterns = static_cast<int>(patterns.size());
    report.missShare = missShare;
    report.releaseShare = releaseShare;
    if (fittedL) {
        report.boundarySamples = boundary;
        report.informativeMisses = informativeMisses;
    }

    for (auto const& r : report.requirements) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s: %.4f / %.4f (%s)", r.id.c_str(), r.current, r.required, r.satisfied ? "met" : "missing");
        report.debug.emplace_back(buf);
    }
    report.debug.emplace_back(fittedL ? "fitted L supplied (test / server)"
                                      : "no local fit: difficulty coverage and informative misses 0, sigma/s LOCKED");
    return report;
}

CalibrationTracker::CalibrationTracker(CalibrationParams params, FamiliarityParams fam, FingerprintParams fp)
    : m_params(std::move(params)), m_fam(std::move(fam)), m_fp(std::move(fp)) {}

void CalibrationTracker::reset() {
    m_samples.clear();
    m_history.clear();
    m_serverOverride.reset();
}

FamiliarityResult CalibrationTracker::add(CalibrationSample sample, int attemptsAtCluster) {
    FamiliarityResult r;
    if (!sample.ratable()) {
        // Non-ratable samples weigh 0 and are not part of the history (familiarityWeights).
        r.weight = 0.0;
        r.timingWeight = 0.0;
        r.attemptWeight = 0.0;
        sample.weight = 0.0;
        m_samples.push_back(std::move(sample));
        return r;
    }
    r = familiarityWeight(similarityMass(sample.fingerprint, sample.recordedAtMs, m_history, m_fam, m_fp), attemptsAtCluster, m_fam);
    sample.weight = r.weight;
    m_history.push_back({sample.fingerprint, sample.recordedAtMs});
    if (m_history.size() > maxHistory) m_history.erase(m_history.begin());
    m_samples.push_back(std::move(sample));
    return r;
}

CalibrationReport CalibrationTracker::report(std::optional<double> fittedL) const {
    CalibrationReport rep = computeCalibration(m_samples, fittedL, {}, m_params);
    if (m_serverOverride) {
        rep.state = *m_serverOverride;
        rep.debug.emplace_back("state replaced by the server's authoritative calibration");
    }
    return rep;
}

void CalibrationTracker::overrideFromServer(CalibrationState const& state) {
    m_serverOverride = state;
}

json::Value calibrationStateToJson(CalibrationState const& s) {
    json::Value o = json::Value::object();
    o.set("complete", s.complete);
    o.set("percent", s.percent);
    json::Value eff = json::Value::object();
    eff.set("current", s.effectiveSamples.current);
    eff.set("required", s.effectiveSamples.required);
    o.set("effectiveSamples", std::move(eff));
    json::Value gm = json::Value::object();
    gm.set("current", s.gamemodes.current);
    gm.set("required", s.gamemodes.required);
    o.set("gamemodes", std::move(gm));
    o.set("timingVariety", s.timingVariety);
    o.set("patternVariety", s.patternVariety);
    o.set("releaseData", name(s.releaseData));
    o.set("difficultyCoverage", s.difficultyCoverage);
    o.set("overallLocked", s.overallLocked);
    return o;
}

bool calibrationStateFromJson(json::Value const& v, CalibrationState& out, std::string* err) {
    if (!v.isObject()) {
        if (err) *err = "calibration state is not an object";
        return false;
    }
    CalibrationState s;
    s.complete = v.getBool("complete");
    s.percent = std::max(0.0, std::min(1.0, v.getNumber("percent")));
    s.effectiveSamples = {v["effectiveSamples"].getNumber("current"), v["effectiveSamples"].getNumber("required")};
    s.gamemodes = {v["gamemodes"].getNumber("current"), v["gamemodes"].getNumber("required")};
    s.timingVariety = v.getNumber("timingVariety");
    s.patternVariety = v.getNumber("patternVariety");
    std::string rd = v.getString("releaseData", "insufficient");
    s.releaseData = rd == "sufficient" ? ReleaseData::Sufficient : rd == "partial" ? ReleaseData::Partial : ReleaseData::Insufficient;
    s.difficultyCoverage = v.getNumber("difficultyCoverage");
    s.overallLocked = v.getBool("overallLocked", true);
    if (!v.has("percent") || !v.has("effectiveSamples")) {
        if (err) *err = "calibration state is missing percent / effectiveSamples";
        return false;
    }
    out = s;
    return true;
}

// ---- what to play next (calibration.ts calibrationHint; the text must match it byte for byte) ----

namespace {

constexpr double kHintEpsilon = 1e-9;

/// Whole units still missing for an UNMET count requirement (effective counts round up). At least
/// 1: an unmet requirement never reads "about 0 more" (calibration.ts remainingCount).
long long remainingCount(double current, double required) {
    return static_cast<long long>(std::max(1.0, std::ceil(required - current - kHintEpsilon)));
}

/// "12%" (half rounds up: Math.floor(x * 100 + 0.5)).
std::string hintPercent(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld%%", static_cast<long long>(std::floor(x * 100.0 + 0.5)));
    return buf;
}

/// An unmet share rounded DOWN (calibration.ts percentBelowText: Math.floor(x * 100 + 1e-9)), so
/// 0.146 reads "14%", never the "15%" it is still short of.
std::string hintPercentBelow(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld%%", static_cast<long long>(std::floor(x * 100.0 + kHintEpsilon)));
    return buf;
}

/// Window width: one decimal below 10 ms, whole ms above (calibration.ts msText).
std::string hintMs(double x) {
    char buf[32];
    if (x < 10.0) std::snprintf(buf, sizeof buf, "%.1f", std::floor(x * 10.0 + 0.5) / 10.0);
    else std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::floor(x + 0.5)));
    return buf;
}

/// A parameter as JavaScript prints a number (whole values without decimals).
std::string hintNumber(double x) {
    char buf[32];
    if (std::floor(x) == x && std::fabs(x) < 1e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(x));
    else std::snprintf(buf, sizeof buf, "%.15g", x);
    return buf;
}

std::string countText(long long n) { return std::to_string(n); }

char const* plural(long long n, char const* one, char const* many) { return n == 1 ? one : many; }

}  // namespace

std::optional<std::string> calibrationHint(std::string const& id, double current, double required, bool satisfied,
                                           CalibrationParams const& p, std::optional<double> visibleL) {
    if (satisfied) return std::nullopt;
    long long n = remainingCount(current, required);
    std::optional<double> L;
    if (visibleL && *visibleL > 0.0) L = visibleL;
    std::string N = countText(n);
    if (id == "effectiveSamples")
        return "Keep playing rated demons: about " + N + " more effective " + plural(n, "sample", "samples") +
               ". New timings count fully; timings you have played many times count less.";
    if (id == "gamemodes")
        return "Play " + N + " more " + plural(n, "gamemode", "gamemodes") + " until each has " + hintNumber(p.gamemodeCountsAt) +
               " effective samples.";
    if (id == "levels") return "Play " + N + " more different rated " + plural(n, "demon", "demons") + ".";
    if (id == "releases")
        return "Play more held inputs (ship, wave, robot): releases are " + hintPercentBelow(current) + " of your samples, " +
               hintPercent(required) + " needed.";
    if (id == "speeds") return "Play parts at " + N + " more " + plural(n, "speed", "speeds") + ".";
    if (id == "difficultyCoverage") {
        std::string band = L ? " (for you now: windows of about " + hintMs(windowMsForHitProbability(*L, p.bandLow)) + "-" +
                                   hintMs(windowMsForHitProbability(*L, p.bandHigh)) + " ms)"
                             : std::string();
        return "Play timings at your limit: about " + N + " more " + plural(n, "sample", "samples") + " at timings you hit between " +
               hintPercent(p.bandLow) + " and " + hintPercent(p.bandHigh) + " of the time" + band +
               ". Practising the hardest parts of your levels gives them fastest.";
    }
    if (id == "misses") {
        std::string band = L ? "; for you now: windows of about " + hintMs(windowMsForHitProbability(*L, p.informativeMissMinPHit)) + " ms or wider"
                             : std::string();
        return "You need about " + N + " more " + plural(n, "miss", "misses") + " at timings you usually make (hit at least " +
               hintPercent(p.informativeMissMinPHit) + " of the time" + band +
               "): they show where your precision ends. A death counts when the timing readout shows it as a miss with a window. "
               "Play hard parts you sometimes fail, not only parts you always pass.";
    }
    if (id == "patterns") return "Play about " + N + " more different " + plural(n, "timing", "timings") + " (other parts and levels).";
    return std::nullopt;
}

CalibrationRequirementStatus calibrationRequirementStatus(RequirementCheck const& check, CalibrationParams const& p,
                                                          std::optional<double> visibleL) {
    CalibrationRequirementStatus s;
    s.id = check.id;
    s.label = check.label;
    s.current = check.current;
    s.required = check.required;
    s.progress = check.required > 0.0 ? std::min(1.0, std::max(0.0, check.current / check.required)) : 1.0;
    s.satisfied = check.satisfied;
    s.remaining = check.satisfied            ? 0.0
                  : check.id == "releases"   ? std::max(0.0, check.required - check.current)
                                             : static_cast<double>(remainingCount(check.current, check.required));
    s.hint = calibrationHint(check.id, check.current, check.required, check.satisfied, p, visibleL);
    return s;
}

std::vector<CalibrationRequirementStatus> calibrationRequirementStatuses(CalibrationReport const& report, CalibrationParams const& p,
                                                                        std::optional<double> visibleL) {
    std::vector<CalibrationRequirementStatus> out;
    out.reserve(report.requirements.size());
    for (auto const& r : report.requirements) out.push_back(calibrationRequirementStatus(r, p, visibleL));
    return out;
}

bool calibrationRequirementsFromJson(json::Value const& response, std::vector<CalibrationRequirementStatus>& out) {
    out.clear();
    auto* list = response.isObject() ? response.find("requirements") : nullptr;
    if (!list || !list->isArray()) return false;
    std::vector<CalibrationRequirementStatus> parsed;
    for (auto const& v : list->asArray()) {
        if (!v.isObject()) return false;
        auto* id = v.find("id");
        auto* current = v.find("current");
        auto* required = v.find("required");
        auto* satisfied = v.find("satisfied");
        if (!id || !id->isString() || !current || !current->isNumber() || !required || !required->isNumber() || !satisfied ||
            !satisfied->isBool())
            return false;
        CalibrationRequirementStatus s;
        s.id = id->asString();
        s.label = v.getString("label");
        s.current = current->asNumber();
        s.required = required->asNumber();
        s.progress = std::max(0.0, std::min(1.0, v.getNumber("progress")));
        s.satisfied = satisfied->asBool();
        s.remaining = std::max(0.0, v.getNumber("remaining"));
        if (auto* h = v.find("hint"); h && h->isString()) s.hint = h->asString();
        parsed.push_back(std::move(s));
    }
    out = std::move(parsed);
    return true;
}

// ---- rating display (docs/RANKS.md "Visibility thresholds") ----

namespace {

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// A finite number member, else nullopt (null / string / NaN never become a figure).
std::optional<double> finiteMember(json::Value const& v, std::string_view key) {
    auto* m = v.find(key);
    if (!m || !m->isNumber() || !std::isfinite(m->asNumber())) return std::nullopt;
    return m->asNumber();
}

/// The first finite number among `keys`.
std::optional<double> firstFinite(json::Value const& v, std::initializer_list<std::string_view> keys) {
    for (auto k : keys) {
        if (auto n = finiteMember(v, k)) return n;
    }
    return std::nullopt;
}

/// A sigma/s is a positive finite number; anything else is "no figure".
std::optional<double> sigmaMember(json::Value const& v, std::initializer_list<std::string_view> keys) {
    auto n = firstFinite(v, keys);
    if (n && *n > 0.0) return n;
    return std::nullopt;
}

int percentInt(double fraction) {
    if (!std::isfinite(fraction)) fraction = 0.0;
    return static_cast<int>(std::floor(std::max(0.0, std::min(1.0, fraction)) * 100.0 + 1e-9));
}

}  // namespace

bool parseRatingDisplay(std::string_view text, RatingDisplay& out) {
    std::string t = lowerAscii(text);
    if (t == "locked") { out = RatingDisplay::Locked; return true; }
    if (t == "visible") { out = RatingDisplay::Visible; return true; }
    if (t == "ranked") { out = RatingDisplay::Ranked; return true; }
    return false;
}

bool parseRankBandId(std::string_view bandId, std::string& rankId, int& division) {
    rankId.clear();
    division = 0;
    std::string id = lowerAscii(bandId);
    if (id.empty()) return false;
    // "<rank>-<division 1..3>" names a division of a Divisions rank; Ascendant tiers ("ascendant-4")
    // are whole ids on the ladder (core/ranks: one Rank per tier), as is anything else.
    auto dash = id.rfind('-');
    if (dash != std::string::npos && dash + 2 == id.size() && id.rfind("ascendant", 0) != 0) {
        char c = id[dash + 1];
        if (c >= '1' && c <= '3') {
            rankId = id.substr(0, dash);
            division = c - '0';
            return true;
        }
    }
    rankId = id;
    return true;
}

std::optional<double> CalibrationDisplay::headlineSigma() const {
    if (locked()) return std::nullopt;
    if (verifiedSigma) return verifiedSigma;
    if (practicalSigma) return practicalSigma;
    return rawSigma;
}

std::optional<double> CalibrationDisplay::privateHeadline() const {
    if (!privatePresent) return std::nullopt;
    if (privatePractical) return privatePractical;
    return privateRaw;
}

namespace {

/// A unit-interval member (a threshold / percent / confidence): finite and in [0,1], else nullopt.
std::optional<double> unitMember(json::Value const& v, std::string_view key) {
    auto n = finiteMember(v, key);
    if (!n || *n < 0.0 || *n > 1.0) return std::nullopt;
    return n;
}

/// GET /v1/me/calibration `privateSigma` (owner decision 2026-10-01) into `d`. Tolerant: anything
/// but an object with a positive figure leaves `d` without a private figure.
void readPrivateSigma(json::Value const& response, CalibrationDisplay& d) {
    d.privatePresent = false;
    d.privateRaw = d.privatePractical = d.privateConfidence = std::nullopt;
    d.privateCalibrationPercent = d.privateMinCalibration = std::nullopt;
    auto* p = response.isObject() ? response.find("privateSigma") : nullptr;
    if (!p || !p->isObject()) return;
    auto raw = sigmaMember(*p, {"raw"});
    auto practical = sigmaMember(*p, {"practical"});
    if (!raw && !practical) return;
    auto percent = unitMember(*p, "calibrationPercent");
    auto minimum = unitMember(*p, "minCalibration");
    // defence in depth: the server never sends a figure below its own threshold
    if (percent && minimum && *percent + 1e-12 < *minimum) return;
    d.privatePresent = true;
    d.privateRaw = raw;
    d.privatePractical = practical;
    if (auto c = finiteMember(*p, "confidence")) d.privateConfidence = std::max(0.0, std::min(1.0, *c));
    d.privateCalibrationPercent = percent;
    d.privateMinCalibration = minimum;
}

/// A private figure with exactly one decimal ("123.4", "118.0"); absurd magnitudes stay short.
std::string privateFigureText(double x) {
    char buf[64];
    if (!std::isfinite(x)) return "?";
    if (std::fabs(x) >= 1e9) std::snprintf(buf, sizeof buf, "%.3g", x);
    else std::snprintf(buf, sizeof buf, "%.1f", x);
    return buf;
}

}  // namespace

bool calibrationDisplayFromJson(json::Value const& response, CalibrationDisplay& out) {
    CalibrationDisplay d;
    if (!response.isObject()) {
        out = d;
        return false;
    }
    // where the fields live: the root, or a nested `display` / `rating` object
    json::Value const* src = &response;
    if (!response.has("displayState")) {
        for (auto key : {"display", "rating"}) {
            auto const& nested = response[key];
            if (nested.isObject() && nested.has("displayState")) {
                src = &nested;
                break;
            }
        }
    }
    bool present = false;
    if (auto* st = src->find("displayState"); st && st->isString() && parseRatingDisplay(st->asString(), d.state)) present = true;
    if (!present) {
        // absent / unknown: the calibration lock decides, exactly as before this field existed
        auto const& overall = response["overall"];
        bool locked = !overall.isObject() || overall.getBool("overallLocked", true);
        d.state = locked ? RatingDisplay::Locked : RatingDisplay::Visible;
    }
    if (auto* h = src->find("rankHint"); h && h->isString()) d.rankHint = h->asString();
    if (!d.locked()) {
        // The figures: flat next to `displayState` (`verifiedSigma` / `verified`, `rawSigma` / `raw`,
        // ...), or - as GET /v1/me/calibration serves them (shared V1CalibrationSigma,
        // tests/fixtures/api/me-calibration-*.json) - inside a `sigma` object { sigma, raw,
        // practical, confidence, verified, verifiedConfidence, rankBandId, ... }.
        json::Value const* fig = src;
        if (auto* nested = src->find("sigma"); nested && nested->isObject()) fig = nested;
        d.verifiedSigma = sigmaMember(*fig, {"verifiedSigma", "verified"});
        d.rawSigma = sigmaMember(*fig, {"rawSigma", "raw"});
        d.practicalSigma = sigmaMember(*fig, {"practicalSigma", "practical"});
        if (!d.practicalSigma && fig != src) d.practicalSigma = sigmaMember(*fig, {"sigma"});   // the headline value
        // the confidence shown with the figure: the verified rating's when that is the figure
        std::optional<double> conf;
        if (d.verifiedSigma) conf = firstFinite(*fig, {"verifiedConfidence"});
        if (!conf) conf = firstFinite(*fig, {"confidence", "ratingConfidence"});
        if (conf) d.confidence = std::max(0.0, std::min(1.0, *conf));
        auto const& rank = (*src)["rank"];
        if (rank.isString()) d.rankId = rank.asString();
        else if (rank.isObject()) {
            d.rankId = rank.getString("rankId", rank.getString("id"));
            d.rankDivision = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(9, rank.getInt("division", 0))));
            d.rankName = rank.getString("name", rank.getString("bandName"));
        }
        if (d.rankId.empty()) {
            // the official band id ("master-1", "grandmaster", "ascendant-4": docs/RANKS.md "Band identifiers")
            std::string bandId = fig->getString("rankBandId", src->getString("rankBandId"));
            if (!bandId.empty()) parseRankBandId(bandId, d.rankId, d.rankDivision);
        }
        if (d.rankName.empty()) d.rankName = src->getString("rankName");
    }
    if (d.state != RatingDisplay::Ranked) {
        d.rankId.clear();
        d.rankDivision = 0;
        d.rankName.clear();
    }
    else d.rankHint.clear();   // nothing to hint at once ranked
    // configuration the server answered with (the Profile tab's note builds on it)
    d.sigmaMinConfidence = unitMember(*src, "sigmaMinConfidence");
    d.rankMinConfidence = unitMember(*src, "rankMinConfidence");
    // the gamemodes the sigma/s still waits for (2026-10-02): every state, root or display object;
    // only known gamemode names, each once, at most 8 (tolerant: anything else is skipped)
    {
        json::Value const* mg = response.find("missingGamemodes");
        if ((!mg || !mg->isArray()) && src != &response) mg = src->find("missingGamemodes");
        if (mg && mg->isArray()) {
            static constexpr std::string_view kModes[] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"};
            for (auto const& v : mg->asArray()) {
                if (!v.isString() || d.missingGamemodes.size() >= 8) continue;
                std::string const g = v.asString();
                bool const known = std::find(std::begin(kModes), std::end(kModes), g) != std::end(kModes);
                if (known && std::find(d.missingGamemodes.begin(), d.missingGamemodes.end(), g) == d.missingGamemodes.end())
                    d.missingGamemodes.push_back(g);
            }
        }
    }
    // v0.14.6: why the last session gave nothing (root `note`, a string or null), every state
    if (auto* note = response.find("note"); note && note->isString()) {
        // ASCII only (GD's bitmap fonts): U+2013 / U+2014 / U+2026 become '-' / '-' / "...",
        // sigma (U+03C3) "sigma", every other non-ASCII byte is dropped
        std::string const& raw = note->asString();
        std::string text;
        for (size_t i = 0; i < raw.size() && text.size() < 320; ++i) {
            unsigned char const c = static_cast<unsigned char>(raw[i]);
            if (c < 0x80) {
                if (c >= 0x20) text.push_back(static_cast<char>(c));
                continue;
            }
            if (c == 0xE2 && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x80) {
                unsigned char const t = static_cast<unsigned char>(raw[i + 2]);
                if (t == 0x93 || t == 0x94) text.push_back('-');
                else if (t == 0xA6) text += "...";
                i += 2;
                continue;
            }
            if (c == 0xCF && i + 1 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x83) {
                text += "sigma";
                i += 1;
            }
        }
        d.sessionNote = text;
    }
    // the verification status (2026-10-02): every state, from the root or the display object
    {
        json::Value const* vs = response.find("verificationStatus");
        if ((!vs || !vs->isObject()) && src != &response) vs = src->find("verificationStatus");
        if (vs && vs->isObject()) {
            auto clip = [](std::string s) {
                if (s.size() > 120) s.resize(120);
                return s;
            };
            d.verificationStatus = clip(vs->getString("status"));
            d.verificationLabel = clip(vs->getString("label", d.verificationStatus));
            d.provisional = vs->getBool("provisional");
            d.provisionalLabel = d.provisional ? clip(vs->getString("provisionalLabel")) : std::string();
            d.verificationReason = clip(vs->getString("reason"));
        }
    }
    // the player's own private figure: in every state, independent of the public one
    readPrivateSigma(response, d);
    if (!d.privatePresent && src != &response) readPrivateSigma(*src, d);
    out = d;
    return present;
}

namespace {
/// "Ship", "UFO" (shared GAMEMODE_LABELS).
std::string gamemodeLabel(std::string const& g) {
    if (g == "ufo") return "UFO";
    std::string s = g;
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}
/// "ship", "ship and ball", "cube, ship and wave".
std::string gamemodeList(std::vector<std::string> const& modes) {
    std::string out;
    for (size_t i = 0; i < modes.size(); ++i) {
        if (i > 0) out += (i + 1 == modes.size()) ? " and " : ", ";
        out += modes[i];
    }
    return out;
}
}  // namespace

std::vector<std::string> missingGamemodeSteps(CalibrationDisplay const& d) {
    std::vector<std::string> out;
    for (auto const& g : d.missingGamemodes)
        out.push_back("Calibrate " + gamemodeLabel(g) + ": play " + g + " levels (needed before your sigma/s shows).");
    return out;
}

std::string missingGamemodesLockText(CalibrationDisplay const& d) {
    if (d.missingGamemodes.empty()) return {};
    return "sigma/s LOCKED until " + gamemodeList(d.missingGamemodes) + (d.missingGamemodes.size() == 1 ? " is" : " are") +
           " calibrated";
}

std::string sigmaHudText(CalibrationDisplay const& d, double calibrationPercent, std::string const& rankName) {
    char buf[96];
    switch (d.state) {
        case RatingDisplay::Locked: {
            std::snprintf(buf, sizeof buf, "LOCKED (calibrating %d%%)", percentInt(calibrationPercent));
            // owner decision 2026-10-01: the player's own estimate next to the lock, never a rating
            if (auto p = d.privateHeadline()) return std::string(buf) + " | private ~" + privateFigureText(*p);
            return buf;
        }
        case RatingDisplay::Visible: {
            auto sigma = d.headlineSigma();
            std::string figure = sigma ? ranks::formatSigma(*sigma) : std::string("-");
            if (d.confidence) {
                std::snprintf(buf, sizeof buf, " (%d%% conf, unranked)", percentInt(*d.confidence));
                return figure + buf;
            }
            return figure + " (unranked)";
        }
        case RatingDisplay::Ranked: {
            auto sigma = d.headlineSigma();
            std::string figure = sigma ? ranks::formatSigma(*sigma) : std::string("-");
            std::string name = !rankName.empty() ? rankName : !d.rankName.empty() ? d.rankName : std::string("ranked");
            return figure + " | " + name;
        }
    }
    return "LOCKED";
}

TopRightSigma topRightSigma(CalibrationDisplay const& d, double calibrationPercent, std::string const& rankName, bool showPrivate) {
    TopRightSigma t;
    char buf[96];
    switch (d.state) {
        case RatingDisplay::Locked: {
            t.big = "LOCKED";
            std::snprintf(buf, sizeof buf, "calibrating %d%%", percentInt(calibrationPercent));
            t.detail = buf;
            if (showPrivate) {
                if (auto p = d.privateHeadline()) t.detail += "  private ~" + privateFigureText(*p);
            }
            return t;
        }
        case RatingDisplay::Visible: {
            auto sigma = d.headlineSigma();
            t.big = sigma ? ranks::formatSigma(*sigma) : std::string("-");
            if (d.confidence) {
                std::snprintf(buf, sizeof buf, "sigma/s  %d%% conf  unranked", percentInt(*d.confidence));
                t.detail = buf;
            }
            else t.detail = "sigma/s  unranked";
            return t;
        }
        case RatingDisplay::Ranked: {
            auto sigma = d.headlineSigma();
            t.big = sigma ? ranks::formatSigma(*sigma) : std::string("-");
            std::string name = !rankName.empty() ? rankName : !d.rankName.empty() ? d.rankName : std::string("ranked");
            t.detail = "sigma/s  " + name;
            return t;
        }
    }
    t.big = "LOCKED";
    return t;
}

std::string rankHintText(CalibrationDisplay const& d, std::optional<double> ladderMinConfidence) {
    if (d.state != RatingDisplay::Visible) return {};
    if (!d.rankHint.empty()) {
        // the server's words ("rank at 90% confidence"), as a sentence on the Profile tab
        std::string hint = d.rankHint;
        hint[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(hint[0])));
        return hint;
    }
    if (ladderMinConfidence && std::isfinite(*ladderMinConfidence) && *ladderMinConfidence > 0.0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "Rank at %d%% confidence", percentInt(*ladderMinConfidence));
        return buf;
    }
    return {};
}

std::string privateSigmaProfileText(CalibrationDisplay const& d) {
    if (!d.privateHeadline()) return {};
    auto num = [](std::optional<double> v) { return v ? privateFigureText(*v) : std::string("-"); };
    std::string conf = d.privateConfidence ? std::to_string(percentInt(*d.privateConfidence)) + "%" : std::string("-");
    return "Private sigma/s (estimate, only you): raw " + num(d.privateRaw) + "   practical " + num(d.privatePractical) +
           "   confidence " + conf;
}

std::string privateSigmaNoteText(CalibrationDisplay const& d, std::optional<double> ladderMinConfidence) {
    if (!d.privateHeadline()) return {};
    auto pct = [](double x) { return std::to_string(percentInt(x)) + "%"; };
    std::string out;
    if (d.privateMinCalibration) out = "shown to you from " + pct(*d.privateMinCalibration) + " calibration";
    std::string pub;
    if (d.sigmaMinConfidence) pub = "public from " + pct(*d.sigmaMinConfidence) + " confidence";
    std::optional<double> rank = ladderMinConfidence && std::isfinite(*ladderMinConfidence) && *ladderMinConfidence > 0.0 &&
                                         *ladderMinConfidence <= 1.0
                                     ? ladderMinConfidence
                                     : d.rankMinConfidence;
    if (rank) pub += (pub.empty() ? "rank at " : ", rank at ") + pct(*rank);
    if (!pub.empty()) out += (out.empty() ? "" : "; ") + pub;
    return out;
}

}  // namespace gprl
