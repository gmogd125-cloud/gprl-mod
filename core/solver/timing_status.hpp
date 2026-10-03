#pragma once
// Timing statuses `gprl-timing-status/2` (docs/TIMING_SOLVER_V2.md §2.10, V2-D6; AUDIT §4, §15, §16;
// v0.8.0 live isolation: docs/LIVE_ISOLATION_DESIGN.md §3.3, §4, §8 TELEMETRY).
//
// Every measured input ends in exactly one `timing_result` carrying ONE status and the reasons
// that led to it. "It is better to return no exact number than a false exact number" (AUDIT §16):
// no precise window leaves the solver unlabelled, and the level analysis turns only `ok` into
// measured windows.
//
// Precedence, top down (reasons accumulate; the status is the highest group any reason reaches):
//
//   live_mutation_detected  (v0.8.0) the development invariant check saw the LIVE game change while
//                        analysis ran (ISOLATION "abort that analysis sample"): the sample is
//                        aborted, never a window, stateReplayValid false, never usable server side;
//                        the isolation breaker ends every open result of the attempt with it
//   state_replay_failed  the deterministic replay could not be trusted (control mismatch, teleport /
//                        dual portal, history lost, snapshot self-test, player 2, a real death the
//                        shadow did not reproduce, the replay breaker; v0.8.0: a control that read
//                        live layer state its own trial did not have (`control_invalid_layer_sync`)
//                        or reached an unisolated game path (`control_invalid_unisolated`))
//   unresolved           nothing reliable was measured (no shift tested, death unrelated, cut by a
//                        restart / the level end, pool, both sides blocked, unbound, platformer,
//                        payload invalid)
//   no_effect            a classification, not a failure: every tested shift re-joined the real run
//   sequence_dependent   a local edge that later inputs could still change (downstream, or self but
//                        the next input shifted along would act before the death) whose
//                        sequence-adjusted side could not be decided (undecided / not measured /
//                        expired / proofs failed), or a miss whose bound is such an edge
//   low_confidence       a window exists but something about it is weak (miss, frozen-world
//                        extension, untested gap, resolution, width below resolution, islands next
//                        to an edge, shadow mismatch nearby, half-tick input, SA not measured)
//   ok                   none of the above; informational reasons are allowed
//
// Consistency (validate.ts and checkTimingResultPayload): the status equals the highest group of
// its non-informational reasons (ok when there is none); reasons are unique, at most 12, ordered
// by group then vocabulary.
//
// Two reasons beyond the design's list (docs/SOLVER_DESIGN.md §14.2): `sa_death_in_span`
// (sequence_dependent: a real / would-be death lies inside an SA job's span, so no delayed replay
// can prove anything there) and `local_edge_adaptable` (informational: a SELF local edge whose next
// input, shifted along, would act before the death - the wave "segment too long" edges of RC1).
//
// v0.7.1 (the Fable review, docs/TIMING_SOLVER_V2_FABLE.md §3.6; vocabulary grown before the first
// release, the version string is unchanged): `speed_change_in_lookahead` (low_confidence, D7),
// `open_range` (D3a), `sa_survived_only` (D3b) and `fail_near_horizon` (D16) (informational), and
// `miss_downstream` now also names the EARLIER jobs of a died run (D6, miss_attribution.hpp).
//
// v0.8.0 (`gprl-timing-status/2`, telemetry revision 5, docs/LIVE_ISOLATION_DESIGN.md §8): status
// `live_mutation_detected` (appended to the status list, precedence above state_replay_failed) and
// its reason `live_mutation_detected` (first in the reason list: the highest group comes first);
// `control_invalid_unisolated` and `control_invalid_layer_sync` close the state_replay_failed group;
// `dual_pair` (informational: measured by the dual PAIR simulator, the result carries the optional
// `dual` block) closes the list. Existing reasons keep their relative order.
//
// PURE C++20; host-tested in tests/timing_status_tests.cpp. The vocabularies mirror
// shared/src/telemetry/schema.ts TIMING_STATUSES / TIMING_STATUS_REASONS (same names, same order).
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gprl::solver::status {

constexpr char const* kTimingStatusVersion = "gprl-timing-status/2";

/// schema.ts TIMING_STATUSES (same order; v0.8.0 appends live_mutation_detected so the indexes of
/// the six older statuses - the engine's per-status counters - do not move).
enum class TimingStatus : uint8_t { Ok, LowConfidence, Unresolved, StateReplayFailed, SequenceDependent, NoEffect, LiveMutationDetected };
constexpr int kTimingStatusCount = 7;

constexpr char const* name(TimingStatus s) {
    switch (s) {
        case TimingStatus::Ok: return "ok";
        case TimingStatus::LowConfidence: return "low_confidence";
        case TimingStatus::Unresolved: return "unresolved";
        case TimingStatus::StateReplayFailed: return "state_replay_failed";
        case TimingStatus::SequenceDependent: return "sequence_dependent";
        case TimingStatus::NoEffect: return "no_effect";
        case TimingStatus::LiveMutationDetected: return "live_mutation_detected";
    }
    return "unresolved";
}

/// Higher = wins. ok 0 < low_confidence < sequence_dependent < no_effect < unresolved <
/// state_replay_failed < live_mutation_detected (v0.8.0: a sample analysed while the live game
/// changed is worse than any replay doubt - its world never existed for the player).
constexpr int precedence(TimingStatus s) {
    switch (s) {
        case TimingStatus::Ok: return 0;
        case TimingStatus::LowConfidence: return 1;
        case TimingStatus::SequenceDependent: return 2;
        case TimingStatus::NoEffect: return 3;
        case TimingStatus::Unresolved: return 4;
        case TimingStatus::StateReplayFailed: return 5;
        case TimingStatus::LiveMutationDetected: return 6;
    }
    return 0;
}

/// The HUD history's word for a status (src/Hud via the engine's "local 4.00 f / seq 10.75 f <word>"
/// suffix; core/display maps the same token): the wire name, except `isolation stop` for a sample the
/// live-state invariant aborted (docs/LIVE_ISOLATION_DESIGN.md §8 TELEMETRY "HUD readout word").
constexpr char const* hudWord(TimingStatus s) {
    return s == TimingStatus::LiveMutationDetected ? "isolation stop" : name(s);
}

/// schema.ts TIMING_STATUS_REASONS (same names, same order: grouped by the status they force,
/// highest group first, informational last).
enum class Reason : uint8_t {
    // live_mutation_detected (v0.8.0)
    LiveMutationDetected,
    // state_replay_failed
    ControlMismatch, ControlInvalidTeleport, ControlInvalidDualPortal, HistoryLost, SnapshotSelftestFailed, DualNotSimulated,
    RealDeathNotReproduced, ReplayBreaker, ControlInvalidUnisolated, ControlInvalidLayerSync,
    // unresolved
    NoShiftTested, NoPassDeathUnrelated, CutByRestart, CutByLevelEnd, PoolExhausted, BlockedBothSides, UnboundInput,
    PlatformerUnsupported, PayloadInvalid,
    // no_effect
    AllShiftsRejoined,
    // sequence_dependent
    SaUndecided, SaNotMeasuredBudget, SaExpired, SaNoNegativeControl, SaControlMismatch, SaNegativeNotReproduced, SaInvalidTrials,
    SaCutByRestart, SaDeathInSpan, MissDownstream,
    // low_confidence
    Miss, FrozenWorldExtension, UntestedGapInBracket, ResolutionAboveMax, WidthBelowResolution, IslandsNearEdge, ShadowMismatchNearby,
    HalfTickInput, SaNotMeasured, SpeedChangeInLookahead,
    // informational (allowed with any status, including ok)
    Isolated, OpenEarly, OpenLate, LimitedByNeighbour, LimitedByHistory, AttemptStart, TransitionInput, LocalEdgeDownstream,
    LocalEdgeAdaptable, OpenRange, SaSurvivedOnly, FailNearHorizon, DualPair,
};
constexpr int kReasonCount = static_cast<int>(Reason::DualPair) + 1;

constexpr char const* name(Reason r) {
    switch (r) {
        case Reason::LiveMutationDetected: return "live_mutation_detected";
        case Reason::ControlMismatch: return "control_mismatch";
        case Reason::ControlInvalidTeleport: return "control_invalid_teleport";
        case Reason::ControlInvalidDualPortal: return "control_invalid_dual_portal";
        case Reason::HistoryLost: return "history_lost";
        case Reason::SnapshotSelftestFailed: return "snapshot_selftest_failed";
        case Reason::DualNotSimulated: return "dual_not_simulated";
        case Reason::RealDeathNotReproduced: return "real_death_not_reproduced";
        case Reason::ReplayBreaker: return "replay_breaker";
        case Reason::ControlInvalidUnisolated: return "control_invalid_unisolated";
        case Reason::ControlInvalidLayerSync: return "control_invalid_layer_sync";
        case Reason::NoShiftTested: return "no_shift_tested";
        case Reason::NoPassDeathUnrelated: return "no_pass_death_unrelated";
        case Reason::CutByRestart: return "cut_by_restart";
        case Reason::CutByLevelEnd: return "cut_by_level_end";
        case Reason::PoolExhausted: return "pool_exhausted";
        case Reason::BlockedBothSides: return "blocked_both_sides";
        case Reason::UnboundInput: return "unbound_input";
        case Reason::PlatformerUnsupported: return "platformer_unsupported";
        case Reason::PayloadInvalid: return "payload_invalid";
        case Reason::AllShiftsRejoined: return "all_shifts_rejoined";
        case Reason::SaUndecided: return "sa_undecided";
        case Reason::SaNotMeasuredBudget: return "sa_not_measured_budget";
        case Reason::SaExpired: return "sa_expired";
        case Reason::SaNoNegativeControl: return "sa_no_negative_control";
        case Reason::SaControlMismatch: return "sa_control_mismatch";
        case Reason::SaNegativeNotReproduced: return "sa_negative_not_reproduced";
        case Reason::SaInvalidTrials: return "sa_invalid_trials";
        case Reason::SaCutByRestart: return "sa_cut_by_restart";
        case Reason::SaDeathInSpan: return "sa_death_in_span";
        case Reason::MissDownstream: return "miss_downstream";
        case Reason::Miss: return "miss";
        case Reason::FrozenWorldExtension: return "frozen_world_extension";
        case Reason::UntestedGapInBracket: return "untested_gap_in_bracket";
        case Reason::ResolutionAboveMax: return "resolution_above_max";
        case Reason::WidthBelowResolution: return "width_below_resolution";
        case Reason::IslandsNearEdge: return "islands_near_edge";
        case Reason::ShadowMismatchNearby: return "shadow_mismatch_nearby";
        case Reason::HalfTickInput: return "half_tick_input";
        case Reason::SaNotMeasured: return "sa_not_measured";
        case Reason::SpeedChangeInLookahead: return "speed_change_in_lookahead";
        case Reason::Isolated: return "isolated";
        case Reason::OpenEarly: return "open_early";
        case Reason::OpenLate: return "open_late";
        case Reason::LimitedByNeighbour: return "limited_by_neighbour";
        case Reason::LimitedByHistory: return "limited_by_history";
        case Reason::AttemptStart: return "attempt_start";
        case Reason::TransitionInput: return "transition_input";
        case Reason::LocalEdgeDownstream: return "local_edge_downstream";
        case Reason::LocalEdgeAdaptable: return "local_edge_adaptable";
        case Reason::OpenRange: return "open_range";
        case Reason::SaSurvivedOnly: return "sa_survived_only";
        case Reason::FailNearHorizon: return "fail_near_horizon";
        case Reason::DualPair: return "dual_pair";
    }
    return "payload_invalid";
}

/// The status a reason forces (informational reasons force nothing: Ok).
constexpr TimingStatus group(Reason r) {
    if (r == Reason::LiveMutationDetected) return TimingStatus::LiveMutationDetected;
    if (r <= Reason::ControlInvalidLayerSync) return TimingStatus::StateReplayFailed;
    if (r <= Reason::PayloadInvalid) return TimingStatus::Unresolved;
    if (r == Reason::AllShiftsRejoined) return TimingStatus::NoEffect;
    if (r <= Reason::MissDownstream) return TimingStatus::SequenceDependent;
    if (r <= Reason::SpeedChangeInLookahead) return TimingStatus::LowConfidence;
    return TimingStatus::Ok;
}
constexpr bool informational(Reason r) { return group(r) == TimingStatus::Ok; }

inline bool parseStatus(std::string_view s, TimingStatus& out) {
    for (int i = 0; i < kTimingStatusCount; ++i) {
        if (s == name(static_cast<TimingStatus>(i))) {
            out = static_cast<TimingStatus>(i);
            return true;
        }
    }
    return false;
}

inline bool parseReason(std::string_view s, Reason& out) {
    for (int i = 0; i < kReasonCount; ++i) {
        if (s == name(static_cast<Reason>(i))) {
            out = static_cast<Reason>(i);
            return true;
        }
    }
    return false;
}

/// Every threshold of the status rules in ONE object with a version (DEV DEFAULTS).
struct TimingStatusConfig {
    char const* version = kTimingStatusVersion;
    double maxResolutionMs = 4.2;     // api/src/processing/config.ts maxResolutionMs (one tick + margin)
    int transitionTicks = 2;          // a gamemode / gravity / size portal this close after the input: transition_input
    double islandNearTicks = 2.0;     // a passing island this close beyond a side's first fail: islands_near_edge
    int maxReasons = 12;              // schema.ts: statusReasons has at most 12 entries
    // v0.7.1 (Fable review, DEV DEFAULTS)
    bool speedChangeFlag = true;      // D7: a real speed change inside [t - maxShift, look-ahead] -> speed_change_in_lookahead
    double horizonTailFrames = 10.0;  // D16: a local fail edge whose death lies this close to the horizon -> fail_near_horizon (info)
};
constexpr TimingStatusConfig kStatusConfig{};

struct StatusResult {
    TimingStatus status = TimingStatus::Ok;
    std::vector<Reason> reasons;
};

/// Unique reasons ordered by group precedence (highest first) then vocabulary, truncated to
/// `maxReasons` (the forcing reasons come first, so truncation never changes the status), and the
/// status = the highest group reached (ok when only informational reasons remain). v0.8.0: a
/// truncation keeps `dual_pair` (it replaces the last kept reason): the reason names the
/// simulator that produced the result and the validators require the `dual` block with it.
inline StatusResult resolveStatus(std::vector<Reason> reasons, TimingStatusConfig const& cfg = kStatusConfig) {
    std::sort(reasons.begin(), reasons.end(), [](Reason a, Reason b) {
        int pa = informational(a) ? -1 : precedence(group(a));
        int pb = informational(b) ? -1 : precedence(group(b));
        if (pa != pb) return pa > pb;
        return static_cast<int>(a) < static_cast<int>(b);
    });
    reasons.erase(std::unique(reasons.begin(), reasons.end()), reasons.end());
    if (cfg.maxReasons > 1 && static_cast<int>(reasons.size()) > cfg.maxReasons) {
        bool dualPair = reasons.back() == Reason::DualPair;   // the last of the vocabulary sorts last
        reasons.resize(static_cast<size_t>(cfg.maxReasons));
        // the first reason is the highest group, so replacing the last one never changes the status
        if (dualPair) reasons.back() = Reason::DualPair;
    }
    else if (static_cast<int>(reasons.size()) > cfg.maxReasons) reasons.resize(static_cast<size_t>(std::max(0, cfg.maxReasons)));
    StatusResult r;
    for (Reason x : reasons) {
        if (informational(x)) continue;
        if (precedence(group(x)) > precedence(r.status)) r.status = group(x);
    }
    r.reasons = std::move(reasons);
    return r;
}

/// The consistency rule both validators apply: unique reasons, at most maxReasons, and the status
/// equal to the highest group of the non-informational reasons (ok without any).
inline bool statusConsistent(TimingStatus s, std::vector<Reason> const& reasons, std::string* why = nullptr,
                             TimingStatusConfig const& cfg = kStatusConfig) {
    auto fail = [&](std::string m) {
        if (why) *why = std::move(m);
        return false;
    };
    if (static_cast<int>(reasons.size()) > cfg.maxReasons) return fail("more than 12 status reasons");
    TimingStatus top = TimingStatus::Ok;
    for (size_t i = 0; i < reasons.size(); ++i) {
        for (size_t j = i + 1; j < reasons.size(); ++j) {
            if (reasons[i] == reasons[j]) return fail(std::string("duplicate status reason ") + name(reasons[i]));
        }
        if (!informational(reasons[i]) && precedence(group(reasons[i])) > precedence(top)) top = group(reasons[i]);
    }
    if (top != s) return fail(std::string("status ") + name(s) + " is inconsistent with its reasons (they reach " + name(top) + ")");
    return true;
}

// ---- statusOf: the rules of §2.10 over the facts of one measured input ----

/// How the input's job ended when it produced no usable local window (or a proof failed).
struct JobFacts {
    std::vector<Reason> ended;   // state_replay_failed / unresolved reasons of the job itself (empty = normal end)
};

/// The local (fixed-sequence) window, when one exists.
struct LocalFacts {
    bool present = false;
    bool miss = false;
    bool extension = false;          // finished in the frozen death pause ([ext])
    bool untestedGap = false;        // a NotTested point inside a bracket
    bool widthBelowResolution = false;
    double resolutionMs = 0.0;
    bool islandsNearEdge = false;
    bool noEffect = false;           // every tested shift re-joined before the next input, no fail
    bool openEarly = false, openLate = false;
    bool limitedByNeighbour = false, limitedByHistory = false, attemptStart = false;
    bool downstream[2] = {false, false};   // early / late fail edge caused downstream
    bool adaptable[2] = {false, false};    // early / late fail edge a follower could still change
    // v0.7.1 (Fable review)
    bool missDownstream = false;     // D6: the control died with the real player, but a LATER input of the
                                     // died run could still have saved it (miss_attribution.hpp): not
                                     // the player's miss, no timing_window, `miss` false on the wire
    bool failNearHorizon = false;    // D16: a fail edge set by a death in the last horizonTailFrames of the look-ahead
};

/// The sequence-adjusted window (none for misses and no-effect inputs).
struct SAFacts {
    bool present = false;            // a `sequence` block exists
    bool decided = false;            // both sides ended at a fail or a limit
    bool sideDecided[2] = {false, false};
    Reason failure = Reason::SaUndecided;   // why a side is undecided / SA did not run (a sequence_dependent reason)
    bool openRange = false;          // D3a: both local sides open to their search limit - sequence = local copy, decided
    bool survivedOnly = false;       // D3b: a side widened on survived-only passes (no rejoin proof)
};

struct ContextFacts {
    bool shadowMismatchNearby = false;
    bool realDeathNotReproduced = false;
    bool replayBreaker = false;
    bool halfTickInput = false;
    bool transitionInput = false;
    bool isolated = false;
    bool speedChangeInSpan = false;  // D7: the real player's speed changed inside [t - maxShift, look-ahead]
    // v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §3.3, §2.7)
    bool liveMutation = false;       // the invariant check aborted this sample (or the isolation breaker ended it)
    bool dualPair = false;           // measured by the dual PAIR simulator (solver-dual = measure): info `dual_pair`
};

/// v0.8.0: a status / reason list that says the live game changed under the analysis. Such a
/// result carries no window, `stateReplayValid` false and never a `timing_window`
/// (docs/LIVE_ISOLATION_DESIGN.md §3.3); both validators enforce it.
inline bool isLiveMutation(StatusResult const& r) { return r.status == TimingStatus::LiveMutationDetected; }

/// D7 (Fable W12): whether a real speed change (frame of the first step that ran the new speed)
/// lies inside a job's span [t - maxShiftTicks, lookAheadFrame] - every shifted trial of the job
/// crossed it (the clones mirror the real player's speed by time, not by x). Bounded by the list.
inline bool speedChangeInSpan(std::vector<double> const& changeFrames, double t, double maxShiftTicks, double lookAheadFrame) {
    for (double f : changeFrames) {
        if (f >= t - maxShiftTicks - 1e-9 && f <= lookAheadFrame + 1e-9) return true;
    }
    return false;
}

inline StatusResult statusOf(JobFacts const& job, LocalFacts const& local, SAFacts const& sa, ContextFacts const& ctx,
                             TimingStatusConfig const& cfg = kStatusConfig) {
    std::vector<Reason> r = job.ended;
    if (ctx.dualPair) r.push_back(Reason::DualPair);
    // v0.8.0 (§3.3): an aborted sample is described by the abort alone - whatever its trials
    // measured came from a world the player never had, so no window fact is added
    bool aborted = ctx.liveMutation || std::find(job.ended.begin(), job.ended.end(), Reason::LiveMutationDetected) != job.ended.end();
    if (aborted) {
        if (ctx.liveMutation) r.push_back(Reason::LiveMutationDetected);
        return resolveStatus(std::move(r), cfg);
    }
    if (ctx.realDeathNotReproduced) r.push_back(Reason::RealDeathNotReproduced);
    if (ctx.replayBreaker) r.push_back(Reason::ReplayBreaker);
    if (ctx.shadowMismatchNearby) r.push_back(Reason::ShadowMismatchNearby);
    if (ctx.halfTickInput) r.push_back(Reason::HalfTickInput);
    if (ctx.transitionInput) r.push_back(Reason::TransitionInput);
    if (!local.present) {
        // no window at all: a job that ended without one must say why
        if (job.ended.empty()) r.push_back(Reason::PayloadInvalid);
        return resolveStatus(std::move(r), cfg);
    }
    // D7 (Fable W12): the clones mirror the real player's speed by TIME, so a shifted trial whose
    // span crosses a real speed change ran the wrong speed for |shift| ticks
    if (ctx.speedChangeInSpan && cfg.speedChangeFlag) r.push_back(Reason::SpeedChangeInLookahead);
    if (local.failNearHorizon) r.push_back(Reason::FailNearHorizon);
    if (local.missDownstream) {
        // D6 (Fable §3.3): an earlier job of a run that died - a later input could still have
        // saved it; neither a hit measurement nor the player's miss, no sequence window
        r.push_back(Reason::MissDownstream);
        if (local.extension) r.push_back(Reason::FrozenWorldExtension);
        if (local.untestedGap) r.push_back(Reason::UntestedGapInBracket);
        return resolveStatus(std::move(r), cfg);
    }
    if (local.miss) r.push_back(Reason::Miss);
    if (local.extension) r.push_back(Reason::FrozenWorldExtension);
    if (local.untestedGap) r.push_back(Reason::UntestedGapInBracket);
    if (local.widthBelowResolution) r.push_back(Reason::WidthBelowResolution);
    if (local.resolutionMs > cfg.maxResolutionMs + 1e-9) r.push_back(Reason::ResolutionAboveMax);
    if (local.islandsNearEdge) r.push_back(Reason::IslandsNearEdge);
    if (local.openEarly) r.push_back(Reason::OpenEarly);
    if (local.openLate) r.push_back(Reason::OpenLate);
    if (local.limitedByNeighbour) r.push_back(Reason::LimitedByNeighbour);
    if (local.limitedByHistory) r.push_back(Reason::LimitedByHistory);
    if (local.attemptStart) r.push_back(Reason::AttemptStart);
    // a downstream edge is always adaptable (a later fixed input acted before the death); a self
    // edge is adaptable when the next input shifted along would act before the death (the wave
    // "segment got too long before the fixed next vertex" early edges, §1 RC1)
    bool adaptable[2] = {local.adaptable[0] || local.downstream[0], local.adaptable[1] || local.downstream[1]};
    bool anyDown = local.downstream[0] || local.downstream[1];
    bool anyAdapt = adaptable[0] || adaptable[1];
    bool selfAdapt = (adaptable[0] && !local.downstream[0]) || (adaptable[1] && !local.downstream[1]);
    if (anyDown) r.push_back(Reason::LocalEdgeDownstream);
    if (selfAdapt) r.push_back(Reason::LocalEdgeAdaptable);
    if (local.noEffect) {
        r.push_back(Reason::AllShiftsRejoined);
        return resolveStatus(std::move(r), cfg);
    }
    if (local.miss) {
        // misses get no sequence window: their continuation died (§2.5)
        if (anyAdapt) r.push_back(Reason::MissDownstream);
        return resolveStatus(std::move(r), cfg);
    }
    if (ctx.isolated) r.push_back(Reason::Isolated);
    if (sa.present && sa.openRange) r.push_back(Reason::OpenRange);
    if (sa.present && sa.survivedOnly) r.push_back(Reason::SaSurvivedOnly);
    if (!(sa.present && sa.decided)) {
        bool blocking = false;
        for (int s = 0; s < 2; ++s) {
            if (adaptable[s] && !(sa.present && sa.sideDecided[s])) blocking = true;
        }
        if (blocking) r.push_back(group(sa.failure) == TimingStatus::SequenceDependent ? sa.failure : Reason::SaUndecided);
        else r.push_back(Reason::SaNotMeasured);
    }
    return resolveStatus(std::move(r), cfg);
}

}  // namespace gprl::solver::status
