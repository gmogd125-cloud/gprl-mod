#include "fingerprint.hpp"

#include <algorithm>
#include <cmath>

namespace gprl {

namespace {

double gaussian(double d, double scale) {
    double z = d / scale;
    return std::exp(-0.5 * z * z);
}

// logKernel in fingerprint.ts: both null -> 1, one null -> nullMismatch, else Gaussian in ln space.
double logKernel(std::optional<double> a, std::optional<double> b, double scale, double nullMismatch) {
    if (!a && !b) return 1.0;
    if (!a || !b) return nullMismatch;
    double la = std::log(std::max(1e-6, *a));
    double lb = std::log(std::max(1e-6, *b));
    return gaussian(la - lb, scale);
}

}  // namespace

SimilarityBreakdown explainSimilarity(TimingFingerprint const& a, TimingFingerprint const& b, FingerprintParams const& p) {
    SimilarityBreakdown out;
    auto put = [&](char const* field, double k) { out.factors.emplace_back(field, k); };
    // Same order as fingerprint.ts explainSimilarity so debug output lines up.
    put("gamemode", a.gamemode == b.gamemode ? 1.0 : p.hardMismatch);
    put("kind", a.kind == b.kind ? 1.0 : p.hardMismatch);
    put("gravity", a.gravity == b.gravity ? 1.0 : p.softMismatch.gravity);
    put("speed", a.speed == b.speed ? 1.0 : p.softMismatch.speed);
    put("mini", a.mini == b.mini ? 1.0 : p.softMismatch.mini);
    put("trajectory", a.trajectory == b.trajectory ? 1.0 : p.softMismatch.trajectory);
    put("horizontalState", a.horizontalState == b.horizontalState ? 1.0 : p.softMismatch.horizontalState);
    put("portalTransition", a.portalTransition == b.portalTransition ? 1.0 : p.softMismatch.portalTransition);
    put("inputDirection", a.inputDirection == b.inputDirection ? 1.0 : p.softMismatch.inputDirection);
    // Unknown geometry (empty hash, UNKNOWN_GEOMETRY_HASH) on either side is neutral: missing data, not a mismatch.
    bool geometryKnown = !a.geometryHash.empty() && !b.geometryHash.empty();
    put("geometryHash", !geometryKnown || a.geometryHash == b.geometryHash ? 1.0 : p.softMismatch.geometryHash);
    auto const& s = p.numericScales;
    put("windowMs", logKernel(a.windowMs, b.windowMs, s.logWindow, p.nullMismatch));
    put("yVelocity", gaussian(a.yVelocity - b.yVelocity, s.yVelocity));
    put("prevInputGapMs", logKernel(a.prevInputGapMs, b.prevInputGapMs, s.logGapMs, p.nullMismatch));
    put("nextInputGapMs", logKernel(a.nextInputGapMs, b.nextInputGapMs, s.logGapMs, p.nullMismatch));
    put("holdMs", logKernel(a.holdMs, b.holdMs, s.logHoldMs, p.nullMismatch));
    double total = 1.0;
    for (auto const& f : out.factors) total *= f.second;
    out.total = std::min(1.0, std::max(0.0, total));
    return out;
}

double similarity(TimingFingerprint const& a, TimingFingerprint const& b, FingerprintParams const& p) {
    // Fast path: hard mismatches dominate and are the common case across a whole history.
    if (p.hardMismatch == 0.0 && (a.gamemode != b.gamemode || a.kind != b.kind)) return 0.0;
    return explainSimilarity(a, b, p).total;
}

std::string patternKey(TimingFingerprint const& fp) {
    std::string key(name(fp.gamemode));
    key.push_back('|');
    key.append(name(fp.kind));
    key.push_back('|');
    key += fp.geometryHash;
    return key;
}

SimilarityMass similarityMass(TimingFingerprint const& fp, double sampleTimeMs, std::vector<FingerprintAt> const& history,
                              FamiliarityParams const& fam, FingerprintParams const& fpParams) {
    SimilarityMass m;
    double const cutoff = sampleTimeMs - fam.memoryDays * 86'400'000.0;
    for (auto const& h : history) {
        if (h.timeMs < cutoff) continue;   // outside the timing-memory horizon
        double s = similarity(fp, h.fp, fpParams);
        if (s < fam.minSimilarity) continue;
        ++m.neighbours;
        m.mass += std::pow(s, fam.beta);
    }
    return m;
}

FamiliarityResult familiarityWeight(SimilarityMass const& mass, int attemptsAtCluster, FamiliarityParams const& p) {
    FamiliarityResult r;
    r.similarityMass = mass.mass;
    r.similarNeighbours = mass.neighbours;
    // MASTER §9 / decision C3 (familiarity.ts timingHistoryWeight): saturating e^(-S/k) by
    // default (1000 identical -> ~340 effective), the pre-C3 reciprocal form by config.
    r.timingWeight = p.form == FamiliarityForm::Reciprocal
        ? 1.0 / (1.0 + p.alpha * std::max(0.0, mass.mass))
        : std::exp(-std::max(0.0, mass.mass) / p.saturationK);
    r.attemptWeight = 1.0 / (1.0 + p.attemptAlpha * std::log1p(static_cast<double>(std::max(0, attemptsAtCluster))));
    if (mass.mass >= p.timingHistoryOverride) {
        r.weight = r.timingWeight;
        r.source = FamiliaritySource::Timing;
    }
    else if (mass.mass <= 0.0) {
        r.weight = r.attemptWeight;
        r.source = FamiliaritySource::Attempt;
    }
    else {
        double mix = mass.mass / p.timingHistoryOverride;
        r.weight = mix * r.timingWeight + (1.0 - mix) * r.attemptWeight;
        r.source = FamiliaritySource::Blend;
    }
    return r;
}

}  // namespace gprl
