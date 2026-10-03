#include "window_event.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "timing_units.hpp"

namespace gprl::solver {

namespace {

double midpointEdge(BoundaryResult const& side) {
    if (side.bounded && !std::isnan(side.failShiftMs)) return 0.5 * (side.passShiftMs + side.failShiftMs);
    return side.passShiftMs;
}

std::string fmt2(char const* f, double a, double b = 0.0, double c = 0.0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

}  // namespace

WindowEventResult buildWindowEvent(WindowResult const& w, WindowEventContext const& ctx, TimingFingerprint fingerprint) {
    WindowEventResult r;
    if (!w.valid) {
        r.error = w.invalidReason.empty() ? "window result invalid" : w.invalidReason;
        return r;
    }
    if (!std::isfinite(ctx.actualMs)) {
        r.error = "actualMs not finite";
        return r;
    }
    double earlyEdge = midpointEdge(w.early);
    double lateEdge = midpointEdge(w.late);
    double earliest = ctx.actualMs + earlyEdge;
    double latest = ctx.actualMs + lateEdge;
    if (!std::isfinite(earliest) || !std::isfinite(latest)) {
        r.error = "edge not finite";
        return r;
    }
    if (latest < earliest) {
        r.error = fmt2("inverted window: earliest %.4f > latest %.4f", earliest, latest);
        return r;
    }
    double resolution = 0.0;
    if (w.early.bounded) resolution = std::max(resolution, w.early.bracketMs);
    if (w.late.bounded) resolution = std::max(resolution, w.late.bracketMs);
    if (resolution <= 0.0) resolution = kTickMs;
    double width = latest - earliest;
    bool bounded = w.early.bounded && w.late.bounded;
    if (bounded && width < resolution && !units::widthBelowResolution(width, resolution)) {
        // v0.7.1: float noise only (units::kWidthNoiseMs). The server compares `latestMs -
        // earliestMs` with `resolutionMs` exactly, so the payload carries that very difference
        // (the same doubles) as its resolution: a full one-tick window is never refused as
        // `window_below_resolution` nor labelled low confidence. Not the v0.6.x clamp: a real
        // gap (3.125 ms over a 4.167 ms bracket) is still reported as is below.
        r.debug.push_back(fmt2("width %.15g equals the resolution %.15g up to float noise", width, resolution));
        resolution = width;
    }
    if (bounded && width < resolution) {
        // v2 (V2-D8, RC3.2): NO clamp. v0.6.x overwrote the resolution with the width here, which
        // hid an untested gap inside a bracket (#595: 16.67 ms reported over a 20.83 ms bracket)
        // and defeated the server's `window_below_resolution`. The true bracket is reported; the
        // timing is `low_confidence` (`width_below_resolution`) and the server gate decides the
        // player path (it rejects it: correct).
        r.widthBelowResolution = true;
        r.debug.push_back(fmt2("width %.4f below the resolution %.4f (reported as is, low confidence)", width, resolution));
    }
    if (!(resolution > 0.0)) {
        r.error = "resolution not positive";
        return r;
    }

    telemetry::TimingWindowPayload p;
    p.inputSeq = ctx.inputSeq;
    p.inputKind = ctx.kind;
    p.earliestMs = earliest;
    p.latestMs = latest;
    p.actualMs = ctx.actualMs;
    p.boundedEarly = w.early.bounded;
    p.boundedLate = w.late.bounded;
    p.resolutionMs = resolution;
    if (ctx.kind == InputKind::Release && std::isfinite(ctx.pressMs)) {
        double hmin = earliest - ctx.pressMs;
        double hmax = latest - ctx.pressMs;
        if (std::isfinite(hmin) && std::isfinite(hmax) && hmax >= hmin) {
            p.holdMinMs = telemetry::Nullable<double>(hmin);
            p.holdMaxMs = telemetry::Nullable<double>(hmax);
        }
    }
    p.scope = "local";
    p.solverVersion = solverVersionFor(ctx.refined);
    fingerprint.kind = ctx.kind;
    fingerprint.windowMs = p.latestMs - p.earliestMs;   // exactly the doubles the server subtracts
    p.fingerprint = std::move(fingerprint);

    r.payload = std::move(p);
    r.widthMs = width;
    r.hit = ctx.actualMs >= earliest && ctx.actualMs <= latest;
    r.ok = true;
    r.debug.push_back(fmt2("edges %+.4f / %+.4f ms (midpoints), width %.4f", earlyEdge, lateEdge, width));
    return r;
}

WindowCheck checkWindowPayload(telemetry::TimingWindowPayload const& p, double eventT, WindowCheckParams const& params) {
    WindowCheck c;
    auto reject = [&](char const* why) {
        c.accepted = false;
        c.reasons.emplace_back(why);
    };
    bool finite = std::isfinite(p.earliestMs) && std::isfinite(p.latestMs) && std::isfinite(p.actualMs) && std::isfinite(p.resolutionMs)
        && std::isfinite(eventT) && std::isfinite(p.fingerprint.windowMs) && (!p.holdMinMs.hasValue() || std::isfinite(*p.holdMinMs))
        && (!p.holdMaxMs.hasValue() || std::isfinite(*p.holdMaxMs));
    if (!finite) {
        reject("non_finite_value");
        return c;
    }
    double windowMs = p.latestMs - p.earliestMs;
    double actualVsLevel = std::fabs(p.actualMs - eventT * 1000.0);
    double fpDelta = std::fabs(p.fingerprint.windowMs - windowMs);
    bool bounded = p.boundedEarly && p.boundedLate;
    if (windowMs < 0.0) reject("inverted_window");
    if (bounded && windowMs >= 0.0 && windowMs < p.resolutionMs) reject("window_below_resolution");
    if (bounded && windowMs > params.maxBoundedWindowMs) reject("window_implausibly_wide");
    if (p.resolutionMs <= 0.0 || p.resolutionMs < params.minResolutionMs || p.resolutionMs > params.maxResolutionMs) reject("resolution_out_of_range");
    if (actualVsLevel > params.actualVsLevelTimeToleranceMs) reject("actual_inconsistent_with_level_time");
    if (fpDelta > params.fingerprintWindowToleranceMs) reject("fingerprint_window_mismatch");
    if (p.fingerprint.kind != p.inputKind) reject("fingerprint_kind_mismatch");
    if (p.holdMinMs.hasValue() && p.holdMaxMs.hasValue() && *p.holdMaxMs < *p.holdMinMs) reject("hold_range_inverted");
    return c;
}

}  // namespace gprl::solver
