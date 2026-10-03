// result_ledger host tests (docs/TIMING_SOLVER_V2.md §3.2, §3.7, §5; V2-D6): exactly ONE
// timing_result per bound job on every lifecycle path - SA measured, SA dropped, expired, no SA
// needed, dropped before its local window, restart, level end, teardown - never two, never none;
// flush / drain bounded by the open entries.
#include "test_util.hpp"

#include "../core/solver/result_ledger.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/sequence_adjusted.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/telemetry.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;

namespace {

struct Data {
    std::string finalReason;
};

void testLifecycles() {
    SECTION("every lifecycle path emits exactly once");
    ResultLedger<Data> l;
    std::map<int, int> emitted;
    auto emit = [&](int id, Data&) { ++emitted[id]; };
    // 1: local -> SA -> done
    CHECK(l.open(1, {}));
    CHECK(!l.open(1, {}));   // a second open of the same job is refused
    CHECK(l.state(1) == LedgerState::AwaitingLocal);
    CHECK(l.localDone(1, true) && l.state(1) == LedgerState::AwaitingSA);
    CHECK(l.drain(emit) == 0);
    CHECK(l.saDone(1));
    // 2: local without SA
    l.open(2, {});
    l.localDone(2, false);
    // 3: dropped before its local window (mismatch / pool / reset of the job)
    l.open(3, {});
    CHECK(l.close(3));
    // 4: SA job dropped / expired -> saDone with the reason recorded by the caller
    l.open(4, {});
    l.localDone(4, true);
    l.data(4)->finalReason = "sa_expired";
    l.saDone(4);
    CHECK(l.drain(emit) == 4);
    CHECK(emitted[1] == 1 && emitted[2] == 1 && emitted[3] == 1 && emitted[4] == 1);
    // transitions of emitted ids are ignored; nothing is emitted twice
    CHECK(!l.localDone(1, false) && !l.saDone(1) && !l.close(1));
    CHECK(l.drain(emit) == 0);
    // 5-7: open at a restart / level end / teardown -> flush finalizes and emits them
    l.open(5, {});
    l.open(6, {});
    l.localDone(6, true);
    l.open(7, {});
    l.localDone(7, false);   // already done: flush just emits it
    std::map<int, LedgerState> before;
    int n = l.flush([&](int id, Data& d, LedgerState s) {
        before[id] = s;
        d.finalReason = s == LedgerState::AwaitingSA ? "sa_cut_by_restart" : "cut_by_restart";
    }, emit);
    CHECK(n == 3);
    CHECK(before.size() == 2 && before[5] == LedgerState::AwaitingLocal && before[6] == LedgerState::AwaitingSA);
    for (int id = 1; id <= 7; ++id) CHECK_MSG(emitted[id] == 1, "job " + std::to_string(id));
    CHECK(l.openCount() == 0);
    CHECK(l.opened() == 7 && l.emitted() == 7);
    // abandon (the layer died without teardown): nothing is pushed any more
    l.open(8, {});
    l.abandon();
    CHECK(l.openCount() == 0 && l.drain(emit) == 0 && emitted.count(8) == 0);
}

void testReentrantEmit() {
    SECTION("an emit callback that drives the ledger (the engine's emitResult -> sink) never emits an entry twice");
    ResultLedger<int> l;
    std::map<int, int> emitted;
    l.open(1, 1);
    l.open(2, 2);
    l.localDone(1, false);
    l.localDone(2, false);
    std::function<void(int, int&)> emit;
    emit = [&](int id, int&) {
        ++emitted[id];
        CHECK(!l.close(id) && !l.saDone(id));         // the emitted entry is already gone
        if (id < 10) {
            l.open(10 + id, 0);                       // a new job binds while a result is emitted
            l.close(10 + id);
        }
        l.drain(emit);                                // nested drain: emits only entries still open
    };
    l.drain(emit);
    CHECK(emitted[1] == 1 && emitted[2] == 1 && emitted[11] == 1 && emitted[12] == 1);
    CHECK(l.openCount() == 0);
}

void testRandomLifecycles() {
    SECTION("1000 random interleavings: opened == emitted once the attempt is flushed");
    ResultLedger<int> l;
    std::map<int, int> emitted;
    uint32_t x = 12345;
    auto rnd = [&]() {
        x = x * 1664525u + 1013904223u;
        return x >> 8;
    };
    int next = 1;
    std::vector<int> open;
    for (int step = 0; step < 1000; ++step) {
        uint32_t r = rnd() % 7;
        if (r <= 2 || open.empty()) {
            l.open(next, next);
            open.push_back(next++);
            continue;
        }
        int id = open[rnd() % open.size()];
        if (r == 3) l.localDone(id, rnd() % 2 == 0);
        else if (r == 4) l.saDone(id);
        else if (r == 5) l.close(id);
        else l.drain([&](int j, int&) { ++emitted[j]; });
    }
    l.flush([](int, int&, LedgerState) {}, [&](int j, int&) { ++emitted[j]; });
    int bad = 0;
    for (int id = 1; id < next; ++id) if (emitted[id] != 1) ++bad;
    CHECK_MSG(bad == 0, std::to_string(bad) + " jobs not emitted exactly once");
    CHECK(l.opened() == l.emitted());
}

void testOpenRangeLifecycle() {
    SECTION("Fable D3a: an open-both-sides, connected input with no SA job emits exactly one ok result that passes the validator and the gate mirror");
    // the local window: every shift passes on both sides (open to the +-10 range)
    PlannerConfig pc;
    PassPlanner local(pc, kNaN);
    auto shifts = local.nextPass();
    local.setLateLimit(18.0 - pc.neighbourMarginFrames);   // the next input 18 ticks later (connected)
    std::vector<ShiftOutcome> outs;
    for (double s : shifts) {
        ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = s;
        o.kind = ShiftKind::Survived;   // wave-like: alive at the horizon, never re-joined
        outs.push_back(o);
    }
    local.ingest(outs, true, false);
    double const t = 360.0, actualMs = 1500.0;
    auto w = local.result(actualMs);
    CHECK(w.valid && !w.boundedEarly && !w.boundedLate);
    // the SA planner the engine builds in saConsider: nothing to simulate, the pair walk is never queued
    SAInput in;
    in.id = 7;
    in.frame = t;
    in.down = true;
    in.local = local.outcomes();
    in.pairWalk = true;
    SAContext ctx;
    ctx.inputs = {{7, t, true, false}, {8, t + 18.0, false, false}, {9, t + 36.0, true, false}};
    SAPlanner sa(kSA, {in}, ctx);
    bool const needsSA = sa.needsTrials();
    CHECK(!needsSA && sa.memberOpenRange(0));
    struct Entry {
        SAResult sa;
        bool saRan = false;
    };
    ResultLedger<Entry> l;
    CHECK(l.open(1, {}));
    l.data(1)->sa = sa.result(0);
    l.data(1)->saRan = true;
    CHECK(l.localDone(1, needsSA));   // no SA job: done at once
    int emitted = 0;
    l.drain([&](int, Entry& e) {
        ++emitted;
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &local.outcomes();
        ev.frame = t;
        ev.nextFrame = t + 18.0;
        ev.nextFollows = true;
        ev.horizonFrame = t + 130.0;
        auto st = status::statusOf({}, localFacts(ev), saFacts(&e.sa), {});
        CHECK(st.status == status::TimingStatus::Ok);
        TimingResultContext c;
        c.inputSeq = 2;
        c.kind = InputKind::Press;
        c.eventT = actualMs / 1000.0;
        c.cluster = {"tr-a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
        auto built = buildTimingResultEvent(c, ev, &e.sa, st, true);
        CHECK_MSG(built.ok, built.error);
        CHECK(built.payload.status == "ok" && built.payload.sequence && built.payload.sequence->decided && built.payload.sequence->adaptationUsed.empty());
        CHECK(std::find(built.payload.statusReasons.begin(), built.payload.statusReasons.end(), "open_range") != built.payload.statusReasons.end());
        std::string err;
        CHECK_MSG(telemetry::validateTimingResult(built.payload, "", &err), err);
        auto gate = checkTimingResultPayload(built.payload, c.eventT, 0.0);
        CHECK_MSG(gate.accepted, gate.reasons.empty() ? "" : gate.reasons[0]);
    });
    CHECK(emitted == 1 && l.openCount() == 0 && l.opened() == 1 && l.emitted() == 1);
}

}  // namespace

int main() {
    testLifecycles();
    testReentrantEmit();
    testRandomLifecycles();
    testOpenRangeLifecycle();
    return gprl::test::finish("result_ledger_tests");
}
