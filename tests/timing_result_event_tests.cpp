// timing_result_event host tests (docs/TIMING_SOLVER_V2.md §4.1, §4.3, §5): the payload built from
// planner results validates (C++ mirror of validate.ts) and passes the server gate mirror; its
// `local` window equals the same input's timing_window edges BIT FOR BIT (buildWindowEvent and
// buildTimingResultEvent use the same doubles); misses carry no sequence window; releases carry
// the hold range (basis sequence / local); a builder failure yields the valid FALLBACK payload
// (unresolved, payload_invalid) so a bound job still ends in exactly one timing_result.
// v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §3.3, §2.7): a live_mutation_detected status drops every
// window / the hold / miss and the replay flag whatever evidence is passed, its fallback keeps the
// status, the gate mirror never accepts it; the dual PAIR block rides along with `dual_pair`.
#include "test_util.hpp"

#include "../core/solver/pass_planner.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_units.hpp"
#include "../core/solver/window_event.hpp"
#include "../core/telemetry.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;

namespace {

PassPlanner planner(std::vector<std::pair<double, double>> const& pass, bool miss = false, int laterFixed = 1) {
    PlannerConfig cfg;
    PassPlanner p(cfg, kNaN);
    auto shifts = p.nextPass();
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = s;
        bool ok = false;
        for (auto const& [a, b] : pass) if (s >= a - 1e-9 && s <= b + 1e-9) ok = true;
        if (ok) o.kind = ShiftKind::Survived;
        else {
            o.kind = ShiftKind::Died;
            o.deathAfterFrames = 40.0;
            o.laterFixed = laterFixed;
            o.objectId = 8;
        }
        outs.push_back(o);
    }
    p.ingest(outs, !miss, miss);
    return p;
}

TimingFingerprint fp() {
    TimingFingerprint f;
    f.gamemode = Gamemode::Wave;
    f.geometryHash = "5e91a0c2";
    return f;
}

void testLocalEqualsTimingWindow() {
    SECTION("the local window equals the timing_window edges bit for bit (also with a sub-tick actualMs)");
    for (double subTick : {0.0, 0.37 * kTickMs}) {
        double eventT = 14.583333333333334;
        double actual = units::actualMs(eventT, subTick);
        auto p = planner({{-2.0, 3.0}});
        auto w = p.result(actual);
        WindowEventContext wc;
        wc.inputSeq = 3157;
        wc.kind = InputKind::Press;
        wc.actualMs = actual;
        auto tw = buildWindowEvent(w, wc, fp());
        CHECK(tw.ok);
        SAResult sa;
        sa.valid = true;
        sa.sequence.present = true;
        sa.sequence.early = {-5.0, -6.0, EdgeStop::Fail, EdgeCause::Self, 0, 18.0, -1, kNaN, false};
        sa.sequence.late = {6.0, 7.0, EdgeStop::Fail, EdgeCause::Self, 0, 30.0, -1, kNaN, false};
        // both sides widened by re-joining pair trials (Fable D3b: proof rejoined, proven = pass)
        sa.sequence.early.proof = sa.sequence.late.proof = SAProof::Rejoined;
        sa.sequence.early.provenPassFrames = -5.0;
        sa.sequence.late.provenPassFrames = 6.0;
        sa.decided = true;
        sa.sideDecided[0] = sa.sideDecided[1] = true;
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &p.outcomes();
        ev.frame = 3500.0;
        ev.nextFrame = 3518.0;
        ev.nextFollows = true;
        ev.horizonFrame = 3630.0;
        TimingResultContext ctx;
        ctx.inputSeq = 3157;
        ctx.kind = InputKind::Press;
        ctx.attemptInputIndex = 3;
        ctx.eventT = eventT;
        ctx.subTickMs = subTick;
        ctx.subtick = subTick > 0.0;
        ctx.x = 4614.0;
        ctx.percentAtInput = 13.042;
        ctx.gamemode = Gamemode::Wave;
        ctx.cluster = {"a0f35ce1b7-a8:668", 3, cluster::Tri::Yes, cluster::Tri::Yes};
        auto st = status::statusOf({}, localFacts(ev), saFacts(&sa), {});
        auto b = buildTimingResultEvent(ctx, ev, &sa, st, true);
        CHECK_MSG(b.ok, b.error);
        CHECK(b.payload.local && b.payload.local->earliestMs == tw.payload.earliestMs && b.payload.local->latestMs == tw.payload.latestMs);
        CHECK(b.payload.actualMs == tw.payload.actualMs);
        auto check = checkTimingResultPayload(b.payload, eventT, subTick, &tw.payload);
        CHECK_MSG(check.accepted, check.reasons.empty() ? "" : check.reasons[0]);
        CHECK(b.payload.status == "ok");
        CHECK(b.payload.local->early.placement == (subTick > 0.0 ? "cbf" : "tick"));
        CHECK(b.payload.local->early.cause && *b.payload.local->early.cause == "downstream");
        // a doctored window is caught by the gate mirror
        auto doctored = tw.payload;
        doctored.earliestMs -= 1e-9;
        CHECK(!checkTimingResultPayload(b.payload, eventT, subTick, &doctored).accepted);
        CHECK(!checkTimingResultPayload(b.payload, eventT + 0.001, subTick, &tw.payload).accepted);
        auto has = [](TimingResultCheck const& c, char const* r) {
            for (auto const& x : c.reasons) if (x == r) return true;
            return false;
        };
        // server gate D1 (api/src/processing/timingResults.ts sequenceContainsLocal): an SA side
        // whose last PASS lies inside the local pass edge is refused even when its midpoint still
        // contains the local edge (the batch validator alone accepts it: the gate stores such a
        // result unusable instead of rejecting the player's whole batch)
        {
            auto q = b.payload;
            auto& sw = q.sequence->window;
            sw.late.passMs = q.local->late.passMs - kTickMs;
            sw.late.failMs = q.local->late.passMs + 3.0 * kTickMs;
            sw.latestMs = q.actualMs + 0.5 * (sw.late.passMs + *sw.late.failMs);
            std::string verr;
            CHECK_MSG(telemetry::validateTimingResult(q, "", &verr), verr);
            auto qc = checkTimingResultPayload(q, eventT, subTick, &tw.payload);
            CHECK(!qc.accepted && has(qc, "sequence_not_containing_local") && !has(qc, "payload_invalid"));
        }
        // ... and so is an undecided side reported at its last pass: 0.5 tick INSIDE the local
        // midpoint (Fable W1: the rule compares the reported edges too)
        {
            auto q = b.payload;
            auto& sw = q.sequence->window;
            sw.late = q.local->late;
            sw.late.stop = "undecided";
            sw.late.failMs.reset();
            sw.late.cause.reset();
            sw.late.laterInputs.reset();
            sw.late.failAfterMs.reset();
            sw.late.failObjectId.reset();
            sw.latestMs = q.actualMs + sw.late.passMs;
            q.sequence->decided = false;
            q.status = "sequence_dependent";
            q.statusReasons = {"sa_undecided", "local_edge_downstream"};
            std::string verr;
            CHECK_MSG(telemetry::validateTimingResult(q, "", &verr), verr);
            auto qc = checkTimingResultPayload(q, eventT, subTick, &tw.payload);
            CHECK(!qc.accepted && has(qc, "sequence_not_containing_local"));
        }
        // Fable D1: an undecided SA side at the local edge carries the local bracket (inherited):
        // valid, accepted by the gate, and its reported edge IS the local edge bit for bit
        {
            SAResult su = sa;
            su.sequence.late = {w.late.passShiftMs / kTickMs, w.late.failShiftMs / kTickMs, EdgeStop::Undecided, EdgeCause::None, -1, kNaN, -1, kNaN, false};
            su.sequence.late.inherited = true;
            su.decided = false;
            su.sideDecided[1] = false;
            auto su2 = status::statusOf({}, localFacts(ev), saFacts(&su), {});
            CHECK(su2.status == status::TimingStatus::SequenceDependent);
            auto bi = buildTimingResultEvent(ctx, ev, &su, su2, true);
            CHECK_MSG(bi.ok, bi.error);
            CHECK(bi.payload.sequence && bi.payload.sequence->window.latestMs == bi.payload.local->latestMs);
            CHECK(bi.payload.sequence->window.late.stop == "undecided" && !bi.payload.sequence->window.late.cause);
            CHECK(bi.payload.sequence->window.late.failMs && *bi.payload.sequence->window.late.failMs == *bi.payload.local->late.failMs);
            auto ic = checkTimingResultPayload(bi.payload, eventT, subTick, &tw.payload);
            CHECK_MSG(ic.accepted, ic.reasons.empty() ? "" : ic.reasons[0]);
            // a carried bracket that is NOT the local one is refused
            auto q = bi.payload;
            *q.sequence->window.late.failMs += kTickMs;
            q.sequence->window.latestMs = q.actualMs + 0.5 * (q.sequence->window.late.passMs + *q.sequence->window.late.failMs);
            std::string verr;
            CHECK(!telemetry::validateTimingResult(q, "", &verr));
            CHECK_MSG(verr.find("/sequence/late/failMs") != std::string::npos, verr);
        }
        // Fable D11: the ENGINE's sub-tick is the source of actualMs (engineSubTickMs), subTickMs stays
        // the tracker's; the two clocks must agree within 0.5 ms
        {
            for (double d : {0.3, 0.6}) {
                TimingResultContext c2 = ctx;
                c2.engineSubTickMs = subTick + d;   // the engine's clock d ms after the tracker's
                auto w2 = p.result(units::actualMs(eventT, c2.engineSubTickMs));   // shifts are from the engine's frame
                LocalEvidence ev2 = ev;
                ev2.window = &w2;
                auto b2 = buildTimingResultEvent(c2, ev2, &sa, st, true);
                CHECK_MSG(b2.ok, b2.error);
                CHECK(b2.payload.engineSubTickMs && *b2.payload.engineSubTickMs == subTick + d && b2.payload.subTickMs == subTick);
                CHECK(b2.payload.actualMs == units::actualMs(eventT, subTick + d));
                auto sc = checkTimingResultPayload(b2.payload, eventT, subTick);
                if (d < 0.5) CHECK_MSG(sc.accepted, sc.reasons.empty() ? "" : sc.reasons[0]);   // the same instant for the gate
                else CHECK(!sc.accepted && has(sc, "subtick_clock_mismatch") && !has(sc, "actual_inconsistent_with_input"));
            }
            // without engineSubTickMs (v0.7.0 payloads) actualMs is the tracker's: the old rule
            auto sc0 = checkTimingResultPayload(b.payload, eventT - 0.6 / 1000.0, subTick + 0.6, &tw.payload);
            CHECK(!sc0.accepted && has(sc0, "subtick_clock_mismatch"));
        }
        // a bounded window wider than 2000 ms is implausible (the sequence window here)
        {
            auto q = b.payload;
            auto& sw = q.sequence->window;
            sw.late.passMs += 3000.0;
            sw.late.failMs = *sw.late.failMs + 3000.0;
            sw.latestMs = q.actualMs + 0.5 * (sw.late.passMs + *sw.late.failMs);
            auto wc = checkTimingResultPayload(q, eventT, subTick, &tw.payload);
            CHECK(!wc.accepted && has(wc, "window_implausibly_wide") && !has(wc, "payload_invalid"));
        }
    }
}

void testMissHoldFallback() {
    SECTION("misses: no sequence window, low_confidence; releases: the hold range; the fallback payload");
    // a miss: the control died with the real player
    auto m = planner({{-4.0, -2.0}}, true, 0);
    auto wm = m.result(2000.0);
    CHECK(wm.valid);
    LocalEvidence ev;
    ev.window = &wm;
    ev.outcomes = &m.outcomes();
    ev.miss = true;
    ev.frame = 480.0;
    TimingResultContext ctx;
    ctx.inputSeq = 9;
    ctx.kind = InputKind::Press;
    ctx.eventT = 2.0;
    ctx.cluster = {"a1:5", 1, cluster::Tri::No, cluster::Tri::Unknown};
    SAResult sa;
    sa.valid = true;
    sa.sequence.present = true;
    auto st = status::statusOf({}, localFacts(ev), saFacts(&sa), {});
    CHECK(st.status == status::TimingStatus::LowConfidence);
    auto b = buildTimingResultEvent(ctx, ev, &sa, st, true);
    CHECK_MSG(b.ok, b.error);
    CHECK(b.payload.miss && b.payload.local && !b.payload.sequence);
    // a release with a known press, no SA result: hold basis local
    auto r = planner({{-2.0, 2.0}}, false, 0);
    auto wr = r.result(1575.0);
    LocalEvidence er;
    er.window = &wr;
    er.outcomes = &r.outcomes();
    er.frame = 378.0;
    TimingResultContext cr;
    cr.inputSeq = 3;
    cr.kind = InputKind::Release;
    cr.eventT = 1.575;
    cr.pressMs = 1500.0;
    cr.pressSeq = 2;
    cr.cluster = {"a1:1", 2, cluster::Tri::Yes, cluster::Tri::No};
    auto sr = status::statusOf({}, localFacts(er), saFacts(nullptr), {});
    auto br = buildTimingResultEvent(cr, er, nullptr, sr, true);
    CHECK_MSG(br.ok, br.error);
    CHECK(br.payload.hold && br.payload.hold->basis == "local");
    if (br.payload.hold) {
        CHECK(br.payload.hold->minMs == br.payload.local->earliestMs - 1500.0 && br.payload.hold->maxMs == br.payload.local->latestMs - 1500.0);
        CHECK(br.payload.hold->pressSeq == 2);
    }
    CHECK(br.payload.status == "low_confidence");   // sa_not_measured: never ok without a decided sequence window
    // an unresolved job without a window
    LocalEvidence none;
    auto su = status::statusOf({{status::Reason::CutByRestart}}, localFacts(none), saFacts(nullptr), {});
    auto bu = buildTimingResultEvent(cr, none, nullptr, su, true);
    CHECK(bu.ok && bu.payload.status == "unresolved" && !bu.payload.local && !bu.payload.hold);
    // the fallback: an invalid payload (a non-finite position) becomes unresolved / payload_invalid, still valid
    TimingResultContext bad = cr;
    bad.x = std::nan("");
    bad.cluster.id = "not an id";
    auto bf = buildTimingResultEvent(bad, er, nullptr, sr, true);
    CHECK(!bf.ok);
    CHECK(bf.payload.status == "unresolved" && bf.payload.statusReasons.size() == 1 && bf.payload.statusReasons[0] == "payload_invalid");
    std::string err;
    CHECK_MSG(telemetry::validateTimingResult(bf.payload, "", &err), err);
    CHECK(bf.payload.cluster.id == "not_an_id:1");
}

void testInvariantsRandom() {
    SECTION("200 random planner outcomes: every built payload validates and passes the gate mirror");
    uint32_t x = 777;
    auto rnd = [&]() {
        x = x * 1664525u + 1013904223u;
        return static_cast<double>(x >> 8) / static_cast<double>(1u << 24);
    };
    int bad = 0;
    for (int n = 0; n < 200; ++n) {
        double a = -std::floor(rnd() * 11.0), b = std::floor(rnd() * 11.0);
        bool miss = rnd() < 0.15;
        auto p = planner(miss ? std::vector<std::pair<double, double>>{{2.0, 5.0}} : std::vector<std::pair<double, double>>{{a, b}}, miss, rnd() < 0.5 ? 0 : 2);
        double eventT = 1.0 + n * 0.1;
        auto w = p.result(eventT * 1000.0);
        if (!w.valid) continue;
        WindowEventContext wc;
        wc.inputSeq = n + 2;
        wc.actualMs = eventT * 1000.0;
        auto tw = buildWindowEvent(w, wc, fp());
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &p.outcomes();
        ev.miss = miss;
        ev.widthBelowResolution = tw.widthBelowResolution;
        ev.frame = eventT * 240.0;
        ev.nextFrame = ev.frame + 18.0;
        ev.nextFollows = true;
        ev.horizonFrame = ev.frame + 130.0;
        SAResult sa;
        sa.valid = true;
        sa.sequence.present = true;
        // the early side undecided right beyond the local edge: it carries the local bracket (Fable D1)
        sa.sequence.early = {w.early.passShiftMs / kTickMs, w.early.bounded ? w.early.failShiftMs / kTickMs : kNaN, EdgeStop::Undecided, EdgeCause::None, -1, kNaN, -1,
                             kNaN, false};
        sa.sequence.early.inherited = w.early.bounded;
        // the late side decided exactly at the local edge (a self fail) or at the range
        if (w.late.bounded) sa.sequence.late = {w.late.passShiftMs / kTickMs, w.late.failShiftMs / kTickMs, EdgeStop::Fail, EdgeCause::Self, 0, 40.0, 8, kNaN, false};
        else sa.sequence.late = {w.late.passShiftMs / kTickMs, kNaN, EdgeStop::Range, EdgeCause::None, -1, kNaN, -1, kNaN, false};
        sa.sideDecided[1] = true;
        TimingResultContext ctx;
        ctx.inputSeq = n + 2;
        ctx.eventT = eventT;
        ctx.cluster = {"a1:1", 1, cluster::Tri::Unknown, cluster::Tri::Unknown};
        auto st = status::statusOf({}, localFacts(ev), saFacts(&sa), {});
        auto built = buildTimingResultEvent(ctx, ev, &sa, st, true);
        auto check = checkTimingResultPayload(built.payload, eventT, 0.0, &tw.payload);
        if (!built.ok || !check.accepted) {
            ++bad;
            if (bad <= 3) std::printf("  case %d: %s %s\n", n, built.error.c_str(), check.reasons.empty() ? "" : check.reasons[0].c_str());
        }
    }
    CHECK(bad == 0);
}

bool hasReason(TimingResultCheck const& c, char const* r) {
    for (auto const& x : c.reasons) {
        if (x == r) return true;
    }
    return false;
}

void testLiveIsolation() {
    SECTION("v0.8.0 live_mutation_detected: no window / hold / miss, replay invalid, fallback keeps the breach, gate mirror refuses");
    // a release with a valid local window, a known press and a decided SA result: everything a
    // measurement could carry - and the invariant aborted the sample
    auto r = planner({{-2.0, 2.0}}, false, 0);
    auto wr = r.result(1575.0);
    CHECK(wr.valid);
    LocalEvidence er;
    er.window = &wr;
    er.outcomes = &r.outcomes();
    er.frame = 378.0;
    er.miss = true;
    SAResult sa;
    sa.valid = true;
    sa.sequence.present = true;
    sa.sequence.early = {-5.0, -6.0, EdgeStop::Fail, EdgeCause::Self, 0, 18.0, -1, kNaN, false};
    sa.sequence.late = {6.0, 7.0, EdgeStop::Fail, EdgeCause::Self, 0, 30.0, -1, kNaN, false};
    sa.decided = true;
    sa.sideDecided[0] = sa.sideDecided[1] = true;
    TimingResultContext cr;
    cr.inputSeq = 3;
    cr.kind = InputKind::Release;
    cr.eventT = 1.575;
    cr.pressMs = 1500.0;
    cr.pressSeq = 2;
    cr.cluster = {"a1:1", 2, cluster::Tri::Yes, cluster::Tri::No};
    status::ContextFacts cx;
    cx.liveMutation = true;
    auto st = status::statusOf({}, localFacts(er), saFacts(&sa), cx);
    CHECK(status::isLiveMutation(st));
    auto b = buildTimingResultEvent(cr, er, &sa, st, true);
    CHECK_MSG(b.ok, b.error);
    CHECK(b.payload.status == "live_mutation_detected" && b.payload.statusReasons == std::vector<std::string>{"live_mutation_detected"});
    CHECK(!b.payload.stateReplayValid && !b.payload.miss);
    CHECK(!b.payload.local && !b.payload.sequence && !b.payload.pair && !b.payload.hold);
    CHECK(b.payload.solverVersion == std::string(solverVersionFor(false)));
    std::string err;
    CHECK_MSG(telemetry::validateTimingResult(b.payload, "", &err), err);
    // the gate mirror refuses it (never usable), for the breach alone - the payload is valid
    auto gate = checkTimingResultPayload(b.payload, 1.575, 0.0);
    CHECK(!gate.accepted && hasReason(gate, "live_mutation_detected") && !hasReason(gate, "payload_invalid"));
    // the fallback keeps the breach: a broken context still yields live_mutation_detected
    TimingResultContext bad = cr;
    bad.x = std::nan("");
    auto bf = buildTimingResultEvent(bad, er, &sa, st, true);
    CHECK(!bf.ok && bf.payload.status == "live_mutation_detected" && bf.status.status == status::TimingStatus::LiveMutationDetected);
    CHECK(bf.payload.statusReasons == std::vector<std::string>{"live_mutation_detected"} && !bf.payload.stateReplayValid);
    CHECK_MSG(telemetry::validateTimingResult(bf.payload, "", &err), err);
    // a normal result's fallback is still unresolved (payload_invalid)
    auto sr = status::statusOf({}, localFacts(er), saFacts(nullptr), {});
    auto bn = buildTimingResultEvent(bad, er, nullptr, sr, true);
    CHECK(!bn.ok && bn.payload.status == "unresolved");
    // the refined (CBF) solver string follows window_event's solverVersionFor
    TimingResultContext refined = cr;
    refined.refined = true;
    CHECK(buildTimingResultEvent(refined, er, &sa, st, true).payload.solverVersion == std::string(solverVersionFor(true)));
}

void testDualPairBlock() {
    SECTION("v0.8.0 dual PAIR result: dual_pair + the dual block validate and pass the gate; dual_pair without the block falls back");
    auto m = planner({{-4.0, -2.0}}, true, 0);
    auto wm = m.result(2000.0);
    LocalEvidence ev;
    ev.window = &wm;
    ev.outcomes = &m.outcomes();
    ev.miss = true;
    ev.frame = 480.0;
    TimingResultContext ctx;
    ctx.inputSeq = 9;
    ctx.kind = InputKind::Press;
    ctx.eventT = 2.0;
    ctx.gamemode = Gamemode::Ship;
    ctx.cluster = {"a1:5", 1, cluster::Tri::No, cluster::Tri::Unknown};
    telemetry::TimingResultDualPayload dual;
    dual.p2Gamemode = Gamemode::Ship;
    dual.p2GravityFlipped = true;
    dual.p2Mini = false;
    dual.sameGravity = false;
    ctx.dual = dual;
    status::ContextFacts cx;
    cx.dualPair = true;
    auto st = status::statusOf({}, localFacts(ev), saFacts(nullptr), cx);
    CHECK(st.status == status::TimingStatus::LowConfidence && st.reasons.back() == status::Reason::DualPair);
    auto b = buildTimingResultEvent(ctx, ev, nullptr, st, true);
    CHECK_MSG(b.ok, b.error);
    CHECK(b.payload.dual && *b.payload.dual == dual && b.payload.local && b.payload.miss);
    auto gate = checkTimingResultPayload(b.payload, 2.0, 0.0);
    CHECK_MSG(gate.accepted, gate.reasons.empty() ? "" : gate.reasons[0]);
    // serialised LAST, after solverVersion (older payloads stay byte-identical)
    telemetry::Event e;
    e.t = 2.0;
    e.tick = 481;
    e.seq = 4;
    e.attemptId = "a1";
    e.payload = b.payload;
    std::string text = json::stringify(telemetry::toJson(e));
    auto at = text.find("\"solverVersion\"");
    auto dualAt = text.find("\"dual\":{\"p2Gamemode\":\"ship\",\"p2GravityFlipped\":true,\"p2Mini\":false,\"sameGravity\":false}}");
    CHECK(at != std::string::npos && dualAt != std::string::npos && dualAt > at);
    // the pair reason without its block: the builder refuses it (fallback, unresolved)
    TimingResultContext noBlock = ctx;
    noBlock.dual.reset();
    auto bf = buildTimingResultEvent(noBlock, ev, nullptr, st, true);
    CHECK(!bf.ok && bf.error.find("/dual") != std::string::npos && bf.payload.status == "unresolved" && !bf.payload.dual);
    // no dual facts at all: no block (every older path is unchanged)
    auto plain = buildTimingResultEvent(noBlock, ev, nullptr, status::statusOf({}, localFacts(ev), saFacts(nullptr), {}), true);
    CHECK(plain.ok && !plain.payload.dual);
}

}  // namespace

int main() {
    testLocalEqualsTimingWindow();
    testMissHoldFallback();
    testInvariantsRandom();
    testLiveIsolation();
    testDualPairBlock();
    return gprl::test::finish("timing_result_event_tests");
}
