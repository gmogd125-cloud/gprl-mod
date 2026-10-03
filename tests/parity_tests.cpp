// core/solver/parity.hpp host tests (docs/SHIP_SOLVER.md §11.1; owner prompt 2026-10-03 §1): the
// replay of the recorded inputs must reproduce the real run before any window is measured; when
// it does not, the FIRST divergence is reported as (tick, field, real, replay, delta).
//
//   compare        exact states -> none; each field is found, in the fixed order; the tolerances
//   Record         keeps the first divergence only
//   exact replay   the fixture's replay of a snapshot matches the recorded run tick for tick;
//                  a perturbed snapshot diverges at once and the record names the field
//   the result     a failed replay builds a timing_result with status state_replay_failed, NO
//                  window, stateReplayValid false and the parity block (validated, round trip)
#include "test_util.hpp"
#include "kinematic_oracle.hpp"

#include "../core/json.hpp"
#include "../core/solver/parity.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_status.hpp"
#include "../core/telemetry.hpp"

#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::test::kin;
namespace par = gprl::solver::parity;

namespace {

par::State stateOf(State const& s) {
    par::State p;
    p.x = s.x;
    p.y = s.y;
    p.vy = s.vy;
    p.onGround = s.onGround;
    p.upsideDown = s.flipped;
    p.gamemode = static_cast<int>(s.mode);
    p.size = s.mini ? 0.6 : 1.0;
    p.speed = s.speed;
    p.held = s.holding ? 1 : 0;
    p.alive = !s.dead;
    return p;
}

void testCompare() {
    SECTION("compare: identical states -> no divergence; every field is reported with real / replay / delta, in the fixed order");
    par::State a;
    a.x = 100.0;
    a.y = 50.0;
    a.vy = -2.3;
    CHECK(!par::compare(a, a).any());
    {
        par::State b = a;
        b.x += 1.0469;
        b.y -= 0.5356;   // both differ: x comes first
        auto d = par::compare(a, b);
        CHECK(d.field == par::Field::X);
        CHECK_NEAR(d.real, 100.0, 1e-12);
        CHECK_NEAR(d.replay, 101.0469, 1e-12);
        CHECK_NEAR(d.delta, 1.0469, 1e-12);
        CHECK(std::string(par::name(d.field)) == "x");
    }
    {
        par::State b = a;
        b.y += 0.0005;   // inside the position tolerance
        CHECK(!par::compare(a, b).any());
        b.y += 0.002;
        CHECK(par::compare(a, b).field == par::Field::Y);
    }
    struct Case {
        par::Field field;
        void (*change)(par::State&);
    };
    Case const cases[] = {
        {par::Field::VelocityY, [](par::State& s) { s.vy += 0.01; }},
        {par::Field::OnGround, [](par::State& s) { s.onGround = !s.onGround; }},
        {par::Field::Gravity, [](par::State& s) { s.upsideDown = !s.upsideDown; }},
        {par::Field::Gamemode, [](par::State& s) { s.gamemode += 1; }},
        {par::Field::Dashing, [](par::State& s) { s.dashing = !s.dashing; }},
        {par::Field::OnSlope, [](par::State& s) { s.onSlope = !s.onSlope; }},
        {par::Field::Size, [](par::State& s) { s.size = 0.6; }},
        {par::Field::Speed, [](par::State& s) { s.speed = 1.1; }},
        {par::Field::Held, [](par::State& s) { s.held = 1; }},
        {par::Field::Rings, [](par::State& s) { s.rings = 2; }},
        {par::Field::LastX, [](par::State& s) { s.lastX += 1.0; }},
        {par::Field::LastY, [](par::State& s) { s.lastY += 1.0; }},
        {par::Field::Dual, [](par::State& s) { s.dual = !s.dual; }},
        {par::Field::Alive, [](par::State& s) { s.alive = false; }},
    };
    for (auto const& c : cases) {
        par::State b = a;
        c.change(b);
        auto d = par::compare(a, b);
        CHECK_MSG(d.field == c.field, par::name(c.field));
        CHECK(d.any() && std::string(par::name(d.field)) != "none");
        CHECK_NEAR(d.delta, d.replay - d.real, 1e-12);
    }
}

void testRecord() {
    SECTION("Record: valid until the first divergence, which it keeps (a later one never replaces it)");
    par::Record r;
    CHECK(r.valid);
    r.note(10.0, {});   // no divergence: still valid
    CHECK(r.valid);
    r.note(811.0, {par::Field::X, 19236.33, 19237.377, 1.047});
    CHECK(!r.valid && r.tick == 811.0 && r.first.field == par::Field::X);
    r.note(900.0, {par::Field::Y, 1.0, 2.0, 1.0});
    CHECK(r.tick == 811.0 && r.first.field == par::Field::X);
}

World shipWorld() {
    World w;
    w.startMode = Mode::Ship;
    w.half = 3.0;
    w.shipGravity = 0.03;
    w.floor.pts = {{-100.0, -3.0}, {5000.0, -3.0}};
    return w;
}

InputSchedule flight() {
    InputSchedule s;
    for (auto const& [tick, down] : std::vector<std::pair<double, bool>>{{100, true}, {106, false}, {116, true}, {122, false}, {132, true}, {138, false}})
        s.inputs.push_back({tick * kTickMs, 1, Button::Jump, down});
    return s;
}

void testExactReplay() {
    SECTION("#13 exact replay parity (prompt §1): a restored snapshot replays the recorded inputs tick for tick; a perturbed one diverges and the record names the field");
    auto s = flight();
    KinematicOracle o(shipWorld());
    o.setReference(s);
    auto recorded = o.run(s, 240);
    State base = o.snapshotAt(s, 110);
    CHECK(o.replayMismatchFrom(base, 240) == -1);   // exact everywhere: windows may be measured
    // the same comparison through parity::compare, tick by tick
    {
        auto replay = o.runFrom(base, s, 240);
        par::Record rec;
        for (auto const& st : replay) rec.note(static_cast<double>(st.tick), par::compare(stateOf(recorded[static_cast<size_t>(st.tick - 1)]), stateOf(st)));
        CHECK(rec.valid);
    }
    // a snapshot that is NOT the recorded state (0.5 units higher): the replay leaves the real run
    State bad = base;
    bad.y += 0.5;
    int64_t const first = o.replayMismatchFrom(bad, 240);
    CHECK(first == base.tick + 1);
    auto replay = o.runFrom(bad, s, 240);
    par::Record rec;
    for (auto const& st : replay) rec.note(static_cast<double>(st.tick), par::compare(stateOf(recorded[static_cast<size_t>(st.tick - 1)]), stateOf(st)));
    CHECK(!rec.valid);
    CHECK(rec.tick == static_cast<double>(first));
    CHECK(rec.first.field == par::Field::Y);
    CHECK_NEAR(rec.first.delta, 0.5, 1e-9);
    std::printf("  first divergence: tick %.0f field %s real %.4f replay %.4f delta %+.4f\n", rec.tick, par::name(rec.first.field), rec.first.real, rec.first.replay, rec.first.delta);

    SECTION("STATE_REPLAY_FAILED: the result carries no window, no estimate, stateReplayValid false and the parity block");
    status::JobFacts jf;
    jf.ended = {status::Reason::ControlMismatch};
    auto st = status::statusOf(jf, status::LocalFacts{}, status::SAFacts{}, status::ContextFacts{});
    CHECK(st.status == status::TimingStatus::StateReplayFailed);
    TimingResultContext ctx;
    ctx.inputSeq = 12;
    ctx.kind = InputKind::Press;
    ctx.attemptInputIndex = 3;
    ctx.eventT = s.inputs[2].tMs / 1000.0;
    ctx.gamemode = Gamemode::Ship;
    ctx.cluster = {"a1:3", 1, cluster::Tri::Unknown, cluster::Tri::Unknown};
    ctx.parity = rec;
    LocalEvidence none;   // the job has no window
    auto built = buildTimingResultEvent(ctx, none, nullptr, st, false);
    CHECK_MSG(built.ok, built.error);
    auto const& p = built.payload;
    CHECK(p.status == "state_replay_failed" && !p.stateReplayValid);
    CHECK(!p.local && !p.sequence && !p.pair && !p.hold);
    CHECK(p.parity.has_value());
    if (p.parity) {
        CHECK(p.parity->field == "y" && p.parity->tick == rec.tick);
        CHECK_NEAR(p.parity->delta, 0.5, 1e-9);
    }
    std::string err;
    CHECK_MSG(telemetry::validateTimingResult(p, "", &err), err);
    // a parity block on a result whose replay was valid is refused
    {
        auto bad2 = p;
        bad2.status = "unresolved";
        bad2.statusReasons = {"no_shift_tested"};
        bad2.stateReplayValid = true;
        CHECK(!telemetry::validateTimingResult(bad2, "", &err));
    }
    {
        auto bad3 = p;
        bad3.parity->field = "colour";
        CHECK(!telemetry::validateTimingResult(bad3, "", &err));
    }
    // round trip
    telemetry::Event ev;
    ev.t = ctx.eventT;
    ev.tick = 116;
    ev.attemptId = "a1";
    ev.payload = p;
    auto text = json::stringify(telemetry::toJson(ev));
    json::Value v;
    CHECK(json::parse(text, v));
    telemetry::Event back;
    std::string perr;
    CHECK_MSG(telemetry::fromJson(v, back, &perr), perr);
    CHECK(std::get<telemetry::TimingResultPayload>(back.payload) == p);
    // a result whose replay was exact carries no parity block
    {
        TimingResultContext ok = ctx;
        ok.parity = {};
        status::JobFacts cut;
        cut.ended = {status::Reason::CutByRestart};
        auto st2 = status::statusOf(cut, status::LocalFacts{}, status::SAFacts{}, status::ContextFacts{});
        auto b2 = buildTimingResultEvent(ok, none, nullptr, st2, true);
        CHECK_MSG(b2.ok, b2.error);
        CHECK(!b2.payload.parity.has_value());
    }
}

}  // namespace

int main() {
    testCompare();
    testRecord();
    testExactReplay();
    return gprl::test::finish("parity_tests");
}
