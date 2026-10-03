#pragma once
// PlayerStateSnapshot + input context -> TimingFingerprint (docs/SOLVER_DESIGN.md §3.6, SPEC §12).
// The tables are pure so tests/fingerprint_build_tests.cpp pins every gamemode x press/release
// row, the trajectory thresholds, the portal ids and the null gaps / hold. The window width
// (`windowMs`) is filled by solver/window_event when the window is known.
//
//   gamemode / speed / gravity / mini   from the pre-input snapshot
//   yVelocity   = snapshot.yVel * 60 * (gravityFlipped ? -1 : 1)  (units/s, gravity-normalised "up";
//                 convention to confirm with the shared owner, fingerprint.ts scale 120 units/s)
//   trajectory  grounded if on the ground; ceiling when the last top collision (bottom when flipped)
//               is set and not on the ground; else rising (> +30), falling (< -30), apex otherwise
//   horizontal  dash if dashing, ground if on the ground, ceiling as above, else air
//   gaps / hold from the engine's input log (null = none within the horizon / unknown)
//   portal      the first portal touched between the input and the horizon, classified by id
//   direction   press: cube/robot/ufo/ship/swing/wave -> up, ball/spider -> flip;
//               release: ship/ufo/swing/wave -> down, others none
#include <optional>
#include <string>

#include "fingerprint.hpp"
#include "snapshot.hpp"
#include "vocab.hpp"

namespace gprl {

struct FingerprintContext {
    InputKind kind = InputKind::Press;
    std::optional<double> prevInputGapMs;
    std::optional<double> nextInputGapMs;
    std::optional<double> holdMs;
    int portalObjectId = 0;          // first portal touched within the horizon after the input, 0 = none
    std::string geometryHash;        // core/geometry_hash hex ("" = unknown, neutral in similarity)
    bool ceilingTouch = false;       // last top collision id set (bottom when gravity is flipped)
    double risingThreshold = 30.0;   // units/s
    double fallingThreshold = -30.0;
};

double yVelocityUnitsPerSecond(double yVelRaw, bool gravityFlipped);
Trajectory trajectoryOf(bool onGround, bool ceilingTouch, double yVelocityUnitsPerSecond, double risingThreshold = 30.0,
                        double fallingThreshold = -30.0);
HorizontalState horizontalStateOf(bool dashing, bool onGround, bool ceilingTouch);
PortalTransition portalTransitionOf(int objectId);
InputDirection inputDirectionOf(Gamemode gamemode, InputKind kind);

TimingFingerprint buildFingerprint(PlayerStateSnapshot const& pre, FingerprintContext const& ctx);

}  // namespace gprl
