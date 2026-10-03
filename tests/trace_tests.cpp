// trace host tests (docs/TIMING_SOLVER_V2.md §2.12, §5; AUDIT §11): which inputs are traced, the
// selection of the five trajectories (reference, earliest valid, just-invalid early, latest valid,
// just-invalid late), the `GPRL trace:` line encoding and its caps (append-only recording, <= 2 KB).
#include "test_util.hpp"

#include "../core/solver/trace.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace gprl::solver;
using namespace gprl::solver::trace;

namespace {

TraceClone clone(double shift, int steps, bool died = false) {
    TraceClone c;
    c.shiftFrames = shift;
    for (int k = 0; k < steps; ++k) {
        TraceStep s;
        s.frame = 3500.0 + k;
        s.x = 4650.0f + 1.3f * k;
        s.y = 238.7f + 1.3f * k;
        s.mode = 'W';
        s.w = s.h = 30.f;
        record(c, s);
    }
    c.passed = !died;
    if (died) {
        c.death.died = true;
        c.death.frame = 3518.0;
        c.death.killerId = -1;
        c.death.x = 4668.2f;
        c.death.y = 262.9f;
        c.death.boxW = c.death.boxH = 30.f;
        c.death.lastCollision[0] = 1717;
        c.death.laterInputs = 0;
    }
    return c;
}

void testWanted() {
    SECTION("traced when on, bounded on both sides, <= the setting (capped at 40) and under 8 per attempt");
    CHECK(!wanted(4.0, true, 0, 0));        // off
    CHECK(wanted(4.0, true, 10, 0));
    CHECK(!wanted(12.0, true, 10, 0));      // wider than the setting
    CHECK(!wanted(4.0, false, 10, 0));      // an open side is not a "claimed N-frame timing"
    CHECK(!wanted(4.0, true, 10, 8));       // the attempt's cap
    CHECK(wanted(40.0, true, 400, 0) && !wanted(41.0, true, 400, 0));
    CHECK(budget::kTraceCaps.maxTracesPerAttempt == 8 && budget::kTraceCaps.maxTraceSteps == 160);
}

void testSelection() {
    SECTION("the five trajectories: reference, earliest valid, early invalid, latest valid, late invalid");
    std::vector<TraceClone> cs;
    for (int s = -4; s <= 4; ++s) cs.push_back(clone(s, 20, s < -2 || s > 3));
    // window [-2, +3] passes, fails at -3 / +4
    auto sel = select(cs, -2.0, -3.0, 3.0, 4.0);
    CHECK(sel.reference >= 0 && cs[static_cast<size_t>(sel.reference)].shiftFrames == 0.0);
    CHECK(cs[static_cast<size_t>(sel.earliestValid)].shiftFrames == -2.0 && cs[static_cast<size_t>(sel.earlyInvalid)].shiftFrames == -3.0);
    CHECK(cs[static_cast<size_t>(sel.latestValid)].shiftFrames == 3.0 && cs[static_cast<size_t>(sel.lateInvalid)].shiftFrames == 4.0);
    // an open side has no just-invalid trajectory; a zero-width side is the reference itself
    auto openSel = select(cs, 0.0, std::nan(""), 3.0, 4.0);
    CHECK(openSel.earlyInvalid == -1 && openSel.earliestValid == openSel.reference);
    auto names = ordered(sel);
    CHECK(names.size() == 5 && std::string(names[0].first) == "reference" && std::string(names[4].first) == "late-invalid");
}

void testEncoding() {
    SECTION("line encoding: header, one line per trajectory, killer / hitbox / lastCollision / later inputs, <= 2 KB");
    Header h;
    h.inputIndex = 47;
    h.down = true;
    h.seq = 3157;
    h.t = 14.5833;
    h.tick = 3501;
    h.x = 4650;
    h.percent = 13.042;
    h.mode = "wave";
    h.local = "16.67 ms [-6.25,+10.42]";
    h.sequence = "45.83 ms [-20.83,+25.00] pair decided";
    auto line = header(h);
    CHECK_MSG(line.find("GPRL trace: input #47 press seq 3157 t=14.5833 tick=3501 x=4650 13.042% wave spd 1.00 grav normal mini 0 | local 16.67 ms [-6.25,+10.42] | sequence 45.83 ms [-20.83,+25.00] pair decided | hold -") == 0, line);
    auto died = clone(-2.0, 18, true);
    auto t = trajectory(47, "early-invalid", died);
    CHECK_MSG(t.find("GPRL trace: #47 early-invalid shift -2: frame0 3500.0 pts 18 x,y: 4650.0,238.7") == 0, t);
    CHECK_MSG(t.find("| died frame 3518.0 on #-1 type -1 at (0.0,0.0)") != std::string::npos, t);
    CHECK(t.find("lastCollision top 1717") != std::string::npos && t.find("laterInputs 0 (self)") != std::string::npos);
    // every second step
    CHECK(t.find("4651.3,240.0") == std::string::npos && t.find("4652.6,241.3") != std::string::npos);
    // caps: recording stops at maxTraceSteps, a line never exceeds 2 KB
    auto big = clone(1.0, 400, false);
    CHECK(big.steps.size() == 160 && big.capped);
    auto tb = trajectory(47, "latest-valid", big, 1);
    CHECK(tb.size() <= 2048);
    CHECK(tb.find("(capped)") != std::string::npos);
    auto tiny = trajectory(47, "latest-valid", big, 1, 300);
    CHECK(tiny.size() <= 300 && tiny.find(" ...") != std::string::npos);
}

void testSASelection() {
    SECTION("the sequence-adjusted block: passing trial at the SA pass edge (beyond the local one), the LAST member tried at the SA fail");
    std::vector<SATraced> trials;
    auto add = [&](int adaptation, double shift, bool passed) {
        SATraced t;
        t.adaptation = adaptation;
        t.shiftFrames = shift;
        t.clone = clone(shift, 10, !passed);
        t.clone.passed = passed;
        trials.push_back(t);
    };
    add(1, -3.0, true);    // pair passes at -3
    add(1, -4.0, false);   // pair dies at -4
    add(2, -4.0, false);   // chain2 dies at -4: the member that decided it
    add(1, 5.0, true);     // late side: pair passes at +5
    add(1, 6.0, false);
    // local window [-2, +3]: the SA window [-3, +5] extends it on both sides
    auto s = selectSA(trials, -3.0, -4.0, 5.0, 6.0, -2.0, 3.0);
    CHECK(s.earliestValid == 0 && s.earlyInvalid == 2 && s.latestValid == 3 && s.lateInvalid == 4);
    // an SA pass edge equal to the local one is the local block's (not repeated)
    auto same = selectSA(trials, -3.0, -4.0, 5.0, 6.0, -3.0, 5.0);
    CHECK(same.earliestValid == -1 && same.latestValid == -1 && same.earlyInvalid == 2);
    // open sides / missing trials: -1
    auto none = selectSA(trials, -3.0, std::nan(""), 5.0, 7.0, -2.0, 3.0);
    CHECK(none.earlyInvalid == -1 && none.lateInvalid == -1);
    View v;
    CHECK(v.serial == 0 && v.trajectories.empty());
}

}  // namespace

int main() {
    testWanted();
    testSelection();
    testSASelection();
    testEncoding();
    return gprl::test::finish("trace_tests");
}
