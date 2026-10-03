// analyzer_recorder_tests: the pure parts of the passive recorder, the per-visit keep policy, the
// frame-pressure sampler and the status texts (core/analyzer_recorder.hpp, core/analyzer_status.hpp;
// docs/BACKGROUND_ANALYZER_DESIGN.md §4.5, AN-D12).
#include "../core/analyzer_recorder.hpp"
#include "../core/analyzer_status.hpp"
#include "test_util.hpp"

#include <cmath>
#include <string>

using namespace gprl;
using namespace gprl::analyzer;

namespace {

PlayerReading reading(float x, float y, bool dead = false) {
    PlayerReading r;
    r.x = x;
    r.y = y;
    r.yVelocity = -1.5;
    r.mode = sim::Gamemode::Ship;
    r.speed = sim::Speed::Double;
    r.mini = true;
    r.onGround = false;
    r.held = true;
    r.dead = dead;
    return r;
}

/// One GD step: processCommands pre-hook (tick record when due, then the inputs of the step),
/// then the player update with `dt` (1/60 s units).
void step(AttemptRecorder& rec, double dt, float x, int pressRelease = 0) {
    if (rec.wantsTick()) rec.recordTick(reading(x, 105.f));
    if (pressRelease == 1) rec.onInput(true);
    if (pressRelease == 2) rec.onInput(false);
    rec.onPlayerUpdate(dt);
}

sim::RecordedAttempt attemptOf(int ticks, bool completed) {
    sim::RecordedAttempt a;
    a.completed = completed;
    a.ticks.resize(static_cast<size_t>(ticks));
    for (int i = 0; i < ticks; ++i) a.ticks[static_cast<size_t>(i)].step = i + 1;
    a.endStep = ticks;
    return a;
}

}  // namespace

int main(int, char**) {
    SECTION("whole ticks: one record per 240 TPS tick, the end state of that tick");
    {
        AttemptRecorder rec;
        sim::StartState start;
        start.x = 0.f;
        start.y = 105.f;
        rec.begin(start, 3, false);
        CHECK(rec.active());
        CHECK(!rec.wantsTick());
        step(rec, 0.25, 0.f, 1);   // tick 1: press at its start
        step(rec, 0.25, 5.f);      // records tick 1 (x = 5)
        step(rec, 0.25, 10.f, 2);  // records tick 2, release in tick 3
        auto a = rec.finish(false, false, 12.5f, nullptr);
        CHECK(!rec.active());
        CHECK(a.attemptIndex == 3);
        CHECK(a.ticks.size() == 2u);
        CHECK(a.ticks[0].step == 1 && a.ticks[1].step == 2);
        CHECK_NEAR(a.ticks[0].x, 5.f, 1e-6);
        CHECK(a.ticks[0].mode == sim::Gamemode::Ship && a.ticks[0].mini && a.ticks[0].held && a.ticks[0].speed == sim::Speed::Double);
        CHECK_NEAR(a.ticks[0].yVelocity, -1.5, 1e-6);
        CHECK(a.inputs.size() == 2u);
        CHECK(a.inputs[0].step == 1 && a.inputs[0].down && a.inputs[0].subTick == 0.0);
        CHECK(a.inputs[1].step == 3 && !a.inputs[1].down);
        CHECK(a.endStep == 2);
        CHECK_NEAR(a.endPercent, 12.5, 1e-6);
    }

    SECTION("half ticks / CBF splits: two processCommands per tick record once; the sub-tick is the simulated fraction");
    {
        AttemptRecorder rec;
        rec.begin({}, 1, true);
        step(rec, 0.125, 0.f);        // first half of tick 1
        CHECK(!rec.wantsTick());
        step(rec, 0.125, 1.f, 1);     // second half: press at sub-tick 0.5
        step(rec, 0.0625, 2.f);       // tick 1 complete -> recorded here; CBF split of tick 2 at 0.25
        step(rec, 0.1875, 3.f, 2);    // release at sub-tick 0.25 of tick 2
        step(rec, 0.25, 4.f);         // tick 2 recorded
        auto a = rec.finish(true, false, 1.f, nullptr);
        CHECK(a.cbf);
        CHECK(a.ticks.size() == 2u);
        CHECK(a.inputs.size() == 2u);
        CHECK(a.inputs[0].step == 1);
        CHECK_NEAR(a.inputs[0].subTick, 0.5, 1e-9);
        CHECK(a.inputs[1].step == 2);
        CHECK_NEAR(a.inputs[1].subTick, 0.25, 1e-9);
        CHECK(a.died);
    }

    SECTION("death: the final reading is the end state of the tick the death happened in");
    {
        AttemptRecorder rec;
        rec.begin({}, 1, false);
        step(rec, 0.25, 0.f);
        step(rec, 0.25, 1.f);   // tick 1 recorded; tick 2 simulated, the player died in its collisions
        auto dead = reading(2.f, 100.f, true);
        auto a = rec.finish(true, false, 0.5f, &dead);
        CHECK(a.ticks.size() == 2u);
        CHECK(a.ticks.back().dead);
        CHECK(a.ticks.back().step == 2);
        CHECK(a.endStep == 2);
    }

    SECTION("the 60 000-tick cap: ticks and inputs stop, the attempt is marked truncated");
    {
        RecorderLimits lim;
        lim.maxTicksPerAttempt = 10;
        AttemptRecorder rec(lim);
        rec.begin({}, 1, false);
        for (int i = 0; i < 25; ++i) step(rec, 0.25, static_cast<float>(i), i == 20 ? 1 : 0);
        CHECK(rec.ticks() == 10);
        CHECK(rec.meta().truncated);
        auto a = rec.finish(false, false, 0.f);
        CHECK(a.ticks.size() == 10u);
        CHECK(a.inputs.empty());
        CHECK(kRecorderLimits.maxTicksPerAttempt == 60000);
        CHECK(kRecorderLimits.maxAttemptsPerVisit == 40);
    }

    SECTION("an inactive recorder ignores everything; NaN deltas are ignored");
    {
        AttemptRecorder rec;
        rec.onPlayerUpdate(0.25);
        rec.onInput(true);
        CHECK(!rec.wantsTick());
        auto a = rec.finish(true, true, 100.f);
        CHECK(a.ticks.empty() && a.inputs.empty());
        rec.begin({}, 1, false);
        rec.onPlayerUpdate(std::nan(""));
        rec.onPlayerUpdate(-1.0);
        CHECK(!rec.wantsTick());
        rec.abandon();
        CHECK(!rec.active());
    }

    SECTION("keep policy before the hand-over: the best 40 (completed first, then longest)");
    {
        AttemptStore store;
        CHECK(!store.offer(attemptOf(10, false)));   // < 24 ticks: verifies nothing
        for (int i = 0; i < 40; ++i) CHECK(store.offer(attemptOf(100 + i, false)));
        CHECK(store.kept() == 40);
        CHECK(!store.offer(attemptOf(50, false)));    // worse than every kept one
        CHECK(store.offer(attemptOf(1000, false)));   // evicts the shortest (100)
        AttemptMeta sp;
        sp.hasStartPos = true;
        sp.startPosX = 1234.f;
        CHECK(store.offer(attemptOf(30, true), sp));  // completed beats any uncompleted; its meta travels with it
        CHECK(store.kept() == 40);
        auto all = store.takeAll();
        CHECK(all.size() == 40u);
        CHECK(all.front().attempt.completed);
        CHECK(all.front().meta.hasStartPos && all.front().meta.startPosX == 1234.f);
        CHECK(all[1].attempt.ticks.size() == 1000u);
        CHECK(!all[1].meta.hasStartPos);
        bool has100 = false, has101 = false;
        for (auto const& a : all) {
            if (a.attempt.ticks.size() == 100u) has100 = true;
            if (a.attempt.ticks.size() == 101u) has101 = true;
        }
        CHECK(!has100 && !has101);   // the two shortest were evicted
        CHECK(store.handedOver());
        CHECK(store.handed() == 40);
        // after the hand-over: the cap is reached, only completed attempts (10 more) still go
        CHECK(!store.admit(attemptOf(5000, false)));
        for (int i = 0; i < 10; ++i) CHECK(store.admit(attemptOf(200, true)));
        CHECK(!store.admit(attemptOf(200, true)));
    }
    {
        AttemptStore store;
        auto none = store.takeAll();
        CHECK(none.empty());
        for (int i = 0; i < 40; ++i) CHECK(store.admit(attemptOf(30, false)));
        CHECK(!store.admit(attemptOf(30, false)));
        CHECK(!store.admit(attemptOf(3, true)));   // too short even when completed
        store.reset();
        CHECK(!store.handedOver() && store.handed() == 0);
    }

    SECTION("frame pressure: the last 30 frames average over 1.5 x the target");
    {
        // VSync on: the slower of the FPS cap and the display
        CHECK_NEAR(targetFrameMs(1.0 / 60.0, 144.0, true), 1000.0 / 60.0, 1e-9);   // FPS cap 60 on a 144 Hz monitor
        CHECK_NEAR(targetFrameMs(1.0 / 9999.0, 144.0, true), 1000.0 / 144.0, 1e-9); // uncapped: the refresh rate decides
        CHECK_NEAR(targetFrameMs(1.0 / 240.0, 60.0, true), 1000.0 / 60.0, 1e-9);   // 240 cap, 60 Hz, VSync: 60 FPS
        CHECK_NEAR(targetFrameMs(0.0, 144.0, true), 1000.0 / 144.0, 1e-9);         // no cap readable: the display
        // VSync off (review fix 2026-10-02): the FPS cap alone, the display does not limit the frame rate
        CHECK_NEAR(targetFrameMs(1.0 / 240.0, 60.0, false), 1000.0 / 240.0, 1e-9);  // 240 cap on a 60 Hz monitor: 4.17 ms
        CHECK_NEAR(targetFrameMs(1.0 / 60.0, 144.0, false), 1000.0 / 60.0, 1e-9);
        CHECK_NEAR(targetFrameMs(1.0 / 9999.0, 144.0, false), kPressure.minTargetMs, 1e-9);   // uncapped: the 1 ms floor
        CHECK_NEAR(targetFrameMs(0.0, 144.0, false), 1000.0 / 60.0, 1e-9);         // nothing usable: 60 FPS
        for (bool vsync : {false, true}) {
            CHECK_NEAR(targetFrameMs(1.0 / 9999.0, 0.0, vsync), kPressure.minTargetMs, 1e-9);
            CHECK_NEAR(targetFrameMs(0.0, 0.0, vsync), 1000.0 / 60.0, 1e-9);
            CHECK_NEAR(targetFrameMs(1.0, 0.0, vsync), kPressure.maxTargetMs, 1e-9);
            CHECK_NEAR(targetFrameMs(std::nan(""), std::nan(""), vsync), 1000.0 / 60.0, 1e-9);
            // VSync never makes the target FASTER than the FPS cap
            CHECK(targetFrameMs(1.0 / 120.0, 240.0, vsync) >= 1000.0 / 120.0 - 1e-9);
        }
        FramePressure fp;
        fp.setTargetMs(4.0);   // 240 Hz
        for (int i = 0; i < 29; ++i) fp.frame(10.0);
        CHECK(!fp.pressure());   // the window is not full yet
        fp.frame(10.0);
        CHECK(fp.pressure());
        CHECK_NEAR(fp.averageMs(), 10.0, 1e-9);
        for (int i = 0; i < 30; ++i) fp.frame(5.9);
        CHECK(!fp.pressure());   // 5.9 < 6.0
        for (int i = 0; i < 30; ++i) fp.frame(6.1);
        CHECK(fp.pressure());
        fp.frame(400.0);         // a pause / load: the window restarts
        CHECK(!fp.pressure());
        CHECK(fp.samples() == 0);
        fp.frame(std::nan(""));
        CHECK(fp.samples() == 0);
    }

    SECTION("status texts: ASCII, the owner's wording");
    {
        status::View v;
        v.simulatorEnabled = true;
        v.stage = status::Stage::Running;
        v.phase = "search";
        v.solvedPercent = 42.7;
        v.elapsedSeconds = 38.2;
        CHECK(status::hudLine(v) == "Level analysis: searching 42%, 38 s");
        v.stage = status::Stage::Done;
        v.hasVerification = true;
        v.verifiedPercent = 96.4;
        CHECK(status::hudLine(v) == "Level analysis: verified 96%");
        v.hasVerification = false;
        v.solvedPercent = 100.0;
        CHECK(status::hudLine(v) == "Level analysis: done, solved 100%");
        v.stage = status::Stage::Cached;
        CHECK(status::hudLine(v) == "Level analysis: cached");
        v.stage = status::Stage::Waiting;
        v.wait = status::WaitReason::RecordSafeAttempt;
        CHECK(status::hudLine(v) == "Record-Safe: waiting for the attempt to end");
        v.stage = status::Stage::Stopped;
        CHECK(status::hudLine(v) == "Level analysis: stopped (isolation)");
        v.stage = status::Stage::Extracting;
        v.extractPercent = 37.9;
        v.upload = "uploaded (stored)";
        CHECK(status::hudLine(v) == "Level analysis: reading level 37% - uploaded (stored)");
        v = {};
        CHECK(status::hudLine(v).empty());
        CHECK(status::ascii("search 42 % \xC2\xB7 38 s") == "search 42 % - 38 s");
        CHECK(status::ascii("\xE2\x89\xA5 x") == "- x");
        CHECK(status::pct(-5.0) == "0%");
        CHECK(status::pct(150.0) == "100%");
        CHECK(status::seconds(1200.0) == "20 min");
    }

    return test::finish("analyzer_recorder_tests");
}
