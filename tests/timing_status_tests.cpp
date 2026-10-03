// timing_status host tests (docs/TIMING_SOLVER_V2.md §2.10, §5; AUDIT §16): one case per reason,
// the precedence (state_replay_failed > unresolved > no_effect > sequence_dependent >
// low_confidence > ok), informational reasons with ok, truncation never changing the status, the
// consistency rule both validators apply, and the dual case (player 2 is never simulated ->
// state_replay_failed dual_not_simulated, no window). v0.8.0 (`gprl-timing-status/2`,
// docs/LIVE_ISOLATION_DESIGN.md §3.3, §4, §8): live_mutation_detected above state_replay_failed,
// an aborted sample described by the abort alone, the LayerSync / unisolated control reasons,
// dual_pair informational (kept through truncation), the HUD word.
#include "test_util.hpp"

#include "../core/solver/timing_status.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace gprl::solver::status;

namespace {

bool has(StatusResult const& r, Reason x) { return std::find(r.reasons.begin(), r.reasons.end(), x) != r.reasons.end(); }

LocalFacts okLocal() {
    LocalFacts l;
    l.present = true;
    l.resolutionMs = 1000.0 / 240.0;
    return l;
}

SAFacts decidedSA() {
    SAFacts s;
    s.present = true;
    s.decided = true;
    s.sideDecided[0] = s.sideDecided[1] = true;
    return s;
}

void testVocabulary() {
    SECTION("vocabulary: 7 statuses, 54 reasons, names round-trip, groups as schema.ts TIMING_STATUS_REASON_GROUPS");
    CHECK(kTimingStatusCount == 7 && kReasonCount == 54);
    // v0.8.0: the new status is APPENDED (the older statuses keep their indexes: engine counters)
    CHECK(static_cast<int>(TimingStatus::NoEffect) == 5 && static_cast<int>(TimingStatus::LiveMutationDetected) == 6);
    CHECK(std::string(name(TimingStatus::LiveMutationDetected)) == "live_mutation_detected");
    // the highest group first, dual_pair last; the existing reasons keep their relative order
    CHECK(static_cast<int>(Reason::LiveMutationDetected) == 0 && static_cast<int>(Reason::ControlMismatch) == 1);
    CHECK(Reason::ReplayBreaker < Reason::ControlInvalidUnisolated && Reason::ControlInvalidUnisolated < Reason::ControlInvalidLayerSync &&
          Reason::ControlInvalidLayerSync < Reason::NoShiftTested);
    CHECK(static_cast<int>(Reason::DualPair) == kReasonCount - 1);
    CHECK(group(Reason::LiveMutationDetected) == TimingStatus::LiveMutationDetected);
    CHECK(group(Reason::ControlInvalidUnisolated) == TimingStatus::StateReplayFailed && group(Reason::ControlInvalidLayerSync) == TimingStatus::StateReplayFailed);
    CHECK(informational(Reason::DualPair) && !informational(Reason::LiveMutationDetected));
    CHECK(std::string(name(Reason::LiveMutationDetected)) == "live_mutation_detected" && std::string(name(Reason::DualPair)) == "dual_pair");
    CHECK(std::string(name(Reason::ControlInvalidUnisolated)) == "control_invalid_unisolated" &&
          std::string(name(Reason::ControlInvalidLayerSync)) == "control_invalid_layer_sync");
    CHECK(precedence(TimingStatus::LiveMutationDetected) > precedence(TimingStatus::StateReplayFailed));
    CHECK(std::string(hudWord(TimingStatus::LiveMutationDetected)) == "isolation stop" && std::string(hudWord(TimingStatus::Ok)) == "ok" &&
          std::string(hudWord(TimingStatus::SequenceDependent)) == "sequence_dependent");
    // v0.7.1 (Fable review): one low_confidence reason inside its group, three informational ones last
    CHECK(group(Reason::SpeedChangeInLookahead) == TimingStatus::LowConfidence);
    CHECK(informational(Reason::OpenRange) && informational(Reason::SaSurvivedOnly) && informational(Reason::FailNearHorizon));
    CHECK(std::string(name(Reason::FailNearHorizon)) == "fail_near_horizon" && std::string(name(Reason::OpenRange)) == "open_range");
    CHECK(std::string(name(Reason::SpeedChangeInLookahead)) == "speed_change_in_lookahead" && std::string(name(Reason::SaSurvivedOnly)) == "sa_survived_only");
    CHECK(kStatusConfig.speedChangeFlag && kStatusConfig.horizonTailFrames == 10.0);
    for (int i = 0; i < kTimingStatusCount; ++i) {
        TimingStatus s;
        CHECK(parseStatus(name(static_cast<TimingStatus>(i)), s) && static_cast<int>(s) == i);
    }
    for (int i = 0; i < kReasonCount; ++i) {
        Reason r;
        CHECK(parseReason(name(static_cast<Reason>(i)), r) && static_cast<int>(r) == i);
    }
    CHECK(group(Reason::ControlMismatch) == TimingStatus::StateReplayFailed && group(Reason::ReplayBreaker) == TimingStatus::StateReplayFailed);
    CHECK(group(Reason::NoShiftTested) == TimingStatus::Unresolved && group(Reason::PayloadInvalid) == TimingStatus::Unresolved);
    CHECK(group(Reason::AllShiftsRejoined) == TimingStatus::NoEffect);
    CHECK(group(Reason::SaUndecided) == TimingStatus::SequenceDependent && group(Reason::MissDownstream) == TimingStatus::SequenceDependent);
    CHECK(group(Reason::Miss) == TimingStatus::LowConfidence && group(Reason::SaNotMeasured) == TimingStatus::LowConfidence);
    CHECK(informational(Reason::Isolated) && informational(Reason::LocalEdgeAdaptable) && informational(Reason::TransitionInput));
    CHECK(std::string(kTimingStatusVersion) == "gprl-timing-status/2");
    CHECK(kStatusConfig.maxResolutionMs == 4.2 && kStatusConfig.transitionTicks == 2 && kStatusConfig.maxReasons == 12);
}

void testPrecedence() {
    SECTION("precedence and accumulation: the highest group wins, every reason is kept (<= 12, ordered)");
    auto r = resolveStatus({Reason::Miss, Reason::Isolated, Reason::SaUndecided, Reason::CutByRestart, Reason::AllShiftsRejoined});
    CHECK(r.status == TimingStatus::Unresolved);
    CHECK(r.reasons.front() == Reason::CutByRestart);   // the forcing reason first
    CHECK(r.reasons.back() == Reason::Isolated);        // informational last
    CHECK(resolveStatus({Reason::ControlMismatch, Reason::CutByRestart}).status == TimingStatus::StateReplayFailed);
    CHECK(resolveStatus({Reason::AllShiftsRejoined, Reason::SaUndecided}).status == TimingStatus::NoEffect);
    CHECK(resolveStatus({Reason::SaUndecided, Reason::Miss}).status == TimingStatus::SequenceDependent);
    CHECK(resolveStatus({Reason::Miss}).status == TimingStatus::LowConfidence);
    CHECK(resolveStatus({Reason::Isolated, Reason::OpenLate}).status == TimingStatus::Ok);
    CHECK(resolveStatus({}).status == TimingStatus::Ok);
    // duplicates collapse; truncation to 12 keeps the forcing reasons (never changes the status)
    std::vector<Reason> many = {Reason::Isolated, Reason::OpenEarly, Reason::OpenLate, Reason::LimitedByNeighbour, Reason::LimitedByHistory,
                                Reason::AttemptStart, Reason::TransitionInput, Reason::LocalEdgeDownstream, Reason::LocalEdgeAdaptable, Reason::Miss,
                                Reason::HalfTickInput, Reason::UntestedGapInBracket, Reason::ShadowMismatchNearby, Reason::ControlMismatch, Reason::Miss};
    auto t = resolveStatus(many);
    CHECK(t.reasons.size() == 12);
    CHECK(t.status == TimingStatus::StateReplayFailed && t.reasons.front() == Reason::ControlMismatch);
    std::string why;
    CHECK(statusConsistent(t.status, t.reasons, &why));
    CHECK(!statusConsistent(TimingStatus::Ok, {Reason::Miss}, &why) && why.find("low_confidence") != std::string::npos);
    CHECK(!statusConsistent(TimingStatus::LowConfidence, {Reason::Miss, Reason::Miss}, &why));
    CHECK(!statusConsistent(TimingStatus::LowConfidence, {}, &why));
    CHECK(statusConsistent(TimingStatus::Ok, {Reason::Isolated}, &why));
}

void testStatusOfEachReason() {
    SECTION("statusOf: one case per reason group and the SA rules");
    // ok: decided sequence window, informational reasons only
    {
        ContextFacts cx;
        cx.isolated = true;
        auto r = statusOf({}, okLocal(), decidedSA(), cx);
        CHECK(r.status == TimingStatus::Ok && has(r, Reason::Isolated));
    }
    // state_replay_failed / unresolved: the job's own end reasons, no window
    for (Reason end : {Reason::ControlMismatch, Reason::ControlInvalidTeleport, Reason::ControlInvalidDualPortal, Reason::HistoryLost,
                       Reason::SnapshotSelftestFailed, Reason::DualNotSimulated, Reason::RealDeathNotReproduced, Reason::ReplayBreaker}) {
        auto r = statusOf({{end}}, {}, {}, {});
        CHECK_MSG(r.status == TimingStatus::StateReplayFailed && has(r, end), name(end));
    }
    for (Reason end : {Reason::NoShiftTested, Reason::NoPassDeathUnrelated, Reason::CutByRestart, Reason::CutByLevelEnd, Reason::PoolExhausted,
                       Reason::BlockedBothSides, Reason::UnboundInput, Reason::PlatformerUnsupported, Reason::PayloadInvalid}) {
        auto r = statusOf({{end}}, {}, {}, {});
        CHECK_MSG(r.status == TimingStatus::Unresolved && has(r, end), name(end));
    }
    // no window and no end reason: never silently ok
    CHECK(statusOf({}, {}, {}, {}).status == TimingStatus::Unresolved);
    // a real death the shadow did not reproduce / the replay breaker override a window
    {
        ContextFacts cx;
        cx.realDeathNotReproduced = true;
        CHECK(statusOf({}, okLocal(), decidedSA(), cx).status == TimingStatus::StateReplayFailed);
        ContextFacts cb;
        cb.replayBreaker = true;
        CHECK(statusOf({}, okLocal(), decidedSA(), cb).status == TimingStatus::StateReplayFailed);
    }
    // no_effect
    {
        auto l = okLocal();
        l.noEffect = true;
        l.openEarly = l.openLate = true;
        auto r = statusOf({}, l, {}, {});
        CHECK(r.status == TimingStatus::NoEffect && has(r, Reason::AllShiftsRejoined));
    }
    // low_confidence, one by one
    auto low = [&](auto&& mutate, Reason expect) {
        auto l = okLocal();
        ContextFacts cx;
        mutate(l, cx);
        auto r = statusOf({}, l, decidedSA(), cx);
        CHECK_MSG(r.status == TimingStatus::LowConfidence && has(r, expect), name(expect));
    };
    low([](LocalFacts& l, ContextFacts&) { l.extension = true; }, Reason::FrozenWorldExtension);
    low([](LocalFacts& l, ContextFacts&) { l.untestedGap = true; }, Reason::UntestedGapInBracket);
    low([](LocalFacts& l, ContextFacts&) { l.resolutionMs = 4.3; }, Reason::ResolutionAboveMax);
    low([](LocalFacts& l, ContextFacts&) { l.widthBelowResolution = true; }, Reason::WidthBelowResolution);
    low([](LocalFacts& l, ContextFacts&) { l.islandsNearEdge = true; }, Reason::IslandsNearEdge);
    low([](LocalFacts&, ContextFacts& c) { c.shadowMismatchNearby = true; }, Reason::ShadowMismatchNearby);
    low([](LocalFacts&, ContextFacts& c) { c.halfTickInput = true; }, Reason::HalfTickInput);
    // a miss: low_confidence (miss), no SA; miss_downstream when its bound is a follower's business
    {
        auto l = okLocal();
        l.miss = true;
        auto r = statusOf({}, l, {}, {});
        CHECK(r.status == TimingStatus::LowConfidence && has(r, Reason::Miss) && !has(r, Reason::SaNotMeasured));
        l.downstream[1] = true;
        auto d = statusOf({}, l, {}, {});
        CHECK(d.status == TimingStatus::SequenceDependent && has(d, Reason::MissDownstream) && has(d, Reason::LocalEdgeDownstream));
    }
    // sequence_dependent: a downstream / adaptable edge whose SA side is not decided, with the SA's reason
    for (Reason why : {Reason::SaUndecided, Reason::SaNotMeasuredBudget, Reason::SaExpired, Reason::SaNoNegativeControl, Reason::SaControlMismatch,
                       Reason::SaNegativeNotReproduced, Reason::SaInvalidTrials, Reason::SaCutByRestart, Reason::SaDeathInSpan}) {
        auto l = okLocal();
        l.downstream[0] = true;
        SAFacts sa;
        sa.present = false;
        sa.failure = why;
        auto r = statusOf({}, l, sa, {});
        CHECK_MSG(r.status == TimingStatus::SequenceDependent && has(r, why) && has(r, Reason::LocalEdgeDownstream), name(why));
    }
    {
        // a self edge a follower could still change (the wave "segment too long" early edge)
        auto l = okLocal();
        l.adaptable[0] = true;
        SAFacts sa = decidedSA();
        sa.decided = false;
        sa.sideDecided[0] = false;
        sa.failure = Reason::SaUndecided;
        auto r = statusOf({}, l, sa, {});
        CHECK(r.status == TimingStatus::SequenceDependent && has(r, Reason::LocalEdgeAdaptable) && has(r, Reason::SaUndecided));
        // the same edge with its SA side decided: ok (the other side undecided but not adaptable -> sa_not_measured)
        sa.sideDecided[0] = true;
        sa.sideDecided[1] = false;
        auto q = statusOf({}, l, sa, {});
        CHECK(q.status == TimingStatus::LowConfidence && has(q, Reason::SaNotMeasured));
    }
    {
        // SA wanted but not run, both local edges self / limits: low_confidence sa_not_measured
        auto l = okLocal();
        SAFacts sa;
        sa.present = false;
        auto r = statusOf({}, l, sa, {});
        CHECK(r.status == TimingStatus::LowConfidence && has(r, Reason::SaNotMeasured));
    }
    {
        // the informational reasons ride along with ok
        auto l = okLocal();
        l.openLate = true;
        l.limitedByNeighbour = true;
        l.limitedByHistory = true;
        l.attemptStart = true;
        l.downstream[1] = true;
        ContextFacts cx;
        cx.transitionInput = true;
        auto r = statusOf({}, l, decidedSA(), cx);
        CHECK(r.status == TimingStatus::Ok);
        for (Reason x : {Reason::OpenLate, Reason::LimitedByNeighbour, Reason::LimitedByHistory, Reason::AttemptStart, Reason::TransitionInput,
                         Reason::LocalEdgeDownstream}) {
            CHECK_MSG(has(r, x), name(x));
        }
    }
}

// ---- v0.7.1: the Fable review's status rules (docs/TIMING_SOLVER_V2_FABLE.md §3.6) ----

void testOpenRangeIsOkDecided() {
    SECTION("Fable D3a openRangeIsOkDecided: both local sides open to the range -> sequence = local copy, decided -> ok (open_range)");
    auto l = okLocal();
    l.openEarly = l.openLate = true;
    SAFacts sa = decidedSA();
    sa.openRange = true;
    auto r = statusOf({}, l, sa, {});
    CHECK(r.status == TimingStatus::Ok);
    CHECK(has(r, Reason::OpenRange) && has(r, Reason::OpenEarly) && has(r, Reason::OpenLate));
    CHECK(!has(r, Reason::SaNotMeasured) && !has(r, Reason::SaUndecided));
    std::string why;
    CHECK(statusConsistent(r.status, r.reasons, &why));
    // open_range is only said when a sequence window exists
    SAFacts none;
    none.openRange = true;
    CHECK(!has(statusOf({}, l, none, {}), Reason::OpenRange));
}

void testSpeedChangeInLookahead() {
    SECTION("Fable D7 speedChangeInLookahead: a real speed change inside the job's span -> low_confidence (speed_change_in_lookahead)");
    ContextFacts cx;
    cx.speedChangeInSpan = true;
    auto r = statusOf({}, okLocal(), decidedSA(), cx);
    CHECK(r.status == TimingStatus::LowConfidence && has(r, Reason::SpeedChangeInLookahead));
    // the versioned switch turns the rule off (TimingStatusConfig.speedChangeFlag)
    TimingStatusConfig off = kStatusConfig;
    off.speedChangeFlag = false;
    CHECK(statusOf({}, okLocal(), decidedSA(), cx, off).status == TimingStatus::Ok);
    // no window: nothing to flag
    CHECK(!has(statusOf({{Reason::CutByRestart}}, {}, {}, cx), Reason::SpeedChangeInLookahead));
    // the span rule (speedChangeInSpan): [t - maxShift, look-ahead], both ends included
    std::vector<double> changes = {1000.0, 1300.0};
    CHECK(speedChangeInSpan(changes, 1010.0, 10.0, 1140.0));    // 1000 = t - 10
    CHECK(!speedChangeInSpan(changes, 1011.0, 10.0, 1140.0));   // 1000 < t - 10
    CHECK(speedChangeInSpan(changes, 1160.0, 10.0, 1300.0));    // 1300 = the look-ahead
    CHECK(!speedChangeInSpan(changes, 1160.0, 10.0, 1299.0));
    CHECK(!speedChangeInSpan({}, 1000.0, 10.0, 2000.0));
}

void testMissDownstreamAndHorizonTail() {
    SECTION("Fable D6 / D16: an earlier job of a died run is sequence_dependent (miss_downstream) without `miss`; fail_near_horizon is informational");
    auto l = okLocal();
    l.missDownstream = true;
    auto r = statusOf({}, l, decidedSA(), {});
    CHECK(r.status == TimingStatus::SequenceDependent && has(r, Reason::MissDownstream) && !has(r, Reason::Miss));
    CHECK(!has(r, Reason::SaNotMeasured) && !has(r, Reason::OpenRange));
    // the attributed miss keeps `miss` (low_confidence)
    auto m = okLocal();
    m.miss = true;
    auto rm = statusOf({}, m, {}, {});
    CHECK(rm.status == TimingStatus::LowConfidence && has(rm, Reason::Miss) && !has(rm, Reason::MissDownstream));
    auto h = okLocal();
    h.failNearHorizon = true;
    auto rh = statusOf({}, h, decidedSA(), {});
    CHECK(rh.status == TimingStatus::Ok && has(rh, Reason::FailNearHorizon));
    SAFacts s = decidedSA();
    s.survivedOnly = true;
    auto rs = statusOf({}, okLocal(), s, {});
    CHECK(rs.status == TimingStatus::Ok && has(rs, Reason::SaSurvivedOnly));
}

void testDual() {
    SECTION("dual: player 2 is never simulated -> state_replay_failed dual_not_simulated, no window");
    auto r = statusOf({{Reason::DualNotSimulated}}, {}, {}, {});
    CHECK(r.status == TimingStatus::StateReplayFailed);
    CHECK(r.reasons.size() == 1 && r.reasons[0] == Reason::DualNotSimulated);
    std::string why;
    CHECK(statusConsistent(r.status, r.reasons, &why));
}

}  // namespace

// ---- v0.8.0: live isolation (docs/LIVE_ISOLATION_DESIGN.md §3.3, §4, §2.7) ----

void testLiveMutation() {
    SECTION("v0.8.0 live_mutation_detected: above state_replay_failed; an aborted sample is described by the abort alone");
    CHECK(resolveStatus({Reason::ControlMismatch, Reason::LiveMutationDetected}).status == TimingStatus::LiveMutationDetected);
    CHECK(resolveStatus({Reason::LiveMutationDetected, Reason::Isolated}).reasons.front() == Reason::LiveMutationDetected);
    std::string why;
    CHECK(statusConsistent(TimingStatus::LiveMutationDetected, {Reason::LiveMutationDetected, Reason::ControlMismatch}, &why));
    CHECK(!statusConsistent(TimingStatus::StateReplayFailed, {Reason::LiveMutationDetected}, &why) && why.find("live_mutation_detected") != std::string::npos);
    CHECK(!statusConsistent(TimingStatus::LiveMutationDetected, {Reason::ControlMismatch}, &why));
    // the context flag over a perfectly good window: nothing of the window is reported
    ContextFacts cx;
    cx.liveMutation = true;
    cx.isolated = true;
    auto l = okLocal();
    l.openLate = true;
    l.miss = true;
    auto r = statusOf({}, l, decidedSA(), cx);
    CHECK(r.status == TimingStatus::LiveMutationDetected && isLiveMutation(r));
    CHECK(r.reasons.size() == 1 && r.reasons[0] == Reason::LiveMutationDetected);
    // the engine may end the job with the reason itself (the isolation breaker on open jobs)
    auto j = statusOf({{Reason::LiveMutationDetected}}, {}, {}, {});
    CHECK(j.status == TimingStatus::LiveMutationDetected && j.reasons.size() == 1);
    // the job's own end reasons are kept next to it (status stays the breach)
    auto k = statusOf({{Reason::ControlMismatch}}, okLocal(), {}, cx);
    CHECK(k.status == TimingStatus::LiveMutationDetected && has(k, Reason::ControlMismatch) && k.reasons.front() == Reason::LiveMutationDetected);
    // a dual pair sample aborted: dual_pair stays (the validators then need the dual block)
    ContextFacts cd = cx;
    cd.dualPair = true;
    auto d = statusOf({}, okLocal(), decidedSA(), cd);
    CHECK(d.status == TimingStatus::LiveMutationDetected && d.reasons.size() == 2 && d.reasons[1] == Reason::DualPair);
    CHECK(!isLiveMutation(statusOf({}, okLocal(), decidedSA(), {})));
}

void testControlInvalidIsolation() {
    SECTION("v0.8.0 control_invalid_layer_sync / control_invalid_unisolated: the replay is not proven -> state_replay_failed");
    for (Reason end : {Reason::ControlInvalidUnisolated, Reason::ControlInvalidLayerSync}) {
        auto r = statusOf({{end}}, {}, {}, {});
        CHECK_MSG(r.status == TimingStatus::StateReplayFailed && has(r, end), name(end));
        // beats unresolved, loses to a breach
        CHECK(resolveStatus({end, Reason::CutByRestart}).status == TimingStatus::StateReplayFailed);
        CHECK(resolveStatus({end, Reason::LiveMutationDetected}).status == TimingStatus::LiveMutationDetected);
    }
}

void testDualPair() {
    SECTION("v0.8.0 dual_pair: informational (allowed with ok), kept through the 12-reason truncation");
    ContextFacts cx;
    cx.dualPair = true;
    auto r = statusOf({}, okLocal(), decidedSA(), cx);
    CHECK(r.status == TimingStatus::Ok && has(r, Reason::DualPair) && r.reasons.back() == Reason::DualPair);
    std::string why;
    CHECK(statusConsistent(r.status, r.reasons, &why));
    // a pair result without a window still names the pair
    auto n = statusOf({{Reason::ControlMismatch}}, {}, {}, cx);
    CHECK(n.status == TimingStatus::StateReplayFailed && has(n, Reason::DualPair));
    // 14 reasons: truncated to 12, dual_pair survives in the last slot, the status is unchanged
    std::vector<Reason> many = {Reason::DualPair, Reason::Isolated, Reason::OpenEarly, Reason::OpenLate, Reason::LimitedByNeighbour, Reason::LimitedByHistory,
                                Reason::AttemptStart, Reason::TransitionInput, Reason::LocalEdgeDownstream, Reason::LocalEdgeAdaptable, Reason::Miss,
                                Reason::HalfTickInput, Reason::UntestedGapInBracket, Reason::ControlInvalidLayerSync};
    auto t = resolveStatus(many);
    CHECK(t.reasons.size() == 12 && t.reasons.back() == Reason::DualPair);
    CHECK(t.status == TimingStatus::StateReplayFailed && t.reasons.front() == Reason::ControlInvalidLayerSync);
    CHECK(statusConsistent(t.status, t.reasons, &why));
    // without dual_pair the truncation is the old one (the 12 first)
    many.erase(many.begin());
    many.push_back(Reason::OpenRange);
    auto u = resolveStatus(many);
    CHECK(u.reasons.size() == 12 && !has(u, Reason::DualPair) && !has(u, Reason::OpenRange));
}

int main() {
    testVocabulary();
    testPrecedence();
    testStatusOfEachReason();
    testOpenRangeIsOkDecided();
    testSpeedChangeInLookahead();
    testMissDownstreamAndHorizonTail();
    testDual();
    testLiveMutation();
    testControlInvalidIsolation();
    testDualPair();
    return gprl::test::finish("timing_status_tests");
}
