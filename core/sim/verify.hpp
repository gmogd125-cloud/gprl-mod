#pragma once
// Physics verification (docs/BACKGROUND_ANALYZER_DESIGN.md §4.5, AN-D3): exactness is measured,
// never assumed. The simulator replays a passively recorded attempt's inputs from its start state
// and compares its trajectory tick by tick with the recorded real one:
//
//   position   |dx| + |dy| <= tolerance (0.5 units by default)
//   yVelocity  |dv| <= 0.05 or <= 1 % of the recorded magnitude
//   discrete   upsideDown, gamemode, mini, speed, onGround, held, dead equal
//   bins       2 % of the level by the recorded x; a bin is `verified` when every recorded tick
//              in it matched; `reason` = the first failing field; a bin overlapping an
//              UnsupportedSpan is `unsupported` (not counted either way)
//   stop       the replay stops at the first divergence beyond `divergenceStop` (5 units) outside
//              the unsupported spans; the bins after it that hold recorded ticks are reported
//              `after_divergence` and COUNT as diverged (honest: the simulator did not reproduce them)
//   spans      (gprl-sim/2) while the RECORDED x is inside an UnsupportedSpan nothing is compared
//              and nothing diverges; at the first recorded tick past the span the engine is
//              re-seeded from that tick (x, y, yVelocity, gamemode, gravity, size, speed, onGround,
//              held; the previous recorded tick as the last position) and the comparison resumes -
//              also after a divergence, so one unsupported mechanic no longer fails the rest of the
//              attempt. `reseeds` counts them.
//   share      verifiedShare = verified bins / countedBins (bins with ticks and not unsupported);
//              countedBins == 0 means there is nothing to verify (the result's hasVerification is
//              then false: analysis.cpp)
//
// Pure C++20, host-tested in tests/sim_verify_tests.cpp.
#include <vector>

#include "result.hpp"
#include "world.hpp"

namespace gprl::sim {

struct VerifyConfig {
    double tolerance = 0.5;        // units of |dx| + |dy|
    double divergenceStop = 5.0;   // units: the replay stops here
    double yVelocityAbs = 0.05;
    double yVelocityRel = 0.01;
    double binPercent = 2.0;
};

/// Replays one attempt. `tolerance` overrides VerifyConfig::tolerance.
VerifyResult verifyAttempt(World const& world, RecordedAttempt const& attempt, double tolerance, VerifyConfig const& cfg = {});
inline VerifyResult verifyAttempt(World const& world, RecordedAttempt const& attempt, VerifyConfig const& cfg = {}) {
    return verifyAttempt(world, attempt, cfg.tolerance, cfg);
}

/// Merges per-attempt results over the same world: a bin is verified when every attempt that
/// recorded ticks in it verified it; `ticks` add up; `maxError` is the maximum; the reason is
/// the first failing one; unsupported stays unsupported.
VerifyResult mergeVerify(std::vector<VerifyResult> const& results);

/// Index of the bin a percent falls in (0..49 with 2 % bins; 100 % belongs to the last bin).
int verifyBinIndex(double percent, double binPercent = 2.0);

}  // namespace gprl::sim
