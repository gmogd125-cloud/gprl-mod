#pragma once
// Helpers shared by the fixture-driven host tests: JSON -> TimingFingerprint / params, ISO-8601
// timestamps -> unix ms (the TypeScript side uses Date.parse).
#include <cstdio>
#include <string>

#include "../core/fingerprint.hpp"
#include "../core/json.hpp"
#include "../core/telemetry.hpp"

namespace gprl::test {

inline FingerprintParams fingerprintParamsFromJson(json::Value const& p) {
    FingerprintParams f;
    f.hardMismatch = p.getNumber("hardMismatch", f.hardMismatch);
    auto const& soft = p["softMismatch"];
    f.softMismatch.gravity = soft.getNumber("gravity", f.softMismatch.gravity);
    f.softMismatch.speed = soft.getNumber("speed", f.softMismatch.speed);
    f.softMismatch.mini = soft.getNumber("mini", f.softMismatch.mini);
    f.softMismatch.trajectory = soft.getNumber("trajectory", f.softMismatch.trajectory);
    f.softMismatch.horizontalState = soft.getNumber("horizontalState", f.softMismatch.horizontalState);
    f.softMismatch.portalTransition = soft.getNumber("portalTransition", f.softMismatch.portalTransition);
    f.softMismatch.inputDirection = soft.getNumber("inputDirection", f.softMismatch.inputDirection);
    f.softMismatch.geometryHash = soft.getNumber("geometryHash", f.softMismatch.geometryHash);
    auto const& sc = p["numericScales"];
    f.numericScales.logWindow = sc.getNumber("logWindow", f.numericScales.logWindow);
    f.numericScales.yVelocity = sc.getNumber("yVelocity", f.numericScales.yVelocity);
    f.numericScales.logGapMs = sc.getNumber("logGapMs", f.numericScales.logGapMs);
    f.numericScales.logHoldMs = sc.getNumber("logHoldMs", f.numericScales.logHoldMs);
    f.nullMismatch = p.getNumber("nullMismatch", f.nullMismatch);
    return f;
}

inline FamiliarityParams familiarityParamsFromJson(json::Value const& p) {
    FamiliarityParams f;
    f.form = p.getString("form", "saturating") == "reciprocal" ? FamiliarityForm::Reciprocal : FamiliarityForm::Saturating;
    f.saturationK = p.getNumber("saturationK", f.saturationK);
    f.alpha = p.getNumber("alpha", f.alpha);
    f.beta = p.getNumber("beta", f.beta);
    f.minSimilarity = p.getNumber("minSimilarity", f.minSimilarity);
    f.memoryDays = p.getNumber("memoryDays", f.memoryDays);
    f.attemptAlpha = p.getNumber("attemptAlpha", f.attemptAlpha);
    f.timingHistoryOverride = p.getNumber("timingHistoryOverride", f.timingHistoryOverride);
    return f;
}

/// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
inline long long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/// "2026-09-28T12:00:00.000Z" -> unix ms. Only the UTC layout the fixtures use; 0 on failure.
inline double isoToUnixMs(std::string const& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
#ifdef _MSC_VER
    // %d-only format: sscanf_s takes the same arguments and avoids the C4996 deprecation warning
    int n = sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi, &s, &ms);
#else
    int n = std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi, &s, &ms);
#endif
    if (n < 6) return 0.0;
    if (n < 7) ms = 0;
    long long days = daysFromCivil(y, mo, d);
    return static_cast<double>(((days * 24 + h) * 60 + mi) * 60 + s) * 1000.0 + ms;
}

}  // namespace gprl::test
