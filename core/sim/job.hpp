#pragma once
// The analysis job the worker thread runs (docs/BACKGROUND_ANALYZER_DESIGN.md §2 "job"). PURE
// C++20: the mod's src/analyzer/Worker.cpp owns the std::thread and calls `Job::run` with a
// `JobControl` whose callbacks the mod implements (may-run polling, time source); the job never
// touches anything but its own World / RecordedAttempts / result.
//
// Phases, in order: Verify (if recorded attempts exist) -> Search (the reference run) -> Windows
// (reference inputs, then recorded inputs of verified sections) -> Assemble. Every phase is
// resumable: `run` returns when `control.mayRun()` says no or the budget is spent, and a later
// `run` continues where it stopped (the job keeps its state). `done()` says the result is final.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "result.hpp"
#include "world.hpp"

namespace gprl::sim {

struct JobConfig {
    int maxShiftTicks = 10;          // like the live solver
    int subTickRefine = 1;           // 0 off, 1 = 1/8 tick, 2 = 1/64 tick (only when the recording says CBF)
    double wallBudgetMs = 10 * 60 * 1000.0;   // the owner: "results can take 10 minutes"
    int beamWidth = 48, beamWidthMax = 256;
    int yieldEveryTicks = 2000;      // the job returns to the caller this often to re-check mayRun()
    double verifyTolerance = 0.5;    // units of |dx| + |dy|
    bool windowsAtRecordedInputs = true;
    bool windowsAtReferenceInputs = true;
    int maxWindows = 4000;
    double searchShare = 0.6;        // budget.hpp: the search stops at this share of wallBudgetMs ("budget"
                                     // unsolved rest) so the windows phase always gets the remainder (review H6)
};

struct JobControl {
    std::function<bool()> mayRun;            // polled every yieldEveryTicks ticks; false = return now
    std::function<double()> nowMs;           // monotonic wall clock (ms)
    std::function<void(std::string const&)> log;   // debug lines (may be empty)
};

enum class JobPhase : uint8_t { Idle = 0, Verify, Search, Windows, Assemble, Done, Failed };
constexpr char const* name(JobPhase p) {
    switch (p) {
        case JobPhase::Idle: return "idle";
        case JobPhase::Verify: return "verify";
        case JobPhase::Search: return "search";
        case JobPhase::Windows: return "windows";
        case JobPhase::Assemble: return "assemble";
        case JobPhase::Done: return "done";
        case JobPhase::Failed: return "failed";
    }
    return "?";
}

struct JobProgress {
    JobPhase phase = JobPhase::Idle;
    double percent = 0.0;            // of the current phase
    double solvedPercent = 0.0;      // of the level (search)
    int windowsDone = 0, windowsTotal = 0;
    uint64_t ticksSimulated = 0;
    double elapsedMs = 0.0;
    std::string line;                // "Analyzing: search 42 % · 1.2 M ticks · 38 s"
};

class Job {
public:
    Job(World world, JobConfig cfg);
    ~Job();
    Job(Job const&) = delete;
    Job& operator=(Job const&) = delete;

    /// Adds a passively recorded attempt (any time before `run` finishes the Verify phase; later
    /// additions are queued for a re-verify pass once the job is Done, which `rerunVerify` runs).
    void addRecordedAttempt(RecordedAttempt attempt);

    /// Runs until done, until `control.mayRun()` says no, or until the wall budget is spent.
    void run(JobControl const& control);

    bool done() const;
    JobProgress progress() const;
    LevelSimResult const& result() const;    // valid when done()
    World const& world() const;

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace gprl::sim
