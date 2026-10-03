#pragma once
// LevelSimResult assembly and its JSON (docs/BACKGROUND_ANALYZER_DESIGN.md §4.6). PURE C++20,
// host-tested in tests/sim_job_tests.cpp (round trip through core/json).
//
// JSON shape (exact keys = the result.hpp field names, FLAT at the top level; the server's
// shared/src/level-sim/validate.ts and docs/contracts/level-sim.md check the same):
//   analyzerVersion, simVersion, gameplayHashVersion, gdLevelId, levelHash (64 hex), gameplayHash
//   (16 hex), computedAt (ISO, Z; the mod sets it), build (non-empty; the mod sets it),
//   objects, gameplayObjects, decorationObjects, lengthX, lengthSeconds, startPositions, tooLarge,
//   coverage:     { physicsPercent, solvedPercent, hasVerification, verifiedPercent (0 without verification),
//                   unsupported: [ { percentFrom, percentTo, x0, x1, mechanic } ], unsolved: [ ... ] }
//   referenceInputs, referenceTicks, trajectoryDigest (DECIMAL STRING of the uint64),
//   windows:      [ { source, tick, frame, tSeconds, percent, down, gamemode, speed, windowMs, earliestMs,
//                     latestMs, resolutionMs, boundedEarly, boundedLate, verified, supported, geometryHash,
//                     geometryHashHex, trials } ]  (<= 4000; windowMs == latestMs - earliestMs exactly)
//   hasVerification (gprl-sim/2: false also when attempts were replayed but no bin could be counted),
//   verification: null (no attempt replayed) | { attempts, ticksCompared, verifiedShare, tolerancePosition,
//                          countedBins, reseeds (gprl-sim/2 extras),
//                          bins: [ { from, to, ticks, verified, unsupported, maxError, firstDivergenceStep, reason } ] }
//   coverage (gprl-sim/2): physicsPercent over the UNION of the unsupported spans, solvedPercent =
//                 the reference run's length outside that union (<= physicsPercent), <= 1000 spans per
//                 list (overlaps merged, then the tail folded into a "multiple" span), a window inside
//                 any emitted span by validate.ts's [percentFrom, percentTo) rule is never `supported`
//   density:      { avgCps, p90Cps, peak1sCps, peak5sCps }
//   sections:     [ { from, to, gamemode, speed, mini, inputs, narrowestMs, medianMs, supported, solved, verified } ]
//   budget:       { cpuMs, wallMs, pausedMs, ticksSimulated, trials, searchRestarts, mode, recordSafe, budgetExhausted }
//   debug:        [ strings ] (<= 50 here, the server allows 64)
// Enums are strings (gamemode "cube".."swing", source "reference" / "recorded"); speed is the
// integer 0..4; booleans are booleans; every string is cut to 128 chars; percents are clamped.
#include <string>
#include <vector>

#include "../json.hpp"
#include "result.hpp"
#include "search.hpp"
#include "world.hpp"

namespace gprl::sim {

/// Builds the result from the pieces the job produced. `verify` may be null (no recorded
/// attempt). Window `verified` flags are set here from the verify bins.
LevelSimResult assemble(World const& world, ReferenceRun const& run, std::vector<SimWindow> const& referenceWindows,
                        std::vector<SimWindow> const& recordedWindows, VerifyResult const* verify, SimBudget budget);

/// Re-applies a (new) verification to an assembled result: verification block, coverage
/// verified fields, window and section `verified` flags.
void applyVerification(LevelSimResult& result, VerifyResult const* verify);

/// Click density over presses: avg = presses / seconds; p90 of 1 s windows stepped 0.25 s;
/// peak1s = the busiest 1 s window; peak5s = the busiest 5 s window / 5.
SimDensity clickDensity(std::vector<RecordedInput> const& inputs, double seconds);

json::Value toJson(LevelSimResult const& r);
std::string toJsonString(LevelSimResult const& r);   // compact

}  // namespace gprl::sim
