#include "fingerprint_build.hpp"

namespace gprl {

double yVelocityUnitsPerSecond(double yVelRaw, bool gravityFlipped) {
    return yVelRaw * 60.0 * (gravityFlipped ? -1.0 : 1.0);
}

Trajectory trajectoryOf(bool onGround, bool ceilingTouch, double vy, double risingThreshold, double fallingThreshold) {
    if (onGround) return Trajectory::Grounded;
    if (ceilingTouch) return Trajectory::Ceiling;
    if (vy > risingThreshold) return Trajectory::Rising;
    if (vy < fallingThreshold) return Trajectory::Falling;
    return Trajectory::Apex;
}

HorizontalState horizontalStateOf(bool dashing, bool onGround, bool ceilingTouch) {
    if (dashing) return HorizontalState::Dash;
    if (onGround) return HorizontalState::Ground;
    if (ceilingTouch) return HorizontalState::Ceiling;
    return HorizontalState::Air;
}

PortalTransition portalTransitionOf(int id) {
    switch (id) {
        case 10: case 11: return PortalTransition::Gravity;
        case 12: case 13: case 47: case 111: case 660: case 745: case 1331: case 1933: return PortalTransition::Gamemode;
        case 200: case 201: case 202: case 203: case 1334: return PortalTransition::Speed;
        case 99: case 101: return PortalTransition::Size;
        case 45: case 46: return PortalTransition::Mirror;
        case 286: case 287: return PortalTransition::Dual;
        default: return PortalTransition::None;
    }
}

InputDirection inputDirectionOf(Gamemode g, InputKind kind) {
    if (kind == InputKind::Press) {
        switch (g) {
            case Gamemode::Ball:
            case Gamemode::Spider: return InputDirection::Flip;
            default: return InputDirection::Up;
        }
    }
    switch (g) {
        case Gamemode::Ship:
        case Gamemode::Ufo:
        case Gamemode::Swing:
        case Gamemode::Wave: return InputDirection::Down;
        default: return InputDirection::None;
    }
}

TimingFingerprint buildFingerprint(PlayerStateSnapshot const& pre, FingerprintContext const& ctx) {
    TimingFingerprint f;
    f.gamemode = pre.gamemode;
    f.speed = pre.speed;
    f.gravity = pre.gravity();
    f.kind = ctx.kind;
    f.mini = pre.mini;
    f.windowMs = 0.0;   // window_event fills it
    f.yVelocity = yVelocityUnitsPerSecond(pre.yVel, pre.gravityFlipped);
    f.trajectory = trajectoryOf(pre.isOnGround, ctx.ceilingTouch, f.yVelocity, ctx.risingThreshold, ctx.fallingThreshold);
    f.horizontalState = horizontalStateOf(pre.isDashing, pre.isOnGround, ctx.ceilingTouch);
    f.prevInputGapMs = ctx.prevInputGapMs;
    f.nextInputGapMs = ctx.nextInputGapMs;
    f.holdMs = ctx.holdMs;
    f.portalTransition = portalTransitionOf(ctx.portalObjectId);
    f.inputDirection = inputDirectionOf(pre.gamemode, ctx.kind);
    f.geometryHash = ctx.geometryHash;
    return f;
}

}  // namespace gprl
