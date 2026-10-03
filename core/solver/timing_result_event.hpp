#pragma once
// Planner results -> telemetry TimingResultPayload (docs/TIMING_SOLVER_V2.md §4.1, V2-D6).
//
//   local     the LOCAL (fixed-sequence) window exactly as window_event builds the timing_window:
//             earliestMs = actualMs + 0.5 x (passShiftMs + failShiftMs) on a bounded side, the
//             pass edge otherwise - the same doubles, so the server's cross-check with the
//             timing_window (`local_mismatch_timing_window`) is bit-exact
//   sequence  the SEQUENCE-ADJUSTED window of core/solver/sequence_adjusted (SAResult, frames ->
//             ms at 1000/240), solverVersion = sequence_adjusted.hpp kSASolverVersion
//   dual      v0.8.0 (revision 5): the player-2 facts of a dual PAIR result (ctx.dual), next to
//             the informational reason `dual_pair`
//   live mutation  v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §3.3): a `live_mutation_detected`
//             status drops every window / the hold / `miss` and forces stateReplayValid false,
//             whatever evidence the caller passes; its fallback keeps the status (a breach is
//             never downgraded to `unresolved`)
//   pair      the PAIR window when its walk completed
//   hold      releases whose press is known: the sequence window (basis `sequence`) or the local
//             one (`local`) expressed as hold durations, next to the local range
//   status    core/solver/timing_status statusOf over the facts built here (localFacts / saFacts)
//
// Every payload is checked with telemetry::validateTimingResult before it is returned; a builder
// failure yields the FALLBACK payload (status unresolved, reason payload_invalid, no windows), so
// a bound job always ends in exactly one valid timing_result.
//
// checkTimingResultPayload mirrors the server gate `timing-result-evidence/0.1` (§4.3; v0.8.0 adds
// `live_mutation_detected`: such a result is never usable) without the allowlist and the stored-row
// checks. PURE C++20; host-tested in tests/timing_result_event_tests.cpp.
#include <optional>
#include <string>
#include <vector>

#include "../telemetry.hpp"
#include "cluster.hpp"
#include "local_window.hpp"
#include "pass_planner.hpp"
#include "sequence_adjusted.hpp"
#include "timing_status.hpp"

namespace gprl::solver {

/// Everything about the input that is not a window.
struct TimingResultContext {
    int64_t inputSeq = 0;
    InputKind kind = InputKind::Press;
    int attemptInputIndex = 1;
    double x = 0.0;
    double percentAtInput = 0.0;
    double subTickMs = 0.0;                // the tracker's: the input event's tSubTick x 1000/240
    double engineSubTickMs = kNaN;         // Fable D11: the engine's own frac(job frame) x 1000/240 (NaN = not reported);
                                           // when finite it is the source of actualMs (the edges are shifts from job.t)
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    std::optional<Gamemode> gamemodeAfter;
    double eventT = 0.0;                   // the input event's t (seconds)
    bool subtick = false;                  // CBF placement: edges are `cbf`
    bool refined = false;                  // a sub-tick pass refined the local window (solverVersionFor(true): the `-cbf` string)
    double pressMs = kNaN;                 // releases: the press's actualMs (NaN = unknown)
    int64_t pressSeq = -1;                 // releases: the press's input seq (-1 = unknown)
    cluster::ClusterRef cluster;
    int controlSimulations = 0;
    int boundarySimulations = 0;           // local shift trials simulated + SA / pair trials charged
    // v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §2.7, solver-dual = measure): the player-2 facts of a
    // result measured by the dual PAIR simulator; the status rules add `dual_pair`
    // (ContextFacts::dualPair) and the validators require this block with that reason
    std::optional<telemetry::TimingResultDualPayload> dual;
};

/// The local window and what the status rules need to know about it.
struct LocalEvidence {
    WindowResult const* window = nullptr; // valid window, or nullptr when the job has none
    std::vector<ShiftOutcome> const* outcomes = nullptr;
    bool miss = false;                     // the player's miss (the attributed one, Fable D6)
    bool missDownstream = false;           // Fable D6: an earlier job of a died run (miss_downstream, `miss` false)
    bool extension = false;                // finished in the frozen death pause ([ext])
    bool widthBelowResolution = false;
    double frame = 0.0;                    // the input's recorded frame
    double nextFrame = kNaN;               // the next logged input's frame (NaN = none)
    bool nextFollows = false;              // the next input may follow (no cluster break)
    double horizonFrame = kNaN;            // end of the local look-ahead
};

/// LocalFacts of timing_status from the local window (§2.10): miss / extension / untested gap /
/// width below resolution / resolution / islands next to an edge / open sides / limit stops /
/// downstream and adaptable fail edges / no_effect (every tested shift re-joined before the next
/// input or the look-ahead, no fail).
status::LocalFacts localFacts(LocalEvidence const& ev, status::TimingStatusConfig const& cfg = status::kStatusConfig);
/// SAFacts from a finished (or finish()ed) SA planner result; `ran` = an SA result exists at all.
status::SAFacts saFacts(SAResult const* sa, status::Reason notRunWhy = status::Reason::SaNotMeasuredBudget);

/// Whether every tested local shift re-joined the real run before the next input (or the
/// look-ahead) without a fail: the input had no timing requirement (`no_effect`).
bool allShiftsRejoined(std::vector<ShiftOutcome> const& outcomes, double frame, double nextFrame, double horizonFrame);

struct TimingResultBuild {
    bool ok = false;                       // false: `payload` is the fallback (unresolved, payload_invalid)
    std::string error;
    telemetry::TimingResultPayload payload;
    status::StatusResult status;
};

TimingResultBuild buildTimingResultEvent(TimingResultContext const& ctx, LocalEvidence const& local, SAResult const* sa,
                                         status::StatusResult const& status, bool stateReplayValid);

/// The v2 window object of a WindowResult (the timing_window's edges, bit-exact).
telemetry::TimingWindowV2Payload localWindowPayload(WindowResult const& w, double actualMs, bool subtick);
/// The v2 window object of an SA / pair window (frames relative to the input). `withProof`: the
/// sequence window carries the optional per-edge `proof` / `provenPassMs` (Fable review D3b).
telemetry::TimingWindowV2Payload saWindowPayload(SAWindow const& w, double actualMs, bool subtick, bool withProof = false);

/// C++ mirror of the server gate timing-result-evidence/0.1 (§4.3, api/src/processing/
/// timingResults.ts, same reason names) without the allowlist, the input kind and the session's
/// physics: the §4.1 rules (`payload_invalid`), W_local ⊆ W_SA on the PASS and the reported edges
/// (`sequence_not_containing_local`, Fable D1, sequenceContainsLocal), a bounded window wider than 2000 ms
/// (`window_implausibly_wide`), `local` equal to the same input's timing_window edges (bit-exact
/// doubles) when one is given, the proof keys (`proof_inconsistent`, Fable D3b), actualMs vs the
/// input event (t x 1000 + the engine's sub-tick when the payload carries engineSubTickMs, else the
/// tracker's; and the tracker copy subTickMs equal to the input's, 0.01 ms) and the engine's sub-tick
/// vs the input event's (0.5 ms, `subtick_clock_mismatch`, Fable D11).
struct TimingResultCheck {
    bool accepted = true;
    std::vector<std::string> reasons;
};
/// Fable review D1 (the server gate's sequenceContainsLocal): W_local ⊆ W_SA on the PASS edges
/// and on the reported edges. Every result the solver builds satisfies it (an SA side that stopped
/// at the local pass edge inherits the local bracket, SAPlanner::result).
bool sequenceContainsLocal(telemetry::TimingWindowV2Payload const& local, telemetry::TimingWindowV2Payload const& sequence);
/// Fable review D3b (the server gate's proofConsistent, `proof_inconsistent`): the optional per-edge
/// proof keys are consistent with the local edge (see the .cpp for the rules).
bool proofConsistent(telemetry::TimingResultPayload const& p);
TimingResultCheck checkTimingResultPayload(telemetry::TimingResultPayload const& p, double eventT, double eventSubTickMs,
                                           telemetry::TimingWindowPayload const* window = nullptr);

}  // namespace gprl::solver
