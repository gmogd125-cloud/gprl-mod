// The analysis job (core/sim/job.hpp): Verify -> Search -> Windows -> Assemble, resumable.
// PURE C++20 (docs/BACKGROUND_ANALYZER_DESIGN.md §2 "job"); the mod's Worker owns the thread.
#include "job.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "analysis.hpp"
#include "budget.hpp"
#include "search.hpp"
#include "verify.hpp"
#include "windows.hpp"

namespace gprl::sim {

namespace {

std::string fmtTicks(uint64_t t) {
    char buf[64];
    if (t < 1000) std::snprintf(buf, sizeof buf, "%llu ticks", static_cast<unsigned long long>(t));
    else if (t < 1000000) std::snprintf(buf, sizeof buf, "%.1f k ticks", static_cast<double>(t) / 1000.0);
    else std::snprintf(buf, sizeof buf, "%.1f M ticks", static_cast<double>(t) / 1000000.0);
    return buf;
}

struct RecordedWindowsTask {
    size_t attempt = 0;
    WindowsProgress progress;
    std::vector<SimWindow> windows;
    bool done = false;
};

// The reference run's inputs are re-centred inside their windows before the windows are measured
// (windows.hpp centreInputs): probe range and rounds. Trials are cheap in the simulator.
constexpr int kCentreProbeShift = 48;
constexpr int kCentreRounds = 3;

}  // namespace

struct Job::Impl {
    World world;
    JobConfig cfg;
    SimBudgetRules rules;
    std::vector<RecordedAttempt> attempts;
    size_t verifiedAttempts = 0;        // attempts the Verify phase consumed
    bool reverifyPending = false;

    JobPhase phase = JobPhase::Idle;
    std::unique_ptr<Search> search;
    ReferenceRun refRun;                             // the job's copy of the reference run (centred)
    size_t centredSegments = 0;
    CentreProgress centre;                           // the segment being centred (resumable, H7)
    bool searchBudgetHit = false;                    // the search reached its share of the wall budget (H6)
    std::vector<WindowsProgress> segmentProgress;    // per reference segment
    std::vector<SimWindow> referenceWindows;
    std::vector<RecordedWindowsTask> recordedTasks;
    size_t recordedTaskIndex = 0;
    std::vector<VerifyResult> verifyResults;
    VerifyResult merged;
    bool hasVerify = false;
    LevelSimResult result;
    std::vector<std::string> debug;

    // time / tick accounting
    double activeMs = 0.0, pausedMs = 0.0;
    double lastRunEnd = 0.0;
    bool everRan = false;
    uint64_t ticks = 0;
    uint64_t ticksAtLastPoll = 0;
    double msAtLastPoll = 0.0;
    double runStartMs = 0.0;
    double nowMsCached = 0.0;
    bool pauseRequested = false;
    bool budgetExhausted = false;
    int trials = 0;
    uint64_t searchTicksSeen = 0;

    Impl(World w, JobConfig c) : world(std::move(w)), cfg(c) {
        rules.wallBudgetMs = cfg.wallBudgetMs;
        rules.yieldEveryTicks = cfg.yieldEveryTicks;
        rules.beamWidth = cfg.beamWidth;
        rules.beamWidthMax = cfg.beamWidthMax;
        rules.maxWindows = cfg.maxWindows;
        rules.searchShare = cfg.searchShare;
        if (world.gameplayObjects <= 0) world.gameplayObjects = static_cast<int>(world.objects.size());
    }

    void log(JobControl const& control, std::string const& line) {
        if (debug.size() < 50) debug.push_back(line);
        if (control.log) control.log(line);
    }

    double now(JobControl const& control) { return control.nowMs ? control.nowMs() : 0.0; }

    /// The yield rule the phases poll: by ticks or by time; checks the wall budget too.
    bool mayContinue(JobControl const& control) {
        double const t = now(control);
        if (!shouldPoll(ticks - ticksAtLastPoll, t - msAtLastPoll, rules)) return true;
        ticksAtLastPoll = ticks;
        msAtLastPoll = t;
        double const spent = activeMs + (t - runStartMs);
        if (budgetSpent(cfg.wallBudgetMs, spent)) {
            budgetExhausted = true;
            return false;
        }
        if (phase == JobPhase::Search && cfg.wallBudgetMs > 0.0 && spent >= searchBudgetMs(rules)) {
            searchBudgetHit = true;   // H6: leave the rest of the budget to the windows
            return false;
        }
        if (control.mayRun && !control.mayRun()) {
            pauseRequested = true;
            return false;
        }
        return true;
    }

    SearchConfig searchConfig() const {
        SearchConfig s;
        s.beamWidth = cfg.beamWidth;
        s.beamWidthMax = cfg.beamWidthMax;
        s.maxShift = cfg.maxShiftTicks;
        s.maxTicks = rules.searchTickCap;
        return s;
    }

    WindowsConfig windowsConfig(bool cbf, int tickOffset, WindowSource source) const {
        WindowsConfig w;
        w.maxShift = cfg.maxShiftTicks;
        w.subTick = cbf && cfg.subTickRefine > 0;
        w.subTickPasses = std::max(1, cfg.subTickRefine);
        w.tickOffset = tickOffset;
        w.source = source;
        w.maxWindows = cfg.maxWindows;
        return w;
    }

    // ---- phases ----

    void runVerify(JobControl const& control) {
        while (verifiedAttempts < attempts.size()) {
            RecordedAttempt const& a = attempts[verifiedAttempts++];
            uint64_t before = ticks;
            VerifyResult v = verifyAttempt(world, a, cfg.verifyTolerance);
            ticks += static_cast<uint64_t>(std::max(0, v.ticksCompared));
            (void)before;
            char buf[160];
            std::snprintf(buf, sizeof buf, "verify attempt %d: %d ticks compared, share %.2f", a.attemptIndex, v.ticksCompared, v.verifiedShare);
            log(control, buf);
            verifyResults.push_back(std::move(v));
            if (!mayContinue(control)) break;
        }
        // merged after every call, not only once all attempts ran: a budget that runs out inside
        // Verify still reports the attempts that were verified (review LOW)
        if (!verifyResults.empty()) {
            merged = mergeVerify(verifyResults);
            hasVerify = true;
        }
    }

    bool runSearch(JobControl const& control) {
        if (!search) search = std::make_unique<Search>(&world, searchConfig());
        auto cont = [&]() {
            uint64_t const t = search->ticksSimulated();
            ticks += t - searchTicksSeen;
            searchTicksSeen = t;
            return mayContinue(control);
        };
        bool done = search->run(cont, 0);
        uint64_t const t = search->ticksSimulated();
        ticks += t - searchTicksSeen;
        searchTicksSeen = t;
        if (!done && (budgetExhausted || searchBudgetHit)) {
            if (searchBudgetHit && !budgetExhausted) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "search stopped at its share of the budget (%.0f of %.0f ms): the windows get the rest", searchBudgetMs(rules),
                              cfg.wallBudgetMs);
                log(control, buf);
            }
            search->stop("budget");
            uint64_t const t2 = search->ticksSimulated();
            ticks += t2 - searchTicksSeen;
            searchTicksSeen = t2;
            done = true;
        }
        if (done) {
            for (auto const& line : search->debug()) log(control, "search: " + line);
            refRun = search->result();
            centredSegments = 0;
            centre = CentreProgress{};
            segmentProgress.assign(refRun.segments.size(), WindowsProgress{});
        }
        return done;
    }

    bool runWindows(JobControl const& control) {
        if (cfg.windowsAtReferenceInputs) {
            // re-centre the reference inputs first: resumable per segment (round / phase / input),
            // polling before every trial, applied input and replay chunk (H7)
            while (centredSegments < refRun.segments.size()) {
                ReferenceSegment& seg = refRun.segments[centredSegments];
                uint64_t before = centre.ticks;
                auto cont = [&]() {
                    ticks += centre.ticks - before;
                    before = centre.ticks;
                    return mayContinue(control);
                };
                bool const centred = centreInputsResumable(world, seg, kCentreProbeShift, kCentreRounds, cont, centre);
                ticks += centre.ticks - before;
                if (!centred) {
                    if (!budgetExhausted) return false;
                    // the budget ran out mid-segment: the segment keeps its last validated inputs
                    centre = CentreProgress{};
                    centredSegments = refRun.segments.size();
                    break;
                }
                if (centre.moved > 0) refreshRun(world, refRun);
                for (auto const& line : centre.debug) log(control, "windows ref: " + line);
                centre = CentreProgress{};
                ++centredSegments;
            }
            for (size_t i = 0; i < refRun.segments.size(); ++i) {
                WindowsProgress& p = segmentProgress[i];
                if (p.done) continue;
                ReferenceSegment const& seg = refRun.segments[i];
                WindowsConfig wc = windowsConfig(false, seg.tickOffset, WindowSource::Reference);
                uint64_t before = p.ticks;
                int trialsBefore = p.trials;
                auto cont = [&]() {
                    ticks += p.ticks - before;
                    before = p.ticks;
                    trials += p.trials - trialsBefore;
                    trialsBefore = p.trials;
                    return mayContinue(control);
                };
                bool done = measureWindows(world, seg.inputs, seg.start, wc, cont, 0, referenceWindows, p);
                ticks += p.ticks - before;
                trials += p.trials - trialsBefore;
                if (!done) {
                    if (budgetExhausted) {
                        p.done = true;
                        p.state.reset();   // M4
                        continue;
                    }
                    return false;
                }
                for (auto const& line : p.debug) log(control, "windows ref: " + line);
            }
        }
        if (cfg.windowsAtRecordedInputs && hasVerify) {
            if (recordedTasks.empty()) {
                for (size_t a = 0; a < attempts.size(); ++a) {
                    RecordedWindowsTask t;
                    t.attempt = a;
                    recordedTasks.push_back(std::move(t));
                }
            }
            for (; recordedTaskIndex < recordedTasks.size(); ++recordedTaskIndex) {
                RecordedWindowsTask& task = recordedTasks[recordedTaskIndex];
                if (task.done) continue;
                RecordedAttempt const& a = attempts[task.attempt];
                WindowsConfig wc = windowsConfig(a.cbf, 0, WindowSource::Recorded);
                WindowsProgress& p = task.progress;
                uint64_t before = p.ticks;
                int trialsBefore = p.trials;
                auto cont = [&]() {
                    ticks += p.ticks - before;
                    before = p.ticks;
                    trials += p.trials - trialsBefore;
                    trialsBefore = p.trials;
                    return mayContinue(control);
                };
                bool done = measureWindows(world, a.inputs, a.start, wc, cont, 0, task.windows, p);
                ticks += p.ticks - before;
                trials += p.trials - trialsBefore;
                if (!done) {
                    if (budgetExhausted) {
                        task.done = true;
                        p.state.reset();   // M4
                        continue;
                    }
                    return false;
                }
                task.done = true;
                for (auto const& line : p.debug) log(control, "windows rec: " + line);
            }
        }
        return true;
    }

    std::vector<SimWindow> recordedWindowsInVerifiedBins() const {
        std::vector<SimWindow> out;
        if (!hasVerify) return out;
        double const binPercent = merged.bins.size() > 1 ? merged.bins[1].from - merged.bins[0].from : 2.0;
        for (auto const& task : recordedTasks)
            for (auto const& w : task.windows) {
                int const idx = verifyBinIndex(w.percent, binPercent);
                if (idx < 0 || idx >= static_cast<int>(merged.bins.size())) continue;
                VerifyBin const& b = merged.bins[static_cast<size_t>(idx)];
                if (b.ticks <= 0 || b.unsupported || !b.verified) continue;
                out.push_back(w);
            }
        return out;
    }

    void assemble(JobControl const& control, double nowMsValue) {
        SimBudget b;
        b.cpuMs = activeMs + (nowMsValue - runStartMs);
        b.pausedMs = pausedMs;
        b.wallMs = b.cpuMs + b.pausedMs;
        b.ticksSimulated = ticks;
        b.trials = trials;
        b.searchRestarts = search ? search->result().restarts : 0;
        b.budgetExhausted = budgetExhausted;
        std::vector<SimWindow> ref = referenceWindows;
        std::vector<SimWindow> rec = recordedWindowsInVerifiedBins();
        if (static_cast<int>(ref.size()) > cfg.maxWindows) ref.resize(static_cast<size_t>(cfg.maxWindows));
        if (static_cast<int>(ref.size() + rec.size()) > cfg.maxWindows) rec.resize(static_cast<size_t>(std::max(0, cfg.maxWindows - static_cast<int>(ref.size()))));
        result = sim::assemble(world, refRun, ref, rec, hasVerify ? &merged : nullptr, b);
        result.debug = debug;
        if (result.debug.size() > 50) result.debug.resize(50);
        char buf[200];
        std::snprintf(buf, sizeof buf, "assembled: %zu windows, solved %.1f %%, physics %.1f %%, %s", result.windows.size(), result.coverage.solvedPercent,
                      result.coverage.physicsPercent, hasVerify ? "verified" : "unverified");
        log(control, buf);
    }

    void reverify(JobControl const& control) {
        // every attempt again (the merge is over all of them), then the result's verification
        verifyResults.clear();
        for (auto const& a : attempts) verifyResults.push_back(verifyAttempt(world, a, cfg.verifyTolerance));
        verifiedAttempts = attempts.size();
        if (!verifyResults.empty()) {
            merged = mergeVerify(verifyResults);
            hasVerify = true;
        }
        applyVerification(result, hasVerify ? &merged : nullptr);
        char buf[120];
        std::snprintf(buf, sizeof buf, "re-verified with %zu attempts: share %.2f", attempts.size(), merged.verifiedShare);
        log(control, buf);
        reverifyPending = false;
    }

    void run(JobControl const& control) {
        double const start = now(control);
        if (everRan && start > lastRunEnd) pausedMs += start - lastRunEnd;
        everRan = true;
        runStartMs = start;
        msAtLastPoll = start;
        ticksAtLastPoll = ticks;
        pauseRequested = false;

        if (phase == JobPhase::Done) {
            if (reverifyPending) reverify(control);
            lastRunEnd = now(control);
            activeMs += lastRunEnd - runStartMs;
            return;
        }
        if (phase == JobPhase::Idle) {
            if (world.endX <= 0.f && world.lengthX <= 0.f) {
                phase = JobPhase::Failed;
                log(control, "failed: the world has no length");
            }
            else {
                if (world.endX <= 0.f) world.endX = world.lengthX;
                phase = attempts.empty() ? JobPhase::Search : JobPhase::Verify;
            }
        }
        bool paused = false;
        while (!paused && phase != JobPhase::Done && phase != JobPhase::Failed) {
            switch (phase) {
                case JobPhase::Verify:
                    runVerify(control);
                    if (verifiedAttempts >= attempts.size()) phase = JobPhase::Search;
                    else paused = true;
                    break;
                case JobPhase::Search:
                    if (runSearch(control)) phase = JobPhase::Windows;
                    else paused = true;
                    break;
                case JobPhase::Windows:
                    if (runWindows(control)) phase = JobPhase::Assemble;
                    else paused = true;
                    break;
                case JobPhase::Assemble:
                    assemble(control, now(control));
                    phase = JobPhase::Done;
                    break;
                default:
                    paused = true;
                    break;
            }
            if (budgetExhausted && phase != JobPhase::Done && phase != JobPhase::Failed) {
                // the wall budget is spent: finish with what exists (Partial)
                if (phase == JobPhase::Verify) phase = JobPhase::Search;
                if (phase == JobPhase::Search) {
                    if (!search) search = std::make_unique<Search>(&world, searchConfig());
                    search->stop("budget");
                    refRun = search->result();
                    centredSegments = refRun.segments.size();
                    segmentProgress.assign(refRun.segments.size(), WindowsProgress{});
                    phase = JobPhase::Windows;
                }
                if (phase == JobPhase::Windows) phase = JobPhase::Assemble;
                paused = false;
            }
        }
        if (phase == JobPhase::Done && reverifyPending) reverify(control);
        lastRunEnd = now(control);
        activeMs += lastRunEnd - runStartMs;
    }

    JobProgress progress() const {
        JobProgress p;
        p.phase = phase;
        p.ticksSimulated = ticks;
        p.elapsedMs = activeMs;
        if (search) p.solvedPercent = search->done() ? search->result().solvedPercent : search->progress() * 100.0;
        int wDone = 0, wTotal = 0;
        for (auto const& sp : segmentProgress) {
            wDone += std::min(sp.next, sp.total);
            wTotal += sp.total;
        }
        for (auto const& t : recordedTasks) {
            wDone += std::min(t.progress.next, t.progress.total);
            wTotal += t.progress.total;
        }
        p.windowsDone = wDone;
        p.windowsTotal = wTotal;
        char buf[200];
        double const secs = activeMs / 1000.0;
        switch (phase) {
            case JobPhase::Idle: p.percent = 0.0; std::snprintf(buf, sizeof buf, "idle"); break;
            case JobPhase::Verify:
                p.percent = attempts.empty() ? 100.0 : 100.0 * static_cast<double>(verifiedAttempts) / static_cast<double>(attempts.size());
                std::snprintf(buf, sizeof buf, "verify %zu/%zu attempts · %s · %.0f s", verifiedAttempts, attempts.size(), fmtTicks(ticks).c_str(), secs);
                break;
            case JobPhase::Search:
                p.percent = search ? search->progress() * 100.0 : 0.0;
                std::snprintf(buf, sizeof buf, "search %.0f %% · %s · %.0f s", p.percent, fmtTicks(ticks).c_str(), secs);
                break;
            case JobPhase::Windows:
                p.percent = wTotal > 0 ? 100.0 * wDone / wTotal : 0.0;
                std::snprintf(buf, sizeof buf, "windows %d/%d · %s · %.0f s", wDone, wTotal, fmtTicks(ticks).c_str(), secs);
                break;
            case JobPhase::Assemble: p.percent = 100.0; std::snprintf(buf, sizeof buf, "assemble · %.0f s", secs); break;
            case JobPhase::Done:
                p.percent = 100.0;
                std::snprintf(buf, sizeof buf, "done · solved %.0f %% · %zu windows · %s · %.0f s", result.coverage.solvedPercent, result.windows.size(),
                              fmtTicks(ticks).c_str(), secs);
                break;
            case JobPhase::Failed: p.percent = 0.0; std::snprintf(buf, sizeof buf, "failed"); break;
        }
        p.line = buf;
        return p;
    }
};

Job::Job(World world, JobConfig cfg) : m_impl(new Impl(std::move(world), cfg)) {}
Job::~Job() { delete m_impl; }

void Job::addRecordedAttempt(RecordedAttempt attempt) {
    m_impl->attempts.push_back(std::move(attempt));
    if (m_impl->phase == JobPhase::Done) m_impl->reverifyPending = true;
    else if (m_impl->phase != JobPhase::Idle && m_impl->phase != JobPhase::Verify) {
        // past the Verify phase: verify it now so the windows of its verified sections can still be measured
        m_impl->reverifyPending = true;
        if (m_impl->phase == JobPhase::Search || m_impl->phase == JobPhase::Windows) {
            RecordedAttempt const& a = m_impl->attempts.back();
            m_impl->verifyResults.push_back(verifyAttempt(m_impl->world, a, m_impl->cfg.verifyTolerance));
            m_impl->verifiedAttempts = m_impl->attempts.size();
            m_impl->merged = mergeVerify(m_impl->verifyResults);
            m_impl->hasVerify = true;
            m_impl->reverifyPending = false;
            if (m_impl->phase == JobPhase::Windows && !m_impl->recordedTasks.empty()) {
                RecordedWindowsTask t;
                t.attempt = m_impl->attempts.size() - 1;
                m_impl->recordedTasks.push_back(std::move(t));
            }
        }
    }
}

void Job::run(JobControl const& control) { m_impl->run(control); }
bool Job::done() const { return m_impl->phase == JobPhase::Done || m_impl->phase == JobPhase::Failed; }
JobProgress Job::progress() const { return m_impl->progress(); }
LevelSimResult const& Job::result() const { return m_impl->result; }
World const& Job::world() const { return m_impl->world; }

}  // namespace gprl::sim
