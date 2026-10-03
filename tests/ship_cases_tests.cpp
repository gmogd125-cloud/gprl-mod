// The SHIP CONTROL cases of "controls/1" (docs/SHIP_SOLVER.md §11.6; owner prompt "SHIP COUNTING /
// COMPENSATED TIMING WINDOW REWRITE" §15, 2026-10-03) on the kinematic DEV FIXTURE physics
// (tests/kinematic_oracle.hpp, NOT Geometry Dash). Every expectation is a RELATION (wider /
// narrower / equal / decided / not decided), never a figure. The prompt's 15 cases:
//
//    1 wide corridor                          ship_pack_tests #1
//    2 high-CPS corridor                      HERE: frozen windows of ~1 frame, compensated windows many times wider
//    3 fixed hold-duration challenge          ship_pack_tests #3; HERE (hold vs phase): duration tight, phase wide
//    4 tight press / forgiving release        HERE: the press stays tight with compensation allowed (a real tight input stays tight)
//    5 forgiving press / tight release        HERE
//    6 mini Ship                              ship_pack_tests #5
//    7 gravity flip                           ship_pack_tests #6
//    8 speed change                           ship_pack_tests #7
//    9 CBF / subframe                         ship_pack_tests #8
//   10 true isolated Ship frame-perfect       ship_pack_tests #12
//   11 compensation-required case             ship_pack_tests #11, compensation_tests
//   12 survives-but-no-rejoin                 HERE: never a pass of the compensated window (`sa_survives_no_rejoin`)
//   13 exact replay failure                   parity_tests (state_replay_failed, no window, the parity block)
//   14 dense correlated cluster               HERE: one measured cluster, effective members < members
//   15 SUPERHATEMEWORLD-like geometry         HERE: the owner's input rhythm of the 93.3-97.8 % section in a fixture corridor;
//                                             printed next to the human counts 17 / 16 / 19 / 15 / 12 as COMPARISON DATA only
//
// plus: the control card (core/solver/control_card.hpp: the debug view of one control and the
// explicit output fields) and the budget fail-safe (a search that ran out of trials is "not
// decided", never a narrow window).
//
// "frames" = frames AVAILABLE: the window from edge to edge in 240 Hz ticks.
#include "ship_worlds.hpp"

#include "../core/solver/cluster.hpp"
#include "../core/solver/control_card.hpp"
#include "../core/solver/ship_control.hpp"

#include <map>
#include <memory>

using namespace gprl;
using namespace gprl::solver;
using namespace gprl::solver::comp;
using namespace gprl::test::kin;
using namespace gprl::test::ship;

namespace {

// =============================================================================================
// One measured stretch of a schedule: every input's local + compensated window, each control's
// phase search, the measured clusters and the timing_result payload of every input - built the
// way the engine builds them (CloneEngine emitResult), so the cards below show what would be sent.

struct Measured {
    std::vector<size_t> inputs;
    std::vector<std::unique_ptr<Member>> members;                    // one per input (stable addresses)
    std::map<size_t, std::unique_ptr<Phase>> phases;                 // by RELEASE index
    std::map<size_t, telemetry::TimingResultPayload> payloads;       // by input index
    cluster::ClusterTracker clusters;
    int trials = 0, maxTrials = 0;

    Member const& member(size_t input) const {
        for (size_t k = 0; k < inputs.size(); ++k) {
            if (inputs[k] == input) return *members[k];
        }
        return *members.front();
    }
};

Measured measure(KinematicOracle& o, InputSchedule const& s, std::vector<size_t> const& inputs, CompConfig cfg = gprl::test::fixtureComp()) {
    Measured r;
    r.inputs = inputs;
    std::vector<control::LoggedInput> log;
    for (size_t i = 0; i < s.inputs.size(); ++i) log.push_back({static_cast<uint32_t>(i + 1), frameOf(s, i), s.inputs[i].down, false});
    for (size_t i : inputs) {
        auto solved = solveAll(o, s, {i}, false, cfg);
        r.members.push_back(std::make_unique<Member>(std::move(solved[0])));
        Member& m = *r.members.back();
        r.trials += m.trials;
        r.maxTrials = std::max(r.maxTrials, m.trials);
        LocalEvidence ev;
        ev.window = &m.window;
        ev.outcomes = &m.local.outcomes();
        ev.frame = frameOf(s, i);
        ev.nextFrame = i + 1 < s.inputs.size() ? frameOf(s, i + 1) : kNaN;
        ev.nextFollows = true;
        ev.horizonFrame = ev.frame + kH * 240.0;
        cluster::ConnectInput ci;
        ci.haveOutcomes = m.window.valid;
        ci.frame = ev.frame;
        ci.horizonFrame = ev.horizonFrame;
        ci.nextFrame = ev.nextFrame;
        ci.outcomes = ev.outcomes;
        r.clusters.setConnectedNext(static_cast<int>(i) + 1, cluster::connectedNext(ci).value);
        TimingResultContext ctx;
        ctx.inputSeq = 100 + static_cast<int64_t>(i);
        ctx.kind = s.inputs[i].down ? InputKind::Press : InputKind::Release;
        ctx.attemptInputIndex = static_cast<int>(i) + 1;
        ctx.eventT = s.inputs[i].tMs / 1000.0;
        ctx.gamemode = Gamemode::Ship;
        ctx.cluster = r.clusters.ref("a1", static_cast<int>(i) + 1);
        ctx.boundarySimulations = m.trials;
        auto c = control::controlOf(log, static_cast<int>(i));
        ControlFacts facts;
        facts.index = c.index;
        if (c.press >= 0) facts.pressSeq = 100 + c.press;
        if (c.release >= 0) facts.releaseSeq = 100 + c.release;
        facts.holdFrames = c.holdFrames(log);
        if (!s.inputs[i].down && c.complete()) {
            ctx.pressMs = s.inputs[static_cast<size_t>(c.press)].tMs;
            ctx.pressSeq = 100 + c.press;
            r.phases[i] = std::make_unique<Phase>(solvePhase(o, s, static_cast<size_t>(c.press), cfg));
            facts.phase = &r.phases[i]->result;
        }
        ctx.control = facts;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&m.result), {});
        auto built = buildTimingResultEvent(ctx, ev, &m.result, st, true);
        CHECK_MSG(built.ok, built.error);
        r.payloads[i] = built.payload;
    }
    return r;
}

/// The card of the control whose release is input `release` (its press = the input before it).
control::Card cardOf(Measured const& r, size_t release) {
    auto press = r.payloads.find(release - 1);
    return control::makeCard(press == r.payloads.end() ? nullptr : &press->second, r.payloads.at(release));
}
void printCard(control::Card const& c) {
    for (auto const& line : control::cardLines(c)) std::printf("    %s\n", line.c_str());
}

bool isBudget(SAResult const& r) { return r.failure == status::Reason::SaNotMeasuredBudget; }

// =============================================================================================
// #2 / #14: a DENSE ship flight. A lead-in climb, then a hold of 8 ticks every 16 (15 presses a
// second) hovering in a corridor `clear` units around its path. Thrust = gravity = 0.03, so one
// tick more or less of hold changes the vertical velocity by 0.06 for good: with every other
// input frozen the ship drifts 0.06 units per tick into the corridor wall, with the next inputs
// free it flies the same path a little higher or lower.

InputSchedule denseFlight(int controls, bool hop = false) {
    std::vector<std::pair<double, bool>> t = {{100, true}, {130, false}};
    for (int i = 0; i < controls; ++i) {
        t.push_back({164.0 + 16.0 * i, true});
        t.push_back({172.0 + 16.0 * i, false});
    }
    // a short hop on the floor long after the flight landed (beyond every look-ahead)
    if (hop) {
        double const last = 172.0 + 16.0 * (controls - 1);
        t.push_back({last + 700.0, true});
        t.push_back({last + 706.0, false});
    }
    return schedule(t);
}
double denseEnd(int controls) { return 172.0 + 16.0 * (controls - 1); }
World denseWorld(InputSchedule const& s, double clear, int lastTick) {
    World w = shipBase();
    w.shipThrust = 0.03;
    w.shipGravity = 0.03;
    auto path = pathOf(w, s, lastTick + 20);
    bandsAround(w, path, 150, lastTick, [=](int) { return clear; }, [=](int) { return clear; });
    return w;
}

void test2HighCps() {
    SECTION("#2 high-CPS corridor: every frozen window is tiny, every compensated window several times wider; both are true at once");
    int const controls = 26;
    auto s = denseFlight(controls);
    double const cps = 240.0 / 16.0;
    for (double clear : {6.0, 10.0}) {
        World w = denseWorld(s, clear, static_cast<int>(denseEnd(controls)) - 2);
        CHECK(recordedSurvives(w, s, 2.6));
        KinematicOracle o(w);
        auto r = measure(o, s, {8, 9, 10, 11, 12, 13});
        std::printf("  clearance %.0f units, %.0f presses per second:\n", clear, cps);
        int decided = 0;
        for (size_t i : r.inputs) {
            Member const& m = r.member(i);
            CHECK(m.window.valid && m.window.boundedEarly && m.window.boundedLate);
            // frozen: a tick or two, and the death that bounds it comes AFTER later inputs (downstream)
            CHECK(localFrames(m.window) <= 3.0 + 1e-9);
            CHECK(downstream(m.window));
            // compensated: never narrower, and here far wider - decided or not, the part that was
            // PROVEN (alive and re-joined) is what is compared
            CHECK(compFrames(m.result) >= localFrames(m.window) + 8.0 - 1e-9);
            CHECK(compFrames(m.result) >= 4.0 * localFrames(m.window) - 1e-9);
            if (m.result.decided) ++decided;
            else CHECK_MSG(isBudget(m.result), status::name(m.result.failure));   // a dense search may run out of budget; nothing else may stop it
            CHECK(m.trials <= gprl::test::fixtureComp().maxTrialsPerInput);
            // every compensated edge beyond the local one came back to the recorded path
            for (SAEdge const* e : {&m.result.sequence.early, &m.result.sequence.late}) {
                if (e->proof != SAProof::Local) CHECK(rejoin::rejoined(e->rejoin));
            }
        }
        for (size_t rel : {9u, 11u, 13u}) printCard(cardOf(r, rel));
        std::printf("  decided %d of %zu | trials per input: average %.1f, most %d (budget %d)\n", decided, r.inputs.size(), static_cast<double>(r.trials) / static_cast<double>(r.inputs.size()),
                    r.maxTrials, gprl::test::fixtureComp().maxTrialsPerInput);
        if (clear >= 10.0) CHECK(decided == static_cast<int>(r.inputs.size()));
    }
}

void test14DenseCluster() {
    SECTION("#14 dense correlated cluster: the dense flight is ONE measured cluster (press, release, press, release ...); the effective member count grows like its square root; a hop long after it is a cluster of its own");
    int const controls = 12;
    auto s = denseFlight(controls, true);
    World w = denseWorld(s, 6.0, static_cast<int>(denseEnd(controls)) - 2);
    CHECK(recordedSurvives(w, s, 4.0));
    KinematicOracle o(w);
    size_t const hopPress = s.inputs.size() - 2, hopRelease = s.inputs.size() - 1;
    auto r = measure(o, s, {8, 9, 10, 11, 12, 13, 14, 15});
    std::string id;
    for (size_t k = 0; k < r.inputs.size(); ++k) {
        auto const& p = r.payloads.at(r.inputs[k]);
        if (k == 0) id = p.cluster.id;
        CHECK(p.cluster.id == id);                                // one cluster
        CHECK(p.cluster.index == static_cast<int64_t>(k) + 1);    // ... that keeps growing
        CHECK(p.cluster.connectedNext.has_value() && *p.cluster.connectedNext);
        if (k > 0) CHECK(p.cluster.connectedPrev.has_value() && *p.cluster.connectedPrev);
    }
    CHECK(cluster::clusterIdOk(id));
    auto card = cardOf(r, 15);
    printCard(card);
    CHECK(card.members == 8);
    CHECK(card.effectiveMembers < card.members - 1e-9 && card.effectiveMembers > 1.0);
    CHECK_NEAR(card.effectiveMembers, std::sqrt(8.0), 1e-9);
    // raw transitions 8, clusters 1, effective sqrt(8): the rating reads the last, never the first
    std::printf("  raw_transition_count 8 | cluster_count 1 | effective_sample_count %.2f\n", card.effectiveMembers);
    // the flight's last inputs and the hop 700 ticks later: not connected (beyond the look-ahead)
    auto tail = measure(o, s, {hopPress - 2, hopPress - 1, hopPress, hopRelease});
    auto const& lastRelease = tail.payloads.at(hopPress - 1);
    auto const& hop = tail.payloads.at(hopPress);
    CHECK(lastRelease.cluster.connectedNext.has_value() && !*lastRelease.cluster.connectedNext);
    CHECK(hop.cluster.id != lastRelease.cluster.id);
    CHECK(hop.cluster.index == 1 && hop.cluster.connectedPrev.has_value() && !*hop.cluster.connectedPrev);
}

void testBudgetFailSafe() {
    SECTION("budget fail-safe (prompt §16): with almost no trials the dense inputs are NOT DECIDED (sa_not_measured_budget, sequence_dependent, no rating evidence) - never a narrow window made up for a search that ran out");
    int const controls = 26;
    auto s = denseFlight(controls);
    World w = denseWorld(s, 6.0, static_cast<int>(denseEnd(controls)) - 2);
    KinematicOracle o(w);
    CompConfig tiny = gprl::test::fixtureComp();
    tiny.maxTrialsPerInput = 5;
    auto r = measure(o, s, {8, 9}, tiny);
    for (size_t i : r.inputs) {
        Member const& m = r.member(i);
        CHECK(m.trials <= tiny.maxTrialsPerInput);
        CHECK(!m.result.decided);
        CHECK_MSG(isBudget(m.result), status::name(m.result.failure));
        CHECK(compFrames(m.result) >= localFrames(m.window) - 1e-9);   // what was proven is kept, nothing is taken away
        auto const& p = r.payloads.at(i);
        CHECK(p.status == "sequence_dependent");
        CHECK(std::find(p.statusReasons.begin(), p.statusReasons.end(), "sa_not_measured_budget") != p.statusReasons.end());
        CHECK(p.sequence && !p.sequence->decided);
    }
    auto card = cardOf(r, 9);
    printCard(card);
    CHECK(!card.usedForRating && card.confidencePct == 0);
    CHECK(control::inputText(card.press).find("not decided") != std::string::npos);
}

// =============================================================================================
// #4: the ship rests on the floor, presses, must pass UNDER a hanging band right after take-off
// (too early = too high) and OVER a block a little later (too late = too low), both while it is
// still holding; the release comes after the block, in open space, and the ship lands on the solid
// floor afterwards. The press is tight in absolute time; nothing later can change what happened
// before it was made.

void test4TightPress() {
    SECTION("#4 tight press / forgiving release: the press is tight frozen AND compensated (a genuinely tight input stays tight); the release is open; the phase tolerance is the press's");
    auto s = schedule({{100, true}, {118, false}});
    World w = shipBase();
    w.shipGravity = 0.06;
    auto path = pathOf(w, s, 420);
    bandsAround(w, path, 100, 104, [](int) { return 2000.0; }, [](int) { return 0.4; });
    bandsAround(w, path, 112, 113, [](int) { return 0.6; }, [](int) { return 2000.0; });
    CHECK(recordedSurvives(w, s, 2.0));
    KinematicOracle o(w);
    auto r = measure(o, s, {0, 1});
    Member const& press = r.member(0);
    Member const& rel = r.member(1);
    printMember("tight press", s, press);
    printMember("tight press", s, rel);
    auto card = cardOf(r, 1);
    printCard(card);
    CHECK(press.window.valid && press.window.boundedEarly && press.window.boundedLate);
    CHECK(rel.window.valid && !rel.window.boundedEarly && !rel.window.boundedLate);
    CHECK(localFrames(press.window) * 3.0 <= localFrames(rel.window) + 1e-9);
    // compensation changes nothing: both deaths happen before the release could act
    CHECK(press.result.decided);
    CHECK_NEAR(compFrames(press.result), localFrames(press.window), 1e-9);
    CHECK(press.result.sequence.early.proof == SAProof::Local && press.result.sequence.late.proof == SAProof::Local);
    CHECK(!downstream(press.window));
    // the whole hold shifted = the press shifted: the same passes
    Phase const& ph = *r.phases.at(1);
    CHECK(ph.result.decided);
    CHECK_NEAR(ph.result.sequence.early.passFrames, press.window.early.passShiftMs / T, 1e-9);
    CHECK_NEAR(ph.result.sequence.late.passFrames, press.window.late.passShiftMs / T, 1e-9);
    CHECK(card.phase && card.phaseDecided);
    // the press is rated at full confidence; the release has no timing requirement (every shift of
    // it re-joined on the floor): shown, never rated
    CHECK(r.payloads.at(1).status == "no_effect");
    CHECK(card.press.usedForRating && !card.release.usedForRating && card.usedForRating && card.confidencePct == 100);
    CHECK(card.release.compensated.open && !card.press.compensated.open);
}

// #5: the ship presses, climbs to a SOLID ceiling and slides along it (whenever it pressed, it is
// in the same state there), releases, and falls through a slot between two bands; then it lands.
// Only the release's absolute time decides whether it meets the slot.

void test5TightRelease() {
    SECTION("#5 forgiving press / tight release: the press is open, the release tight; the hold's valid durations and its phase tolerance are the release's window");
    auto s = schedule({{100, true}, {150, false}});
    World w = shipBase();
    w.shipGravity = 0.06;
    w.ceil.pts = {{-100.0, 18.0}, {5000.0, 18.0}};
    auto path = pathOf(w, s, 420);
    bandsAround(w, path, 163, 164, [](int) { return 0.8; }, [](int) { return 0.8; });
    CHECK(recordedSurvives(w, s, 2.0));
    KinematicOracle o(w);
    auto r = measure(o, s, {0, 1});
    Member const& press = r.member(0);
    Member const& rel = r.member(1);
    printMember("tight release", s, press);
    printMember("tight release", s, rel);
    auto card = cardOf(r, 1);
    printCard(card);
    CHECK(press.window.valid && !press.window.boundedEarly && !press.window.boundedLate);
    CHECK(rel.window.valid && rel.window.boundedEarly && rel.window.boundedLate);
    CHECK(localFrames(rel.window) * 3.0 <= localFrames(press.window) + 1e-9);
    CHECK(press.result.decided && rel.result.decided);
    CHECK_NEAR(compFrames(rel.result), localFrames(rel.window), 1e-9);   // nothing follows the release: tight stays tight
    // the valid hold durations = the recorded hold + the release's window
    CHECK(card.holdRange);
    CHECK_NEAR(card.holdFrames, 50.0, 1e-9);
    CHECK_NEAR(card.holdMaxFrames - card.holdMinFrames, compFrames(rel.result), 1e-6);
    CHECK(card.holdMinFrames <= card.holdFrames && card.holdFrames <= card.holdMaxFrames);
    // the whole hold shifted: the press does not matter, the release does - the release's passes
    Phase const& ph = *r.phases.at(1);
    CHECK(ph.result.decided);
    CHECK_NEAR(ph.result.sequence.early.passFrames, rel.window.early.passShiftMs / T, 1e-9);
    CHECK_NEAR(ph.result.sequence.late.passFrames, rel.window.late.passShiftMs / T, 1e-9);
    CHECK(card.press.compensated.open && !card.release.compensated.open);
    // the other way round: the press has no timing requirement, the release is rated
    CHECK(r.payloads.at(0).status == "no_effect");
    CHECK(!card.press.usedForRating && card.release.usedForRating && card.usedForRating && card.confidencePct == 100);
}

// #3 (hold duration vs phase): the critical world of compensation_tests - a short hold from the
// floor into a corridor around the recorded arc. The hold's DURATION has one tick; the hold as a
// whole can come several ticks earlier or later.

void testHoldVsPhase() {
    SECTION("#3 hold duration vs phase: the duration tolerance (release alone) is tight, the phase tolerance (whole hold shifted) several times wider; the press alone is tight, compensated it is at least as wide as the phase");
    auto s = schedule({{100, true}, {108, false}});
    bool found = false;
    for (double clear : {2.4, 1.6, 3.0}) {
        World w = criticalWorld(s, clear);
        if (!recordedSurvives(w, s, 2.0)) continue;
        KinematicOracle o(w);
        auto r = measure(o, s, {0, 1});
        Member const& press = r.member(0);
        Member const& rel = r.member(1);
        Phase const& ph = *r.phases.at(1);
        double const phase = compFrames(ph.result);
        if (!(ph.result.decided && press.result.decided && phase >= localFrames(rel.window) + 3.0)) continue;
        found = true;
        printMember("hold", s, press);
        printMember("hold", s, rel);
        printCard(cardOf(r, 1));
        CHECK(localFrames(rel.window) <= 2.0 + 1e-9);                         // duration: tight
        CHECK(localFrames(press.window) <= 2.0 + 1e-9);                       // the press alone changes the duration: tight
        CHECK(phase >= 3.0 * localFrames(rel.window) - 1e-9);                 // phase: wide
        // the phase shift is ONE of the compensations the free search may find: never wider than it
        CHECK(ph.result.sequence.early.passFrames >= press.result.sequence.early.passFrames - 1e-9);
        CHECK(ph.result.sequence.late.passFrames <= press.result.sequence.late.passFrames + 1e-9);
        CHECK(rejoin::rejoined(ph.result.sequence.early.rejoin) && rejoin::rejoined(ph.result.sequence.late.rejoin));
        break;
    }
    CHECK_MSG(found, "no clearance gave a tight duration with a wide phase");
}

// =============================================================================================
// #12: a hold from the floor, then a floor band the ship must clear, then open space for good (no
// floor to land on, no velocity cap). A press one tick late leaves the ship too low; a LONGER hold
// clears the band - but then the ship has another vertical velocity and flies away from the
// recorded path for ever. It survived the obstacle. It never came back.

void test12SurvivesNoRejoin() {
    SECTION("#12 survives-but-no-rejoin: the surviving compensation is NOT a pass of the compensated window (sa_survives_no_rejoin, not decided, no evidence); the rule before controls/1 counted it");
    auto s = schedule({{100, true}, {108, false}});
    World w = shipBase();
    w.shipGravity = 0.06;
    w.shipMaxVy = 1000.0;
    w.floor.pts = {{-100.0, -3.0}, {1.3 * 109.0, -3.0}, {1.3 * 109.5, -1e7}, {5000.0, -1e7}};
    auto path = pathOf(w, s, 520);
    bandsAround(w, path, 118, 122, [](int) { return 0.8; }, [](int) { return 5000.0; });
    CHECK(recordedSurvives(w, s, 2.0));
    KinematicOracle o(w);
    CompConfig const cfg = gprl::test::fixtureComp();
    auto r = measure(o, s, {0, 1});
    Member const& m = r.member(0);
    printMember("no re-join", s, m);
    CHECK(m.window.valid && m.window.boundedLate && downstream(m.window));
    double const localLate = m.window.late.passShiftMs / T;
    // the late side: not widened, not decided, and it says why
    CHECK_NEAR(m.result.sequence.late.passFrames, localLate, 1e-9);
    CHECK(m.result.survivedNoRejoin[1] && !m.result.sideDecided[1] && !m.result.decided);
    CHECK_MSG(m.result.failure == status::Reason::SaSurvivesNoRejoin, status::name(m.result.failure));
    auto const& p = r.payloads.at(0);
    CHECK(p.status == "sequence_dependent");
    CHECK(std::find(p.statusReasons.begin(), p.statusReasons.end(), "sa_survives_no_rejoin") != p.statusReasons.end());
    auto card = cardOf(r, 1);
    printCard(card);
    CHECK(card.rejoined == control::Rejoined::No);
    CHECK(!card.press.usedForRating && card.press.confidence == 0.0);   // the press: no rating evidence
    std::printf("    press:   %s\n", control::fieldsLine(card, false).c_str());
    CHECK(control::fieldsLine(card, false).find("rejoin_success=no") != std::string::npos);
    // ground truth at the first shift beyond the local window: release offsets that SURVIVE exist,
    // none of them re-joins
    int alive = 0, rejoined = 0;
    for (int off = -5; off <= 7; ++off) {
        CompOutcome out;
        if (!compScheduleRun(o, s, 0, localLate + 1.0, {static_cast<double>(off)}, cfg, 0, out)) continue;
        if (out.kind != CompOutcome::Kind::Pass) continue;
        ++alive;
        if (rejoin::rejoined(out.rejoin)) ++rejoined;
    }
    std::printf("  shift %+.0f: %d release offsets survive, %d re-join\n", localLate + 1.0, alive, rejoined);
    CHECK(alive >= 1 && rejoined == 0);
    // the rule before controls/1 (alive at the look-ahead = a pass): the same shifts widen the window
    CompConfig old = cfg;
    old.requireRejoin = false;
    auto before = solveAll(o, s, {0}, false, old);
    printMember("survive = pass", s, before[0]);
    CHECK(before[0].result.sequence.late.passFrames >= localLate + 3.0 - 1e-9);
}

// =============================================================================================
// #15: the owner's input rhythm in the last Ship corridor of SUPERHATEMEWORLD (attempt 13 of
// 2026-10-03, 93.3-97.8 %, half speed; ticks 581 P 591 R 628 P 641 R 660 P 676 R 695 P 712 R 728 P
// 743 R) flown on the DEV FIXTURE ship in a straight corridor `clear` units beyond the path's
// highest and lowest point. NOT the level: the fixture's physics and corridor are not GD's, so its
// frame counts are not comparable with the human counts 17 / 16 / 19 / 15 / 12 - they are printed
// side by side as comparison data. What IS checked: the relations the rewrite promises.

void test15ReferenceGeometry() {
    SECTION("#15 SUPERHATEMEWORLD-like rhythm (comparison data, nothing here is the real level): compensated >= frozen for every input, not decided only for budget / no re-join");
    // lead-in (take-off), then the ten inputs at the owner's spacing
    auto s = schedule({{100, true}, {140, false}, {168, true}, {178, false}, {215, true}, {228, false}, {247, true}, {263, false}, {282, true}, {299, false}, {315, true}, {330, false}});
    World base = shipBase();
    base.startSpeed = 0.5;
    base.shipThrust = 0.05;
    base.shipGravity = 0.039;
    auto path = pathOf(base, s, 700);
    double ymin = 1e9, ymax = -1e9;
    for (int k = 150; k <= 345; ++k) {
        ymin = std::min(ymin, path[static_cast<size_t>(k - 1)].y);
        ymax = std::max(ymax, path[static_cast<size_t>(k - 1)].y);
    }
    int const human[5] = {17, 16, 19, 15, 12};
    for (double clear : {2.0, 6.0}) {
        World w = base;
        double const x0 = path[154].x, x1 = path[339].x;
        w.spikes.push_back({x0, ymin - w.half - clear - 400.0, x1, ymin - w.half - clear, 40});
        w.spikes.push_back({x0, ymax + w.half + clear, x1, ymax + w.half + clear + 400.0, 41});
        CHECK(recordedSurvives(w, s, 2.8));
        KinematicOracle o(w);
        auto r = measure(o, s, {2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
        std::printf("  corridor %.0f units beyond the path's range [%.1f, %.1f] (ship half %.0f):\n", clear, ymin, ymax, w.half);
        std::printf("    control | press frozen / compensated | release frozen / compensated | hold | phase early / late | human count (which input: unknown)\n");
        for (int c = 0; c < 5; ++c) {
            size_t const pi = 2 + 2 * static_cast<size_t>(c), ri = pi + 1;
            auto card = cardOf(r, ri);
            auto side = [](control::CardInput const& in) {
                char b[96];
                if (!in.compensated.present) std::snprintf(b, sizeof b, "%4.1f%s /   n/a", in.local.frames, in.local.open ? "+" : " ");
                else std::snprintf(b, sizeof b, "%4.1f%s / %s%4.1f%s", in.local.frames, in.local.open ? "+" : " ", in.decided ? "  " : ">=", in.compensated.frames, in.compensated.open ? "+" : " ");
                return std::string(b);
            };
            std::printf("    #%d      | %-26s | %-28s | %4.0f | %5.1f / %-5.1f%s     | %d\n", card.index, side(card.press).c_str(), side(card.release).c_str(), card.holdFrames,
                        card.phaseEarlyFrames, card.phaseLateFrames, card.phaseDecided ? " " : "+", human[c]);
            for (size_t i : {pi, ri}) {
                Member const& m = r.member(i);
                CHECK(m.window.valid);
                CHECK(compFrames(m.result) >= localFrames(m.window) - 1e-9);
                if (!m.result.decided) CHECK_MSG(isBudget(m.result) || m.result.failure == status::Reason::SaSurvivesNoRejoin, status::name(m.result.failure));
                CHECK(m.trials <= gprl::test::fixtureComp().maxTrialsPerInput);
            }
        }
        std::printf("    trials per input: average %.1f, most %d\n", static_cast<double>(r.trials) / 10.0, r.maxTrials);
        if (clear <= 2.0) printCard(cardOf(r, 5));
    }
}

// =============================================================================================
// The control card on hand-made payloads: the text and the explicit fields.

telemetry::TimingWindowV2Payload windowMs(double earlyFrames, double lateFrames, bool boundedEarly, bool boundedLate) {
    telemetry::TimingWindowV2Payload w;
    w.earliestMs = 1000.0 + earlyFrames * T;
    w.latestMs = 1000.0 + lateFrames * T;
    w.early.stop = boundedEarly ? "fail" : "range";
    w.late.stop = boundedLate ? "fail" : "range";
    w.resolutionMs = T;
    return w;
}

void testCard() {
    SECTION("control card: PRESS / RELEASE local + compensated, HOLD original + valid, PHASE, SEQUENCE; not decided = '>=' + the reason; a failed replay = no window; the explicit fields");
    telemetry::TimingResultPayload press;
    press.inputKind = InputKind::Press;
    press.status = "ok";
    press.stateReplayValid = true;
    press.actualMs = 1000.0;
    press.local = windowMs(-0.5, 0.5, true, true);
    telemetry::SequenceWindowV2Payload sq;
    sq.window = windowMs(-7.5, 8.5, true, true);
    sq.decided = true;
    press.sequence = sq;
    press.cluster.id = "a1:5";
    press.cluster.index = 5;
    telemetry::TimingControlPayload k;
    k.index = 3;
    k.role = "press";
    k.pressSeq = 41;
    k.followers = 3;
    k.rejoinEarly = telemetry::TimingRejoinPayload{"approx", 20.0 * T, 0.96, 0.01};
    k.rejoinLate = telemetry::TimingRejoinPayload{"parallel", 190.0 * T, 3.84, 0.0};
    press.control = k;

    telemetry::TimingResultPayload rel = press;
    rel.inputKind = InputKind::Release;
    rel.status = "sequence_dependent";
    rel.statusReasons = {"sa_not_measured_budget"};
    rel.sequence->decided = false;
    rel.cluster.index = 6;
    rel.hold = telemetry::HoldRangeV2Payload{41, 6.0 * T, 10.0 * T, "sequence", 7.5 * T, 8.5 * T};
    rel.control->role = "release";
    rel.control->releaseSeq = 42;
    rel.control->holdMs = 8.0 * T;
    rel.control->phase = telemetry::TimingPhasePayload{-7.0 * T, 8.0 * T, "neighbour", "fail", true, 12};
    rel.control->rejoinEarly.reset();
    rel.control->rejoinLate.reset();

    auto card = control::makeCard(&press, rel);
    auto lines = control::cardLines(card);
    for (auto const& l : lines) std::printf("    %s\n", l.c_str());
    CHECK(lines.size() == 6);
    if (lines.size() == 6) {
        CHECK(lines[0] == "SHIP CONTROL #3");
        CHECK(lines[1] == "  PRESS     local 1.0 f | compensated 16.0 f");
        CHECK(lines[2] == "  RELEASE   local 1.0 f | compensated >= 16.0 f (not decided: sa_not_measured_budget)");
        CHECK(lines[3] == "  HOLD      original 8.0 f | valid at least 6.0 - 10.0 f (sequence)");
        CHECK(lines[4] == "  PHASE     early 7.0 f | late 8.0 f");
        CHECK(lines[5] == "  SEQUENCE  cluster #5 | members 6 | effective 2.4 | rejoined yes (parallel) | confidence 70%");
    }
    CHECK(card.press.usedForRating && !card.release.usedForRating && card.usedForRating);
    CHECK_NEAR(card.press.confidence, control::kCardParams.parallelWeight, 1e-12);   // the weaker of approx / parallel
    // the explicit fields of the prompt's §17, by name
    std::string const f = control::fieldsLine(card, false);
    std::printf("    %s\n", f.c_str());
    for (char const* name : {"local_window_frames=1.00", "compensated_window_frames=16.00", "phase_early_frames=7.00", "phase_late_frames=8.00", "original_hold_frames=8.00",
                             "min_hold_frames=6.00", "max_hold_frames=10.00", "cluster_id=a1:5", "cluster_members=5", "effective_weight=0.447", "rejoin_success=yes", "rejoin_tick=190.00",
                             "rejoin_error_y=3.840", "rejoin_error_vy=0.0000", "state_replay_valid=true", "first_divergence_tick=na", "solver_confidence=0.70", "used_for_rating=true"}) {
        CHECK_MSG(f.find(name) != std::string::npos, name);
    }
    // the release's compensated window is not decided: no figure is given for it
    CHECK(control::fieldsLine(card, true).find("compensated_window_frames=na") != std::string::npos);
    CHECK(control::fieldsLine(card, true).find("used_for_rating=false") != std::string::npos);

    // a replay that left the real run: no window, no confidence, the first divergence
    telemetry::TimingResultPayload bad;
    bad.inputKind = InputKind::Release;
    bad.status = "state_replay_failed";
    bad.statusReasons = {"control_mismatch"};
    bad.stateReplayValid = false;
    bad.parity = telemetry::TimingParityPayload{811.0, "x", 19236.33, 19237.38, 1.05};
    bad.cluster.id = "a1:20";
    bad.cluster.index = 2;
    bad.control = telemetry::TimingControlPayload{};
    bad.control->index = 9;
    bad.control->role = "release";
    auto failed = control::makeCard(nullptr, bad);
    auto fl = control::cardLines(failed);
    for (auto const& l : fl) std::printf("    %s\n", l.c_str());
    CHECK(fl.size() == 6 && fl[1] == "  PRESS     no result");
    CHECK(fl.size() == 6 && fl[2] == "  RELEASE   replay failed at tick 811 (x): no window");
    CHECK(failed.confidencePct == 0 && !failed.usedForRating);
    std::string const bf = control::fieldsLine(failed, true);
    CHECK(bf.find("state_replay_valid=false") != std::string::npos && bf.find("first_divergence_tick=811") != std::string::npos);
    CHECK(bf.find("local_window_frames=na") != std::string::npos && bf.find("used_for_rating=false") != std::string::npos);

    // a control whose edges are all local: rejoined n/a, full confidence
    telemetry::TimingResultPayload lp = press, lr = press;
    lp.control->rejoinEarly.reset();
    lp.control->rejoinLate.reset();
    lr.inputKind = InputKind::Release;
    lr.control = lp.control;
    lr.control->role = "release";
    auto local = control::makeCard(&lp, lr);
    CHECK(local.rejoined == control::Rejoined::NotApplicable && local.confidencePct == 100 && local.usedForRating);
}

}  // namespace

int main() {
    test2HighCps();
    testHoldVsPhase();
    test4TightPress();
    test5TightRelease();
    test12SurvivesNoRejoin();
    test14DenseCluster();
    testBudgetFailSafe();
    test15ReferenceGeometry();
    testCard();
    return gprl::test::finish("ship_cases_tests");
}
