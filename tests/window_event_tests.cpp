// window_event host tests (docs/SOLVER_DESIGN.md §9.3): WindowResult -> payload with midpoint
// edges, the 1000/240 conversion, hold fields Value / Absent, solver version by `refined`,
// windowMs == latestMs - earliestMs bit-exact, miss payloads, validateBatch + checkBatchInvariants
// with a window after attempt_end, stable canonical bytes, and the C++ mirror of windows.ts.
#include "test_util.hpp"

#include "../core/crypto.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/timing_units.hpp"
#include "../core/solver/window_event.hpp"
#include "../core/telemetry.hpp"

#include <cmath>

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::telemetry;

namespace {

TimingFingerprint sampleFingerprint() {
    TimingFingerprint f;
    f.gamemode = Gamemode::Cube;
    f.speed = Speed::Normal;
    f.geometryHash = "0123456789abcdef";
    f.inputDirection = InputDirection::Up;
    return f;
}

WindowResult coarseHit(double actualMs, double lastPassEarly, double lastPassLate, bool boundedEarly = true, bool boundedLate = true) {
    WindowResult w;
    w.valid = true;
    w.actualMs = actualMs;
    w.early.side = Side::Earlier;
    w.early.bounded = boundedEarly;
    w.early.passShiftMs = -lastPassEarly * kTickMs;
    w.early.failShiftMs = boundedEarly ? -(lastPassEarly + 1.0) * kTickMs : kNaN;
    w.early.bracketMs = boundedEarly ? kTickMs : 0.0;
    w.late.side = Side::Later;
    w.late.bounded = boundedLate;
    w.late.passShiftMs = lastPassLate * kTickMs;
    w.late.failShiftMs = boundedLate ? (lastPassLate + 1.0) * kTickMs : kNaN;
    w.late.bracketMs = boundedLate ? kTickMs : 0.0;
    w.boundedEarly = boundedEarly;
    w.boundedLate = boundedLate;
    w.earliestMs = actualMs + w.early.passShiftMs;
    w.latestMs = actualMs + w.late.passShiftMs;
    w.resolutionMs = std::max(w.early.bracketMs, w.late.bracketMs);
    return w;
}

void testFramePerfect() {
    SECTION("a frame perfect reports 4.17 ms [-2.08, +2.08] at tick resolution");
    auto w = coarseHit(1000.0, 0.0, 0.0);
    WindowEventContext ctx;
    ctx.inputSeq = 12;
    ctx.kind = InputKind::Press;
    ctx.actualMs = 1000.0;
    auto r = buildWindowEvent(w, ctx, sampleFingerprint());
    CHECK_MSG(r.ok, r.error);
    CHECK_NEAR(r.payload.earliestMs, 1000.0 - 0.5 * kTickMs, 1e-9);
    CHECK_NEAR(r.payload.latestMs, 1000.0 + 0.5 * kTickMs, 1e-9);
    CHECK_NEAR(r.widthMs, kTickMs, 1e-9);
    CHECK(r.payload.boundedEarly && r.payload.boundedLate);
    CHECK(r.payload.resolutionMs <= r.widthMs);
    CHECK_NEAR(r.payload.resolutionMs, kTickMs, 1e-9);
    CHECK(r.payload.solverVersion == "gprl-clone/6");
    CHECK(r.payload.scope == "local");
    CHECK(r.payload.inputSeq == 12);
    CHECK(r.payload.inputKind == InputKind::Press);
    CHECK(r.payload.holdMinMs.isAbsent() && r.payload.holdMaxMs.isAbsent());
    CHECK(r.payload.fingerprint.windowMs == r.payload.latestMs - r.payload.earliestMs);   // bit exact
    CHECK(r.payload.fingerprint.kind == InputKind::Press);
    CHECK(r.hit);
    auto c = checkWindowPayload(r.payload, 1.0);
    CHECK_MSG(c.accepted, c.reasons.empty() ? "" : c.reasons[0]);
}

void testMidpointsAndUnbounded() {
    SECTION("midpoint edges on bounded sides, conservative edge on an unbounded side");
    auto w = coarseHit(2000.0, 2.0, 3.0, true, false);
    WindowEventContext ctx;
    ctx.actualMs = 2000.0;
    auto r = buildWindowEvent(w, ctx, sampleFingerprint());
    CHECK(r.ok);
    CHECK_NEAR(r.payload.earliestMs, 2000.0 - 2.5 * kTickMs, 1e-9);
    CHECK_NEAR(r.payload.latestMs, 2000.0 + 3.0 * kTickMs, 1e-9);
    CHECK(!r.payload.boundedLate && r.payload.boundedEarly);
    CHECK_NEAR(r.payload.resolutionMs, kTickMs, 1e-9);
    // both unbounded: resolution is one tick
    auto u = coarseHit(2000.0, 10.0, 10.0, false, false);
    auto ru = buildWindowEvent(u, ctx, sampleFingerprint());
    CHECK(ru.ok);
    CHECK_NEAR(ru.payload.resolutionMs, kTickMs, 1e-9);
    CHECK(checkWindowPayload(ru.payload, 2.0).accepted);
    // mixed brackets: refined late side (1/8 tick), coarse early side. v2 (V2-D8, RC3.2): the
    // resolution is NEVER clamped to the width any more - the widest bracket is reported, the
    // result says widthBelowResolution (low confidence) and the server mirror rejects the window
    // for the player path (window_below_resolution), which is the honest outcome
    auto m = coarseHit(2000.0, 0.0, 0.0);
    m.late.failShiftMs = 0.125 * kTickMs;
    m.late.bracketMs = 0.125 * kTickMs;
    auto rm = buildWindowEvent(m, ctx, sampleFingerprint());
    CHECK(rm.ok);
    CHECK_NEAR(rm.payload.resolutionMs, kTickMs, 1e-12);
    CHECK(rm.widthMs < rm.payload.resolutionMs);
    CHECK(rm.widthBelowResolution);
    auto cm = checkWindowPayload(rm.payload, 2.0);
    CHECK(!cm.accepted);
    CHECK(cm.reasons.size() == 1 && cm.reasons[0] == "window_below_resolution");
    // #595 (RC3, v0.6.2 log): `+1D@97 +2A +3A +4..+7 cancelled +8D` on a MISS was reported as a
    // 16.67 ms window at a clamped resolution; without the clamp the untested bracket shows
    auto g = coarseHit(2000.0, 0.0, 0.0);
    g.early.passShiftMs = 2.0 * kTickMs;   // the passing run [+2, +3] of the miss
    g.early.failShiftMs = 1.0 * kTickMs;
    g.early.bracketMs = kTickMs;
    g.late.passShiftMs = 3.0 * kTickMs;
    g.late.failShiftMs = 8.0 * kTickMs;   // +4..+7 cancelled: the bracket is 5 ticks wide
    g.late.bracketMs = 5.0 * kTickMs;
    auto rg = buildWindowEvent(g, ctx, sampleFingerprint());
    CHECK(rg.ok);
    CHECK_NEAR(rg.widthMs, 4.0 * kTickMs, 1e-9);            // midpoints 1.5 .. 5.5 ticks
    CHECK_NEAR(rg.payload.resolutionMs, 5.0 * kTickMs, 1e-9);   // 20.83 ms, the true bracket
    CHECK(rg.widthBelowResolution);
    CHECK(!checkWindowPayload(rg.payload, 2.0).accepted);
    // a normal coarse window is not affected
    auto n = coarseHit(2000.0, 2.0, 3.0);
    auto rn = buildWindowEvent(n, ctx, sampleFingerprint());
    CHECK(rn.ok && !rn.widthBelowResolution);
}

void testReleaseHold() {
    SECTION("release windows carry hold bounds relative to the press; presses leave them absent");
    auto w = coarseHit(1100.0, 1.0, 1.0);
    WindowEventContext ctx;
    ctx.kind = InputKind::Release;
    ctx.actualMs = 1100.0;
    ctx.pressMs = 1050.0;
    auto r = buildWindowEvent(w, ctx, sampleFingerprint());
    CHECK(r.ok);
    CHECK(r.payload.holdMinMs.hasValue() && r.payload.holdMaxMs.hasValue());
    CHECK_NEAR(*r.payload.holdMinMs, r.payload.earliestMs - 1050.0, 1e-12);
    CHECK_NEAR(*r.payload.holdMaxMs, r.payload.latestMs - 1050.0, 1e-12);
    CHECK(*r.payload.holdMaxMs >= *r.payload.holdMinMs);
    CHECK(r.payload.inputKind == InputKind::Release && r.payload.fingerprint.kind == InputKind::Release);
    // unknown press: absent, never null (the golden fixture form)
    ctx.pressMs = kNaN;
    auto r2 = buildWindowEvent(w, ctx, sampleFingerprint());
    CHECK(r2.ok && r2.payload.holdMinMs.isAbsent() && r2.payload.holdMaxMs.isAbsent());
    CHECK(checkWindowPayload(r.payload, 1.1).accepted);
}

void testSolverVersionAndMiss() {
    SECTION("solver version by refinement; a miss payload has the actual time outside");
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = s;
        o.kind = (s >= -4.0 && s <= -2.0) ? ShiftKind::Survived : ShiftKind::Died;
        o.deathAfterFrames = 2.0;
        o.objectId = 8;
        outs.push_back(o);
    }
    p.ingest(outs, false, true);
    auto w = p.result(3000.0);
    CHECK(w.valid);
    WindowEventContext ctx;
    ctx.actualMs = 3000.0;
    ctx.refined = true;
    auto r = buildWindowEvent(w, ctx, sampleFingerprint());
    CHECK_MSG(r.ok, r.error);
    CHECK(r.payload.solverVersion == "gprl-clone/6-cbf");
    CHECK(!r.hit);
    CHECK(r.payload.actualMs > r.payload.latestMs);
    CHECK_NEAR(r.payload.earliestMs, 3000.0 - 4.5 * kTickMs, 1e-9);
    CHECK_NEAR(r.payload.latestMs, 3000.0 - 1.5 * kTickMs, 1e-9);
    CHECK(checkWindowPayload(r.payload, 3.0).accepted);
    CHECK(std::string(solverVersionFor(false)) == "gprl-clone/6");
}

void testNeverEmitsBadValues() {
    SECTION("non-finite or inverted values never leave the builder");
    WindowEventContext ctx;
    ctx.actualMs = 1000.0;
    auto bad = coarseHit(1000.0, 0.0, 0.0);
    bad.valid = false;
    bad.invalidReason = "control clone did not reproduce the real player";
    auto r1 = buildWindowEvent(bad, ctx, sampleFingerprint());
    CHECK(!r1.ok && r1.error == bad.invalidReason);
    auto inv = coarseHit(1000.0, -3.0, -3.0, false, false);   // pass edges crossed
    auto r2 = buildWindowEvent(inv, ctx, sampleFingerprint());
    CHECK(!r2.ok);
    auto nan = coarseHit(1000.0, 0.0, 0.0);
    nan.late.passShiftMs = kNaN;
    auto r3 = buildWindowEvent(nan, ctx, sampleFingerprint());
    CHECK(!r3.ok);
    ctx.actualMs = kNaN;
    auto r4 = buildWindowEvent(coarseHit(1000.0, 0.0, 0.0), ctx, sampleFingerprint());
    CHECK(!r4.ok);
}

void testServerMirrorRejections() {
    SECTION("the windows.ts mirror flags every reason the server checks");
    auto w = coarseHit(1000.0, 1.0, 1.0);
    WindowEventContext ctx;
    ctx.actualMs = 1000.0;
    auto good = buildWindowEvent(w, ctx, sampleFingerprint()).payload;
    CHECK(checkWindowPayload(good, 1.0).accepted);
    auto has = [](WindowCheck const& c, char const* reason) {
        for (auto const& r : c.reasons) if (r == reason) return true;
        return false;
    };
    auto p = good;
    p.latestMs = p.earliestMs - 1.0;
    CHECK(has(checkWindowPayload(p, 1.0), "inverted_window"));
    p = good;
    p.resolutionMs = p.latestMs - p.earliestMs + 0.01;
    CHECK(has(checkWindowPayload(p, 1.0), "window_below_resolution"));
    p = good;
    p.latestMs = p.earliestMs + 3000.0;
    p.fingerprint.windowMs = 3000.0;
    CHECK(has(checkWindowPayload(p, 1.0), "window_implausibly_wide"));
    p = good;
    p.resolutionMs = 0.001;
    CHECK(has(checkWindowPayload(p, 1.0), "resolution_out_of_range"));
    p = good;
    CHECK(has(checkWindowPayload(p, 2.0), "actual_inconsistent_with_level_time"));
    p = good;
    p.fingerprint.windowMs += 1.0;
    CHECK(has(checkWindowPayload(p, 1.0), "fingerprint_window_mismatch"));
    p = good;
    p.fingerprint.kind = InputKind::Release;
    CHECK(has(checkWindowPayload(p, 1.0), "fingerprint_kind_mismatch"));
    p = good;
    p.holdMinMs = Nullable<double>(10.0);
    p.holdMaxMs = Nullable<double>(5.0);
    CHECK(has(checkWindowPayload(p, 1.0), "hold_range_inverted"));
    p = good;
    p.earliestMs = kNaN;
    auto c = checkWindowPayload(p, 1.0);
    CHECK(!c.accepted && c.reasons.size() == 1 && c.reasons[0] == "non_finite_value");
}

void testOneTickWindowAtLevelTime() {
    SECTION("v0.7.1: a one-tick window at any level time is not below its resolution (float noise of actualMs + edge; owner's 19.47.37 log: 18 of 21 gate refusals)");
    // every tick of a 2-minute attempt, whole and half tick: frame perfect [0, 0] -> width 1 tick
    int noisy = 0, n = 0;
    bool allOk = true;
    for (int k = 0; k <= 28800; ++k) {
        for (double frac : {0.0, 0.5}) {
            double actual = (k + frac) * kTickMs;
            double rawWidth = (actual + 0.5 * kTickMs) - (actual - 0.5 * kTickMs);
            if (rawWidth < kTickMs) ++noisy;   // the v0.7.0 builder flagged and the server refused these
            auto w = coarseHit(actual, 0.0, 0.0);
            WindowEventContext ctx;
            ctx.actualMs = actual;
            auto r = buildWindowEvent(w, ctx, sampleFingerprint());
            auto c = checkWindowPayload(r.payload, actual / 1000.0);
            double width = r.payload.latestMs - r.payload.earliestMs;
            bool ok = r.ok && !r.widthBelowResolution && c.accepted && !(width < r.payload.resolutionMs)
                && std::fabs(r.payload.resolutionMs - kTickMs) <= units::kWidthNoiseMs;
            if (!ok && allOk) CHECK_MSG(false, "first failing actualMs: " + std::to_string(actual) + (c.reasons.empty() ? std::string() : " " + c.reasons[0]));
            allOk = allOk && ok;
            ++n;
        }
    }
    CHECK(allOk);
    CHECK(n == 57602);
    CHECK(noisy > 0);   // non-vacuous: the noise is real at these magnitudes
    // the log's input #12 (t = 1.8083 s, tick 434): (actual + 2.083) - (actual - 2.083) misses 1 tick
    {
        double actual = 434.0 * kTickMs;
        CHECK(units::widthBelowResolution(4.166666666666629, kTickMs) == false);
        auto r = buildWindowEvent(coarseHit(actual, 0.0, 0.0), [&] { WindowEventContext c; c.actualMs = actual; return c; }(), sampleFingerprint());
        CHECK(r.ok && !r.widthBelowResolution);
        CHECK(r.payload.resolutionMs <= r.payload.latestMs - r.payload.earliestMs);   // exactly the server's comparison
    }
    // a REAL gap is still reported as is (no v0.6.x clamp): early bracket half a tick, late one
    // tick -> width 0.75 tick < resolution 1 tick, flagged and refused by the mirror
    {
        double actual = 434.0 * kTickMs;
        auto w = coarseHit(actual, 0.0, 0.0);
        w.early.failShiftMs = -0.5 * kTickMs;
        w.early.bracketMs = 0.5 * kTickMs;
        WindowEventContext ctx;
        ctx.actualMs = actual;
        auto r = buildWindowEvent(w, ctx, sampleFingerprint());
        CHECK(r.ok && r.widthBelowResolution);
        CHECK_NEAR(r.payload.latestMs - r.payload.earliestMs, 0.75 * kTickMs, 1e-9);
        CHECK_NEAR(r.payload.resolutionMs, kTickMs, 1e-12);
        auto c = checkWindowPayload(r.payload, actual / 1000.0);
        CHECK(!c.accepted && c.reasons.size() == 1 && c.reasons[0] == "window_below_resolution");
    }
    CHECK(units::widthBelowResolution(kTickMs - 2e-6, kTickMs));
    CHECK(!units::widthBelowResolution(kTickMs - 5e-7, kTickMs));
    CHECK(!units::widthBelowResolution(kTickMs, kTickMs));
}

void testBatchWithWindowAfterAttemptEnd() {
    SECTION("a window emitted after attempt_end validates and passes the ordering invariants");
    Batch b;
    b.sessionId = "sess-1";
    b.seq = 1;
    b.nonce = "n1";
    b.clientBuild = "gprl-geode 0.4.0+win";
    Event start;
    start.t = 0.0;
    start.tick = 0;
    start.seq = 1;
    start.attemptId = "a1";
    AttemptStartPayload sp;
    sp.attemptNo = 1;
    start.payload = sp;
    b.events.push_back(start);
    Event in;
    in.t = 1.0;
    in.tick = 240;
    in.seq = 2;
    in.attemptId = "a1";
    in.payload = InputPayload{1, Button::Jump, true, 0.0};
    b.events.push_back(in);
    Event end;
    end.t = 1.5;
    end.tick = 360;
    end.seq = 3;
    end.attemptId = "a1";
    AttemptEndPayload ep;
    ep.reason = AttemptEndReason::Death;
    ep.percent = 12.0;
    end.payload = ep;
    b.events.push_back(end);
    auto w = coarseHit(1000.0, 1.0, 2.0);
    WindowEventContext ctx;
    ctx.inputSeq = 2;
    ctx.actualMs = 1000.0;
    auto r = buildWindowEvent(w, ctx, sampleFingerprint());
    CHECK(r.ok);
    Event win;
    win.t = 1.0;
    win.tick = 240;
    win.seq = 4;
    win.attemptId = "a1";
    win.payload = r.payload;
    b.events.push_back(win);
    std::string err;
    CHECK_MSG(validateBatch(b, &err), err);
    CHECK_MSG(checkBatchInvariants(b, -1, {}, &err), err);
    // canonical bytes are stable across two serialisations of equal content
    std::string c1 = canonicalBody(b);
    Batch b2;
    CHECK(parseBatch(serializeBatch(b, false), b2, &err));
    std::string c2 = canonicalBody(b2);
    CHECK(c1 == c2);
    CHECK(c1.find("\"kind\":\"timing_window\"") != std::string::npos);
    CHECK(c1.find("\"solverVersion\":\"gprl-clone/6\"") != std::string::npos);
    CHECK(c1.find("holdMinMs") == std::string::npos);   // absent for a press
    CHECK(crypto::toHex(crypto::sha256(c1)) == crypto::toHex(crypto::sha256(c2)));
}

}  // namespace

int main() {
    testFramePerfect();
    testMidpointsAndUnbounded();
    testReleaseHold();
    testSolverVersionAndMiss();
    testNeverEmitsBadValues();
    testServerMirrorRejections();
    testBatchWithWindowAfterAttemptEnd();
    testOneTickWindowAtLevelTime();
    return gprl::test::finish("window_event_tests");
}
