// trace_view host tests (AUDIT §11; docs/TIMING_SOLVER_V2.md §2.12): the in-game debug view of one
// traced timing built from the engine's last trace (core/solver/trace View) - the trajectories in
// their roles (local block and the sequence-adjusted "sa-" block), the AUDIT §11 fields (timestamp,
// type, gamemode, speed, gravity, local window, sequence window, hold, death point, why invalid).
#include "test_util.hpp"

#include "../core/solver/trace_view.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;

namespace {

constexpr double kTick = 1000.0 / 240.0;

trace::TraceClone clone(double shift, int steps, bool died, int laterInputs = 0, bool extension = false, int killer = -1) {
    trace::TraceClone c;
    c.shiftFrames = shift;
    for (int k = 0; k < steps; ++k) {
        trace::TraceStep s;
        s.frame = 3500.0 + k;
        s.x = 4650.0f + 1.3f * static_cast<float>(k);
        s.y = 238.7f + static_cast<float>(shift) + 1.3f * static_cast<float>(k);
        s.mode = 'W';
        s.w = s.h = 30.f;
        trace::record(c, s);
    }
    c.passed = !died;
    if (died) {
        c.death.died = true;
        c.death.frame = 3518.0;
        c.death.killerId = killer;
        c.death.killerType = killer >= 0 ? 7 : -1;
        c.death.x = 4668.2f;
        c.death.y = 262.9f;
        c.death.boxW = c.death.boxH = 30.f;
        c.death.laterInputs = laterInputs;
        c.death.extension = extension;
    }
    return c;
}

trace::Header header(bool down) {
    trace::Header h;
    h.inputIndex = 47;
    h.down = down;
    h.seq = 3157;
    h.t = 14.5833;
    h.tick = 3501;
    h.x = 4650.0;
    h.percent = 13.0421;
    h.mode = "wave";
    h.speed = 1.0;
    // the engine's window texts (CloneEngine windowText: width, edges, 240 FPS frames from the ms)
    h.local = "20.83 ms [-10.42,+10.42] (5.00 f)";
    h.sequence = "50.00 ms [-22.92,+27.08] (12.00 f) pair decided";
    if (!down) h.hold = "[83.33, 100.00] ms sequence";
    return h;
}

/// The engine's View as CloneEngine::traceEmit builds it: the local block in trace::ordered order,
/// then the sequence-adjusted block.
trace::View engineView(bool down, int serial = 3) {
    trace::View ev;
    ev.serial = serial;
    ev.header = header(down);
    ev.trajectories.emplace_back("reference", clone(0, 40, false));
    ev.trajectories.emplace_back("earliest-valid", clone(-2, 40, false));
    ev.trajectories.emplace_back("early-invalid", clone(-3, 18, true, 0, false, 1717));
    ev.trajectories.emplace_back("latest-valid", clone(2, 40, false));
    ev.trajectories.emplace_back("late-invalid", clone(3, 30, true, 2));
    ev.trajectories.emplace_back("sa-earliest-valid (pair)", clone(-5, 40, false));
    ev.trajectories.emplace_back("sa-late-invalid (chain2)", clone(7, 25, true, 0));
    return ev;
}

bool contains(std::vector<std::string> const& lines, std::string const& needle) {
    for (auto const& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

void testRolesAndPaths() {
    SECTION("roles from the log's role texts (local + sa- block), paths in order, shifts in ms, nothing traced = empty");
    auto v = trace_view::fromEngine(engineView(true));
    CHECK(v.any && v.serial == 3);
    CHECK(v.paths.size() == 7);
    CHECK(v.paths[0].role == trace_view::Role::Reference && v.paths[0].shiftFrames == 0.0);
    CHECK(v.paths[1].role == trace_view::Role::EarliestValid && v.paths[1].shiftFrames == -2.0);
    CHECK(v.paths[2].role == trace_view::Role::EarlyInvalid && v.paths[2].death.died);
    CHECK(v.paths[3].role == trace_view::Role::LatestValid);
    CHECK(v.paths[4].role == trace_view::Role::LateInvalid && v.paths[4].death.laterInputs == 2);
    CHECK(v.paths[5].role == trace_view::Role::SaEarliestValid && v.paths[5].name == "sa-earliest-valid (pair)");
    CHECK(v.paths[6].role == trace_view::Role::SaLateInvalid);
    CHECK(trace_view::sequenceRole(v.paths[6].role) && trace_view::invalidRole(v.paths[6].role));
    CHECK(trace_view::validRole(v.paths[5].role) && !trace_view::invalidRole(v.paths[5].role));
    CHECK(v.paths[2].points.size() == 18 && v.paths[0].points.size() == 40);
    CHECK_NEAR(v.paths[2].shiftMs, -3.0 * kTick, 1e-12);
    CHECK_NEAR(v.paths[2].points[1].x, 4651.3, 1e-3);
    // the role names of trace::ordered all map to their role
    auto ord = trace::ordered(trace::Selection{0, 1, 2, 3, 4});
    for (size_t i = 0; i < ord.size(); ++i) CHECK(trace_view::roleOf(ord[i].first) == static_cast<trace_view::Role>(i));
    CHECK(trace_view::roleOf("something-else") == trace_view::Role::Other);
    // serial 0 = the engine has no trace on this level
    trace::View none;
    CHECK(!trace_view::fromEngine(none).any);
    CHECK(trace_view::panelLines(trace_view::fromEngine(none)).empty());
    CHECK(trace_view::summaryLine(trace_view::fromEngine(none)).empty());
}

void testFields() {
    SECTION("AUDIT §11 fields: timestamp / type / gamemode / speed / gravity / local / sequence / hold / death point / why");
    auto v = trace_view::fromEngine(engineView(false));
    auto lines = trace_view::panelLines(v);
    CHECK(lines.size() >= 5);
    CHECK_MSG(lines[0] == "TRACE input #47 release (seq 3157) | t 14.5833 s tick 3501 | 13.042% x 4650.0", lines[0]);
    CHECK_MSG(lines[1] == "wave | speed normal (1x) | gravity normal | mini no", lines[1]);
    CHECK_MSG(lines[2] == "local (fixed-sequence) 20.83 ms [-10.42,+10.42] (5.00 f)", lines[2]);
    CHECK_MSG(lines[3] == "sequence-adjusted 50.00 ms [-22.92,+27.08] (12.00 f) pair decided", lines[3]);
    CHECK_MSG(lines[4] == "hold [83.33, 100.00] ms sequence", lines[4]);
    // the invalid trajectories carry their death point and WHY they failed
    CHECK(contains(lines, "early-invalid shift -3 (-12.50 ms): died f3518.0 on #1717 type 7 at (4668.2, 262.9) box 30x30"));
    CHECK(contains(lines, "  why: self: died before any later input acted"));
    CHECK(contains(lines, "late-invalid shift +3 (+12.50 ms): died f3518.0, no object (#-1) at (4668.2, 262.9) box 30x30"));
    CHECK(contains(lines, "  why: downstream: 2 later inputs applied as performed first (not this input alone)"));
    CHECK(contains(lines, "earliest-valid shift -2 (-8.33 ms): passed"));
    CHECK(contains(lines, "sa-earliest-valid (pair) shift -5 (-20.83 ms): passed"));
    CHECK(contains(lines, "sa-late-invalid (chain2) shift +7 (+29.17 ms): died"));
    // the Session tab's one-line form: the local block's invalid causes only
    CHECK_MSG(trace_view::summaryLine(v) ==
                  "Trace #47 release t 14.5833 s 13.042% wave | local 20.83 ms [-10.42,+10.42] (5.00 f) | seq 50.00 ms [-22.92,+27.08] (12.00 f) "
                  "pair decided | early-invalid self, late-invalid downstream 2",
              trace_view::summaryLine(v));
}

void testReasonsAndMissing() {
    SECTION("why invalid: self / downstream / frozen-world extension / unresolved; missing windows, speed, gravity");
    trace::TraceDeath d;
    CHECK(trace_view::invalidReason(d).find("not resolved") != std::string::npos);
    CHECK(trace_view::causeWord(d) == "unresolved");
    d.died = true;
    d.laterInputs = 0;
    CHECK(trace_view::invalidReason(d).rfind("self", 0) == 0);
    CHECK(trace_view::causeWord(d) == "self");
    d.laterInputs = 1;
    CHECK(trace_view::invalidReason(d) == "downstream: 1 later input applied as performed first (not this input alone)");
    CHECK(trace_view::causeWord(d) == "downstream 1");
    d.extension = true;
    CHECK(trace_view::invalidReason(d).rfind("frozen death pause", 0) == 0);
    CHECK(trace_view::causeWord(d) == "extension");
    d = {};
    d.died = true;
    d.laterInputs = -1;
    CHECK(trace_view::invalidReason(d) == "cause unknown");

    auto ev = engineView(true);
    ev.header.sequence.clear();
    ev.header.flipped = true;
    ev.header.mini = true;
    ev.header.speed = 4.0;
    auto lines = trace_view::panelLines(trace_view::fromEngine(ev));
    CHECK_MSG(lines[1] == "wave | speed fastest (4x) | gravity flipped | mini yes", lines[1]);
    CHECK_MSG(lines[3] == "sequence-adjusted - (not measured)", lines[3]);
    CHECK_MSG(lines[4] == "hold - (press)", lines[4]);
    CHECK(std::string(trace_view::speedLabel(0.5)) == "slow" && std::string(trace_view::speedLabel(2.0)) == "fast" &&
          std::string(trace_view::speedLabel(3.0)) == "faster");
    // a death with a killer rect keeps it for the overlay's orange box
    auto c = clone(3, 5, true, 0, false, 8);
    c.death.rectX = 4660.f;
    c.death.rectY = 250.f;
    c.death.rectW = c.death.rectH = 30.f;
    trace::View one;
    one.serial = 1;
    one.trajectories.emplace_back("late-invalid", c);
    auto v = trace_view::fromEngine(one);
    CHECK(v.paths.size() == 1 && v.paths[0].death.rectW == 30.f && v.paths[0].death.killerId == 8);
}

}  // namespace

int main() {
    testRolesAndPaths();
    testFields();
    testReasonsAndMissing();
    return gprl::test::finish("trace_view_tests");
}
