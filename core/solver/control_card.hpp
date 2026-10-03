#pragma once
// The developer / debug view of one SHIP CONTROL (docs/SHIP_SOLVER.md §11.7; owner prompt "SHIP
// COUNTING / COMPENSATED TIMING WINDOW REWRITE" §10 and §17, 2026-10-03): the press's and the
// release's timing_result of one hold as ONE card,
//
//   SHIP CONTROL #3
//     PRESS     local 1.0 f | compensated 15.0 f
//     RELEASE   local 1.0 f | compensated >= 15.0 f (not decided: sa_not_measured_budget)
//     HOLD      original 8.0 f | valid 6.0 - 10.0 f (sequence)
//     PHASE     early 7.0 f | late 8.0 f
//     SEQUENCE  cluster #5 | members 6 | effective 2.4 | rejoined yes (parallel) | confidence 70%
//
// and the explicit per-input fields of §17 as one `name=value` line (fieldsLine).
//
// Every figure is read from the two TimingResultPayloads (what is sent is what is shown):
//
//   local / compensated   latestMs - earliestMs of `local` / `sequence`, in 240 Hz frames: the
//                         width from edge to edge = the number of frames the input could have
//                         been made in ("frames available"). `+` after the figure: a side ended
//                         at a search limit, not at a fail (the true window is at least this wide)
//   LOCAL                 one input moved, every other input replayed as performed (frozen)
//   COMPENSATED           the nearby later inputs may adjust; a pass must survive AND re-join the
//                         recorded run (rejoin.hpp). Not decided = `>=` the proven part + the reason;
//                         never a narrower figure made up for a search that ran out of budget
//   HOLD                  the recorded duration; the valid durations = the release's `hold` range
//   PHASE                 how far the whole hold shifts with its duration kept (`control.phase`)
//   cluster / members     the measured cluster id's first input and this control's position in it
//                         (the cluster may still grow); effective = members ^ clusterWeightExponent
//   rejoined              the weakest re-join among the compensated edges (n/a: every edge is local)
//   rated                 per input, the mirror of the server's rules: its replay was exact, its
//                         status is ok / low_confidence and its compensated window is decided. An
//                         input every tested shift of which re-joined has NO timing requirement
//                         (`no_effect`): it is shown and never rated
//   confidence            the weakest re-join's weight x 100 over the RATED inputs of the control;
//                         0 when none is rated (a failed replay, a search that was not decided)
//
// CardParams MIRRORS the server's evidence parameters (shared/src/rating/params.ts `evidence`,
// timing-evidence/3) for DISPLAY only: the server decides what a result is worth.
//
// PURE C++20 (no Geode includes); host-tested in tests/ship_cases_tests.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "../telemetry.hpp"

namespace gprl::solver::control {

struct CardParams {
    char const* version = "gprl-control-card/1";
    double clusterWeightExponent = 0.5;   // EvidenceParams.clusterWeightExponent
    double approxWeight = 0.9;            // EvidenceParams.rejoinWeights.approx
    double parallelWeight = 0.7;          // EvidenceParams.rejoinWeights.parallel
};
inline constexpr CardParams kCardParams{};

struct CardWindow {
    bool present = false;
    double frames = 0.0;   // edge to edge: frames available
    bool open = false;     // a side ended at a limit, not at a fail: at least this wide
};

/// One input (press or release) of the control, as its timing_result says.
struct CardInput {
    bool present = false;          // a timing_result exists
    bool replayValid = true;
    std::string status;
    std::string reason;            // its first status reason ("" = none)
    CardWindow local;
    CardWindow compensated;
    bool decided = false;          // the compensated window is decided on both sides
    std::string rejoin[2];         // early / late: the re-join kind of a compensated edge ("" = a local edge)
    double rejoinAfterFrames[2] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    double rejoinErrorY[2] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    double rejoinErrorVy[2] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    double parityTick = std::numeric_limits<double>::quiet_NaN();   // the replay's first divergence (NaN = none)
    std::string parityField;
    std::string clusterId;
    int clusterIndex = 0;
    bool usedForRating = false;    // the mirror of the server's rules: replay valid, status ok / low_confidence, decided
    double confidence = 0.0;       // 0..1
};

enum class Rejoined : uint8_t { NotApplicable, Yes, No };

struct Card {
    int index = 0;
    CardInput press, release;
    double holdFrames = std::numeric_limits<double>::quiet_NaN();
    bool holdRange = false;
    double holdMinFrames = 0.0, holdMaxFrames = 0.0;
    std::string holdBasis;
    bool holdAtLeast = false;      // the release's compensated window is not decided: the range is what was proven
    bool phase = false;
    double phaseEarlyFrames = 0.0, phaseLateFrames = 0.0;   // both >= 0
    bool phaseDecided = false;
    int cluster = 0;               // the cluster's first input (attemptInputIndex)
    int members = 0;               // members up to this control
    double effectiveMembers = 0.0;
    Rejoined rejoined = Rejoined::NotApplicable;
    std::string rejoinKind;        // the weakest kind among the compensated edges
    int confidencePct = 0;
    bool usedForRating = false;    // at least one of its inputs is rated
};

namespace detail {
/// ms -> 240 Hz frames (never "-0.0").
inline double framesOf(double ms) {
    double const f = ms / kTickMs;
    return f == 0.0 ? 0.0 : f;
}
inline bool boundedSide(telemetry::TimingEdgeV2Payload const& e) { return e.stop == "fail" || e.failMs.has_value(); }
inline CardWindow windowOf(telemetry::TimingWindowV2Payload const& w) {
    CardWindow c;
    c.present = true;
    c.frames = std::max(0.0, framesOf(w.latestMs - w.earliestMs));
    c.open = !boundedSide(w.early) || !boundedSide(w.late);
    return c;
}
/// Order of weakness: exact / level_end (1) > approx > parallel.
inline double rejoinWeight(std::string const& kind, CardParams const& p) {
    if (kind == "approx") return p.approxWeight;
    if (kind == "parallel") return p.parallelWeight;
    return 1.0;
}
inline std::string fmt(char const* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
}  // namespace detail

inline CardInput cardInput(telemetry::TimingResultPayload const& p, CardParams const& params = kCardParams) {
    CardInput c;
    c.present = true;
    c.replayValid = p.stateReplayValid;
    c.status = p.status;
    if (!p.statusReasons.empty()) c.reason = p.statusReasons.front();
    if (p.local) c.local = detail::windowOf(*p.local);
    if (p.sequence) {
        c.compensated = detail::windowOf(p.sequence->window);
        c.decided = p.sequence->decided;
    }
    if (p.control) {
        auto take = [&](int side, std::optional<telemetry::TimingRejoinPayload> const& r) {
            if (!r) return;
            c.rejoin[side] = r->kind;
            c.rejoinAfterFrames[side] = detail::framesOf(r->afterMs);
            c.rejoinErrorY[side] = r->errorY;
            c.rejoinErrorVy[side] = r->errorVy;
        };
        take(0, p.control->rejoinEarly);
        take(1, p.control->rejoinLate);
    }
    if (p.parity) {
        c.parityTick = p.parity->tick;
        c.parityField = p.parity->field;
    }
    c.clusterId = p.cluster.id;
    c.clusterIndex = static_cast<int>(p.cluster.index);
    c.usedForRating = c.replayValid && !p.miss && (p.status == "ok" || p.status == "low_confidence") && c.compensated.present && c.decided;
    c.confidence = c.usedForRating ? std::min(detail::rejoinWeight(c.rejoin[0], params), detail::rejoinWeight(c.rejoin[1], params)) : 0.0;
    return c;
}

/// The card of one control. `press` may be null (held before the attempt, or its result was not
/// kept); `release` is the result that completes the control.
inline Card makeCard(telemetry::TimingResultPayload const* press, telemetry::TimingResultPayload const& release, CardParams const& params = kCardParams) {
    Card c;
    if (press) c.press = cardInput(*press, params);
    c.release = cardInput(release, params);
    if (release.control) {
        auto const& k = *release.control;
        c.index = static_cast<int>(k.index);
        if (k.holdMs) c.holdFrames = detail::framesOf(*k.holdMs);
        if (k.phase) {
            c.phase = true;
            c.phaseEarlyFrames = detail::framesOf(-k.phase->earlyMs);
            c.phaseLateFrames = detail::framesOf(k.phase->lateMs);
            c.phaseDecided = k.phase->decided;
        }
    }
    if (release.hold) {
        c.holdRange = true;
        c.holdMinFrames = detail::framesOf(release.hold->minMs);
        c.holdMaxFrames = detail::framesOf(release.hold->maxMs);
        c.holdBasis = release.hold->basis;
        c.holdAtLeast = release.hold->basis == "sequence" && !c.release.decided;
    }
    // the cluster: "<attemptId>:<first member>"
    auto colon = release.cluster.id.rfind(':');
    if (colon != std::string::npos) c.cluster = std::atoi(release.cluster.id.c_str() + colon + 1);
    c.members = std::max(1, static_cast<int>(release.cluster.index));
    c.effectiveMembers = std::pow(static_cast<double>(c.members), params.clusterWeightExponent);
    // the weakest re-join among the compensated edges of both inputs
    double weakest = 2.0;
    bool noRejoin = false;
    for (CardInput const* in : {&c.press, &c.release}) {
        if (!in->present) continue;
        if (in->reason == "sa_survives_no_rejoin") noRejoin = true;
        for (auto const& kind : in->rejoin) {
            if (kind.empty()) continue;
            double w = detail::rejoinWeight(kind, params);
            // on equal weights the first kind stays
            if (w < weakest) {
                weakest = w;
                c.rejoinKind = kind;
            }
        }
    }
    c.rejoined = noRejoin ? Rejoined::No : c.rejoinKind.empty() ? Rejoined::NotApplicable : Rejoined::Yes;
    // the weakest confidence among the RATED inputs (an input without a timing requirement is
    // not evidence either way and does not pull the control down)
    double conf = 2.0;
    for (CardInput const* in : {&c.press, &c.release}) {
        if (!in->present || !in->usedForRating) continue;
        c.usedForRating = true;
        conf = std::min(conf, in->confidence);
    }
    c.confidencePct = c.usedForRating ? static_cast<int>(std::lround(100.0 * conf)) : 0;
    return c;
}

inline std::string inputText(CardInput const& in) {
    using detail::fmt;
    if (!in.present) return "no result";
    if (!in.replayValid) {
        std::string s = "replay failed";
        if (std::isfinite(in.parityTick)) s += fmt(" at tick %.0f", in.parityTick) + " (" + in.parityField + ")";
        return s + ": no window";
    }
    std::string const why = in.reason.empty() ? in.status : in.reason;
    if (!in.local.present) return "no window (" + why + ")";
    std::string s = "local " + fmt("%.1f", in.local.frames) + (in.local.open ? "+ f" : " f") + " | compensated ";
    if (!in.compensated.present) return s + "not measured (" + why + ")";
    if (!in.decided) return s + ">= " + fmt("%.1f", in.compensated.frames) + " f (not decided: " + why + ")";
    s += fmt("%.1f", in.compensated.frames) + (in.compensated.open ? "+ f" : " f");
    if (in.status == "no_effect") s += " (no timing requirement: not rated)";
    else if (!in.usedForRating) s += " (not rated: " + why + ")";
    return s;
}

/// The compact HUD suffix of one connected-control input: "compensated 15.0 f", "compensated >=
/// 15.0 f" (not decided: what was proven), "replay failed". The HUD line's own figure is the
/// FROZEN (local) window; this is what the nearby inputs make of it.
inline std::string hudText(CardInput const& in) {
    using detail::fmt;
    if (!in.present) return {};
    if (!in.replayValid) return "replay failed";
    if (!in.local.present) return "no window";
    if (!in.compensated.present) return "compensated: not measured";
    if (!in.decided) return "compensated >= " + fmt("%.1f f", in.compensated.frames);
    return "compensated " + fmt("%.1f", in.compensated.frames) + (in.compensated.open ? "+ f" : " f");
}

/// The card as log / HUD lines (§10 of the prompt).
inline std::vector<std::string> cardLines(Card const& c) {
    using detail::fmt;
    std::vector<std::string> out;
    out.push_back("SHIP CONTROL #" + std::to_string(c.index));
    out.push_back("  PRESS     " + inputText(c.press));
    out.push_back("  RELEASE   " + inputText(c.release));
    std::string hold = "  HOLD      original " + (std::isfinite(c.holdFrames) ? fmt("%.1f f", c.holdFrames) : std::string("unknown"));
    if (c.holdRange) hold += std::string(" | valid ") + (c.holdAtLeast ? "at least " : "") + fmt("%.1f", c.holdMinFrames) + " - " + fmt("%.1f f", c.holdMaxFrames) + " (" + c.holdBasis + ")";
    else hold += " | valid range not measured";
    out.push_back(hold);
    if (c.phase) out.push_back("  PHASE     early " + fmt("%.1f f", c.phaseEarlyFrames) + " | late " + fmt("%.1f f", c.phaseLateFrames) + (c.phaseDecided ? "" : " (not decided: at least)"));
    else out.push_back("  PHASE     not measured");
    std::string rj = c.rejoined == Rejoined::Yes ? "yes (" + c.rejoinKind + ")" : c.rejoined == Rejoined::No ? "no" : "n/a (local)";
    out.push_back("  SEQUENCE  cluster #" + std::to_string(c.cluster) + " | members " + std::to_string(c.members) + " | effective " + fmt("%.1f", c.effectiveMembers) + " | rejoined " + rj
                  + " | confidence " + std::to_string(c.confidencePct) + "%" + (c.usedForRating ? "" : " (not used for rating)"));
    return out;
}

/// The explicit fields of §17 for ONE input of a card, as `name=value` pairs on one line (`na` =
/// not measured). Frames are 240 Hz ticks.
inline std::string fieldsLine(Card const& c, bool release, CardParams const& params = kCardParams) {
    using detail::fmt;
    CardInput const& in = release ? c.release : c.press;
    auto num = [](bool have, double v, char const* f = "%.2f") { return have && std::isfinite(v) ? detail::fmt(f, v) : std::string("na"); };
    // the edge whose re-join the fields describe: the weaker of the two (the late one on a tie)
    int side = -1;
    for (int i = 0; i < 2; ++i) {
        if (in.rejoin[i].empty()) continue;
        if (side < 0 || detail::rejoinWeight(in.rejoin[i], params) <= detail::rejoinWeight(in.rejoin[side], params)) side = i;
    }
    std::string s;
    s += "local_window_frames=" + num(in.local.present, in.local.frames);
    s += " compensated_window_frames=" + num(in.compensated.present && in.decided, in.compensated.frames);
    s += " phase_early_frames=" + num(c.phase, c.phaseEarlyFrames);
    s += " phase_late_frames=" + num(c.phase, c.phaseLateFrames);
    s += " original_hold_frames=" + num(true, c.holdFrames);
    s += " min_hold_frames=" + num(c.holdRange, c.holdMinFrames);
    s += " max_hold_frames=" + num(c.holdRange, c.holdMaxFrames);
    s += " cluster_id=" + (in.clusterId.empty() ? std::string("na") : in.clusterId);
    s += " cluster_members=" + std::to_string(std::max(in.clusterIndex, 0));
    s += " effective_weight=" + (in.clusterIndex > 0 ? fmt("%.3f", std::pow(static_cast<double>(in.clusterIndex), params.clusterWeightExponent - 1.0)) : std::string("na"));
    s += std::string(" rejoin_success=") + (in.reason == "sa_survives_no_rejoin" ? "no" : side >= 0 ? "yes" : "na");
    s += " rejoin_tick=" + num(side >= 0, side >= 0 ? in.rejoinAfterFrames[side] : 0.0);
    s += " rejoin_error_y=" + num(side >= 0, side >= 0 ? in.rejoinErrorY[side] : 0.0, "%.3f");
    s += " rejoin_error_vy=" + num(side >= 0, side >= 0 ? in.rejoinErrorVy[side] : 0.0, "%.4f");
    s += std::string(" state_replay_valid=") + (in.present ? (in.replayValid ? "true" : "false") : "na");
    s += " first_divergence_tick=" + num(true, in.parityTick, "%.0f");
    s += " solver_confidence=" + (in.present ? fmt("%.2f", in.confidence) : std::string("na"));
    s += std::string(" used_for_rating=") + (in.present ? (in.usedForRating ? "true" : "false") : "na");
    return s;
}

}  // namespace gprl::solver::control
