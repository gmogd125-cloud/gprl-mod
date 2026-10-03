// The analysis job (core/sim/job + analysis): the full pipeline on a small world completes;
// pausing via mayRun() = false returns early and resumes; the wall budget ends the job with a
// Partial result; recorded attempts feed verification and recorded windows (also after Done);
// the JSON output parses back with the required keys and versions; sections / density / coverage.
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

#include "../core/json.hpp"
#include "../core/sim/analysis.hpp"
#include "../core/sim/engine.hpp"
#include "../core/sim/gameplay_hash.hpp"
#include "../core/sim/job.hpp"
#include "../core/sim/search.hpp"
#include "../core/sim/verify.hpp"
#include "sim_analysis_world.hpp"

using namespace gprl::sim;
using gprl::json::Value;

namespace {

struct FakeClock {
    double ms = 0.0;
    double perCall = 1.0;
    double operator()() {
        ms += perCall;
        return ms;
    }
};

World world() {
    World w = simworld::tripleSpikeWorld(300.f, 1200.f);
    w.objects.push_back(simworld::spike(800.f));
    w.objects.push_back(simworld::speedChange(950.f, Speed::Double));
    simworld::finalize(w);
    w.gameplayHash = gameplayHash(w);
    return w;
}

RecordedAttempt attemptFromEngine(World const& w, std::vector<RecordedInput> const& inputs, int index) {
    RecordedAttempt a;
    a.attemptIndex = index;
    a.start = w.start;
    a.inputs = inputs;
    a.ticks = replayTrajectory(w, w.start, inputs, 100000);
    a.endStep = a.ticks.empty() ? 0 : a.ticks.back().step;
    a.died = !a.ticks.empty() && a.ticks.back().dead;
    a.completed = !a.ticks.empty() && !a.ticks.back().dead;
    return a;
}

bool hasKeys(Value const& v, std::vector<char const*> const& keys, char const* where) {
    bool ok = true;
    for (auto k : keys)
        if (!v.has(k)) {
            std::printf("  missing key %s.%s\n", where, k);
            ok = false;
        }
    return ok;
}

void testPipeline() {
    SECTION("the full pipeline on a small world completes and the JSON round-trips");
    World w = world();
    JobConfig cfg;
    cfg.maxShiftTicks = 10;
    Job job(w, cfg);
    FakeClock clock;
    std::vector<std::string> logs;
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    control.log = [&](std::string const& s) { logs.push_back(s); };
    CHECK(!job.done());
    CHECK(job.progress().phase == JobPhase::Idle);
    job.run(control);
    CHECK(job.done());
    JobProgress p = job.progress();
    CHECK(p.phase == JobPhase::Done);
    std::printf("  %s\n", p.line.c_str());
    for (auto const& l : logs) std::printf("    %s\n", l.c_str());
    LevelSimResult const& r = job.result();
    CHECK(r.analyzerVersion == "gprl-analyzer/2");
    CHECK(r.simVersion == "gprl-sim/2");
    CHECK(r.gameplayHashVersion == std::string(kGameplayHashVersion));
    CHECK(std::string(kGameplayHashVersion) == "gprl-gameplay-hash/2");
    CHECK(r.gdLevelId == 1);
    CHECK(r.levelHash.size() == 64);
    CHECK(r.gameplayHash.size() == 16);
    CHECK_NEAR(r.coverage.physicsPercent, 100.0, 1e-9);
    CHECK_NEAR(r.coverage.solvedPercent, 100.0, 1e-9);
    CHECK(!r.coverage.hasVerification);
    CHECK(r.coverage.unsupported.empty() && r.coverage.unsolved.empty());
    CHECK(r.referenceInputs >= 2);
    CHECK(r.referenceTicks > 0);
    CHECK(r.trajectoryDigest != 0);
    CHECK(!r.windows.empty());
    CHECK(static_cast<int>(r.windows.size()) <= r.referenceInputs);
    int bothBounded = 0;
    for (auto const& win : r.windows) {
        CHECK(win.source == WindowSource::Reference);
        CHECK(!win.verified);
        CHECK(win.supported);
        CHECK(win.windowMs >= 0.0);
        if (win.boundedEarly && win.boundedLate) {
            ++bothBounded;
            // the reference inputs were re-centred: a both-bounded window is symmetric within one tick
            CHECK_MSG(std::fabs(win.earliestMs + win.latestMs) <= 1000.0 / 240.0 + 1e-6,
                      "window at tick " + std::to_string(win.tick) + " not centred: " + std::to_string(win.earliestMs) + " / " + std::to_string(win.latestMs));
        }
        std::printf("  window tick %d %s [%.2f, %.2f] bounded %d/%d\n", win.tick, win.down ? "press" : "release", win.earliestMs, win.latestMs,
                    win.boundedEarly ? 1 : 0, win.boundedLate ? 1 : 0);
    }
    CHECK(bothBounded >= 1);   // the spike jumps have real edges on both sides once centred
    bool centreLine = false;
    for (auto const& line : r.debug)
        if (line.find("centre:") != std::string::npos) centreLine = true;
    CHECK(centreLine);
    CHECK(!r.hasVerification);
    CHECK(r.density.avgCps > 0.0);
    CHECK(r.density.peak1sCps >= r.density.p90Cps);
    CHECK(r.density.peak1sCps >= r.density.avgCps);
    CHECK(r.sections.size() == 2);   // cut at the speed change
    if (r.sections.size() == 2) {
        CHECK_NEAR(r.sections[0].from, 0.0, 1e-9);
        CHECK_NEAR(r.sections[0].to, 950.0 / 1200.0 * 100.0, 1e-6);
        CHECK_NEAR(r.sections[1].to, 100.0, 1e-9);
        CHECK(r.sections[0].speed == Speed::Normal);
        CHECK(r.sections[1].speed == Speed::Double);
        CHECK(r.sections[0].solved && r.sections[1].solved);
        CHECK(r.sections[0].supported && r.sections[1].supported);
        CHECK(!r.sections[0].verified);
        CHECK(r.sections[0].inputs >= 2);
    }
    CHECK(r.budget.ticksSimulated > 0);
    CHECK(r.budget.trials > 0);
    CHECK(r.budget.cpuMs > 0.0);
    CHECK(!r.budget.budgetExhausted);
    CHECK(r.lengthSeconds > 0.0);
    CHECK(r.objects == 5 && r.gameplayObjects == 5 && r.decorationObjects == 0);
    CHECK(r.debug.size() <= 50 && !r.debug.empty());

    std::string text = toJsonString(r);
    CHECK(text.size() < 2 * 1024 * 1024);
    Value parsed;
    gprl::json::ParseError err;
    CHECK(gprl::json::parse(text, parsed, &err));
    // the server validator's shape (shared/src/level-sim/validate.ts): flat top level, result.hpp names
    CHECK(hasKeys(parsed, {"analyzerVersion", "simVersion", "gameplayHashVersion", "gdLevelId", "levelHash", "gameplayHash", "computedAt", "build", "objects",
                           "gameplayObjects", "decorationObjects", "lengthX", "lengthSeconds", "startPositions", "tooLarge", "coverage", "referenceInputs",
                           "referenceTicks", "trajectoryDigest", "windows", "hasVerification", "verification", "density", "sections", "budget", "debug"}, "root"));
    CHECK(!parsed.has("world") && !parsed.has("reference"));
    CHECK(parsed.getString("analyzerVersion") == "gprl-analyzer/2");
    CHECK(parsed.getString("simVersion") == "gprl-sim/2");
    CHECK(parsed.getString("gameplayHashVersion") == "gprl-gameplay-hash/2");
    auto lowerHex = [](std::string const& s) {
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    };
    CHECK(parsed.getString("gameplayHash").size() == 16 && lowerHex(parsed.getString("gameplayHash")));
    CHECK(parsed.getString("levelHash").size() == 64 && lowerHex(parsed.getString("levelHash")));
    CHECK(parsed["gdLevelId"].isNumber() && parsed["objects"].isNumber() && parsed["gameplayObjects"].isNumber() && parsed["decorationObjects"].isNumber());
    CHECK(parsed["lengthX"].isNumber() && parsed["lengthSeconds"].isNumber() && parsed["startPositions"].isNumber() && parsed["tooLarge"].isBool());
    CHECK(hasKeys(parsed["coverage"], {"physicsPercent", "solvedPercent", "hasVerification", "verifiedPercent", "unsupported", "unsolved"}, "coverage"));
    CHECK(parsed["coverage"]["verifiedPercent"].isNumber() && parsed["coverage"].getNumber("verifiedPercent") == 0.0);
    CHECK(parsed["coverage"]["hasVerification"].isBool() && !parsed["coverage"].getBool("hasVerification"));
    CHECK(parsed["coverage"]["unsupported"].isArray() && parsed["coverage"]["unsolved"].isArray());
    CHECK(parsed["referenceInputs"].isNumber() && parsed["referenceTicks"].isNumber());
    CHECK(parsed["trajectoryDigest"].isString());
    CHECK(parsed["trajectoryDigest"].asString() == std::to_string(r.trajectoryDigest));
    CHECK(parsed["hasVerification"].isBool() && !parsed.getBool("hasVerification"));
    CHECK(parsed["verification"].isNull());
    CHECK(parsed["windows"].isArray() && parsed["windows"].asArray().size() == r.windows.size());
    CHECK(parsed["windows"].asArray().size() <= 4000);
    for (auto const& win : parsed["windows"].asArray()) {
        CHECK(hasKeys(win, {"source", "tick", "frame", "tSeconds", "percent", "down", "gamemode", "speed", "windowMs", "earliestMs", "latestMs", "resolutionMs",
                            "boundedEarly", "boundedLate", "verified", "supported", "geometryHash", "geometryHashHex", "trials"}, "windows[]"));
        CHECK(win.getString("source") == "reference");
        CHECK(win.getString("gamemode") == "cube");
        CHECK(win["tick"].isNumber() && win.getNumber("tick") == std::floor(win.getNumber("tick")));
        CHECK(win["down"].isBool());
        CHECK(win["speed"].isNumber() && win.getInt("speed") >= 0 && win.getInt("speed") <= 4 && win.getNumber("speed") == std::floor(win.getNumber("speed")));
        CHECK(win["boundedEarly"].isBool() && win["boundedLate"].isBool() && win["verified"].isBool() && win["supported"].isBool());
        CHECK(win.getNumber("percent") >= 0.0 && win.getNumber("percent") <= 100.0);
        CHECK(win.getNumber("resolutionMs") > 0.0);
        CHECK(std::fabs(win.getNumber("windowMs") - (win.getNumber("latestMs") - win.getNumber("earliestMs"))) <= 0.05);
        CHECK(win["geometryHash"].isNumber() && win.getNumber("geometryHash") >= 0.0 && win.getNumber("geometryHash") <= 4294967295.0);
        CHECK(win.getString("geometryHashHex").size() <= 16 && lowerHex(win.getString("geometryHashHex")));
        CHECK(win["trials"].isNumber());
    }
    CHECK(hasKeys(parsed["density"], {"avgCps", "p90Cps", "peak1sCps", "peak5sCps"}, "density"));
    CHECK(parsed["sections"].isArray() && parsed["sections"].asArray().size() == 2);
    for (auto const& sec : parsed["sections"].asArray()) {
        CHECK(hasKeys(sec, {"from", "to", "gamemode", "speed", "mini", "inputs", "narrowestMs", "medianMs", "supported", "solved", "verified"}, "sections[]"));
        CHECK(sec.getNumber("from") >= 0.0 && sec.getNumber("to") <= 100.0 && sec.getNumber("from") < sec.getNumber("to"));
        CHECK(sec["mini"].isBool() && sec["supported"].isBool() && sec["solved"].isBool() && sec["verified"].isBool());
        CHECK(sec.getString("gamemode") == "cube");
    }
    CHECK(hasKeys(parsed["budget"], {"cpuMs", "wallMs", "pausedMs", "ticksSimulated", "trials", "searchRestarts", "mode", "recordSafe", "budgetExhausted"}, "budget"));
    CHECK(parsed["budget"]["ticksSimulated"].isNumber() && parsed["budget"]["recordSafe"].isBool() && parsed["budget"]["budgetExhausted"].isBool());
    CHECK(parsed["budget"]["mode"].isString());
    CHECK(parsed["debug"].isArray() && parsed["debug"].asArray().size() <= 64);
    for (auto const& line : parsed["debug"].asArray()) CHECK(line.isString() && line.asString().size() <= 128);

    SECTION("strings are cut to 128 chars; computedAt / build pass through as the mod sets them");
    LevelSimResult longStrings = r;
    longStrings.computedAt = "2026-10-02T12:00:00Z";
    longStrings.build = "gprl-geode 0.12.0+win";
    longStrings.debug.push_back(std::string(300, 'x'));
    longStrings.coverage.unsolved.push_back(CoverageSpan{10.0, 20.0, 100.f, 200.f, std::string(200, 'm')});
    longStrings.budget.mode = std::string(150, 'o');
    Value parsedLong;
    CHECK(gprl::json::parse(toJsonString(longStrings), parsedLong));
    CHECK(parsedLong.getString("computedAt") == "2026-10-02T12:00:00Z");
    CHECK(parsedLong.getString("build") == "gprl-geode 0.12.0+win");
    CHECK(parsedLong["debug"].asArray().back().asString().size() == 128);
    CHECK(parsedLong["coverage"]["unsolved"].asArray().back().getString("mechanic").size() == 128);
    CHECK(parsedLong["budget"].getString("mode").size() == 128);
    // every string is at most 128 chars (the server's cap)
    std::function<bool(Value const&)> strings = [&](Value const& v) {
        if (v.isString()) return v.asString().size() <= 128;
        if (v.isArray()) {
            for (auto const& e : v.asArray())
                if (!strings(e)) return false;
        }
        if (v.isObject()) {
            for (auto const& m : v.asObject())
                if (!strings(m.second)) return false;
        }
        return true;
    };
    CHECK(strings(parsed));
    // the compact string is JSON.stringify-compatible: stringify(parse(text)) == text
    CHECK(gprl::json::stringify(parsed) == text);
}

void testPauseAndResume() {
    SECTION("mayRun() = false returns early; a later run() resumes and finishes with the same result");
    World w = world();
    Job job(w, {});
    FakeClock clock;
    int polls = 0;
    JobControl control;
    control.mayRun = [&] { return ++polls <= 2; };
    control.nowMs = [&] { return clock(); };
    job.run(control);
    CHECK(!job.done());
    JobProgress p = job.progress();
    CHECK(p.phase == JobPhase::Search || p.phase == JobPhase::Windows);
    std::printf("  paused: %s\n", p.line.c_str());
    CHECK(p.line.find("search") != std::string::npos || p.line.find("windows") != std::string::npos);
    int runs = 0;
    control.mayRun = [] { return true; };
    while (!job.done() && runs++ < 1000) job.run(control);
    CHECK(job.done());
    Job direct(w, {});
    JobControl plain;
    plain.mayRun = [] { return true; };
    plain.nowMs = [&] { return clock(); };
    direct.run(plain);
    CHECK(direct.result().trajectoryDigest == job.result().trajectoryDigest);
    CHECK(direct.result().windows.size() == job.result().windows.size());
    CHECK(job.result().budget.pausedMs >= 0.0);
}

void testBudget() {
    SECTION("a spent wall budget ends the job early with budgetExhausted and a Partial coverage");
    World w = world();
    JobConfig cfg;
    cfg.wallBudgetMs = 40.0;
    cfg.yieldEveryTicks = 100;
    cfg.searchShare = 1.0;   // the wall-budget path itself (the search's share is tested in testSearchShare)
    Job job(w, cfg);
    FakeClock clock;
    clock.perCall = 5.0;
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    job.run(control);
    CHECK(job.done());
    LevelSimResult const& r = job.result();
    CHECK(r.budget.budgetExhausted);
    CHECK(r.coverage.solvedPercent < 100.0);
    bool budgetSpan = false;
    for (auto const& u : r.coverage.unsolved)
        if (u.mechanic == "budget") budgetSpan = true;
    CHECK(budgetSpan);
    std::printf("  budget: %s\n", job.progress().line.c_str());
}

void testRecordedAttempts() {
    SECTION("recorded attempts: Verify runs first, windows are measured at the recorded inputs of verified bins");
    World w = world();
    Search s(&w, {});
    CHECK(s.run([] { return true; }, 0));
    auto inputs = s.result().segments.front().inputs;
    RecordedAttempt a = attemptFromEngine(w, inputs, 1);
    Job job(w, {});
    job.addRecordedAttempt(a);
    FakeClock clock;
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    job.run(control);
    CHECK(job.done());
    LevelSimResult const& r = job.result();
    CHECK(r.hasVerification);
    CHECK(r.coverage.hasVerification);
    CHECK_NEAR(r.coverage.verifiedPercent, 100.0, 1e-9);
    CHECK(r.verification.attempts == 1);
    int recorded = 0, reference = 0, verified = 0;
    for (auto const& win : r.windows) {
        if (win.source == WindowSource::Recorded) ++recorded;
        else ++reference;
        if (win.verified) ++verified;
    }
    CHECK(recorded >= 1);
    CHECK(reference >= 1);
    CHECK(verified == static_cast<int>(r.windows.size()));
    for (auto const& sec : r.sections) CHECK(sec.verified);
    Value parsed;
    CHECK(gprl::json::parse(toJsonString(r), parsed));
    CHECK(parsed["verification"].isObject());
    CHECK(hasKeys(parsed["verification"], {"attempts", "ticksCompared", "verifiedShare", "tolerancePosition", "bins"}, "verification"));
    CHECK(parsed["verification"]["bins"].asArray().size() == 50);
    CHECK(hasKeys(parsed["verification"]["bins"][0], {"from", "to", "ticks", "verified", "unsupported", "maxError", "firstDivergenceStep", "reason"}, "bins[0]"));
    CHECK(parsed["coverage"]["verifiedPercent"].isNumber());

    SECTION("an attempt added after Done is verified on the next run() and updates the verification + flags");
    World w2 = world();
    Job late(w2, {});
    late.run(control);
    CHECK(late.done());
    CHECK(!late.result().hasVerification);
    RecordedAttempt bad = attemptFromEngine(w2, inputs, 2);
    for (size_t i = bad.ticks.size() / 2; i < bad.ticks.size(); ++i) bad.ticks[i].y += 6.f;   // diverges from the middle on
    late.addRecordedAttempt(bad);
    CHECK(late.done());
    late.run(control);
    CHECK(late.done());
    LevelSimResult const& r2 = late.result();
    CHECK(r2.hasVerification);
    CHECK(r2.verification.attempts == 1);
    CHECK(r2.coverage.verifiedPercent < 100.0);
    CHECK(r2.coverage.verifiedPercent > 0.0);
    int ver = 0, unver = 0;
    for (auto const& win : r2.windows) (win.verified ? ver : unver)++;
    CHECK(ver >= 1 && unver >= 1);
    bool someSectionUnverified = false;
    for (auto const& sec : r2.sections)
        if (!sec.verified) someSectionUnverified = true;
    CHECK(someSectionUnverified);
    std::printf("  late attempt: verified %.1f %%, %d/%d windows verified\n", r2.coverage.verifiedPercent, ver, ver + unver);
}

void testPartialWorlds() {
    SECTION("unsupported span + wall: physicsPercent < 100, sections cut at the span, honest coverage");
    World w = simworld::base(1500.f);
    w.objects.push_back(simworld::spike(300.f));
    simworld::addUnsupported(w, 600.f, 900.f, "dual_portal");
    simworld::addHazardWall(w, 1200.f, 10);
    simworld::finalize(w);
    Job job(w, {});
    FakeClock clock;
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    job.run(control);
    CHECK(job.done());
    LevelSimResult const& r = job.result();
    CHECK_NEAR(r.coverage.physicsPercent, 80.0, 1e-9);
    CHECK(r.coverage.solvedPercent < 80.0);
    CHECK(r.coverage.unsupported.size() == 1 && r.coverage.unsupported.front().mechanic == "dual_portal");
    CHECK(r.coverage.unsolved.size() >= 1);
    bool spanSection = false, unsolvedSection = false;
    for (auto const& sec : r.sections) {
        if (!sec.supported) spanSection = true;
        if (sec.supported && !sec.solved) unsolvedSection = true;
    }
    CHECK(spanSection);
    CHECK(unsolvedSection);
    for (auto const& win : r.windows) CHECK(win.supported);
    std::printf("  partial: %s\n", job.progress().line.c_str());

    SECTION("a world without length fails cleanly");
    World none;
    Job bad(none, {});
    bad.run(control);
    CHECK(bad.done());
    CHECK(bad.progress().phase == JobPhase::Failed);
}

void testDensity() {
    SECTION("click density: avg / p90 / peaks over presses");
    std::vector<RecordedInput> in;
    for (int i = 0; i < 10; ++i) {
        RecordedInput p;
        p.step = 1 + i * 24;   // 10 presses in the first second
        p.down = true;
        in.push_back(p);
        RecordedInput r = p;
        r.step += 2;
        r.down = false;
        in.push_back(r);
    }
    SimDensity d = clickDensity(in, 10.0);
    CHECK_NEAR(d.avgCps, 1.0, 1e-9);
    CHECK_NEAR(d.peak1sCps, 10.0, 1e-9);
    CHECK_NEAR(d.peak5sCps, 2.0, 1e-9);
    CHECK(d.p90Cps >= 0.0 && d.p90Cps <= 10.0);
    CHECK(clickDensity({}, 10.0).avgCps == 0.0);
}

LevelSimResult runJob(World const& w, std::vector<RecordedAttempt> const& attempts = {}, JobConfig cfg = {}) {
    Job job(w, cfg);
    for (auto const& a : attempts) job.addRecordedAttempt(a);
    FakeClock clock;
    clock.perCall = 0.01;
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    job.run(control);
    CHECK(job.done());
    return job.result();
}

bool anyDebug(LevelSimResult const& r, char const* text) {
    for (auto const& l : r.debug)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

void testCoverageFixes() {
    SECTION("C2: overlapping unsupported spans of different mechanics: physicsPercent is the share outside their UNION, >= solvedPercent");
    {
        World w = simworld::base(2400.f);
        simworld::addUnsupported(w, 1000.f, 1600.f, "move_trigger");
        simworld::addUnsupported(w, 1050.f, 1650.f, "rotate_trigger");
        simworld::addUnsupported(w, 1100.f, 1700.f, "toggle_trigger");
        simworld::addUnsupported(w, 1185.f, 1785.f, "moved_by_trigger");
        simworld::addUnsupported(w, 1245.f, 1845.f, "toggled_by_trigger");
        simworld::finalize(w);
        LevelSimResult r = runJob(w);
        double const unionPercent = (1845.0 - 1000.0) / 2400.0 * 100.0;   // the /1 sum (3000 units) gave 0 %
        CHECK_NEAR(r.coverage.physicsPercent, 100.0 - unionPercent, 1e-6);
        CHECK(r.coverage.physicsPercent >= r.coverage.solvedPercent);
        CHECK(r.coverage.solvedPercent > 50.0);
        CHECK(r.coverage.unsupported.size() == 5);   // every span listed verbatim
        std::printf("  union: physics %.2f %%, solved %.2f %%\n", r.coverage.physicsPercent, r.coverage.solvedPercent);
    }

    SECTION("M1 / M2: after a skipped span the level time keeps the span's travel time (window ticks, lengthSeconds)");
    {
        World w = simworld::base(3000.f);
        simworld::addUnsupported(w, 500.f, 1100.f, "dual_portal");
        w.objects.push_back(simworld::spike(1500.f));
        w.objects.push_back(simworld::spike(2400.f));
        simworld::finalize(w);
        LevelSimResult r = runJob(w);
        double const perTick = unitsPerTick(Speed::Normal);
        CHECK_NEAR(r.lengthSeconds, 3000.0 / perTick / 240.0, 0.05);   // /1: only the simulated ticks (~2.4 s short)
        int after = 0;
        for (auto const& win : r.windows) {
            double const x = win.percent / 100.0 * 3000.0;
            if (x < 1100.0) continue;
            ++after;
            CHECK_MSG(std::abs(win.tick - x / perTick) < 3.0, "window tick " + std::to_string(win.tick) + " at x " + std::to_string(x));
            CHECK_NEAR(win.tSeconds, win.tick / 240.0, 1e-9);
        }
        CHECK(after >= 2);
    }

    SECTION("LOW: a window exactly at a span's start percent is unsupported (validate.ts [percentFrom, percentTo) rule)");
    {
        World w = simworld::base(2400.f);
        w.objects.push_back(simworld::spike(400.f));
        simworld::finalize(w);
        Search s(&w, {});
        CHECK(s.run([] { return true; }, 0));
        ReferenceRun run = s.result();
        CHECK(!run.inputs.empty());
        // 50 spans with awkward float starts and one window exactly at each span's serialised
        // percentFrom: x = percent / 100 * endX may round below x0, the server's rule still says inside
        World spanned = w;
        std::vector<SimWindow> wins;
        int roundTripBelow = 0;
        for (int k = 0; k < 50; ++k) {
            float const x0 = 300.1f + 40.3f * static_cast<float>(k);
            simworld::addUnsupported(spanned, x0, x0 + 10.f, "move_trigger");
            SimWindow win;
            win.percent = static_cast<double>(x0) / 2400.0 * 100.0;
            win.windowMs = 50.0;
            win.earliestMs = -25.0;
            win.latestMs = 25.0;
            wins.push_back(win);
            if (win.percent / 100.0 * 2400.0 < static_cast<double>(x0)) ++roundTripBelow;
            SimWindow justBefore = win;
            justBefore.percent = std::nextafter(win.percent, 0.0);
            if (justBefore.percent / 100.0 * 2400.0 < static_cast<double>(x0) - 1e-3) wins.push_back(justBefore);
        }
        LevelSimResult r = assemble(spanned, run, {wins}, {}, nullptr, SimBudget{});
        CHECK(r.coverage.unsupported.size() == 50);
        int checked = 0;
        for (auto const& win : r.windows) {
            bool atStart = false;
            for (auto const& s : r.coverage.unsupported)
                if (win.percent == s.percentFrom) atStart = true;
            if (atStart) {
                ++checked;
                CHECK(!win.supported);
            }
        }
        CHECK(checked == 50);
        std::printf("  start edge: %d of 50 x round trips fell below x0, all 50 windows unsupported\n", roundTripBelow);
    }

    SECTION("LOW: more than 1000 spans are capped at the server's 1000 (merged, the tail folded); windows in a folded gap are unsupported");
    {
        World w = simworld::base(30000.f);
        for (int i = 0; i < 1200; ++i) simworld::addUnsupported(w, 100.f + 24.f * i, 110.f + 24.f * i, "item_trigger");
        simworld::finalize(w);
        ReferenceRun run;   // no run needed for the coverage lists
        SimWindow gap;
        gap.percent = (100.0 + 24.0 * 1100 + 17.0) / 30000.0 * 100.0;   // between two of the folded spans
        LevelSimResult r = assemble(w, run, {gap}, {}, nullptr, SimBudget{});
        CHECK(r.coverage.unsupported.size() == 1000);
        CHECK(r.coverage.unsupported.back().mechanic == "multiple");
        CHECK(!r.windows.front().supported);
        double physics = 0.0;
        CHECK(r.coverage.physicsPercent < 100.0);
        (void)physics;
    }

    SECTION("LOW: 1-unit unsolved stretches never make sections narrower than 1 unit");
    {
        World w = simworld::base(2400.f);
        simworld::finalize(w);
        ReferenceRun run;
        ReferenceSegment seg;
        seg.x0 = 0.f;
        seg.x1 = 2400.f;
        run.segments.push_back(seg);
        CoverageSpan u;
        u.x0 = 1000.f;
        u.x1 = 1000.5f;
        u.percentFrom = 1000.0 / 24.0;
        u.percentTo = 1000.5 / 24.0;
        u.mechanic = "search_exhausted";
        run.unsolved.push_back(u);
        LevelSimResult r = assemble(w, run, {}, {}, nullptr, SimBudget{});
        bool narrow = false;
        for (auto const& sec : r.sections)
            if ((sec.to - sec.from) / 100.0 * 2400.0 < 1.0 - 1e-6) narrow = true;
        CHECK(!narrow);
        CHECK(!r.sections.empty() && r.sections.back().to == 100.0 && r.sections.front().from == 0.0);
    }
}

void testVerificationFixes() {
    SECTION("M5: recorded attempts whose ticks all lie in unsupported bins: hasVerification false, verifiedPercent 0, the block keeps countedBins 0");
    World w = simworld::base(2400.f);
    simworld::addUnsupported(w, 10.f, 700.f, "toggle_trigger");
    simworld::finalize(w);
    w.gameplayHash = gameplayHash(w);
    RecordedAttempt a;
    a.attemptIndex = 1;
    a.start = w.start;
    a.ticks = replayTrajectory(w, w.start, {}, 400);
    a.endStep = static_cast<int>(a.ticks.size());
    LevelSimResult r = runJob(w, {a});
    CHECK(!r.hasVerification);
    CHECK(!r.coverage.hasVerification);
    CHECK(r.coverage.verifiedPercent == 0.0);
    CHECK(r.verification.attempts == 1 && r.verification.countedBins == 0);
    Value parsed;
    CHECK(gprl::json::parse(toJsonString(r), parsed));
    CHECK(!parsed.getBool("hasVerification"));
    CHECK(parsed["verification"].isObject());
    CHECK(parsed["verification"].getInt("countedBins", -1) == 0);
    CHECK(parsed["verification"].getNumber("verifiedShare", -1.0) == 0.0);
    for (auto const& win : r.windows) CHECK(!win.verified);

    SECTION("LOW: the wall budget running out inside Verify still reports the attempts verified so far");
    World v = world();
    Search s(&v, {});
    CHECK(s.run([] { return true; }, 0));
    auto inputs = s.result().segments.front().inputs;
    JobConfig cfg;
    cfg.wallBudgetMs = 10.0;
    cfg.yieldEveryTicks = 1;
    Job job(v, cfg);
    job.addRecordedAttempt(attemptFromEngine(v, inputs, 1));
    job.addRecordedAttempt(attemptFromEngine(v, inputs, 2));
    job.addRecordedAttempt(attemptFromEngine(v, inputs, 3));
    FakeClock clock;
    clock.perCall = 6.0;   // the second poll is past the 10 ms budget
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    job.run(control);
    CHECK(job.done());
    LevelSimResult const& rb = job.result();
    CHECK(rb.budget.budgetExhausted);
    CHECK(rb.verification.attempts >= 1 && rb.verification.attempts < 3);
    CHECK(rb.hasVerification);
    CHECK_NEAR(rb.coverage.verifiedPercent, 100.0, 1e-9);
}

void testSearchShare() {
    SECTION("H6: the search stops at its share of the wall budget and the windows phase still runs on what it found");
    World w = simworld::tripleSpikeWorld(300.f, 6000.f);
    simworld::finalize(w);
    w.gameplayHash = gameplayHash(w);
    JobConfig cfg;
    cfg.wallBudgetMs = 5000.0;   // FakeClock: 1 ms per clock read, the search reads it once per beam tick
    cfg.searchShare = 0.6;
    Job job(w, cfg);
    FakeClock clock;
    JobControl control;
    control.mayRun = [] { return true; };
    control.nowMs = [&] { return clock(); };
    std::vector<std::string> logs;
    control.log = [&](std::string const& l) { logs.push_back(l); };
    job.run(control);
    CHECK(job.done());
    LevelSimResult const& r = job.result();
    bool stopped = false;
    for (auto const& l : logs)
        if (l.find("search stopped at its share") != std::string::npos) stopped = true;
    CHECK(stopped);
    CHECK(r.coverage.solvedPercent < 100.0);
    bool budgetSpan = false;
    for (auto const& u : r.coverage.unsolved)
        if (u.mechanic == "budget") budgetSpan = true;
    CHECK(budgetSpan);
    CHECK(!r.windows.empty());           // the spike jump's window was measured in the remaining budget
    CHECK(!r.budget.budgetExhausted);
    std::printf("  share: solved %.1f %%, %zu windows, %.0f ms\n", r.coverage.solvedPercent, r.windows.size(), r.budget.cpuMs);
}

void testLongLevelPolls() {
    SECTION("H7: a 5-minute single-segment level with >= 500 inputs never goes more than 50 ms between mayRun polls");
    // a spike every 300 units and a hazard slab after each landing: a held re-jump hits the slab, so
    // every spike needs its own press AND release (~620 inputs in one segment)
    double const seconds = 300.0;
    float const endX = static_cast<float>(311.58 * seconds);
    World w = simworld::base(endX, 77);
    for (float x = 600.f; x < endX - 300.f; x += 300.f) {
        w.objects.push_back(simworld::spike(x));
        w.objects.push_back(simworld::hazardBlock(x + 100.f, 45.f));
    }
    simworld::finalize(w);
    w.gameplayHash = gameplayHash(w);
    Job job(w, JobConfig{});
    auto t0 = std::chrono::steady_clock::now();
    auto realMs = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    double last = 0.0, longest = 0.0;
    int polls = 0;
    JobControl control;
    control.mayRun = [&] {
        double const t = realMs();
        longest = std::max(longest, t - last);
        last = t;
        ++polls;
        return true;
    };
    control.nowMs = [&] { return realMs(); };
    job.run(control);
    double const end = realMs();
    longest = std::max(longest, end - last);
    CHECK(job.done());
    LevelSimResult const& r = job.result();
    std::printf("  long level: %d inputs, %zu windows, %.0f ms, %d polls, longest gap %.1f ms\n", r.referenceInputs, r.windows.size(), end, polls, longest);
    CHECK(r.referenceInputs >= 500);
    CHECK_NEAR(r.coverage.solvedPercent, 100.0, 1e-9);
    CHECK(r.coverage.unsolved.empty());
    CHECK(!anyDebug(r, "segment 2"));    // one segment
    CHECK(anyDebug(r, "centre:"));
    CHECK(static_cast<int>(r.windows.size()) >= 500);
    CHECK_MSG(longest <= 50.0, "longest gap between mayRun polls " + std::to_string(longest) + " ms");
}

void testDeadlocked(std::string const& repo) {
    SECTION("Deadlocked (review repro, tests/fixtures/sim/deadlocked-world.csv): physics over the span union, physics >= solved, few unsolved stretches");
    World w;
    bool const loaded = simworld::loadWorldCsv(repo + "/tests/fixtures/sim/deadlocked-world.csv", w);
    CHECK_MSG(loaded, "fixture readable: " + repo + "/tests/fixtures/sim/deadlocked-world.csv");
    if (!loaded) return;
    CHECK(w.objects.size() == 5915 && w.unsupported.size() == 27);
    CHECK(gameplayHash(w) == "1c91529ee4b9cd98");   // the dump round-trips exactly (the repro's hash)
    w.gameplayHash = gameplayHash(w);
    double const endX = w.endX;
    std::vector<std::pair<double, double>> iv;
    for (auto const& u : w.unsupported) iv.emplace_back(std::clamp<double>(u.x0, 0.0, endX), std::clamp<double>(u.x1, 0.0, endX));
    std::sort(iv.begin(), iv.end());
    double unionLen = 0.0, cs = -1.0, ce = -1.0;
    for (auto const& s : iv) {
        if (cs < 0.0) {
            cs = s.first;
            ce = s.second;
        }
        else if (s.first <= ce) ce = std::max(ce, s.second);
        else {
            unionLen += ce - cs;
            cs = s.first;
            ce = s.second;
        }
    }
    if (cs >= 0.0) unionLen += ce - cs;
    LevelSimResult r = runJob(w);
    std::printf("  Deadlocked: physics %.2f %%, solved %.2f %%, %zu unsolved, %zu windows, %.1f s, %d inputs\n", r.coverage.physicsPercent, r.coverage.solvedPercent,
                r.coverage.unsolved.size(), r.windows.size(), r.lengthSeconds, r.referenceInputs);
    CHECK_NEAR(r.coverage.physicsPercent, (endX - unionLen) / endX * 100.0, 1e-6);
    CHECK_NEAR(r.coverage.physicsPercent, 16.6, 0.1);   // the reviewer's union: 83.4 % unsupported (gprl-sim/1 summed: 0 %)
    CHECK(r.coverage.physicsPercent >= r.coverage.solvedPercent);
    CHECK(r.coverage.solvedPercent > 12.0);              // 10.1 % in gprl-sim/1 (restarts died at once)
    CHECK(r.coverage.unsolved.size() <= 3);              // 17 one-unit stretches in gprl-sim/1
    CHECK(r.lengthSeconds > 90.0 && r.lengthSeconds < 110.0);
    bool insideSupported = false;
    for (auto const& win : r.windows)
        for (auto const& s : r.coverage.unsupported)
            if (win.supported && win.percent >= s.percentFrom && win.percent < s.percentTo) insideSupported = true;
    CHECK(!insideSupported);
    Value parsed;
    CHECK(gprl::json::parse(toJsonString(r), parsed));
    // the simulator's own run from the level start verifies (no anti-cheat spike in a gprl-extract/2 World)
    RecordedAttempt a;
    a.attemptIndex = 1;
    a.start = w.start;
    a.ticks = replayTrajectory(w, w.start, {}, 600);
    VerifyResult v = verifyAttempt(w, a);
    CHECK(v.countedBins >= 1);
    CHECK_NEAR(v.verifiedShare, 1.0, 1e-12);
}

}  // namespace

int main(int argc, char** argv) {
    std::string const repo = argc > 1 ? argv[1] : "D:/GPRL";
    testPipeline();
    testPauseAndResume();
    testBudget();
    testRecordedAttempts();
    testPartialWorlds();
    testDensity();
    testCoverageFixes();
    testVerificationFixes();
    testSearchShare();
    testDeadlocked(repo);
    testLongLevelPolls();
    return gprl::test::finish("sim_job_tests");
}
