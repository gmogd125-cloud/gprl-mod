#pragma once
// Lockstep COMPENSATION planner `gprl-clone-sa/6` (docs/SHIP_SOLVER.md §4 and §11 "controls/1";
// decisions SH-D6 / SH-D7 and CT-D1..CT-D9): the COMPENSATED / PLAYABLE window of a
// CONNECTED-CONTROL input (ship by default) measured without delayed replays.
//
// The question (owner prompts 2026-10-02 §4 and 2026-10-03 §7): "can a real player still survive
// this timing shift by naturally adjusting nearby Ship inputs?" - not "does the exact original
// macro survive if I move only one click?". For the measured MEMBER (one input at frame t; for a
// PHASE search the rigid pair press + release of one hold, `CompMember::rigid`) shifted by s the
// planner lets the next k logged inputs (the FOLLOWERS: up to a cluster break, inside the
// horizon) move by INDEPENDENT offsets d_1..d_k (ticks, within [s - offsetRangeTicks,
// s + offsetRangeTicks], order-preserving). Per shift (one slot of the walk):
//
//   1. BASE trial      the schedule of the nearest passing shift moved along by the difference
//                      (WARM start: a verified compensation one tick further), else every follower
//                      at +s (UNIFORM: the pair / chain semantics of sequence_adjusted.hpp)
//   2. PROBES P_j      the base with follower j moved one more tick (+probeOffsetTicks): only
//                      when no response model for k followers is known yet, or the cached one
//                      failed to produce a pass at this shift
//   3. RESPONSE MODEL  from the deviation of the base and each probe to the RECORDED run (dy, dvy)
//                      at a common frame, the least-squares offsets that return the ship to the
//                      recorded trajectory, rounded to the lattice -> verification trials V_1, V_2
//                      (never more than verifyRoundings per model); the per-tick response traces
//                      are kept per k and re-used by the other shifts (ship dynamics are piecewise
//                      linear, the response barely depends on the shift) - every candidate is still
//                      SIMULATED, a model never decides anything
//   4. nothing passed  a fixed later input acted before some death, an early follower could still
//                      act, or a schedule survived without re-joining, and more followers can move
//                      -> k + 1 from step 1; else FAIL (every schedule died inside its adapted
//                      span: "no compensation in range survives"), or UNDECIDED: the follower cap
//                      was reached (`sa_not_measured_budget`), a fixed input no follower may move
//                      acted (`sa_undecided`), or a schedule survived but none re-joined
//                      (`sa_survives_no_rejoin`)
//
// A PASS beyond the local window needs a RE-JOIN (core/solver/rejoin.hpp, CT-D2): exact, within
// the tolerance (alive `settleFrames` after it), or the level end. A trial that is merely alive
// at its look-ahead (`rejoin::Kind::None`) is SURVIVES_NO_REJOIN: never a pass of this window.
//
// SA(s) = pass (proof `local` / `rejoined` / `compensated`), fail, or undecided (budget, an
// invalid trial, a follower that never came before the horizon passed, cut by a restart, no
// re-join). The window is the contiguous pass run around 0 per side; W_local ⊆ W_SA: the walk
// decides only shifts beyond the local bracket (slots inside the local run are passes, the local
// planner's untested gap is skipped). The walk's range is the local range plus `extraRangeTicks`
// (a compensated window can be wider than the range the frozen search uses). Every result is an
// SAResult, so the status rules (timing_status.hpp) and the event builder
// (timing_result_event.cpp) are shared with the delayed-replay planner.
//
// EAGER: every died local shift gets its compensation search as soon as its local copy died (in
// lockstep the followers that acted are already logged), nearest shifts first, so a trial starts
// from a snapshot at most ~horizon + range old. The ENGINE (src/solver/CloneEngine
// runCompensation) spawns the trials from the history ring, proves the catch-up with a delayed
// control that must match the live player step by step, feeds the re-join tracker and the
// deviation samples off the ring and ingests outcomes; `runCompAgainstOracle` is the reference
// driver on the pull oracle.
//
// PURE C++20 (no Geode includes); host-tested in tests/compensation_tests.cpp and the Ship pack
// tests/ship_pack_tests.cpp. Every loop is bounded: slots <= 2 x rangeTicks, trials per input
// <= maxTrialsPerInput, followers <= maxFollowers, verifications <= verifyRoundings per model.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "boundary_search.hpp"
#include "pass_planner.hpp"
#include "rejoin.hpp"
#include "sequence_adjusted.hpp"
#include "timing_status.hpp"

namespace gprl::solver::comp {

constexpr char const* kCompSolverVersion = "gprl-clone-sa/6";
/// Followers one schedule may move at most (the planner's arrays; CompConfig::maxFollowers <= this).
constexpr int kMaxFollowers = 6;

/// Every threshold in ONE versioned object (DEV DEFAULTS; docs/SHIP_SOLVER.md §4.2, §11.4).
struct CompConfig {
    char const* version = kCompSolverVersion;
    int maxShiftTicks = 10;               // the walk's range per side BEFORE extraRangeTicks (the local range)
    int extraRangeTicks = 6;              // controls/1: the compensated walk goes this much further than the local range
    int startFollowersMax = 3;            // a slot's first schedule moves at most this many followers; more join on causal evidence
    int maxRebases = 2;                   // CT-D5: per family, the search continues from its best trial at most this often
    int maxFollowers = 5;                 // a COMPUTE cap, not a causal rule: reaching it with more followers
                                          // available ends the shift `sa_not_measured_budget`, never a fail (3 until v0.14.x)
    int offsetRangeTicks = 6;             // a follower's offset stays within [min(s, 0) - this, max(s, 0) + this]: between
                                          // "moved along with the member" and "as recorded", and a little beyond (s +- 3 until v0.14.x)
    int probeOffsetTicks = 1;             // the response probe moves one follower by this much more
    int maxTrialsPerInput = 64;           // base + probes + verifications charged to one input (40 until v0.14.5)
    int verifyRoundings = 2;              // verification trials per response model at most
    int lookaheadSlots = 1;               // undecided slots beyond the frontier that may run trials at once (per side)
    double priorityMaxLocalWidthTicks = 12.0;   // the engine skips the search for wider local windows (sa_not_measured_budget)
    double horizonFrames = 120.0;         // followers within t + this many frames (0.5 s)
    double lookAheadFrames = 186.0;       // a trial's look-ahead after its LAST moved input: the settle length (rejoin.parallelFrames)
                                          // + one step of slack; a re-join ends the trial earlier
    double neighbourMarginFrames = 0.0024;   // two inputs keep at least this distance (0.01 ms)
    int convergeSteps = 16;               // steps a trial must match the recorded run to count as re-joined exactly
    double ridge = 1e-6;                  // least-squares regularisation (k offsets, 2 equations)
    // controls/1 (docs/SHIP_SOLVER.md §11)
    bool requireRejoin = true;            // CT-D2: a pass beyond the local window must re-join (rejoin.hpp)
    bool warmStart = true;                // CT-D5: a slot's first trial continues the nearest passing schedule
    bool reuseResponse = true;            // CT-D5: the probes' response traces are cached per k and re-used
    bool ownProbesLastResort = false;     // CT-D5: at the largest family, measure every follower's response at the slot itself
                                          // before it ends `sa_survives_no_rejoin` (k + 2 more trials; off: the budget goes to the other side)
    rejoin::RejoinConfig rejoin;          // the re-join tolerances the ENGINE / the test adapter apply (the planner reads kinds)

    constexpr int rangeTicks() const { return maxShiftTicks + (extraRangeTicks > 0 ? extraRangeTicks : 0); }
};
inline constexpr CompConfig kComp{};

/// A logged input after the member (the attempt's input log, time order).
struct CompFollower {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = true;
    bool breakBefore = false;   // a cluster break between the previous input and this one: never moved
};

/// An input moved RIGIDLY with the member (the same shift): the release of the member press in a
/// PHASE search (the whole hold shifted, its duration kept).
struct CompRigid {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = false;
};

/// The measured input (or, with `rigid`, the measured control).
struct CompMember {
    uint32_t id = 0;
    double frame = 0.0;
    bool down = true;
    bool subtick = false;
    double earlyLimitFrames = kNaN;       // |shift| limit on the early side (history / previous input), NaN = range
    LimitKind earlyLimitKind = LimitKind::History;
    std::vector<CompRigid> rigid;         // ascending frames, all after `frame`; empty for an input's own window
};

enum class Role : uint8_t { Uniform, Probe, Verify, Warm };
constexpr char const* name(Role r) {
    switch (r) {
        case Role::Uniform: return "uniform";
        case Role::Probe: return "probe";
        case Role::Verify: return "verify";
        case Role::Warm: return "warm";
    }
    return "uniform";
}

/// One compensated schedule to simulate.
struct CompTrial {
    int id = -1;
    double shiftFrames = 0.0;
    int followers = 0;                    // k
    Role role = Role::Uniform;
    int probeIndex = -1;                  // Probe: which follower (0-based) carries the extra tick
    SAAdaptation adaptation = SAAdaptation::Pair;   // by the offsets: every follower at the shift = pair/chain_k, else comp_k
    std::vector<SAMoved> moved;           // the member first, its rigid inputs, then the followers at recorded + offset
    std::vector<double> offsetsFrames;    // per follower, relative to its recorded frame
    double assessFrame = 0.0;             // the first fixed input after the moved ones (or last moved + horizon)
    double lookAheadFrame = 0.0;          // survived-only when alive here with every moved input applied (a re-join ends the trial earlier)
    double attributeAfterFrame = 0.0;     // laterFixed counts non-moved inputs recorded after this frame
    double devFromFrame = 0.0;            // deviation samples wanted from this frame on (the member's shifted frame)
    double lastMovedFrame = 0.0;          // the latest moved input: the re-join tracker starts after it was applied
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
    // Pass: how the trial re-joined the recorded run (rejoin.hpp). `None` = alive at the look-ahead
    // without a re-join (SURVIVES_NO_REJOIN): with requireRejoin never a pass of the window.
    rejoin::Kind rejoin = rejoin::Kind::None;
    double rejoinFrame = kNaN;    // first frame of the re-join streak (the level end for LevelEnd)
    double rejoinErrY = kNaN;     // largest |dy| / |dvy| over that streak (0 for exact / level end)
    double rejoinErrVy = kNaN;
    double deathFrame = kNaN;     // Died: end of the step the clone died in
    int laterFixed = 0;           // Died: non-moved later inputs applied before the death
    int objectId = -1;
    double deathX = kNaN;
    bool extension = false;
    std::vector<DevSample> dev;   // ascending frames, from devFromFrame to the end (empty when nothing was sampled)
    std::string reason;           // Invalid: why
    bool notTested = false;       // Invalid: the trial could not be decided (cancelled, crossed): an
                                  // undecided shift (`sa_undecided`), not a failed proof (`sa_invalid_trials`)
    bool exactRejoin() const { return rejoin == rejoin::Kind::Exact; }
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
    /// Followers known so far (within the horizon, before a break, up to the cap).
    int followers() const { return followersUsable(); }
    /// The most followers any PASS of this planner moved (0 = none): the causal span of the member.
    int followersUsed() const;
    CompConfig const& config() const { return m_cfg; }
    CompMember const& member() const { return m_m; }

    SAResult result() const;
    std::string describe() const;

    // ---- pure helpers (host-tested) ----
    /// The deviation sample at or before `frame` (the latest one); false when none.
    static bool devAt(std::vector<DevSample> const& dev, double frame, DevSample& out);
    /// Least-squares offsets (ticks, NOT rounded) that zero `base` under the columns `J` (one per
    /// follower; a column of NaN = unknown response, its offset stays 0): minimises
    /// |base + J d|^2 + ridge |d|^2. Bounded by k <= kMaxFollowers.
    static std::vector<double> solveOffsets(DevSample const& base, std::vector<DevSample> const& columns, double probe, double ridge);

private:
    enum class St : uint8_t { Open, Pass, Fail, Undecided };
    enum class Stage : uint8_t { None, Base, Probes, Verify, Wait, Done };
    struct Slot {
        double mag = 0.0;
        // the local view
        bool localKnown = false;
        ShiftOutcome local;
        // the compensation search
        Stage stage = Stage::None;
        int k = 0;
        std::vector<double> base;         // the base trial's follower offsets (uniform or warm)
        int baseId = -1;
        bool baseWarm = false;
        std::vector<int> probeIds;        // per follower: this slot's own probe around `base` (-1 = none)
        std::vector<double> probeSteps;   // the offset each probe actually carries on its follower (+-probeOffsetTicks, 0 = none)
        bool budgetCut = false;           // a probe / candidate of this slot was refused by the trial budget: never a fail
        bool wantOwnProbes = false;       // the cached response's candidates failed: measure EVERY follower here
        bool ownAll = false;              // that last resort ran for the current k
        bool modelBuilt = false;          // the candidates of the current model were generated
        int bestId = -1;                  // the trial of this slot (current family) that got furthest: alive closest to
        double bestScore = -1e300;        // the recorded run, else the one that died latest (scoreOf)
        int rebases = 0;                  // times the family's base was replaced by its best trial
        std::vector<int> verifyIds;       // the verification in flight
        std::vector<int> allVerifyIds;    // every finished verification of the current k
        bool fixedActed = false;          // some trial of this slot died after a fixed later input acted
        bool survived = false;            // some trial of this slot was alive at its look-ahead without re-joining
        std::vector<std::vector<double>> triedOffsets;   // offset vectors already simulated (dedupe)
        // the decision
        St st = St::Open;
        SAAdaptation used = SAAdaptation::Local;
        SAProof proof = SAProof::Local;
        std::vector<double> offsets;      // of the pass (empty for local) or the failing verification
        rejoin::Kind rejoin = rejoin::Kind::None;   // of the pass (None for a local pass)
        double rejoinFrame = kNaN;
        double rejoinErrY = kNaN;
        double rejoinErrVy = kNaN;
        bool haveDeath = false;
        double deathFrame = kNaN;
        int laterFixed = 0;
        int objectId = -1;
        double deathX = kNaN;
        bool extension = false;
        status::Reason why = status::Reason::SaUndecided;
        bool neighbourLimit = false;      // no legal schedule at this shift (a fixed input in the way): a `neighbour` stop
        std::vector<std::vector<double>> pendingCands;   // verification candidates of the current model not yet requested
    };
    struct Side {
        std::vector<Slot> slots;          // ascending magnitude
        double limit = 0.0;               // |shift| limit (frames)
        LimitKind limitKind = LimitKind::Range;
        // the limit the WALK uses: `limit`, or the local range when the local side is OPEN (it never
        // failed inside the range the frozen search uses: nothing to compensate, so the side ends at
        // the local range the way it always did and the extra range is not searched; CT-D6)
        double walkLimit = 0.0;
        LimitKind walkKind = LimitKind::Range;
        EdgeStop localOpenStop = EdgeStop::Range;   // what limited the local side when it is open (range / neighbour / ...)
        double localRun = 0.0;
        double localFail = kNaN;
    };
    /// The probes' response per tick of offset on one follower, as a trace over frames (CT-D5).
    struct Response {
        std::vector<std::vector<DevSample>> columns;   // [follower][frame-ordered samples of (dP - dBase) / probe]
    };

    Slot* slotAt(int side, double mag);
    bool decideLocal(int side, Slot& s);
    void advanceSlot(int side, Slot& s, bool frontier);
    void advanceSide(int side);
    void advanceAll();
    std::vector<double> offsetsOf(int trialId) const;
    bool canFollowerAct(double s, double deathFrame, int fromFollower = 0) const;
    int followersUsable() const;
    bool moreFollowersBeyondCap() const;
    bool legalOffsets(double s, std::vector<double>& offsets, double& lastMoved) const;
    int request(int side, Slot& slot, double s, Role role, int probeIndex, std::vector<double> offsets);
    CompOutcome const* known(int id) const;
    bool isPass(CompOutcome const& o) const;
    void passSlot(Slot& slot, double s, CompOutcome const& o, std::vector<double> offsets);
    void afterVerifications(int side, Slot& slot, double s);
    void requestProbe(int side, Slot& slot, double s, int follower);
    void undecide(int side, Slot& slot, status::Reason why);
    void noteOutcome(Slot& slot, int trialId, CompOutcome const& o);
    double scoreOf(CompOutcome const& o) const;
    void rebase(Slot& slot, int trialId, std::vector<double> offsets);
    bool ownColumn(Slot const& slot, int follower, std::vector<DevSample>& col) const;
    Response modelOf(Slot const& slot) const;
    bool probesInFlight(int k, Slot const* except) const;
    void grow(int side, Slot& slot, double s);
    std::vector<std::vector<double>> candidates(Slot const& slot, double s, Response const* model) const;
    bool warmBase(int side, Slot const& slot, double s, std::vector<double>& base) const;
    void resetModel(Slot& slot);
    void failSlot(Slot& slot, CompOutcome const* death);
    SAEdge edgeOf(int side) const;
    bool openCut(int side, Slot const& slot) const;
    double lastRigidFrame() const { return m_m.rigid.empty() ? m_m.frame : m_m.rigid.back().frame; }

    CompConfig m_cfg{};
    CompMember m_m{};
    std::vector<CompFollower> m_followers;
    bool m_breakSeen = false;             // a follower across a break arrived: no more followers
    bool m_capSeen = false;               // a follower beyond maxFollowers arrived (inside the horizon, no break): the cap binds
    double m_now = -1e300;
    bool m_isolated = false;
    bool m_localFinal = false;
    Side m_side[2];
    std::map<int, CompOutcome> m_known;
    std::map<int, std::pair<int, double>> m_trialSlot;   // trial id -> (side, mag)
    std::map<int, std::vector<double>> m_trialOffsets;   // trial id -> its follower offsets (handed out)
    std::map<int, Response> m_response;                  // k -> the cached response model
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
/// on the test oracle and fills the outcome (kind, re-join, death, deviation samples). Returns the
/// trials run. `rigid` > 0: the member is the rigid group movingIndex .. movingIndex + rigid (a
/// PHASE search); `local` is then null (no lockstep facts exist for a pair).
template <class Trial>
int runCompAgainstOracle(InputSchedule const& schedule, size_t movingIndex, PassPlanner const* local, CompConfig const& cfg, std::vector<size_t> const& breaks,
                         Trial&& trial, CompPlanner& out, size_t rigid) {
    CompMember m;
    m.id = static_cast<uint32_t>(movingIndex + 1);
    m.frame = schedule.inputs[movingIndex].tMs / kTickMs;
    m.down = schedule.inputs[movingIndex].down;
    // the engine's rule (CloneEngine::input): the distance to the previous input (the past is
    // fixed), else the attempt start - NOT the local planner's limit, which is cut at its range
    if (movingIndex > 0) m.earlyLimitFrames = m.frame - schedule.inputs[movingIndex - 1].tMs / kTickMs - cfg.neighbourMarginFrames;
    else m.earlyLimitFrames = m.frame;
    m.earlyLimitKind = movingIndex > 0 ? LimitKind::Neighbour : LimitKind::AttemptStart;
    for (size_t r = 1; r <= rigid && movingIndex + r < schedule.inputs.size(); ++r) {
        auto const& in = schedule.inputs[movingIndex + r];
        m.rigid.push_back({static_cast<uint32_t>(movingIndex + r + 1), in.tMs / kTickMs, in.down});
    }
    std::vector<CompFollower> followers;
    for (size_t i = movingIndex + 1 + m.rigid.size(); i < schedule.inputs.size(); ++i) {
        CompFollower f;
        f.id = static_cast<uint32_t>(i + 1);
        f.frame = schedule.inputs[i].tMs / kTickMs;
        f.down = schedule.inputs[i].down;
        for (size_t b : breaks) if (b == i) f.breakBefore = true;
        followers.push_back(f);
    }
    out = CompPlanner(cfg, m, followers);
    out.setNow(1e300);
    if (local) out.finalizeLocal(local->outcomes(), local->late().limitKnown ? local->late().limit : kNaN);
    else out.finalizeLocal({}, kNaN);
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
template <class Trial>
int runCompAgainstOracle(InputSchedule const& schedule, size_t movingIndex, PassPlanner const& local, CompConfig const& cfg, std::vector<size_t> const& breaks,
                         Trial&& trial, CompPlanner& out) {
    return runCompAgainstOracle(schedule, movingIndex, &local, cfg, breaks, std::forward<Trial>(trial), out, 0);
}

}  // namespace gprl::solver::comp
