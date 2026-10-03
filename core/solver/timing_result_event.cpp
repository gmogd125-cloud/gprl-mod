#include "timing_result_event.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "timing_units.hpp"
#include "window_event.hpp"

namespace gprl::solver {

namespace {

constexpr double kEps = 1e-6;

std::optional<bool> triToBool(cluster::Tri t) {
    if (t == cluster::Tri::Yes) return true;
    if (t == cluster::Tri::No) return false;
    return std::nullopt;
}

double midpointOffsetMs(BoundaryResult const& side) {
    // exactly window_event.cpp midpointEdge: the same doubles the timing_window carries
    if (side.bounded && !std::isnan(side.failShiftMs)) return 0.5 * (side.passShiftMs + side.failShiftMs);
    return side.passShiftMs;
}

telemetry::TimingEdgeV2Payload localEdge(BoundaryResult const& b, bool subtick) {
    telemetry::TimingEdgeV2Payload e;
    e.passMs = b.passShiftMs;
    bool bounded = b.bounded && std::isfinite(b.failShiftMs);
    e.placement = subtick ? "cbf" : "tick";
    if (bounded) {
        e.failMs = b.failShiftMs;
        e.stop = "fail";
        EdgeCause cause = b.edge.attributed ? b.edge.cause : EdgeCause::Self;
        if (cause == EdgeCause::None) cause = EdgeCause::Self;
        e.cause = name(cause);
        if (b.edge.laterInputs >= 0) e.laterInputs = b.edge.laterInputs;
        if (std::isfinite(b.edge.failAfterMs)) e.failAfterMs = b.edge.failAfterMs;
        e.failObjectId = b.edge.failObjectId;
        return e;
    }
    EdgeStop stop = b.edge.attributed ? b.edge.stop : EdgeStop::Range;
    if (stop == EdgeStop::Fail) stop = EdgeStop::Untested;   // never "fail" without a fail
    e.stop = name(stop);
    return e;
}

telemetry::TimingEdgeV2Payload saEdge(SAEdge const& s, bool subtick) {
    telemetry::TimingEdgeV2Payload e;
    e.passMs = s.passFrames * kTickMs;
    e.placement = subtick ? "cbf" : "tick";
    if (s.bounded()) {
        e.failMs = s.failFrames * kTickMs;
        e.stop = "fail";
        EdgeCause cause = s.cause == EdgeCause::None ? EdgeCause::Self : s.cause;
        e.cause = name(cause);
        if (s.laterInputs >= 0) e.laterInputs = s.laterInputs;
        if (std::isfinite(s.failAfterFrames)) e.failAfterMs = s.failAfterFrames * kTickMs;
        e.failObjectId = s.failObjectId;
        return e;
    }
    EdgeStop stop = s.stop == EdgeStop::Fail ? EdgeStop::Undecided : s.stop;
    e.stop = name(stop);
    // Fable D1: the inherited local bracket (the reported edge = the local midpoint); no cause -
    // the SA side did not end at a fail of its own
    if (s.inherited && std::isfinite(s.failFrames)) e.failMs = s.failFrames * kTickMs;
    return e;
}

double edgeOffset(telemetry::TimingEdgeV2Payload const& e) { return e.failMs ? 0.5 * (e.passMs + *e.failMs) : e.passMs; }

std::string sanitizeId(std::string const& id) {
    std::string out;
    for (char c : id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out += ok ? c : '_';
        if (out.size() >= 64) break;
    }
    return out.empty() ? std::string("attempt") : out;
}

}  // namespace

bool allShiftsRejoined(std::vector<ShiftOutcome> const& outcomes, double frame, double nextFrame, double horizonFrame) {
    int tested = 0;
    for (auto const& o : outcomes) {
        if (o.kind == ShiftKind::NotTested) continue;
        if (std::fabs(o.appliedFrames) <= 1e-9) continue;
        ++tested;
        if (o.kind != ShiftKind::Resynced) return false;
        double rejoin = std::isnan(o.rejoinAfterFrames) ? o.deathAfterFrames : o.rejoinAfterFrames;
        if (std::isnan(rejoin)) return false;
        double at = frame + rejoin;
        double limit = !std::isnan(nextFrame) ? nextFrame : horizonFrame;
        if (!std::isnan(limit) && at > limit + kEps) return false;
    }
    return tested > 0;
}

status::LocalFacts localFacts(LocalEvidence const& ev, status::TimingStatusConfig const& cfg) {
    status::LocalFacts f;
    if (!ev.window || !ev.window->valid) return f;
    auto const& w = *ev.window;
    f.present = true;
    f.miss = ev.miss;
    f.missDownstream = ev.missDownstream;
    f.extension = ev.extension;
    f.untestedGap = w.early.budgetExhausted || w.late.budgetExhausted;
    f.widthBelowResolution = ev.widthBelowResolution;
    double res = 0.0;
    if (w.early.bounded) res = std::max(res, w.early.bracketMs);
    if (w.late.bounded) res = std::max(res, w.late.bracketMs);
    f.resolutionMs = res > 0.0 ? res : kTickMs;
    BoundaryResult const* sides[2] = {&w.early, &w.late};
    for (int s = 0; s < 2; ++s) {
        auto const& b = *sides[s];
        if (b.bounded && std::isfinite(b.failShiftMs)) {
            for (auto const& is : b.islands) {
                double inner = std::min(std::fabs(is.fromShiftMs), std::fabs(is.toShiftMs));
                if (inner - std::fabs(b.failShiftMs) <= cfg.islandNearTicks * kTickMs + kEps) f.islandsNearEdge = true;
            }
            // Fable D16: a fail edge set by a death in the last horizonTailFrames of the look-ahead
            // (46 of the v0.6.2 log's local edges): how many local edges are horizon artefacts
            if (std::isfinite(ev.horizonFrame) && std::isfinite(b.edge.failAfterMs)) {
                double death = ev.frame + b.edge.failAfterMs / kTickMs;
                if (death >= ev.horizonFrame - cfg.horizonTailFrames - kEps) f.failNearHorizon = true;
            }
            EdgeCause cause = b.edge.attributed ? b.edge.cause : EdgeCause::Self;
            if (cause == EdgeCause::Downstream) f.downstream[s] = true;
            else if (cause == EdgeCause::Extension) f.adaptable[s] = true;   // frozen world: undecidable without a replay
            else if (ev.nextFollows && !std::isnan(ev.nextFrame) && std::isfinite(b.edge.failAfterMs)) {
                // a self death: the next input shifted along acts before it (the wave "segment got
                // too long before the fixed next vertex" early edges)
                double s0 = b.failShiftMs / kTickMs;
                double death = ev.frame + b.edge.failAfterMs / kTickMs;
                if (ev.nextFrame + s0 < death - kEps) f.adaptable[s] = true;
            }
        }
        EdgeStop stop = b.edge.attributed ? b.edge.stop : (b.bounded ? EdgeStop::Fail : EdgeStop::Range);
        if (stop == EdgeStop::Neighbour) f.limitedByNeighbour = true;
        if (stop == EdgeStop::History) f.limitedByHistory = true;
        if (stop == EdgeStop::AttemptStart) f.attemptStart = true;
    }
    f.openEarly = !w.early.bounded;
    f.openLate = !w.late.bounded;
    if (!ev.miss && !w.early.bounded && !w.late.bounded && ev.outcomes) {
        f.noEffect = allShiftsRejoined(*ev.outcomes, ev.frame, ev.nextFrame, ev.horizonFrame);
    }
    return f;
}

status::SAFacts saFacts(SAResult const* sa, status::Reason notRunWhy) {
    status::SAFacts f;
    if (!sa || !sa->valid) {
        f.present = false;
        f.failure = notRunWhy;
        return f;
    }
    f.present = sa->sequence.present;
    f.decided = sa->decided;
    f.sideDecided[0] = sa->sideDecided[0];
    f.sideDecided[1] = sa->sideDecided[1];
    f.failure = sa->failure;
    f.openRange = sa->openRange;
    f.survivedOnly = sa->survivedOnly;
    return f;
}

telemetry::TimingWindowV2Payload localWindowPayload(WindowResult const& w, double actualMs, bool subtick) {
    telemetry::TimingWindowV2Payload p;
    p.early = localEdge(w.early, subtick);
    p.late = localEdge(w.late, subtick);
    p.earliestMs = actualMs + midpointOffsetMs(w.early);
    p.latestMs = actualMs + midpointOffsetMs(w.late);
    double res = 0.0;
    if (w.early.bounded) res = std::max(res, w.early.bracketMs);
    if (w.late.bounded) res = std::max(res, w.late.bracketMs);
    p.resolutionMs = res > 0.0 ? res : kTickMs;
    p.trials = w.trials;
    return p;
}

telemetry::TimingWindowV2Payload saWindowPayload(SAWindow const& w, double actualMs, bool subtick, bool withProof) {
    telemetry::TimingWindowV2Payload p;
    p.early = saEdge(w.early, subtick);
    p.late = saEdge(w.late, subtick);
    if (withProof) {
        // Fable review D3b (the sequence window): the weakest proof of the widening passes and the
        // last pass a re-join / the local rule proved (same doubles as passMs when not survived)
        for (auto [pe, se] : {std::pair{&p.early, &w.early}, std::pair{&p.late, &w.late}}) {
            pe->proof = name(se->proof);
            double proven = std::isfinite(se->provenPassFrames) ? se->provenPassFrames : se->passFrames;
            pe->provenPassMs = proven == se->passFrames ? pe->passMs : proven * kTickMs;
        }
    }
    p.earliestMs = actualMs + edgeOffset(p.early);
    p.latestMs = actualMs + edgeOffset(p.late);
    p.resolutionMs = (w.resolutionFrames > 0.0 ? w.resolutionFrames : 1.0) * kTickMs;
    p.trials = w.trials;
    return p;
}

TimingResultBuild buildTimingResultEvent(TimingResultContext const& ctx, LocalEvidence const& local, SAResult const* sa,
                                         status::StatusResult const& st, bool stateReplayValid) {
    TimingResultBuild b;
    b.status = st;
    telemetry::TimingResultPayload p;
    // Fable review D11: the engine's sub-tick (when reported) is the source of actualMs - every
    // shift is measured from the engine's frame of the input; subTickMs stays the tracker's
    bool const engineSub = std::isfinite(ctx.engineSubTickMs);
    double const actualMs = units::actualMs(ctx.eventT, engineSub ? ctx.engineSubTickMs : ctx.subTickMs);
    p.inputSeq = ctx.inputSeq;
    p.inputKind = ctx.kind;
    p.attemptInputIndex = std::max(1, ctx.attemptInputIndex);
    p.x = ctx.x;
    p.percentAtInput = ctx.percentAtInput;
    p.subTickMs = ctx.subTickMs;
    if (engineSub) p.engineSubTickMs = ctx.engineSubTickMs;
    p.gamemode = ctx.gamemode;
    p.speed = ctx.speed;
    p.gamemodeAfter = ctx.gamemodeAfter;
    p.status = status::name(st.status);
    for (auto r : st.reasons) p.statusReasons.emplace_back(status::name(r));
    // v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §3.3): a sample aborted by the live-state invariant is
    // never a measurement - whatever its trials produced, nothing of it is reported
    bool const aborted = status::isLiveMutation(st);
    p.stateReplayValid = stateReplayValid && !aborted;
    p.miss = !aborted && local.window && local.miss;
    p.actualMs = actualMs;
    if (!aborted && local.window && local.window->valid) p.local = localWindowPayload(*local.window, actualMs, ctx.subtick);
    if (p.local && sa && sa->valid && sa->sequence.present && !local.miss) {
        telemetry::SequenceWindowV2Payload sq;
        sq.window = saWindowPayload(sa->sequence, actualMs, ctx.subtick, true);
        sq.solverVersion = kSASolverVersion;
        sq.decided = sa->decided;
        for (auto a : sa->adaptationUsed) sq.adaptationUsed.emplace_back(name(a));
        sq.undecidedShifts = sa->undecidedShifts;
        // v0.14.0 (docs/SHIP_SOLVER.md §4.1, revision 6): the follower offsets of the compensated
        // pass that set each side (empty when the side was set by a local / uniform pass or not at all)
        telemetry::CompensationPayload cp;
        for (double f : sa->sequence.early.followerOffsetsFrames) cp.earlyOffsetsMs.push_back(f * kTickMs);
        for (double f : sa->sequence.late.followerOffsetsFrames) cp.lateOffsetsMs.push_back(f * kTickMs);
        // revision 7: up to 6 followers (comp::kMaxFollowers)
        if (cp.earlyOffsetsMs.size() > 6) cp.earlyOffsetsMs.resize(6);
        if (cp.lateOffsetsMs.size() > 6) cp.lateOffsetsMs.resize(6);
        // present only when a side was set by a pass that moved followers: a window set by local
        // passes alone carries no block, so every older payload stays byte-identical
        if (!cp.earlyOffsetsMs.empty() || !cp.lateOffsetsMs.empty()) sq.compensation = cp;
        p.sequence = sq;
        if (sa->pair.present) p.pair = saWindowPayload(sa->pair, actualMs, ctx.subtick);
    }
    if (ctx.kind == InputKind::Release && p.local && std::isfinite(ctx.pressMs) && ctx.pressSeq >= 0) {
        telemetry::HoldRangeV2Payload h;
        h.pressSeq = ctx.pressSeq;
        h.localMinMs = p.local->earliestMs - ctx.pressMs;
        h.localMaxMs = p.local->latestMs - ctx.pressMs;
        if (p.sequence) {
            h.minMs = p.sequence->window.earliestMs - ctx.pressMs;
            h.maxMs = p.sequence->window.latestMs - ctx.pressMs;
            h.basis = "sequence";
        }
        else {
            h.minMs = h.localMinMs;
            h.maxMs = h.localMaxMs;
            h.basis = "local";
        }
        p.hold = h;
    }
    p.cluster.id = ctx.cluster.id;
    p.cluster.index = std::max(1, ctx.cluster.index);
    p.cluster.connectedPrev = triToBool(ctx.cluster.connectedPrev);
    p.cluster.connectedNext = triToBool(ctx.cluster.connectedNext);
    p.boundarySimulations = std::max(0, ctx.boundarySimulations);
    p.controlSimulations = std::max(0, ctx.controlSimulations);
    p.solverVersion = solverVersionFor(ctx.refined);
    p.dual = ctx.dual;   // v0.8.0: the dual pair's player-2 facts (the validators tie it to `dual_pair`)
    // v0.15.0 (docs/SHIP_SOLVER.md §11.3, revision 7): the Ship control of a connected-control result
    if (ctx.control && !aborted) {
        telemetry::TimingControlPayload k;
        k.index = std::max(1, ctx.control->index);
        k.role = ctx.kind == InputKind::Press ? "press" : "release";
        if (ctx.control->pressSeq >= 0) k.pressSeq = ctx.control->pressSeq;
        if (ctx.control->releaseSeq >= 0) k.releaseSeq = ctx.control->releaseSeq;
        if (std::isfinite(ctx.control->holdFrames) && ctx.control->holdFrames >= 0.0) k.holdMs = ctx.control->holdFrames * kTickMs;
        if (SAResult const* ph = ctx.control->phase; ph && ph->valid && ph->sequence.present && ctx.kind == InputKind::Release) {
            telemetry::TimingPhasePayload x;
            x.earlyMs = std::min(0.0, ph->sequence.early.edgeFrames() * kTickMs);
            x.lateMs = std::max(0.0, ph->sequence.late.edgeFrames() * kTickMs);
            x.earlyStop = name(ph->sequence.early.stop);
            x.lateStop = name(ph->sequence.late.stop);
            x.decided = ph->decided;
            x.trials = ph->trials;
            k.phase = x;
        }
        if (p.sequence && sa) {
            k.followers = std::clamp(sa->followersUsed, 0, 6);
            std::pair<SAEdge const*, std::optional<telemetry::TimingRejoinPayload>*> const sides[] = {{&sa->sequence.early, &k.rejoinEarly}, {&sa->sequence.late, &k.rejoinLate}};
            for (auto const& [e, dst] : sides) {
                if (!rejoin::rejoined(e->rejoin)) continue;
                telemetry::TimingRejoinPayload r;
                r.kind = rejoin::name(e->rejoin);
                r.afterMs = std::isfinite(e->rejoinAfterFrames) ? e->rejoinAfterFrames * kTickMs : 0.0;
                r.errorY = std::isfinite(e->rejoinErrY) ? std::fabs(e->rejoinErrY) : 0.0;
                r.errorVy = std::isfinite(e->rejoinErrVy) ? std::fabs(e->rejoinErrVy) : 0.0;
                *dst = r;
            }
        }
        p.control = k;
    }
    // v0.15.0 (docs/SHIP_SOLVER.md §11.1): where the replay first left the real run
    if (!ctx.parity.valid && ctx.parity.first.any() && !p.stateReplayValid) {
        telemetry::TimingParityPayload x;
        x.tick = std::max(0.0, ctx.parity.tick);
        x.field = parity::name(ctx.parity.first.field);
        x.real = ctx.parity.first.real;
        x.replay = ctx.parity.first.replay;
        x.delta = ctx.parity.first.delta;
        if (std::isfinite(x.real) && std::isfinite(x.replay) && std::isfinite(x.delta)) p.parity = x;
    }
    std::string err;
    if (telemetry::validateTimingResult(p, "", &err)) {
        b.ok = true;
        b.payload = std::move(p);
        return b;
    }
    // FALLBACK: never leave a bound job without its one result, never emit an invalid one
    b.error = err;
    telemetry::TimingResultPayload fb;
    fb.inputSeq = std::max<int64_t>(0, ctx.inputSeq);
    fb.inputKind = ctx.kind;
    fb.attemptInputIndex = std::max(1, ctx.attemptInputIndex);
    fb.x = std::isfinite(ctx.x) ? ctx.x : 0.0;
    fb.percentAtInput = std::isfinite(ctx.percentAtInput) ? std::clamp(ctx.percentAtInput, 0.0, 100.0) : 0.0;
    fb.subTickMs = std::isfinite(ctx.subTickMs) ? std::clamp(ctx.subTickMs, 0.0, kTickMs * 0.999) : 0.0;
    if (engineSub) fb.engineSubTickMs = std::clamp(ctx.engineSubTickMs, 0.0, kTickMs * 0.999);
    fb.gamemode = ctx.gamemode;
    fb.speed = ctx.speed;
    fb.gamemodeAfter = ctx.gamemodeAfter;
    // v0.8.0: a live-state breach is never downgraded - its fallback keeps `live_mutation_detected`
    // (the attempt flag the server derives from it must not depend on the rest of the payload)
    status::Reason const fbReason = aborted ? status::Reason::LiveMutationDetected : status::Reason::PayloadInvalid;
    fb.status = status::name(status::group(fbReason));
    fb.statusReasons = {status::name(fbReason)};
    fb.stateReplayValid = stateReplayValid && !aborted;
    fb.miss = false;
    double const fbActual = units::actualMs(ctx.eventT, fb.engineSubTickMs ? *fb.engineSubTickMs : fb.subTickMs);
    fb.actualMs = std::isfinite(fbActual) ? fbActual : 0.0;
    std::string prefix = ctx.cluster.id.substr(0, ctx.cluster.id.find(':'));
    fb.cluster.id = cluster::clusterIdOk(ctx.cluster.id) ? ctx.cluster.id : sanitizeId(prefix) + ":" + std::to_string(std::clamp(ctx.attemptInputIndex, 1, 999999));
    fb.cluster.index = std::max(1, ctx.cluster.index);
    fb.cluster.connectedPrev = triToBool(ctx.cluster.connectedPrev);
    fb.cluster.connectedNext = triToBool(ctx.cluster.connectedNext);
    fb.boundarySimulations = std::max(0, ctx.boundarySimulations);
    fb.controlSimulations = std::max(0, ctx.controlSimulations);
    fb.solverVersion = solverVersionFor(ctx.refined);
    b.status.status = status::group(fbReason);
    b.status.reasons = {fbReason};
    b.payload = std::move(fb);
    return b;
}

bool sequenceContainsLocal(telemetry::TimingWindowV2Payload const& local, telemetry::TimingWindowV2Payload const& sequence) {
    // api/src/processing/timingResults.ts sequenceContainsLocal (Fable review D1): every local pass
    // is an SA pass AND the reported edges contain the local ones (the inherited edge makes the
    // second hold for an SA side that stopped at the local pass edge)
    double const tol = telemetry::kTimingEdgeToleranceMs;
    return sequence.early.passMs <= local.early.passMs + tol && sequence.late.passMs >= local.late.passMs - tol
        && sequence.earliestMs <= local.earliestMs + tol && sequence.latestMs >= local.latestMs - tol;
}

bool proofConsistent(telemetry::TimingResultPayload const& p) {
    // api/src/processing/timingResults.ts proofConsistent (Fable review D3b): vocabulary, a finite
    // provenPassMs, and on a SEQUENCE side: provenPassMs between the local pass edge and the SA
    // pass edge, a `survived` side carries it, a `local` / `rejoined` side's proven pass is its pass
    // edge, a `local` side never widened
    double const tol = telemetry::kTimingEdgeToleranceMs;
    auto vocabOk = [](telemetry::TimingEdgeV2Payload const& e) {
        bool proofOk = !e.proof || *e.proof == "local" || *e.proof == "rejoined" || *e.proof == "survived" || *e.proof == "compensated";
        return proofOk && (!e.provenPassMs || std::isfinite(*e.provenPassMs));
    };
    for (auto const* w : {p.local ? &*p.local : nullptr, p.sequence ? &p.sequence->window : nullptr, p.pair ? &*p.pair : nullptr}) {
        if (w && (!vocabOk(w->early) || !vocabOk(w->late))) return false;
    }
    if (!p.sequence || !p.local) return true;
    for (int side = 0; side < 2; ++side) {
        auto const& s = side == 0 ? p.sequence->window.early : p.sequence->window.late;
        double const localPass = side == 0 ? p.local->early.passMs : p.local->late.passMs;
        double const lo = side == 0 ? s.passMs : localPass;
        double const hi = side == 0 ? localPass : s.passMs;
        if (s.provenPassMs && !(*s.provenPassMs >= lo - tol && *s.provenPassMs <= hi + tol)) return false;
        if (s.proof && *s.proof == "survived" && !s.provenPassMs) return false;
        // v0.14.0: a `compensated` pass is a proven pass like `rejoined` (verified by a simulation)
        if (s.proof && (*s.proof == "local" || *s.proof == "rejoined" || *s.proof == "compensated") && s.provenPassMs && !(std::fabs(*s.provenPassMs - s.passMs) <= tol)) return false;
        if (s.proof && *s.proof == "local" && !(std::fabs(s.passMs - localPass) <= tol)) return false;
    }
    return true;
}

TimingResultCheck checkTimingResultPayload(telemetry::TimingResultPayload const& p, double eventT, double eventSubTickMs,
                                           telemetry::TimingWindowPayload const* window) {
    // reason names and thresholds = api/src/processing/timingResults.ts evaluateTimingResultEvidence
    // / config.ts TIMING_RESULT_EVIDENCE_0_1 (edgeToleranceMs 1e-6, maxBoundedWindowMs 2000,
    // actualVsInputToleranceMs 0.01, subTickClockToleranceMs 0.5)
    TimingResultCheck c;
    std::string err;
    bool const valid = telemetry::validateTimingResult(p, "", &err);
    if (!valid) {
        c.accepted = false;
        c.reasons.push_back("payload_invalid");
    }
    // v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §3.3, §5.2): a sample aborted by the live-state
    // invariant is stored as evidence of the breach, never usable
    if (p.status == status::name(status::TimingStatus::LiveMutationDetected)) {
        c.accepted = false;
        c.reasons.push_back("live_mutation_detected");
    }
    if (valid && p.local && p.sequence && !sequenceContainsLocal(*p.local, p.sequence->window)) {
        c.accepted = false;
        c.reasons.push_back("sequence_not_containing_local");
    }
    if (valid && !proofConsistent(p)) {
        c.accepted = false;
        c.reasons.push_back("proof_inconsistent");
    }
    auto wide = [](std::optional<telemetry::TimingWindowV2Payload> const& w) {
        return w && w->early.failMs && w->late.failMs && w->latestMs - w->earliestMs > 2000.0;
    };
    if (wide(p.local) || (p.sequence && wide(std::optional<telemetry::TimingWindowV2Payload>(p.sequence->window))) || wide(p.pair)) {
        c.accepted = false;
        c.reasons.push_back("window_implausibly_wide");
    }
    // Fable review D11: with the optional engineSubTickMs the ENGINE's sub-tick is the source of
    // actualMs while subTickMs stays the tracker's copy of the input event's; the two clocks must
    // agree within 0.5 ms (without it both are the tracker's)
    double const actualSub = p.engineSubTickMs ? *p.engineSubTickMs : p.subTickMs;
    if (!(std::fabs(p.actualMs - (eventT * 1000.0 + actualSub)) <= 0.01) || !(std::fabs(p.subTickMs - eventSubTickMs) <= 0.01)) {
        c.accepted = false;
        c.reasons.push_back("actual_inconsistent_with_input");
    }
    if (!(std::fabs(actualSub - eventSubTickMs) <= 0.5)) {
        c.accepted = false;
        c.reasons.push_back("subtick_clock_mismatch");
    }
    if (window && p.local) {
        // bit-exact: the builder uses the timing_window's own doubles
        if (p.local->earliestMs != window->earliestMs || p.local->latestMs != window->latestMs) {
            c.accepted = false;
            c.reasons.push_back("local_mismatch_timing_window");
        }
    }
    return c;
}

}  // namespace gprl::solver
