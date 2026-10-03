// fingerprint_build host tests (docs/SOLVER_DESIGN.md §9.5): every gamemode x press / release
// row of the input-direction table, trajectory thresholds, horizontal state, the portal id
// classes, null gaps / hold, yVelocity convention, and that the built fingerprint serialises like
// the golden fixture's shape.
#include "test_util.hpp"

#include "../core/fingerprint_build.hpp"
#include "../core/telemetry.hpp"

using namespace gprl;

namespace {

void testInputDirection() {
    SECTION("input direction per gamemode and kind");
    struct Row { Gamemode g; InputDirection press; InputDirection release; };
    Row rows[] = {
        {Gamemode::Cube, InputDirection::Up, InputDirection::None},   {Gamemode::Ship, InputDirection::Up, InputDirection::Down},
        {Gamemode::Ball, InputDirection::Flip, InputDirection::None}, {Gamemode::Ufo, InputDirection::Up, InputDirection::Down},
        {Gamemode::Wave, InputDirection::Up, InputDirection::Down},   {Gamemode::Robot, InputDirection::Up, InputDirection::None},
        {Gamemode::Spider, InputDirection::Flip, InputDirection::None}, {Gamemode::Swing, InputDirection::Up, InputDirection::Down},
    };
    for (auto const& r : rows) {
        CHECK_MSG(inputDirectionOf(r.g, InputKind::Press) == r.press, std::string(name(r.g)) + " press");
        CHECK_MSG(inputDirectionOf(r.g, InputKind::Release) == r.release, std::string(name(r.g)) + " release");
    }
}

void testTrajectory() {
    SECTION("trajectory and horizontal state");
    CHECK(trajectoryOf(true, false, 500.0) == Trajectory::Grounded);
    CHECK(trajectoryOf(true, true, 500.0) == Trajectory::Grounded);
    CHECK(trajectoryOf(false, true, 500.0) == Trajectory::Ceiling);
    CHECK(trajectoryOf(false, false, 30.1) == Trajectory::Rising);
    CHECK(trajectoryOf(false, false, 30.0) == Trajectory::Apex);
    CHECK(trajectoryOf(false, false, -30.0) == Trajectory::Apex);
    CHECK(trajectoryOf(false, false, -30.1) == Trajectory::Falling);
    CHECK(trajectoryOf(false, false, 0.0) == Trajectory::Apex);
    CHECK(horizontalStateOf(true, true, true) == HorizontalState::Dash);
    CHECK(horizontalStateOf(false, true, true) == HorizontalState::Ground);
    CHECK(horizontalStateOf(false, false, true) == HorizontalState::Ceiling);
    CHECK(horizontalStateOf(false, false, false) == HorizontalState::Air);
    // gravity-normalised "up": raw yVel is in units per 1/60 s step, flipped gravity inverts the sign
    CHECK_NEAR(yVelocityUnitsPerSecond(3.07, false), 184.2, 1e-9);
    CHECK_NEAR(yVelocityUnitsPerSecond(3.07, true), -184.2, 1e-9);
    CHECK_NEAR(yVelocityUnitsPerSecond(-3.07, true), 184.2, 1e-9);
}

void testPortals() {
    SECTION("portal object ids -> transition class");
    for (int id : {10, 11}) CHECK(portalTransitionOf(id) == PortalTransition::Gravity);
    for (int id : {12, 13, 47, 111, 660, 745, 1331, 1933}) CHECK(portalTransitionOf(id) == PortalTransition::Gamemode);
    for (int id : {200, 201, 202, 203, 1334}) CHECK(portalTransitionOf(id) == PortalTransition::Speed);
    for (int id : {99, 101}) CHECK(portalTransitionOf(id) == PortalTransition::Size);
    for (int id : {45, 46}) CHECK(portalTransitionOf(id) == PortalTransition::Mirror);
    for (int id : {286, 287}) CHECK(portalTransitionOf(id) == PortalTransition::Dual);
    for (int id : {0, 1, 747, 2902, 35, 1717}) CHECK(portalTransitionOf(id) == PortalTransition::None);
}

void testBuild() {
    SECTION("snapshot + context -> fingerprint fields");
    PlayerStateSnapshot pre;
    pre.gamemode = Gamemode::Ship;
    pre.speed = Speed::Fast;
    pre.gravityFlipped = true;
    pre.mini = true;
    pre.yVel = -2.0;    // flipped: moving "up" in gravity-normalised terms
    pre.isOnGround = false;
    pre.isDashing = false;
    FingerprintContext ctx;
    ctx.kind = InputKind::Release;
    ctx.prevInputGapMs = 45.83;
    ctx.nextInputGapMs = std::nullopt;
    ctx.holdMs = 45.83;
    ctx.portalObjectId = 1334;
    ctx.geometryHash = "0123456789abcdef";
    auto f = buildFingerprint(pre, ctx);
    CHECK(f.gamemode == Gamemode::Ship && f.speed == Speed::Fast && f.gravity == Gravity::Flipped && f.mini);
    CHECK(f.kind == InputKind::Release);
    CHECK(f.windowMs == 0.0);   // window_event fills it
    CHECK_NEAR(f.yVelocity, 120.0, 1e-9);
    CHECK(f.trajectory == Trajectory::Rising);
    CHECK(f.horizontalState == HorizontalState::Air);
    CHECK(f.prevInputGapMs && *f.prevInputGapMs == 45.83);
    CHECK(!f.nextInputGapMs);
    CHECK(f.holdMs && *f.holdMs == 45.83);
    CHECK(f.portalTransition == PortalTransition::Speed);
    CHECK(f.inputDirection == InputDirection::Down);
    CHECK(f.geometryHash == "0123456789abcdef");
    // grounded cube press with nothing known: nulls stay null, direction up
    PlayerStateSnapshot cube;
    cube.isOnGround = true;
    FingerprintContext c2;
    auto g = buildFingerprint(cube, c2);
    CHECK(g.trajectory == Trajectory::Grounded && g.horizontalState == HorizontalState::Ground);
    CHECK(!g.prevInputGapMs && !g.nextInputGapMs && !g.holdMs);
    CHECK(g.inputDirection == InputDirection::Up && g.portalTransition == PortalTransition::None);
    CHECK(g.geometryHash.empty());
    // the JSON shape: nulls written as null (like tools/fixture-gen), every key present
    auto j = telemetry::fingerprintToJson(g);
    std::string s = json::stringify(j);
    CHECK(s.find("\"prevInputGapMs\":null") != std::string::npos);
    CHECK(s.find("\"inputDirection\":\"up\"") != std::string::npos);
    CHECK(s.find("\"geometryHash\":\"\"") != std::string::npos);
    TimingFingerprint back;
    std::string err;
    CHECK_MSG(telemetry::fingerprintFromJson(j, back, &err), err);
    CHECK(back == g);
    // ceiling: last top collision set while airborne
    FingerprintContext c3;
    c3.ceilingTouch = true;
    PlayerStateSnapshot air;
    air.yVel = 5.0;
    auto h = buildFingerprint(air, c3);
    CHECK(h.trajectory == Trajectory::Ceiling && h.horizontalState == HorizontalState::Ceiling);
    // dashing wins the horizontal state
    PlayerStateSnapshot dash;
    dash.isDashing = true;
    dash.isOnGround = true;
    CHECK(buildFingerprint(dash, FingerprintContext{}).horizontalState == HorizontalState::Dash);
}

}  // namespace

int main() {
    testInputDirection();
    testTrajectory();
    testPortals();
    testBuild();
    return gprl::test::finish("fingerprint_build_tests");
}
