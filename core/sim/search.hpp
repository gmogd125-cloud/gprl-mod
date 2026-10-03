#pragma once
// The reference run: a beam search over press / release decisions in the isolated simulator
// (docs/BACKGROUND_ANALYZER_DESIGN.md §4.3, AN-D5). PURE C++20, host-tested in
// tests/sim_search_tests.cpp.
//
// Per tick every beam branch spawns {none} plus {press} (when the button is up) or {release}
// (when it is down); the engine steps each child one tick. Dead children are dropped. Children
// are ranked by (x progress, score) where
//
//   score = min(coast, H) - inputPenalty * inputs + (settled ? settledBonus : 0)
//
// and `coast` is the projected survival: how many ticks the child survives WITHOUT another
// input (a rollout, capped at H = lookaheadTicks). x is time-driven in GD, so every branch at
// the same tick has the same x; "x progress" only orders branches once speeds differ. The coast
// is the x the branch is guaranteed to reach, which is what the design calls progress: a branch
// about to die ranks below any branch with a safe future, so the beam steers before the death.
// Fewer inputs break ties (soft, not lexicographic: an old lineage with fewer inputs cannot
// starve the main line's children), then the settled rule of core/solver/settle.hpp, then the
// stable order (parent rank, "none" before an input). Children are de-duplicated by
// Engine::stateHash; at overflow a coarse diversity cap keeps the beam spread over (y, yVel).
//
// Every `checkpointEveryPercent` of x the search sets a checkpoint from the first settled branch
// (or the best branch when nothing is settled, flagged). An empty beam restarts from the last
// checkpoint with the width doubled up to the cap; at the cap it backtracks one checkpoint, up to
// `maxCheckpointBacktrack`; when that fails too the stretch [checkpoint x, farthest death x] is
// recorded `unsolved` ("search_exhausted") and the search resumes `jumpMargin` units past the
// farthest death (a new segment). A branch entering an UnsupportedSpan ends the segment the same
// way ("unsupported": the span is listed in `skippedUnsupported`, never guessed).
//
// The restart state (gprl-sim/2, review H1): a StartPos at / after the skipped stretch (within
// `startPosReach` units of the restart x, outside every span, no dual / mirror / platformer) is
// GD's own state and wins. Otherwise the start is SYNTHETIC: gamemode / speed / size / gravity
// come from the last gamemode / speed / size / gravity objects with x <= the restart x (on top of
// the level start's), and y from candidates - for ground modes the floor (or the ceiling when
// upside down) then the top (bottom) of every solid under (over) the restart x, for flyers the
// middle of every free vertical gap around it, nearest to the last known y first - each probed
// for `startProbeTicks` ticks with no input and with the button held; the first candidate that
// survives either is kept (none -> the first, flagged). Synthetic starts are logged
// ("synthetic start ...") and marked on the segment. Deterministic: stable sorts, no RNG.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine.hpp"
#include "result.hpp"
#include "world.hpp"

namespace gprl::sim {

struct SearchConfig {
    int beamWidth = 48;
    int beamWidthMax = 256;
    int maxShift = 10;                     // not used by the search itself (the job's windows config); kept for symmetry
    double checkpointEveryPercent = 1.0;
    uint64_t maxTicks = 400'000'000;       // engine ticks the whole search may spend (rollouts included); 0 = unlimited
    int lookaheadTicks = 48;               // H: rollout horizon of the coast score (0.2 s)
    double inputPenalty = 2.0;             // score ticks per input
    double settledBonus = 5.0;             // > 2 * inputPenalty so a settled 2-input branch beats an unsettled 1-input one
    int diversityCap = 3;                  // branches per coarse (mode, held, y / 16, yVel / 2) cell at overflow; 0 = off
    int maxCheckpointBacktrack = 2;        // checkpoints a restart may go back at the maximum width (per stretch of real progress)
    float jumpMargin = 60.f;               // units past a death / a span end where a new segment starts
    int maxRestarts = 10000;               // global guard
    float startPosReach = 600.f;           // a StartPos up to this far past the restart x replaces a synthetic start
    int startProbeTicks = 120;             // survival probe of a synthetic start candidate (0.5 s: one cube jump lands)
};

/// One continuous stretch of the reference run: from a start state (the level start, or the
/// player placed past an unsolved / unsupported stretch) to where it ended.
struct ReferenceSegment {
    StartState start;
    int tickOffset = 0;                    // level tick of this segment's tick 0: the previous segment's end tick
                                           // + gapTicksBefore (gprl-sim/2; /1 laid segments end to end and lost
                                           // the time of every skipped stretch)
    int ticks = 0;                         // whole ticks simulated in the segment
    float x0 = 0.f, x1 = 0.f;              // x covered [x0, x1]
    std::vector<RecordedInput> inputs;     // step relative to the segment (1 = the first step)
    std::vector<RecordedTick> trajectory;  // one per step (step relative to the segment)
    bool completed = false;                // reached World::endX
    std::string endReason;                 // "completed" | "unsupported" | "unsolved" | "budget" | "stopped"
    // ---- gprl-sim/2 (additive) ----
    int gapTicksBefore = 0;                // level ticks NOT simulated between the previous segment's end x and x0:
                                           // the speed-integrated travel time (ticksAcross) of the skipped stretch
    std::string startKind = "level";       // "level" (the world's start) | "startpos" (a StartPos at / after the
                                           // skipped stretch) | "synthetic" (state from the portals before x0, y
                                           // from a candidate that survived the probe; flagged in the debug lines)
    bool syntheticStart = false;           // startKind == "synthetic"
};

struct ReferenceRun {
    std::vector<RecordedInput> inputs;     // flattened over the segments, steps global (segment tickOffset added)
    std::vector<RecordedTick> ticks;       // flattened, steps global
    std::vector<ReferenceSegment> segments;
    bool reachedEnd = false;               // the last segment reached endX
    bool completed = false;                // reachedEnd AND no unsolved AND no skipped span: the run clears the level
    double solvedPercent = 0.0;            // x covered by the segments / endX * 100
    std::vector<CoverageSpan> unsolved;    // search failures (mechanic "search_exhausted" / "budget" / "stopped")
    std::vector<CoverageSpan> skippedUnsupported;   // World::unsupported spans the search jumped over
    int restarts = 0;
    int checkpoints = 0;
    int unsettledCheckpoints = 0;
    int replayMismatches = 0;              // segments whose committed inputs did NOT reproduce the beam's end on a plain replay
                                           // (an engine save/restore identity defect: the beam lives in restored states)
    int beamWidthFinal = 0;
    uint64_t ticksSimulated = 0;
    uint64_t trajectoryDigest = 0;         // FNV-1a over the flattened trajectory + inputs (determinism golden)
};

class Search {
public:
    Search(World const* world, SearchConfig cfg);
    ~Search();
    Search(Search const&) = delete;
    Search& operator=(Search const&) = delete;

    /// Runs until done, until `mayContinue()` says no (polled once per beam tick), or until this
    /// call spent `tickBudget` engine ticks. Returns done().
    bool run(std::function<bool()> const& mayContinue, uint64_t tickBudget);
    /// Gives up now: the best branch is committed, the rest of the level is an unsolved span with
    /// this mechanic ("budget" / "stopped"), and the run is finalised.
    void stop(char const* reason);

    bool done() const;
    double progress() const;               // 0..1 of x
    uint64_t ticksSimulated() const;
    int beamWidth() const;
    ReferenceRun const& result() const;    // final when done(); partial counters before
    std::vector<std::string> const& debug() const;

private:
    struct Impl;
    Impl* m_impl;
};

/// Rebuilds the flattened views of a run from its segments (after an edit of a segment's inputs
/// / trajectory, e.g. windows.hpp centreInputs): inputs, ticks, solvedPercent, trajectoryDigest.
/// Tick offsets keep each segment's gapTicksBefore (the flattened steps jump over skipped time).
void refreshRun(World const& world, ReferenceRun& run);

/// GD's x travel per tick at a speed: double(float m_playerSpeed) * m_speedMultiplier * 0.25.
double unitsPerTick(Speed speed);
/// The speed in force at x: the world start's, then every visible SpeedChange with o.x <= x (x order).
Speed speedAtX(World const& world, double x);
/// Speed-integrated level time in ticks (240 TPS) to travel x0 -> x1 starting at `speed`, switching
/// at every visible SpeedChange with x0 < o.x <= x1. 0 when x1 <= x0. For the stretches the
/// reference run did not simulate (unsupported / unsolved gaps, the rest of an unfinished level).
double ticksAcross(World const& world, double x0, double x1, Speed speed);

/// Replays `inputs` (steps relative to `start`, sub-ticks honoured) from `start` for up to
/// `maxTicks` whole ticks and returns one RecordedTick per step; stops at death / completion.
/// Shared by the search (segment trajectories), the verifier and the tests.
std::vector<RecordedTick> replayTrajectory(World const& world, StartState const& start, std::vector<RecordedInput> const& inputs, int maxTicks,
                                           uint64_t* ticksSimulated = nullptr);

/// FNV-1a 64 over a trajectory + inputs (quantized: x, y to 0.01, yVelocity to 0.001, the discrete fields).
uint64_t trajectoryDigest(std::vector<RecordedTick> const& ticks, std::vector<RecordedInput> const& inputs);

/// RecordedTick from the engine's current state (step = engine tick).
RecordedTick recordTick(Engine const& engine);

/// Steps the engine through one whole tick applying `inputs` (all with floor(frame) == the
/// current tick) at their sub-tick positions; `frames` are tick coordinates (tick + fraction).
struct TickInput {
    double frame = 0.0;   // tick + fraction in [tick, tick + 1)
    bool down = true;
};
void stepTickWithInputs(Engine& engine, std::vector<TickInput> const& inputs);

}  // namespace gprl::sim
