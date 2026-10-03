#pragma once
// Lockstep COMPENSATION planner `gprl-clone-sa/5` (docs/SHIP_SOLVER.md §4, decisions SH-D6 / SH-D7;
// PROMPT §2, §4, §5): the sequence-adjusted window of a CONNECTED-CONTROL input (ship by default)
// measured without delayed replays.
//
// The question (PROMPT §4): "could a real player reasonably compensate for this timing shift
// using nearby inputs?" - not "does the exact original macro still survive". For input i (frame
// t) shifted by s the planner lets the next k logged inputs (the FOLLOWERS, within the horizon,
// up to a cluster break) move by INDEPENDENT offsets d_1..d_k (ticks, within
// [s - offsetRangeTicks, s + offsetRangeTicks], order-preserving):
//
//   1. uniform  U_k   every follower at +s (the pair / chain semantics of sequence_adjusted.hpp)
//   2. probes   P_j   U_k with follower j moved one more tick (+probeOffsetTicks)
//   3. response model: from the deviation of U_k and each P_j to the RECORDED run at a common
//      frame (Δy, Δvy), the least-squares offsets that return the ship to the recorded
//      trajectory, rounded to the lattice -> verification trials V_1, V_2 (never more than
//      verifyRoundings); a pass is a pass only when the simulation says so (re-joined / settled)
//   4. no verification survived: a fixed later input acted before some death and more followers
//      can move -> k + 1 from step 1; else FAIL (self: "no compensation in range survives")
//
// SA(s) = pass (proof `local` / `rejoined` / `compensated` / `survived`), fail, or undecided
// (budget, an invalid trial, a follower that never came before the horizon passed, cut by a
// restart). The window is the contiguous pass run around 0 per side; W_local ⊆ W_SA: the walk
// decides only shifts beyond the local bracket (slots inside the local run are passes, the local
// planner's untested gap is skipped). Every result is an SAResult, so the status rules
// (timing_status.hpp) and the event builder (timing_result_event.cpp) are shared with the
// delayed-replay planner.
//
// EAGER: every died local shift gets its compensation search as soon as its local copy died (in
// lockstep the followers that acted are already logged), nearest shifts first, so a trial starts
// from a snapshot at most ~horizon + maxShift old. The ENGINE (src/solver/CloneEngine
// runCompensation) spawns the trials from the history ring, proves the catch-up with a delayed
// control that must match the live player step by step, reads the deviation samples off the ring
// and ingests outcomes; `runCompAgainstOracle` is the reference driver on the pull oracle.
//
// PURE C++20 (no Geode includes); host-tested in tests/compensation_tests.cpp and the Ship pack
// tests/ship_pack_tests.cpp. Every loop is bounded: slots <= 2 x maxShiftTicks, trials per input
// <= maxTrialsPerInput, followers <= maxFollowers, verifications <= verifyRoundings.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "boundary_search.hpp"
#include "pass_planner.hpp"
#include "sequence_adjusted.hpp"
#include "timing_status.hpp"

namespace gprl::solver::comp {

constexpr char const* kCompSolverVersion = "gprl-clone-sa/5";

/// Every threshold in ONE versioned object (DEV DEFAULTS; docs/SHIP_SOLVER.md §4.2).
struct CompConfig {
    char const* version = kCompSolverVersion;
    int maxShiftTicks = 10;               // the walk's range per side (the local range)
    int maxFollowers = 3;                 // comp1..comp3
    int offsetRangeTicks = 3;             // a follower's offset stays within s +- this
    int probeOffsetTicks = 1;             // the response probe moves one follower by this much more
    int maxTrialsPerInput = 64;           // uniform + probes + verifications charged to one input (40 until v0.14.5: a fully
                                          // compensable input with 2 followers needs 16 slots x 3 trials = 48 and ended
                                          // `sa_not_measured_budget`, i.e. excluded; trials are ~2.7x shorter since flySeconds 0.75)
    int verifyRoundings = 2;              // verification trials per (shift, k) at most
    int lookaheadSlots = 1;               // undecided slots beyond the frontier that may run trials at once (per side)
    double priorityMaxLocalWidthTicks = 12.0;   // the engine skips the search for wider local windows (sa_not_measured_budget)
    double horizonFrames = 120.0;         // followers within t + this many frames (0.5 s)
    double lookAheadFrames = 130.0;       // a trial's look-ahead after its LAST moved input (the engine
                                          // raises it to the settle length when the settled look-ahead is on)
    double neighbourMarginFrames = 0.0024;   // two inputs keep at least this distance (0.01 ms)
    int convergeSteps = 16;               // steps a trial must match the recorded run to count as re-joined
    double ridge = 1e-6;                  // least-squares regularisation (k offsets, 2 equations)
};
inline constexpr CompConfig kComp{};

/// A logged input after the member (the attempt's input log, time order).
struct CompFollower {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = true;
    bool breakBefore = false;   // a cluster break between the previous input and this one: never moved
};

/// The measured input.
struct CompMember {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = true;
    bool subtick = false;
    double earlyLimitFrames = kNaN;       // |shift| limit on the early side (history / previous input), NaN = range
    LimitKind earlyLimitKind = LimitKind::History;
};

enum class Role : uint8_t { Uniform, Probe, Verify };
constexpr char const* name(Role r) { return r == Role::Uniform ? "uniform" : r == Role::Probe ? "probe" : "verify"; }

/// One compensated schedule to simulate.
struct CompTrial {
    int id = -1;
    double shiftFrames = 0.0;
    int followers = 0;                    // k
    Role role = Role::Uniform;
    int probeIndex = -1;                  // Probe: which follower (0-based) carries the extra tick
    SAAdaptation adaptation = SAAdaptation::Pair;   // uniform: pair/chain_k; probe/verify: comp_k
    std::vector<SAMoved> moved;           // the member first, then the followers at recorded + offset
    std::vector<double> offsetsFrames;    // per follower, relative to its recorded frame
    double assessFrame = 0.0;             // the first fixed input after the moved ones (or last moved + horizon)
    double lookAheadFrame = 0.0;          // pass when alive here with every moved input applied (or re-joined earlier)
    double attributeAfterFrame = 0.0;     // laterFixed counts non-moved inputs recorded after this frame
    double devFromFrame = 0.0;            // deviation samples wanted from this frame on (the member's shifted frame)
};

/// Deviation from the recorded run after one step: y and y velocity of the trial minus the real player's.
struct DevSample {
    double frame = 0.0;
    double dy = 0.0;
    double dvy = 0.0;
};

struct CompOutcome {
    enum class Kind : uint8_t { Pass, Died, Invalid };
    int id = -1;
    Kind kind = Kind::Invalid;
    bool rejoined = false;        // Pass: re-joined the recorded run (else settled / survived)
    double deathFrame = kNaN;     // Died: end of the step the clone died in
    int laterFixed = 0;           // Died: non-moved later inputs applied before the death
    int objectId = -1;
    double deathX = kNaN;
    bool extension = false;
    std::vector<DevSample> dev;   // ascending frames, from devFromFrame to the end (empty when nothing was sampled)
    std::string reason;           // Invalid: why
    bool notTested = false;       // Invalid: the trial could not be decided (unsettled at the look-ahead, cancelled): an
                                  // undecided shift (`sa_undecided`), not a failed proof (`sa_invalid_trials`)
};

class CompPlanner {
public:
    CompPlanner() = default;
    CompPlanner(CompConfig const& cfg, CompMember member, std::vector<CompFollower> followers = {});

    // ---- facts from the engine (any order, any time before finish) ----
    /// A later input was logged (time order). Ignored beyond the horizon or after a break.
    void addFollower(CompFollower f);
    /// The engine's current frame: decides when "no follower came" (the horizon passed).
    void setNow(double frame);
    /// One lockstep local outcome (a shifted copy that died / passed / was not tested).
    void noteLocal(ShiftOutcome const& o);
    /// The local pass resolved: its final outcomes and limits (W_local ⊆ W_SA from here on).
    void finalizeLocal(std::vector<ShiftOutcome> const& all, double lateLimitFrames);
    bool localFinal() const { return m_localFinal; }

    // ---- trials ----
    /// Up to `max` trials to run next (nearest shifts first). Empty = nothing right now.
    std::vector<CompTrial> nextBatch(int max);
    void ingest(CompOutcome const& outcome);
    /// No further trials: every open side ends undecided with `why`.
    void finish(status::Reason why);
    bool done() const;
    bool finished() const { return m_finished; }
    bool hasPending() const { return !m_finished && !m_queue.empty(); }
    int outstanding() const { return m_outstanding; }
    int requested() const { return m_requested; }
    int trials() const { return m_requested; }
    bool isolated() const { return m_isolated; }
    /// Followers known so far (within the horizon, before a break).
    int followers() const { return static_cast<int>(m_followers.size()); }

    SAResult result() const;
    std::string describe() const;

    // ---- pure helpers (host-tested) ----
    /// The deviation sample at or before `frame` (the latest one); false when none.
    static bool devAt(std::vector<DevSample> const& dev, double frame, DevSample& out);
    /// Least-squares offsets (ticks, NOT rounded) that zero `base` under the columns `J` (one per
    /// follower; a column of NaN = unknown response, its offset stays 0): minimises
    /// |base + J d|^2 + ridge |d|^2. Bounded by k <= 3.
    static std::vector<double> solveOffsets(DevSample const& base, std::vector<DevSample> const& columns, double probe, double ridge);

private:
    enum class St : uint8_t { Open, Pass, Fail, Undecided };
    enum class Stage : uint8_t { None, Uniform, Probes, Verify, Done };
    struct Slot {
        double mag = 0.0;
        // the local view
        bool localKnown = false;
        ShiftOutcome local;
        // the compensation search
        Stage stage = Stage::None;
        int k = 0;
        int uniformId = -1;
        std::vector<int> probeIds;
        std::vector<int> verifyIds;
        int verifications = 0;
        bool fixedActed = false;          // some trial of this slot died after a fixed later input acted
        std::vector<std::vector<double>> triedOffsets;   // offset vectors already simulated (dedupe)
        // the decision
        St st = St::Open;
        SAAdaptation used = SAAdaptation::Local;
        SAProof proof = SAProof::Local;
        std::vector<double> offsets;      // of the pass (empty for local) or the failing verification
        bool haveDeath = false;
        double deathFrame = kNaN;
        int laterFixed = 0;
        int objectId = -1;
        double deathX = kNaN;
        bool extension = false;
        status::Reason why = status::Reason::SaUndecided;
        bool neighbourLimit = false;      // no legal schedule at this shift (a fixed input in the way): a `neighbour` stop
    };
    struct Side {
        std::vector<Slot> slots;          // ascending magnitude
        double limit = 0.0;               // |shift| limit (frames)
        LimitKind limitKind = LimitKind::Range;
        double localRun = 0.0;
        double localFail = kNaN;
    };

    Slot* slotAt(int side, double mag);
    bool decideLocal(int side, Slot& s);
    void advanceSlot(int side, Slot& s);
    void advanceSide(int side);
    void advanceAll();
    std::vector<double> offsetsOf(int trialId) const;
    bool canFollowerAct(double s, double deathFrame, int fromFollower = 0) const;
    int followersUsable() const;
    bool legalOffsets(double s, std::vector<double>& offsets, double& lastMoved) const;
    int request(int side, Slot& slot, double s, Role role, int probeIndex, std::vector<double> offsets);
    CompOutcome const* known(int id) const;
    void decideFromVerifications(int side, Slot& slot, double s);
    std::vector<std::vector<double>> candidates(Slot const& slot, double s) const;
    void failSlot(Slot& slot, CompOutcome const* death);
    SAEdge edgeOf(int side) const;

    CompConfig m_cfg{};
    CompMember m_m{};
    std::vector<CompFollower> m_followers;
    bool m_breakSeen = false;             // a follower across a break arrived: no more followers
    double m_now = -1e300;
    bool m_isolated = false;
    bool m_localFinal = false;
    Side m_side[2];
    std::map<int, CompOutcome> m_known;
    std::map<int, std::pair<int, double>> m_trialSlot;   // trial id -> (side, mag)
    std::map<int, std::vector<double>> m_trialOffsets;   // trial id -> its follower offsets (handed out)
    std::deque<CompTrial> m_queue;
    int m_nextId = 0;
    int m_outstanding = 0;
    int m_requested = 0;
    bool m_finished = false;
    status::Reason m_finishReason = status::Reason::SaUndecided;
};

/// The engine's spawn loop (pure, like spawnSATrials): at most `parallel - running` iterations,
/// each spawning exactly one trial or breaking. Returns the number spawned.
template <class Alloc, class Spawn>
int spawnCompTrials(CompPlanner& planner, int running, int parallel, Alloc&& alloc, Spawn&& spawn) {
    int spawned = 0;
    for (int guard = 0; guard < parallel && running + spawned < parallel && planner.hasPending(); ++guard) {
        int idx = alloc();
        if (idx < 0) break;
        auto batch = planner.nextBatch(1);
        if (batch.empty()) break;
        if (!spawn(idx, batch.front())) {
            CompOutcome o;
            o.id = batch.front().id;
            o.kind = CompOutcome::Kind::Invalid;
            o.reason = "could not be spawned";
            planner.ingest(o);
            break;
        }
        ++spawned;
    }
    return spawned;
}

/// Reference driver for host tests: the member `movingIndex` of `schedule` with its finished
/// local planner; followers = the schedule's inputs after it (within the horizon, `breaks` = indices
/// with a cluster break before them). `trial(sched, trial, outcome)` runs one compensated schedule
/// on the test oracle and fills the outcome (kind, death, deviation samples). Returns the trials run.
template <class Trial>
int runCompAgainstOracle(InputSchedule const& schedule, size_t movingIndex, PassPlanner const& local, CompConfig const& cfg, std::vector<size_t> const& breaks,
                         Trial&& trial, CompPlanner& out) {
    CompMember m;
    m.id = static_cast<uint32_t>(movingIndex + 1);
    m.frame = schedule.inputs[movingIndex].tMs / kTickMs;
    m.down = schedule.inputs[movingIndex].down;
    m.earlyLimitFrames = local.early().limit;
    m.earlyLimitKind = movingIndex > 0 ? LimitKind::Neighbour : LimitKind::AttemptStart;
    std::vector<CompFollower> followers;
    for (size_t i = movingIndex + 1; i < schedule.inputs.size(); ++i) {
        CompFollower f;
        f.id = static_cast<uint32_t>(i + 1);
        f.frame = schedule.inputs[i].tMs / kTickMs;
        f.down = schedule.inputs[i].down;
        for (size_t b : breaks) if (b == i) f.breakBefore = true;
        followers.push_back(f);
    }
    out = CompPlanner(cfg, m, followers);
    out.setNow(1e300);
    out.finalizeLocal(local.outcomes(), local.late().limitKnown ? local.late().limit : kNaN);
    int trials = 0;
    int const maxRounds = cfg.maxTrialsPerInput + 8;
    for (int round = 0; round < maxRounds && !out.done(); ++round) {
        auto batch = out.nextBatch(1 << 20);
        if (batch.empty()) break;
        for (auto const& t : batch) {
            InputSchedule sched = schedule;
            for (auto const& mv : t.moved) {
                size_t idx = static_cast<size_t>(mv.id) - 1;
                if (idx < sched.inputs.size()) sched.inputs[idx].tMs = mv.frame * kTickMs;
            }
            CompOutcome o;
            o.id = t.id;
            trial(sched, t, o);
            ++trials;
            out.ingest(o);
        }
    }
    return trials;
}

}  // namespace gprl::solver::comp
