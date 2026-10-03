// Determinism host tests (SPEC §45 "trajectory determinism", the part that exists before the
// Phase 3 GD adapter):
//   A. Classification replay: a recorded sequence of the game callbacks the tracker sees
//      (PlayerObject::update deltas incl. GD half ticks and Click Between Frames splits,
//      handleButton, gamemode flag changes, m_playerSpeed, destroyPlayer outcomes, a TPS change) is
//      replayed through core/classify - the same functions src/Tracker.cpp calls - into
//      gprl.telemetry/1 events. Two replays (fresh state, and reused state after reset) must give
//      byte-identical canonical bodies, and the values must equal the ones derived by hand.
//   B. Solver replay: the replayed inputs become an InputSchedule; LocalWindowSolver and
//      HoldRangeSolver run over the SYNTHETIC oracle twice (fresh oracle, and the same oracle after
//      reset) and must produce identical results AND identical trial sequences.
//   C. The recorded nine-input schedule of tests/fixtures/solver/determinism-local-window.json:
//      every case twice, identical run to run, and equal to the fixture's hand-derived `analytic`
//      block within the search resolution. Each case's `golden` block pins the exact solver output
//      trial by trial (the shape tests/fixture-checks/solver.mjs validates): when the fixture
//      carries one, this run must reproduce it byte for byte (canonical JSON); `--write` produces it.
//   D. v0.7.0 (docs/TIMING_SOLVER_V2.md §5, AUDIT §4, §17) on the kinematic DEV FIXTURE physics
//      (tests/kinematic_oracle.hpp, not GD):
//        kinematicRestore  capture a snapshot at step k, restore it: the state is identical bit for
//                          bit, the restored run equals the uninterrupted run, and a local window
//                          solved from the snapshot equals the one solved from the level start
//        replayOriginal    restore + the ORIGINAL inputs reproduce the recorded trajectory exactly
//                          (the lockstep control rule); a PERTURBED restore is detected at its first
//                          step and the timing becomes state_replay_failed (control_mismatch)
//        v2 pipeline       the whole v2 path (PassPlanner + SAPlanner + both event builders) twice
//                          -> byte-identical canonical timing_window / timing_result events
// The recording in A is SYNTHETIC (hand-written in the shape of a CBF session): nothing has run
// inside Geometry Dash yet, so no real callback log exists. Replaying a real log through the GD
// adapter (GdOracle, hidden clone vs the real player) is the Phase 3 test tests/README.md lists.
//
// Usage: determinism_tests.exe <repo root> [--fixture <path>] [--write <path>]
//   <repo root>        D:\GPRL (default D:/GPRL)
//   --fixture <path>   read the C fixture from <path> instead of the repo copy
//   --write <path>     write the C fixture with every case's `golden` (re)generated to <path>
//                      (only when every check passed; tests/run_tests.ps1 -Write targets the repo
//                      fixture in place). Existing goldens are not compared in this mode.
#include "test_util.hpp"
#include "kinematic_oracle.hpp"
#include "synthetic_oracle.hpp"

#include "../core/classify.hpp"
#include "../core/crypto.hpp"
#include "../core/json.hpp"
#include "../core/solver/local_window.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/sequence_adjusted.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/window_event.hpp"
#include "../core/telemetry.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace gprl;
using namespace gprl::telemetry;
using gprl::test::SyntheticOracle;

namespace {

// ---- A. the recorded callback log ----

struct Raw {
    enum class Kind { Update, Button, Modes, PlayerSpeed, Destroy, Tps } kind = Kind::Update;
    int64_t tick = 0;      // m_currentProgress / 2 when the callback ran
    double dt = 0.0;       // Update: PlayerObject::update(dt) of player 1 (1/60 s units)
    bool down = false;     // Button
    int button = 1;
    bool isPlayer1 = true;
    classify::ModeFlags modes;   // Modes (after a toggle*Mode)
    int portalId = 0;
    double speed = 0.9;          // PlayerSpeed (a state sample reads m_playerSpeed)
    classify::DestroyFacts destroy;
    bool tpsBypass = false;      // Tps (attempt start re-reads the menus)
    double tps = 240.0;
};

Raw update(int64_t tick, double dt) { Raw r; r.kind = Raw::Kind::Update; r.tick = tick; r.dt = dt; return r; }
Raw button(int64_t tick, bool down, int b = 1, bool p1 = true) {
    Raw r; r.kind = Raw::Kind::Button; r.tick = tick; r.down = down; r.button = b; r.isPlayer1 = p1; return r;
}
Raw modes(int64_t tick, classify::ModeFlags f, int portal) { Raw r; r.kind = Raw::Kind::Modes; r.tick = tick; r.modes = f; r.portalId = portal; return r; }
Raw speed(int64_t tick, double v) { Raw r; r.kind = Raw::Kind::PlayerSpeed; r.tick = tick; r.speed = v; return r; }
Raw destroy(int64_t tick, bool deadAfter, int slot = 1, bool spike = false) {
    Raw r; r.kind = Raw::Kind::Destroy; r.tick = tick; r.destroy.attemptOpen = true; r.destroy.wasDeadBefore = false;
    r.destroy.playerSlot = slot; r.destroy.anticheatSpike = spike; r.destroy.deadAfter = deadAfter; return r;
}

/// SYNTHETIC recording: cube at normal speed, a whole-tick click, a GD half-tick click, a ship
/// portal, CBF-split clicks, a speed portal, a noclip-swallowed death, the anti-cheat spike, an
/// unrecorded button, player-2 inputs, then a real death.
std::vector<Raw> recording() {
    classify::ModeFlags ship;
    ship.ship = true;
    std::vector<Raw> r = {
        update(0, 0.25), update(1, 0.25),
        button(2, true), update(2, 0.25),                       // on the tick boundary: 0
        update(3, 0.125), button(3, false), update(3, 0.125),   // GD half tick: 0.5
        update(4, 0.25), modes(5, ship, 13), update(5, 0.25),   // ship portal (object 13)
        update(6, 0.1), button(6, true), update(6, 0.15),       // CBF split at 0.4
        update(7, 0.03), button(7, false), update(7, 0.2), button(7, true), update(7, 0.02),   // 0.12, 0.92
        speed(8, static_cast<double>(1.1f)), update(8, 0.25),   // speed portal: fast
        destroy(9, false),                                      // noclip swallowed a death: would-be
        destroy(9, false, 1, true),                             // GD anti-cheat spike: ignored
        button(9, true, 4),                                     // not a GPRL button: dropped
        update(9, 0.0625), button(9, true, 1, false), update(9, 0.1875),   // player 2 press at 0.25
        update(10, 0.25), button(11, false, 1, false),          // player 2 release, whole tick
        update(11, 0.25), destroy(12, true),                    // the real death ends the attempt
    };
    return r;
}

/// The tracker's pipeline over plain values: one attempt, events with seq / t / tick like
/// Tracker.cpp (t = tick / 240 here; the tracker reads m_levelTime).
Batch replay(std::vector<Raw> const& log, classify::SubTickClock& clock) {
    Batch b;
    b.sessionId = "determinism";
    b.seq = 0;
    b.nonce = "n";
    b.clientBuild = "gprl-geode host-test";
    int64_t seq = 0;
    std::string attempt = "det-a1";
    auto make = [&](int64_t tick) {
        Event e;
        e.seq = seq++;
        e.tick = tick;
        e.t = static_cast<double>(tick) / kTicksPerSecond;
        e.attemptId = attempt;
        return e;
    };
    clock.reset(classify::tickDtFor(false, 240.0));
    Gamemode mode = Gamemode::Cube;
    Speed spd = Speed::Normal;
    bool open = true;
    bool noclipSeen = false;
    {
        Event e = make(0);
        e.payload = AttemptStartPayload{};
        b.events.push_back(e);
    }
    for (auto const& r : log) {
        if (!open) break;
        switch (r.kind) {
            case Raw::Kind::Update: clock.onPlayerUpdate(r.dt); break;
            case Raw::Kind::Button: {
                auto in = classify::classifyButton(r.down, r.button, r.isPlayer1);
                if (!in) break;
                Event e = make(r.tick);
                e.payload = InputPayload{in->player, in->button, in->down, clock.fraction()};
                b.events.push_back(e);
                break;
            }
            case Raw::Kind::Modes: {
                Gamemode now = classify::gamemodeFromFlags(r.modes);
                if (now == mode) break;
                Event e = make(r.tick);
                e.payload = GamemodeChangePayload{1, mode, now, r.portalId};
                mode = now;
                b.events.push_back(e);
                break;
            }
            case Raw::Kind::PlayerSpeed: {
                spd = classify::speedFromMultiplier(r.speed);
                Event e = make(r.tick);
                StateSamplePayload s;
                s.state.tick = r.tick;
                s.state.levelTime = e.t;
                s.state.subTick = clock.fraction();
                s.state.gamemode = mode;
                s.state.speed = spd;
                s.state.mini = classify::isMini(1.0);
                e.payload = s;
                b.events.push_back(e);
                break;
            }
            case Raw::Kind::Destroy: {
                auto verdict = classify::classifyDestroy(r.destroy);
                if (verdict == classify::DestroyVerdict::Ignore) break;
                Event e = make(r.tick);
                e.payload = DeathPayload{50.0, 100.0 + static_cast<double>(r.tick), 8, verdict == classify::DestroyVerdict::WouldBeDeath};
                b.events.push_back(e);
                if (verdict == classify::DestroyVerdict::WouldBeDeath) {
                    noclipSeen = true;
                    break;
                }
                Event end = make(r.tick);
                AttemptEndPayload p{AttemptEndReason::Death, 50.0, false, !noclipSeen};
                p.noclipSeen = noclipSeen;
                end.payload = p;
                b.events.push_back(end);
                open = false;
                break;
            }
            case Raw::Kind::Tps: clock.reset(classify::tickDtFor(r.tpsBypass, r.tps)); break;
        }
    }
    return b;
}

void testClassificationReplay() {
    SECTION("A. replaying the recorded callback log twice gives byte-identical telemetry");
    auto log = recording();
    classify::SubTickClock fresh1, fresh2, reused;
    Batch a = replay(log, fresh1);
    Batch b = replay(log, fresh2);
    // reused state: dirty the clock first, the attempt-start reset must clear it
    reused.onPlayerUpdate(0.1);
    Batch c = replay(log, reused);
    std::string ca = canonicalBody(a), cb = canonicalBody(b), cc = canonicalBody(c);
    CHECK(ca == cb);
    CHECK(ca == cc);
    CHECK(crypto::toHex(crypto::sha256(ca)) == crypto::toHex(crypto::sha256(cc)));
    std::string err;
    CHECK_MSG(validateBatch(a, &err), err);
    CHECK_MSG(checkBatchInvariants(a, -1, {}, &err), err);

    SECTION("A. the replayed values are the hand-derived ones");
    struct ExpectedInput {
        int player;
        bool down;
        double subTick;
        int64_t tick;
    };
    std::vector<ExpectedInput> const expectedInputs = {
        {1, true, 0.0, 2}, {1, false, 0.5, 3}, {1, true, 0.4, 6}, {1, false, 0.12, 7}, {1, true, 0.92, 7}, {2, true, 0.25, 9}, {2, false, 0.0, 11},
    };
    std::vector<ExpectedInput> got;
    std::vector<Gamemode> modesSeen;
    int wouldBe = 0, deaths = 0, samples = 0;
    std::optional<AttemptEndPayload> end;
    for (auto const& e : a.events) {
        if (auto* in = std::get_if<InputPayload>(&e.payload)) got.push_back({in->player, in->down, in->tSubTick, e.tick});
        if (auto* g = std::get_if<GamemodeChangePayload>(&e.payload)) {
            modesSeen.push_back(g->to);
            CHECK(g->from == Gamemode::Cube && g->portalObjectId == 13);
        }
        if (auto* s = std::get_if<StateSamplePayload>(&e.payload)) {
            ++samples;
            CHECK(s->state.speed == Speed::Fast && s->state.gamemode == Gamemode::Ship && !s->state.mini);
        }
        if (auto* d = std::get_if<DeathPayload>(&e.payload)) (d->wouldBe ? wouldBe : deaths) += 1;
        if (auto* p = std::get_if<AttemptEndPayload>(&e.payload)) end = *p;
    }
    CHECK_MSG(got.size() == expectedInputs.size(), std::to_string(got.size()) + " inputs");
    for (size_t i = 0; i < got.size() && i < expectedInputs.size(); ++i) {
        auto const& x = expectedInputs[i];
        std::string what = "input " + std::to_string(i);
        CHECK_MSG(got[i].player == x.player && got[i].down == x.down && got[i].tick == x.tick, what);
        CHECK_MSG(std::fabs(got[i].subTick - x.subTick) < 1e-9, what + " tSubTick " + std::to_string(got[i].subTick));
    }
    CHECK((modesSeen == std::vector<Gamemode>{Gamemode::Ship}));
    CHECK(samples == 1);
    CHECK(wouldBe == 1);   // the anti-cheat spike was not counted
    CHECK(deaths == 1);
    CHECK(end.has_value() && end->reason == AttemptEndReason::Death && end->noclipSeen && *end->noclipSeen && !end->legit);
    CHECK(a.events.size() == 1 + 7 + 1 + 1 + 2 + 1);   // start, inputs, gamemode, sample, deaths, end

    SECTION("A. a different recording gives a different body (the comparison is not vacuous)");
    auto other = log;
    other[4].dt = 0.1;   // the first half of the GD half tick shrinks: that release reads 0.4, not 0.5
    classify::SubTickClock fresh3;
    CHECK(canonicalBody(replay(other, fresh3)) != ca);
}

// ---- B. solver replay over the synthetic oracle ----

bool sameNumber(double x, double y) { return (std::isnan(x) && std::isnan(y)) || x == y; }

json::Value toJson(solver::BoundaryResult const& r) {
    json::Value o = json::Value::object();
    o.set("valid", r.valid);
    o.set("limitMs", r.limitMs);
    o.set("blocked", r.blocked);
    o.set("bounded", r.bounded);
    o.set("passShiftMs", r.passShiftMs);
    o.set("failShiftMs", r.failShiftMs);   // NaN prints as null
    o.set("bracketMs", r.bracketMs);
    o.set("nonMonotonic", r.nonMonotonic);
    o.set("budgetExhausted", r.budgetExhausted);
    o.set("trialCount", r.trialCount);
    json::Value trials = json::Value::array();
    for (auto const& t : r.trials) {
        json::Value to = json::Value::object();
        to.set("index", t.index);
        to.set("shiftMs", t.shiftMs);
        to.set("tMs", t.tMs);
        to.set("pass", t.pass);
        to.set("phase", solver::name(t.phase));
        to.set("outcome", solver::name(t.outcome.kind));
        trials.push(std::move(to));
    }
    o.set("trials", std::move(trials));
    json::Value debug = json::Value::array();
    for (auto const& d : r.debug) debug.push(json::Value(d));
    o.set("debug", std::move(debug));
    return o;
}

std::string fingerprintOf(solver::WindowResult const& w) {
    json::Value o = json::Value::object();
    o.set("solverVersion", w.solverVersion);
    o.set("valid", w.valid);
    o.set("kind", name(w.kind));
    o.set("actualMs", w.actualMs);
    o.set("earliestMs", w.earliestMs);
    o.set("latestMs", w.latestMs);
    o.set("earliestFailMs", w.earliestFailMs);
    o.set("latestFailMs", w.latestFailMs);
    o.set("boundedEarly", w.boundedEarly);
    o.set("boundedLate", w.boundedLate);
    o.set("blockedEarly", w.blockedEarly);
    o.set("blockedLate", w.blockedLate);
    o.set("resolutionMs", w.resolutionMs);
    o.set("trials", w.trials);
    o.set("early", toJson(w.early));
    o.set("late", toJson(w.late));
    json::Value debug = json::Value::array();
    for (auto const& d : w.debug) debug.push(json::Value(d));
    o.set("debug", std::move(debug));
    return json::canonical(o);
}

void testSolverReplay() {
    SECTION("B. the replayed inputs as a schedule: local windows are identical run to run");
    classify::SubTickClock clock;
    Batch a = replay(recording(), clock);
    solver::InputSchedule schedule;
    for (auto const& e : a.events) {
        if (auto* in = std::get_if<InputPayload>(&e.payload)) {
            schedule.inputs.push_back({(static_cast<double>(e.tick) + in->tSubTick) * kTickMs, in->player, in->button, in->down});
        }
    }
    CHECK(schedule.inputs.size() == 7);
    solver::LocalWindowConfig cfg;
    cfg.search.resolutionMs = 0.05;
    int compared = 0;
    for (size_t i = 0; i < schedule.inputs.size(); ++i) {
        double actual = schedule.inputs[i].tMs;
        // a different, asymmetric (sub-frame for some inputs) pass interval per input
        std::vector<SyntheticOracle::Interval> pass = {{actual - (1.1 + 0.7 * static_cast<double>(i)), actual + (2.9 - 0.35 * static_cast<double>(i))}};
        SyntheticOracle o1(pass, i, 0.0), o2(pass, i, 0.0);
        solver::LocalWindowSolver s1(o1, cfg), s2(o2, cfg);
        auto w1 = s1.solve(0, schedule, i);
        auto w2 = s2.solve(0, schedule, i);
        std::string f1 = fingerprintOf(w1), f2 = fingerprintOf(w2);
        CHECK_MSG(f1 == f2, "input " + std::to_string(i));
        CHECK_MSG(o1.trialTimes() == o2.trialTimes(), "trial sequence of input " + std::to_string(i));
        CHECK(sameNumber(w1.earliestFailMs, w2.earliestFailMs) && sameNumber(w1.latestFailMs, w2.latestFailMs));
        // the same oracle object again after reset: no hidden state carried between solves
        o1.reset();
        auto w3 = s1.solve(0, schedule, i);
        CHECK_MSG(fingerprintOf(w3) == f1, "reused oracle, input " + std::to_string(i));
        CHECK_MSG(o1.trialTimes() == o2.trialTimes(), "reused oracle trial sequence, input " + std::to_string(i));
        CHECK_MSG(w1.valid, "input " + std::to_string(i) + " " + w1.invalidReason);
        ++compared;
    }
    CHECK(compared == 7);

    SECTION("B. hold ranges of a replayed press/release pair are identical run to run");
    // player 1: press at index 2 (tick 6 + 0.4), release at index 3 (tick 7 + 0.12)
    double press = schedule.inputs[2].tMs, release = schedule.inputs[3].tMs;
    std::vector<SyntheticOracle::Interval> pass = {{release - 2.0, release + 1.5}};
    SyntheticOracle o1(pass, 3, 0.0), o2(pass, 3, 0.0);
    solver::HoldRangeSolver h1(o1, cfg), h2(o2, cfg);
    auto r1 = h1.solveMovingRelease(0, schedule, 2, 3);
    auto r2 = h2.solveMovingRelease(0, schedule, 2, 3);
    CHECK(r1.valid == r2.valid);
    CHECK(sameNumber(r1.minHoldMs, r2.minHoldMs) && sameNumber(r1.maxHoldMs, r2.maxHoldMs) && r1.trials == r2.trials);
    CHECK(r1.boundedMin == r2.boundedMin && r1.boundedMax == r2.boundedMax);
    CHECK(fingerprintOf(r1.window) == fingerprintOf(r2.window));
    CHECK(o1.trialTimes() == o2.trialTimes());
    CHECK_NEAR(r1.actualHoldMs, release - press, 1e-9);
}

// ---- C. the recorded schedule of tests/fixtures/solver/determinism-local-window.json ----

std::string g_root;
std::string g_fixturePath;   // --fixture (empty = the repo copy)
std::string g_writePath;     // --write (empty = compare mode)

/// One side of a golden window: every number the search produced, and every trial in order. No
/// debug prose (it is explanation, not output). NaN fail shifts print as null.
json::Value goldenSide(solver::BoundaryResult const& r) {
    json::Value o = json::Value::object();
    o.set("valid", r.valid);
    o.set("limitMs", r.limitMs);
    o.set("blocked", r.blocked);
    o.set("bounded", r.bounded);
    o.set("passShiftMs", r.passShiftMs);
    o.set("failShiftMs", r.failShiftMs);
    o.set("bracketMs", r.bracketMs);
    o.set("nonMonotonic", r.nonMonotonic);
    o.set("budgetExhausted", r.budgetExhausted);
    json::Value islands = json::Value::array();
    for (auto const& is : r.islands) {
        json::Value io = json::Value::object();
        io.set("fromShiftMs", is.fromShiftMs);
        io.set("toShiftMs", is.toShiftMs);
        islands.push(std::move(io));
    }
    o.set("islands", std::move(islands));
    o.set("trialCount", r.trialCount);
    json::Value trials = json::Value::array();
    for (auto const& t : r.trials) {
        json::Value to = json::Value::object();
        to.set("index", t.index);
        to.set("shiftMs", t.shiftMs);
        to.set("tMs", t.tMs);
        to.set("phase", solver::name(t.phase));
        to.set("pass", t.pass);
        to.set("outcome", solver::name(t.outcome.kind));
        trials.push(std::move(to));
    }
    o.set("trials", std::move(trials));
    return o;
}

/// The golden block of a local-window case (field names = tests/fixture-checks/solver.mjs).
json::Value goldenWindow(solver::WindowResult const& w) {
    json::Value o = json::Value::object();
    o.set("solverVersion", w.solverVersion);
    o.set("valid", w.valid);
    o.set("kind", name(w.kind));
    o.set("actualMs", w.actualMs);
    o.set("earliestMs", w.earliestMs);
    o.set("latestMs", w.latestMs);
    o.set("earliestFailMs", w.earliestFailMs);
    o.set("latestFailMs", w.latestFailMs);
    o.set("boundedEarly", w.boundedEarly);
    o.set("boundedLate", w.boundedLate);
    o.set("blockedEarly", w.blockedEarly);
    o.set("blockedLate", w.blockedLate);
    o.set("nonMonotonic", w.nonMonotonic);
    o.set("budgetExhausted", w.budgetExhausted);
    o.set("resolutionMs", w.resolutionMs);
    o.set("widthMs", w.widthMs());
    o.set("widthFrames240", w.widthFrames240());
    o.set("trials", w.trials);
    o.set("early", goldenSide(w.early));
    o.set("late", goldenSide(w.late));
    return o;
}

/// The golden block of a hold-range case: the hold numbers plus the moved input's window.
json::Value goldenHold(solver::HoldRangeResult const& h) {
    json::Value o = json::Value::object();
    o.set("solverVersion", h.solverVersion);
    o.set("valid", h.valid);
    o.set("movedRelease", h.movedRelease);
    o.set("pressMs", h.pressMs);
    o.set("releaseMs", h.releaseMs);
    o.set("actualHoldMs", h.actualHoldMs);
    o.set("minHoldMs", h.minHoldMs);
    o.set("maxHoldMs", h.maxHoldMs);
    o.set("boundedMin", h.boundedMin);
    o.set("boundedMax", h.boundedMax);
    o.set("resolutionMs", h.resolutionMs);
    o.set("trials", h.trials);
    o.set("window", goldenWindow(h.window));
    return o;
}

void testSolverFixture() {
    SECTION("C. fixture solver/determinism-local-window.json: analytic windows, identical run to run");
    std::string path = g_fixturePath.empty() ? g_root + "/tests/fixtures/solver/determinism-local-window.json" : g_fixturePath;
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    json::Value fx;
    json::ParseError pe;
    CHECK_MSG(json::parse(text, fx, &pe), pe.message);
    CHECK(fx.getString("kind") == "gprl.solver/determinism");

    auto const& c = fx["config"];
    solver::LocalWindowConfig cfg;
    cfg.search.resolutionMs = c.getNumber("resolutionMs");
    cfg.search.maxTrials = static_cast<int>(c.getInt("maxTrials"));
    cfg.search.maxShiftMs = c.getNumber("maxShiftTicks") * kTickMs;
    cfg.search.coarseStepTicks = static_cast<int>(c.getInt("coarseStepTicks"));
    cfg.search.islandScanSteps = static_cast<int>(c.getInt("islandScanSteps"));
    cfg.horizonSeconds = c.getNumber("horizonSeconds");
    cfg.neighbourMarginMs = c.getNumber("neighbourMarginMs");

    solver::InputSchedule schedule;
    for (auto const& in : fx["schedule"].asArray()) {
        Button b = Button::Jump;
        CHECK_MSG(parse(in.getString("button"), b), in.getString("button"));
        schedule.inputs.push_back({in.getNumber("tMs"), static_cast<int>(in.getInt("player")), b, in.getBool("down")});
    }
    CHECK(schedule.inputs.size() == 9);

    double tol = cfg.search.resolutionMs + 1e-9;
    int cases = 0;
    int goldensCompared = 0;
    int goldensMatched = 0;
    std::vector<json::Value> goldens;
    for (auto const& cs : fx["cases"].asArray()) {
        std::string what = cs.getString("name");
        std::vector<SyntheticOracle::Interval> pass;
        for (auto const& iv : cs["passIntervals"].asArray()) pass.push_back({iv[0].asNumber(), iv[1].asNumber()});
        double history = cs.getNumber("historyStartMs");
        auto const& x = cs["analytic"];
        solver::WindowResult w1, w2;
        std::vector<double> t1, t2;
        json::Value golden;
        if (auto* hold = cs.find("hold")) {
            size_t press = static_cast<size_t>(hold->getInt("pressIndex"));
            size_t release = static_cast<size_t>(hold->getInt("releaseIndex"));
            bool moveRelease = hold->getBool("moveRelease");
            size_t moving = moveRelease ? release : press;
            SyntheticOracle o1(pass, moving, history), o2(pass, moving, history);
            solver::HoldRangeSolver h1(o1, cfg), h2(o2, cfg);
            auto r1 = moveRelease ? h1.solveMovingRelease(0, schedule, press, release) : h1.solveMovingPress(0, schedule, press, release);
            auto r2 = moveRelease ? h2.solveMovingRelease(0, schedule, press, release) : h2.solveMovingPress(0, schedule, press, release);
            CHECK_MSG(r1.valid && r2.valid, what);
            CHECK_MSG(sameNumber(r1.minHoldMs, r2.minHoldMs) && sameNumber(r1.maxHoldMs, r2.maxHoldMs), what + " run to run");
            CHECK_MSG(std::fabs(r1.minHoldMs - x.getNumber("minHoldMs")) <= tol, what + " minHoldMs " + std::to_string(r1.minHoldMs));
            CHECK_MSG(std::fabs(r1.maxHoldMs - x.getNumber("maxHoldMs")) <= tol, what + " maxHoldMs " + std::to_string(r1.maxHoldMs));
            CHECK_MSG(r1.boundedMin == x.getBool("boundedMin") && r1.boundedMax == x.getBool("boundedMax"), what + " hold bounds");
            CHECK_MSG(json::canonical(goldenHold(r1)) == json::canonical(goldenHold(r2)), what + " hold golden run to run");
            golden = goldenHold(r1);
            w1 = r1.window;
            w2 = r2.window;
            t1 = o1.trialTimes();
            t2 = o2.trialTimes();
        }
        else {
            size_t moving = static_cast<size_t>(cs.getInt("movingIndex"));
            SyntheticOracle o1(pass, moving, history), o2(pass, moving, history);
            solver::LocalWindowSolver s1(o1, cfg), s2(o2, cfg);
            w1 = s1.solve(0, schedule, moving);
            w2 = s2.solve(0, schedule, moving);
            t1 = o1.trialTimes();
            t2 = o2.trialTimes();
            golden = goldenWindow(w1);
        }
        // the pinned golden: this build must reproduce the stored solver output exactly
        if (g_writePath.empty()) {
            if (auto const* stored = cs.find("golden")) {
                std::string want = json::canonical(*stored), got = json::canonical(golden);
                CHECK_MSG(got == want, what + " golden differs from the fixture (regenerate with tests/run_tests.ps1 -Write only if the solver change is intended)");
                ++goldensCompared;
                if (got == want) ++goldensMatched;
            }
        }
        goldens.push_back(golden);
        // run to run: identical result and identical trial sequence
        CHECK_MSG(fingerprintOf(w1) == fingerprintOf(w2), what + " result run to run");
        CHECK_MSG(t1 == t2, what + " trial sequence run to run");
        // the hand-derived analytic expectation
        CHECK_MSG(w1.valid, what + " " + w1.invalidReason);
        CHECK_MSG(std::fabs(w1.earliestMs - x.getNumber("earliestMs")) <= tol, what + " earliestMs " + std::to_string(w1.earliestMs));
        CHECK_MSG(std::fabs(w1.latestMs - x.getNumber("latestMs")) <= tol, what + " latestMs " + std::to_string(w1.latestMs));
        CHECK_MSG(w1.boundedEarly == x.getBool("boundedEarly") && w1.boundedLate == x.getBool("boundedLate"), what + " bounded");
        CHECK_MSG(w1.blockedEarly == x.getBool("blockedEarly") && w1.blockedLate == x.getBool("blockedLate"), what + " blocked");
        CHECK_MSG(w1.nonMonotonic == x.getBool("nonMonotonic"), what + " nonMonotonic");
        int64_t islands = static_cast<int64_t>(w1.early.islands.size() + w1.late.islands.size());
        CHECK_MSG(islands == x.getInt("islandCount"), what + " islands " + std::to_string(islands));
        ++cases;
    }
    CHECK(cases == 7);

    if (g_writePath.empty()) {
        std::printf("  goldens: %d of %d cases compared, %d reproduced exactly\n", goldensCompared, cases, goldensMatched);
        if (goldensCompared < cases)
            std::printf("  NOTE: %d of %d cases carry no `golden` block (not compared); generate them with tests/run_tests.ps1 -Write\n",
                        cases - goldensCompared, cases);
        return;
    }
    // --write: only a fully passing run may pin its output
    if (gprl::test::g_failures != 0) {
        std::printf("  --write: NOT written, %d check(s) failed\n", gprl::test::g_failures);
        CHECK_MSG(false, "--write refused: fix the failures first");
        return;
    }
    auto* arr = fx.find("cases");
    for (size_t i = 0; arr && i < arr->asArray().size() && i < goldens.size(); ++i) arr->asArray()[i].set("golden", goldens[i]);
    std::ofstream out(g_writePath, std::ios::binary | std::ios::trunc);
    out << json::stringifyPretty(fx) << "\n";
    out.close();
    CHECK_MSG(static_cast<bool>(out), "--write failed: " + g_writePath);
    std::printf("  --write: %zu golden block(s) written to %s\n", goldens.size(), g_writePath.c_str());
}

// ---- D. v0.7.0 (docs/TIMING_SOLVER_V2.md §5; AUDIT §4, §17) on the kinematic DEV FIXTURE physics ----

namespace kin = gprl::test::kin;

gprl::solver::InputSchedule kinSchedule() {
    gprl::solver::InputSchedule s;
    for (auto [tick, down] : std::vector<std::pair<double, bool>>{{100, true}, {118, false}, {136, true}, {154, false}}) s.inputs.push_back({tick * kTickMs, 1, Button::Jump, down});
    return s;
}

/// A wave corridor around the schedule's free path with a tight spot at ticks 190-193 (DEV FIXTURE).
kin::World kinWorld(gprl::solver::InputSchedule const& s) {
    kin::World w;
    w.startMode = kin::Mode::Wave;
    auto path = kin::freePath(w, s, 330);
    kin::corridorAround(w, path, [](double x) { double k = x / 1.3; return (k >= 190 && k < 193) ? 3.0 : (k < 118 ? 15.0 : 11.0); },
                        [](double x) { double k = x / 1.3; return (k >= 190 && k < 193) ? 4.0 : (k < 136 ? 12.0 : 17.0); });
    return w;
}

void testKinematicRestore() {
    SECTION("D. kinematicRestore: snapshot at step k -> restore -> identical bit for bit; the resumed run equals the uninterrupted one; windows solved from a snapshot equal those from the start");
    using namespace gprl::solver;
    auto s = kinSchedule();
    kin::KinematicOracle o(kinWorld(s));
    auto full = o.run(s, 330);
    CHECK(full.size() == 330 && !full.back().dead);   // the reference run survives its own corridor
    for (int64_t k : {int64_t(1), int64_t(50), int64_t(99), int64_t(100), int64_t(101), int64_t(117), int64_t(150), int64_t(250)}) {
        kin::State snap = o.snapshotAt(s, k);
        CHECK(snap.tick == k);
        CHECK(snap.identical(full[static_cast<size_t>(k - 1)]));   // the capture is the recorded state after k ticks
        kin::State restored = snap;                                 // restore: every field, by value
        CHECK(restored.identical(snap));
        auto resumed = o.runFrom(restored, s, 330);
        CHECK(resumed.size() == full.size() - static_cast<size_t>(k));
        bool same = resumed.size() == full.size() - static_cast<size_t>(k);
        for (size_t i = 0; same && i < resumed.size(); ++i) same = resumed[i].identical(full[static_cast<size_t>(k) + i]);
        CHECK_MSG(same, "the run resumed from the snapshot at tick " + std::to_string(k) + " differs from the uninterrupted run");
    }
    // the engine spawns every shifted copy from a ring snapshot (SD §3.3): a local window solved
    // with all trials starting from a snapshot 20 ticks before the input equals the window solved
    // from the level start, trial by trial
    PlannerConfig pc;
    for (size_t i = 0; i < s.inputs.size(); ++i) {
        double t = s.inputs[i].tMs / kTickMs;
        double earlyLimit = i == 0 ? 10.0 : std::min(10.0, t - s.inputs[i - 1].tMs / kTickMs - pc.neighbourMarginFrames);
        auto solve = [&](std::optional<kin::State> base) {
            o.setBase(std::move(base));
            PassPlanner p(pc, earlyLimit);
            if (i + 1 < s.inputs.size()) p.setLateLimit(s.inputs[i + 1].tMs / kTickMs - t - pc.neighbourMarginFrames);
            o.setReference(s);
            runAgainstOracle(o, p, 0, s, i, 0.5 + 10.0 / 240.0);
            return p;
        };
        auto fromStart = solve(std::nullopt);
        auto fromSnapshot = solve(o.snapshotAt(s, static_cast<int64_t>(t) - 20));
        auto a = fromStart.result(s.inputs[i].tMs);
        auto b = fromSnapshot.result(s.inputs[i].tMs);
        CHECK(a.valid && b.valid);
        CHECK(a.earliestMs == b.earliestMs && a.latestMs == b.latestMs && a.resolutionMs == b.resolutionMs);
        CHECK_MSG(fromStart.describe() == fromSnapshot.describe(), fromStart.describe() + " vs " + fromSnapshot.describe());
    }
    o.setBase(std::nullopt);
}

void testReplayOriginal() {
    SECTION("D. replayOriginal: restore + the ORIGINAL inputs reproduce the recorded trajectory exactly; a perturbed restore is detected -> state_replay_failed");
    using namespace gprl::solver;
    auto s = kinSchedule();
    kin::KinematicOracle o(kinWorld(s));
    o.setReference(s);
    for (int64_t k : {int64_t(60), int64_t(99), int64_t(110), int64_t(118), int64_t(140)}) {
        CHECK_MSG(o.replayMismatchFrom(o.snapshotAt(s, k), 300) == -1, "an exact restore at tick " + std::to_string(k) + " must replay the recorded run exactly");
    }
    // one field of the restored state off by the smallest amount the physics carries: the control
    // rule (every step, no tolerance) sees it at the first replayed step
    struct Perturb {
        char const* what;
        void (*apply)(kin::State&);
    };
    const Perturb perturbs[] = {
        {"y + 1e-9", [](kin::State& x) { x.y += 1e-9; }},
        {"x + 1e-7", [](kin::State& x) { x.x += 1e-7; }},
        {"held button flipped", [](kin::State& x) { x.holding = !x.holding; }},
        {"speed 1 -> 1.0000001", [](kin::State& x) { x.speed = 1.0000001; }},
        {"mini toggled", [](kin::State& x) { x.mini = !x.mini; }},
    };
    for (auto const& p : perturbs) {
        kin::State bad = o.snapshotAt(s, 110);   // mid-hold: every field acts on the next step
        p.apply(bad);
        int64_t at = o.replayMismatchFrom(bad, 300);
        CHECK_MSG(at == 111, std::string(p.what) + ": first mismatch at tick " + std::to_string(at));
    }
    // the solver's rule for such a replay (§2.10, AUDIT §4, §16): no window is emitted; the input's
    // timing_result is state_replay_failed (control_mismatch) with stateReplayValid false
    auto st = status::statusOf({{status::Reason::ControlMismatch}}, {}, {}, {});
    CHECK(st.status == status::TimingStatus::StateReplayFailed);
    TimingResultContext ctx;
    ctx.inputSeq = 7;
    ctx.kind = InputKind::Press;
    ctx.attemptInputIndex = 3;
    ctx.eventT = 136.0 / 240.0;
    ctx.gamemode = Gamemode::Wave;
    ctx.cluster = {"d-a1:1", 3, cluster::Tri::Yes, cluster::Tri::Unknown};
    auto built = buildTimingResultEvent(ctx, LocalEvidence{}, nullptr, st, false);
    CHECK_MSG(built.ok, built.error);
    CHECK(built.payload.status == "state_replay_failed" && !built.payload.stateReplayValid && !built.payload.local && !built.payload.sequence);
    CHECK(built.payload.statusReasons.size() == 1 && built.payload.statusReasons[0] == "control_mismatch");
}

/// A synthetic wave step log (DEV FIXTURE physics) through the local PassPlanner, the SAPlanner
/// and both event builders; returns the canonical JSON of every timing_window and timing_result,
/// in input order.
std::vector<std::string> v2Pipeline() {
    using namespace gprl::solver;
    auto s = kinSchedule();
    kin::KinematicOracle o(kinWorld(s));
    std::vector<PassPlanner> locals;
    for (size_t i = 0; i < s.inputs.size(); ++i) {
        PlannerConfig pc;
        double t = s.inputs[i].tMs / kTickMs;
        PassPlanner p(pc, i == 0 ? 10.0 : std::min(10.0, t - s.inputs[i - 1].tMs / kTickMs - pc.neighbourMarginFrames));
        if (i + 1 < s.inputs.size()) p.setLateLimit(s.inputs[i + 1].tMs / kTickMs - t - pc.neighbourMarginFrames);
        o.setReference(s);
        runAgainstOracle(o, p, 0, s, i, 0.5 + 10.0 / 240.0);
        locals.push_back(p);
    }
    SARunInput in;
    in.members = {0, 1, 2, 3};
    for (auto const& p : locals) in.locals.push_back(&p);
    SAPlanner sa;
    runSAAgainstOracle(o, 0, s, in, sa);
    std::vector<std::string> out;
    cluster::ClusterTracker clusters;
    for (size_t i = 0; i < s.inputs.size(); ++i) clusters.setConnectedNext(static_cast<int>(i + 1), i + 1 < s.inputs.size() ? cluster::Tri::Yes : cluster::Tri::No);
    for (size_t i = 0; i < s.inputs.size(); ++i) {
        double eventT = s.inputs[i].tMs / 1000.0;
        auto w0 = locals[i].result(eventT * 1000.0);
        WindowEventContext wc;
        wc.inputSeq = static_cast<int64_t>(2 + i);
        wc.kind = s.inputs[i].down ? InputKind::Press : InputKind::Release;
        wc.actualMs = eventT * 1000.0;
        TimingFingerprint f;
        f.gamemode = Gamemode::Wave;
        auto tw = buildWindowEvent(w0, wc, f);
        auto r = sa.result(static_cast<int>(i));
        LocalEvidence ev;
        ev.window = &w0;
        ev.outcomes = &locals[i].outcomes();
        ev.frame = s.inputs[i].tMs / kTickMs;
        ev.nextFrame = i + 1 < s.inputs.size() ? s.inputs[i + 1].tMs / kTickMs : kNaN;
        ev.nextFollows = true;
        ev.horizonFrame = ev.frame + 130.0;
        TimingResultContext ctx;
        ctx.inputSeq = wc.inputSeq;
        ctx.kind = wc.kind;
        ctx.attemptInputIndex = static_cast<int>(i + 1);
        ctx.eventT = eventT;
        ctx.gamemode = Gamemode::Wave;
        ctx.cluster = clusters.ref("d-a1", static_cast<int>(i + 1));
        auto st = status::statusOf({}, localFacts(ev), saFacts(&r), {});
        auto tr = buildTimingResultEvent(ctx, ev, &r, st, true);
        Event a, b;
        a.t = b.t = eventT;
        a.tick = b.tick = static_cast<int64_t>(ev.frame) + 1;
        a.attemptId = b.attemptId = "d-a1";
        a.seq = static_cast<int64_t>(10 + 2 * i);
        b.seq = a.seq + 1;
        a.payload = tw.payload;
        b.payload = tr.payload;
        out.push_back(json::canonical(toJson(a)));
        out.push_back(json::canonical(toJson(b)));
    }
    return out;
}

void testV2Pipeline() {
    SECTION("D. v2 path (timeline + PassPlanner + SAPlanner + builders) twice -> byte-identical canonical events");
    auto first = v2Pipeline();
    auto second = v2Pipeline();
    CHECK(first.size() == 8);
    CHECK(first == second);
    for (auto const& e : first) CHECK(e.find("NaN") == std::string::npos && e.find("null,\"stop\":\"fail\"") == std::string::npos);
    CHECK(first.size() > 1 && first[1].find("\"kind\":\"timing_result\"") != std::string::npos);
    CHECK(first.size() > 1 && first[1].find("\"id\":\"d-a1:1\"") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    g_root = "D:/GPRL";
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--fixture" && i + 1 < argc) g_fixturePath = argv[++i];
        else if (a == "--write" && i + 1 < argc) g_writePath = argv[++i];
        else if (positional++ == 0) g_root = argv[i];
        else {
            std::printf("usage: determinism_tests <repo root> [--fixture <path>] [--write <path>] (unknown argument '%s')\n", argv[i]);
            return 2;
        }
    }
    testClassificationReplay();
    testSolverReplay();
    testSolverFixture();
    testKinematicRestore();
    testReplayOriginal();
    testV2Pipeline();
    return gprl::test::finish("determinism_tests");
}
