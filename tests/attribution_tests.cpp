// Death attribution host tests (docs/TIMING_SOLVER_V2.md §1 RC1, §2.3, §5 T-ATT-2) on a REAL LOG
// EXTRACT: the owner's Geode log `Geode 2026-09-30 13.28.40.log` (mod v0.6.2, Deadlocked = GD main
// level 20, cbf=0), the pass-0 outcome strings of jobs 668-674 (the owner's table rows 4, 1, 2, 5,
// 6, 8, 10) copied verbatim from the `GPRL solver: job K pass 0 resolved in 130 steps: ...` lines
// (log lines 9874-9922), and the recorded frames of inputs 667-680 from the `GPRL solver: input #N
// ... frame=F` lines. No personal data (no account or session ids).
//
// With the exact rule of §2.3 (an input logged at frame F was applied before a death reported at
// deathFrame D iff F < D; `@N` is relative to the UNSHIFTED input frame) the owner rows' edges
// classify as the table of §1 RC1: every late edge and three early edges downstream, four early
// edges self (a death one step BEFORE the next input: audit A counted those as downstream). The
// PassPlanner fed the same outcomes reproduces the emitted v0.6.2 windows. The self / downstream
// counts of this extract are a regression pin of OLD solver data (not a target).
//
// T-ATT-3 (v0.7.1, Fable review D6): the five jobs the death at frame 4025 turned into five MISS
// windows (log lines 9157-9171) attribute to ONE miss - the latest input with a passing run.
#include "test_util.hpp"

#include "../core/solver/cluster.hpp"
#include "../core/solver/miss_attribution.hpp"
#include "../core/solver/pass_planner.hpp"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::solver;

namespace {

// REAL LOG EXTRACT (read-only copy, see the header)
struct Job {
    int input;
    double frame;
    char const* kind;
    char const* outcomes;
    double windowMs;
    double earlyMs, lateMs;
    int ownerRow;
};

const Job kJobs[] = {
    {668, 3435, "press", "-10D@75.0 -9D@76.0 -8D@77.0 -7D@78.0 -6D@79.0 -5D@80.0 -4D@81.0 -3D@82.0 -2D@83.0 -1A +0A +1A +2A +3A +4D@129.0 +5D@128.0 +6D@99.0 +7D@97.0 +8D@96.0 +9D@95.0 +10D@94.0", 20.83, -6.25, 14.58, 4},
    {669, 3474, "release", "-10D@55.0 -9D@56.0 -8D@57.0 -7D@58.0 -6D@60.0 -5D@89.0 -4D@90.0 -3D@92.0 -2A -1A +0A +1A +2D@44.0 +3D@43.0 +4D@42.0 +5D@41.0 +6D@40.0 +7D@39.0 +8D@38.0 +9D@37.0 +10D@36.0", 16.67, -10.42, 6.25, 1},
    {670, 3500, "press", "-10D@10.0 -9D@11.0 -8D@12.0 -7D@13.0 -6D@14.0 -5D@15.0 -4D@16.0 -3D@17.0 -2D@18.0 -1A +0A +1A +2A +3D@66.0 +4D@64.0 +5D@63.0 +6D@34.0 +7D@32.0 +8D@31.0 +9D@30.0 +10D@29.0", 16.67, -6.25, 10.42, 2},
    {671, 3518, "release", "-10D@11.0 -9D@12.0 -8D@13.0 -7D@14.0 -6D@16.0 -5D@45.0 -4D@46.0 -3D@48.0 -2A -1A +0A +1A +2A +3A +4D@30.0 +5D@29.0 +6D@28.0 +7D@27.0 +8D@26.0 +9D@25.0 +10D@24.0", 25.00, -10.42, 14.58, 5},
    {672, 3535, "press", "-10D@7.0 -9D@8.0 -8D@9.0 -7D@10.0 -6D@11.0 -5D@12.0 -4D@13.0 -3A -2A -1A +0A +1A +2A +3D@31.0 +4D@29.0 +5D@28.0 +6D@27.0 +7D@26.0 +8D@25.0 +9D@24.0 +10D@23.0", 25.00, -14.58, 10.42, 6},
    {673, 3548, "release", "-10D@10.0 -9D@11.0 -8D@12.0 -7D@13.0 -6D@14.0 -5D@15.0 -4D@16.0 -3D@18.0 -2A -1A +0A +1A +2A +3A +4A +5D@34.0 +6D@32.0 +7D@31.0 +8D@30.0 +9D@29.0 +10D@28.0", 29.17, -10.42, 18.75, 8},
    {674, 3566, "press", "-10D@10.0 -9D@11.0 -8D@12.0 -7D@13.0 -6D@14.0 -5D@16.0 -4A -3A -2A -1A +0A +1A +2A +3A +4A +5D@30.0 +6D@28.0 +7D@27.0 +8D@26.0 +9D@25.0 +10D@24.0", 37.50, -18.75, 18.75, 10},
};

// recorded frames of the attempt's inputs around the rows (#667 .. #680)
const double kFrames[] = {3327, 3435, 3474, 3500, 3518, 3535, 3548, 3566, 3583, 3600, 3616, 3624, 3689, 3854};

/// "-2D@83.0" -> shift -2, Died, deathAfter 83 (from the UNSHIFTED input); "+1A" -> Survived.
std::vector<ShiftOutcome> parse(Job const& j) {
    std::vector<ShiftOutcome> out;
    std::istringstream in(j.outcomes);
    std::string tok;
    while (in >> tok) {
        ShiftOutcome o;
        size_t k = 0;
        while (k < tok.size() && (tok[k] == '+' || tok[k] == '-' || (tok[k] >= '0' && tok[k] <= '9'))) ++k;
        o.nominalFrames = o.appliedFrames = std::stod(tok.substr(0, k));
        char kind = k < tok.size() ? tok[k] : '?';
        if (o.nominalFrames == 0.0) continue;   // the control
        if (kind == 'A') o.kind = ShiftKind::Survived;
        else if (kind == 'R') o.kind = ShiftKind::Resynced;
        else if (kind == 'D') {
            o.kind = ShiftKind::Died;
            size_t at = tok.find('@');
            o.deathAfterFrames = std::stod(tok.substr(at + 1));
            // the offline rule (§2.3): later inputs logged before the death frame were applied
            double death = j.frame + o.deathAfterFrames;
            int later = 0;
            for (double f : kFrames) if (f > j.frame && f < death) ++later;
            o.laterFixed = later;
        }
        else o.kind = ShiftKind::NotTested;
        out.push_back(o);
    }
    return out;
}

struct Expect {
    int input;
    EdgeCause early;
    int earlyLater;
    EdgeCause late;
    int lateLater;
};

// docs/TIMING_SOLVER_V2.md §1 RC1 table (recounted with the exact rule)
const Expect kExpect[] = {
    {668, EdgeCause::Downstream, 2, EdgeCause::Downstream, 5},
    {669, EdgeCause::Downstream, 4, EdgeCause::Downstream, 1},
    {670, EdgeCause::Self, 0, EdgeCause::Downstream, 3},
    {671, EdgeCause::Downstream, 2, EdgeCause::Downstream, 1},
    {672, EdgeCause::Self, 0, EdgeCause::Downstream, 1},
    {673, EdgeCause::Self, 0, EdgeCause::Downstream, 1},
    {674, EdgeCause::Self, 0, EdgeCause::Downstream, 1},
};

void testOwnerRows() {
    SECTION("T-ATT-2: the owner rows of the Deadlocked v0.6.2 log classify as §1 RC1 (exact rule F < D)");
    int self = 0, downstream = 0;
    for (size_t n = 0; n < std::size(kJobs); ++n) {
        auto const& j = kJobs[n];
        auto outs = parse(j);
        CHECK(outs.size() == 20);
        PlannerConfig cfg;   // cbf=0: whole ticks, no limit points
        // early limit: the previous input (and the ring); late limit: the next input
        double prev = 0.0, next = 0.0;
        for (size_t k = 0; k < std::size(kFrames); ++k) {
            if (kFrames[k] == j.frame) {
                prev = kFrames[k - 1];
                next = kFrames[k + 1];
            }
        }
        PassPlanner p(cfg, j.frame - prev - cfg.neighbourMarginFrames);
        p.setEarlyLimitKind(LimitKind::Neighbour);
        p.nextPass();
        p.setLateLimit(next - j.frame - cfg.neighbourMarginFrames);
        p.ingest(outs, true, false);
        double actual = j.frame * kTickMs;
        auto w = p.result(actual);
        CHECK(w.valid);
        // the planner reproduces the emitted window (midpoint edges, SD D8)
        double early = 0.5 * (w.early.passShiftMs + w.early.failShiftMs);
        double late = 0.5 * (w.late.passShiftMs + w.late.failShiftMs);
        CHECK_MSG(std::fabs(early - j.earlyMs) < 0.006 && std::fabs(late - j.lateMs) < 0.006 && std::fabs((late - early) - j.windowMs) < 0.006,
                  "input #" + std::to_string(j.input));
        auto const& e = kExpect[n];
        CHECK(e.input == j.input);
        CHECK_MSG(w.early.edge.cause == e.early && w.early.edge.laterInputs == e.earlyLater,
                  "#" + std::to_string(j.input) + " early " + name(w.early.edge.cause) + " " + std::to_string(w.early.edge.laterInputs));
        CHECK_MSG(w.late.edge.cause == e.late && w.late.edge.laterInputs == e.lateLater,
                  "#" + std::to_string(j.input) + " late " + name(w.late.edge.cause) + " " + std::to_string(w.late.edge.laterInputs));
        for (auto const* side : {&w.early, &w.late}) {
            if (side->edge.cause == EdgeCause::Self) ++self;
            else if (side->edge.cause == EdgeCause::Downstream) ++downstream;
        }
        std::printf("  owner row %2d  #%d %-7s frame %.0f  %.2f ms [%+.2f,%+.2f]  early %s/%dL  late %s/%dL | %s\n", j.ownerRow, j.input, j.kind, j.frame,
                    late - early, early, late, name(w.early.edge.cause), w.early.edge.laterInputs, name(w.late.edge.cause), w.late.edge.laterInputs,
                    p.describe().c_str());
        // wave: no passing shift ever re-joined (`A`, never `R`): every row is connected to the next input
        cluster::ConnectInput ci;
        ci.haveOutcomes = true;
        ci.frame = j.frame;
        ci.horizonFrame = j.frame + 130.0;
        ci.nextFrame = next;
        ci.outcomes = &outs;
        CHECK(cluster::connectedNext(ci).value == cluster::Tri::Yes);
    }
    // regression pin of the OLD solver's data on this extract (not a target): 4 self, 10 downstream
    std::printf("  extract (7 owner rows, 14 edges): self %d, downstream %d\n", self, downstream);
    CHECK(self == 4 && downstream == 10);
}

// REAL LOG EXTRACT (same log, lines 9157-9171): the five jobs that were open when the real player
// died at frame 4025 on object -1 (attempt a6), their pass-0 outcome strings verbatim from the
// `GPRL solver: job K pass 0 resolved in N steps: ...` lines (`c` = cancelled by the v0.6.x
// pruning, `e` = finished in the death pause), their recorded frames and early limits from the
// `input #K ... frame=F ... early limit L` lines. v0.6.2 emitted FIVE MISS windows for this one death.
struct MissJob {
    int input;
    double frame;
    double earlyLimit;
    char const* outcomes;
};
const MissJob kMissJobs[] = {
    {595, 3930, 83.0, "-10D@22.0 -9D@55.0 -8D@56.0 -7D@57.0 -6D@58.0 -5D@59.0 -4D@91.0 -3D@92.0 -2D@93.0 -1D@94.0 +0D +1D@97.0e +2Ae +3Ae +4ce +5ce +6ce +7ce +8D@39.0 +9D@38.0 +10D@10.0"},
    {596, 3952, 22.0, "-10D@15.0 -9D@16.0 -8D@17.0 -7ce -6ce -5ce -4ce -3Ae -2Ae -1D@75.0e +0D +1D@72.0 +2D@71.0 +3D@70.0 +4D@69.0 +5D@37.0 +6D@36.0 +7D@35.0 +8D@34.0 +9D@33.0 +10D@10.0"},
    {597, 3969, 17.0, "-10D@15.0 -9D@16.0 -8D@17.0 -7D@18.0 -6D@19.0 -5D@20.0 -4D@52.0 -3D@53.0 -2D@54.0 -1D@55.0 +0D +1D@58.0e +2Ae +3Ae +4ce +5ce +6ce +7ce +8D@8.0 +9D@8.0 +10D@8.0"},
    {598, 3989, 20.0, "-10D@14.0 -9D@15.0 -8ce -7ce -6ce -5ce -4ce -3Ae -2Ae -1D@38.0e +0D +1D@35.0 +2D@34.0 +3D@33.0 +4D@32.0 +5D@31.0 +6D@6.0 +7D@6.0 +8D@6.0 +9D@6.0 +10D@6.0"},
    {599, 4004, 15.0, "-10D@11.0 -9D@12.0 -8D@13.0 -7D@14.0 -6D@15.0 -5D@16.0 -4D@17.0 -3D@18.0 -2D@19.0 -1D@20.0 +0D +1D@23.0e +2Ae +3Ae +4ce +5ce +6ce +7ce +8ce +9ce +10ce"},
};

void testOneDeathOneMiss() {
    SECTION("T-ATT-3 (Fable D6): the death at frame 4025 (jobs 595-599 of attempt a6) is ONE miss - the latest input with a passing run - not five");
    PlannerConfig cfg;
    std::vector<MissCandidate> candidates;
    int validMissWindows = 0;
    for (size_t n = 0; n < std::size(kMissJobs); ++n) {
        auto const& mj = kMissJobs[n];
        Job j{mj.input, mj.frame, "", mj.outcomes, 0.0, 0.0, 0.0, 0};
        auto outs = parse(j);
        CHECK(outs.size() == 20);
        PassPlanner p(cfg, mj.earlyLimit - cfg.neighbourMarginFrames);
        p.setEarlyLimitKind(LimitKind::Neighbour);
        p.nextPass();
        if (n + 1 < std::size(kMissJobs)) p.setLateLimit(kMissJobs[n + 1].frame - mj.frame - cfg.neighbourMarginFrames);
        p.ingest(outs, false, true);   // the control died with the real player
        auto w = p.result(mj.frame * kTickMs);
        CHECK(p.miss());
        bool hasPassRun = w.valid;
        if (hasPassRun) ++validMissWindows;
        candidates.push_back({mj.input, mj.frame, hasPassRun});
        std::printf("  #%d frame %.0f: miss window %s [%+.2f,%+.2f] ms | %s\n", mj.input, mj.frame, hasPassRun ? "valid" : "none",
                    hasPassRun ? w.earliestMs - mj.frame * kTickMs : 0.0, hasPassRun ? w.latestMs - mj.frame * kTickMs : 0.0, p.describe().c_str());
    }
    // v0.6.2 emitted a MISS window for every one of them
    CHECK(validMissWindows == 5);
    auto who = attributeMiss(candidates);
    CHECK(who.has_value() && *who == 599);
    // misses <= real deaths on the extract: one death, one miss, four miss_downstream results
    int misses = 0, downstream = 0;
    for (auto const& c : candidates) {
        if (!c.hasPassRun) continue;
        if (who && c.jobId == *who) ++misses;
        else ++downstream;
    }
    std::printf("  one real death (frame 4025, object -1): misses %d (was 5), miss_downstream %d\n", misses, downstream);
    CHECK(misses == 1 && downstream == 4);
}

}  // namespace

int main() {
    testOwnerRows();
    testOneDeathOneMiss();
    return gprl::test::finish("attribution_tests");
}
