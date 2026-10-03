// sim_modes_tests: the truth tables of core/sim/modes.hpp (docs/BACKGROUND_ANALYZER_DESIGN.md §5,
// §10 item 2). Record-Safe never allows clones or bot-playback evidence; an active attempt never
// allows the simulator in Record-Safe (a pause does); frame pressure always stops the simulator;
// passive never simulates.
#include "../core/sim/modes.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace gprl::sim::modes;

namespace {

ModeConfig cfg(bool enabled, AnalysisMode m, bool recordSafe, bool lowCpu = true) {
    ModeConfig c;
    c.enabled = enabled;
    c.mode = m;
    c.recordSafe = recordSafe;
    c.lowCpu = lowCpu;
    return c;
}

RuntimeFacts facts(bool attempt, bool inLevel, bool paused, bool pressure) {
    RuntimeFacts f;
    f.attemptActive = attempt;
    f.inLevel = inLevel;
    f.paused = paused;
    f.framePressure = pressure;
    return f;
}

constexpr AnalysisMode kModes[] = {AnalysisMode::Passive, AnalysisMode::Offline, AnalysisMode::Full};

}  // namespace

int main(int, char**) {
    SECTION("parse: the three setting strings, unknown -> full (pre-0.12.0 behaviour)");
    CHECK(parse("passive") == AnalysisMode::Passive);
    CHECK(parse("offline") == AnalysisMode::Offline);
    CHECK(parse("full") == AnalysisMode::Full);
    CHECK(parse("") == AnalysisMode::Full);
    CHECK(parse("Offline") == AnalysisMode::Full);
    CHECK(parse(nullptr) == AnalysisMode::Full);
    CHECK(std::strcmp(name(AnalysisMode::Offline), "offline") == 0);
    CHECK(std::strcmp(name(parse(name(AnalysisMode::Passive))), "passive") == 0);

    SECTION("clonesAllowed: enabled + not Record-Safe, in EVERY analysis mode (owner decision 2026-10-02)");
    int allowed = 0;
    for (bool enabled : {false, true}) {
        for (auto m : kModes) {
            for (bool rs : {false, true}) {
                bool expect = enabled && !rs;
                bool got = clonesAllowed(cfg(enabled, m, rs));
                CHECK_MSG(got == expect, std::string("enabled ") + (enabled ? "1" : "0") + " mode " + name(m) + " rs " + (rs ? "1" : "0"));
                if (got) ++allowed;
                if (rs) CHECK_MSG(!got, "Record-Safe never allows clones");
            }
        }
    }
    CHECK(allowed == 3);   // passive, offline and full all measure the player's own windows

    SECTION("botPlaybackEvidenceAllowed: never in Record-Safe, never disabled");
    for (bool enabled : {false, true}) {
        for (auto m : kModes) {
            for (bool rs : {false, true}) {
                CHECK(botPlaybackEvidenceAllowed(cfg(enabled, m, rs)) == (enabled && !rs));
                // GdOracle refreshGate relies on this: the clone gate already closes for every config
                // in which bot playback evidence is not allowed (no separate bot + Record-Safe branch)
                CHECK(botPlaybackEvidenceAllowed(cfg(enabled, m, rs)) == clonesAllowed(cfg(enabled, m, rs)));
            }
        }
    }

    SECTION("simulatorEnabled: offline and full, never passive or disabled");
    CHECK(!simulatorEnabled(cfg(true, AnalysisMode::Passive, false)));
    CHECK(simulatorEnabled(cfg(true, AnalysisMode::Offline, false)));
    CHECK(simulatorEnabled(cfg(true, AnalysisMode::Full, false)));
    CHECK(simulatorEnabled(cfg(true, AnalysisMode::Full, true)));
    CHECK(!simulatorEnabled(cfg(false, AnalysisMode::Full, false)));

    SECTION("simAllowedNow: the full truth table (2 x 3 x 2 config x 16 facts)");
    int cases = 0;
    for (bool enabled : {false, true}) {
        for (auto m : kModes) {
            for (bool rs : {false, true}) {
                for (int bits = 0; bits < 16; ++bits) {
                    bool attempt = bits & 1, inLevel = bits & 2, paused = bits & 4, pressure = bits & 8;
                    auto c = cfg(enabled, m, rs);
                    auto f = facts(attempt, inLevel, paused, pressure);
                    bool expect = enabled && m != AnalysisMode::Passive && !pressure && !(rs && attempt && !paused);
                    CHECK_MSG(simAllowedNow(c, f) == expect, std::string("case ") + std::to_string(cases));
                    // the owner rules, stated directly
                    if (rs && attempt && !paused) CHECK_MSG(!simAllowedNow(c, f), "Record-Safe + active attempt never simulates");
                    if (pressure) CHECK_MSG(!simAllowedNow(c, f), "frame pressure never simulates");
                    if (m == AnalysisMode::Passive) CHECK_MSG(!simAllowedNow(c, f), "passive never simulates");
                    ++cases;
                }
            }
        }
    }
    CHECK(cases == 192);
    // Record-Safe allows the simulator in menus, in the pause menu and between attempts
    CHECK(simAllowedNow(cfg(true, AnalysisMode::Offline, true), facts(false, false, false, false)));
    CHECK(simAllowedNow(cfg(true, AnalysisMode::Offline, true), facts(true, true, true, false)));
    CHECK(simAllowedNow(cfg(true, AnalysisMode::Offline, true), facts(false, true, false, false)));
    // without Record-Safe the simulator runs during attempts (below-normal priority, AN-D12)
    CHECK(simAllowedNow(cfg(true, AnalysisMode::Offline, false), facts(true, true, false, false)));

    SECTION("extractionSliceUs: <= 1 ms while playing, min(8 ms, 25% of the frame) otherwise, 0 under pressure / without the simulator");
    constexpr double k60 = 1000.0 / 60.0, k144 = 1000.0 / 144.0, k240 = 1000.0 / 240.0, k30 = 1000.0 / 30.0;
    // playing: the 0.5 / 1 ms caps (a 60 FPS frame allows 4.16 ms, so the playing cap wins)
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Offline, false, true), facts(true, true, false, false), k60) == 500);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Offline, false, false), facts(true, true, false, false), k60) == 1000);
    // ... and a fast frame lowers it further: 240 FPS -> 25% of 4.17 ms = 1041 us -> the 1 ms cap; 500 Hz -> 500 us
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Offline, false, false), facts(true, true, false, false), k240) == 1000);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Offline, false, false), facts(true, true, false, false), 2.0) == 500);
    // not playing (death pause, pause, between attempts): 25% of the frame, never above 8 ms - the
    // 8 ms slice belongs to setupHasCompleted only (the caller passes kSliceLimits.setupUs there)
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, true, true), facts(true, true, true, false), k60) == 4166);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), k60) == 4166);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), k144) == 1736);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), k30) == 8000);   // 25% of 33.3 ms = 8.3 ms -> the 8 ms cap
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), 50.0) == 8000);
    CHECK(kSliceLimits.setupUs == 8000);
    // an unusable target counts as 60 FPS (never 0, never 8 ms by accident)
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), 0.0) == 4166);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), -5.0) == 4166);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), std::nan("")) == 4166);
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Full, false, true), facts(false, true, false, false), 1e9) == 4166);
    // frame pressure: no slice at all (playing or not)
    for (bool attempt : {false, true}) {
        for (bool lowCpu : {false, true}) {
            CHECK_MSG(extractionSliceUs(cfg(true, AnalysisMode::Full, false, lowCpu), facts(attempt, true, false, true), k60) == 0, "pressure skips the slice");
        }
    }
    // no simulator: nothing
    CHECK(extractionSliceUs(cfg(true, AnalysisMode::Passive, false), facts(false, true, false, false), k60) == 0);
    CHECK(extractionSliceUs(cfg(false, AnalysisMode::Full, false), facts(false, true, false, false), k60) == 0);
    // the owner rules over a range of targets: <= 1 ms while playing, <= 8 ms and <= 25% of the frame always
    for (double target : {1.0, 2.5, k240, k144, 10.0, k60, 20.0, k30, 50.0}) {
        for (bool lowCpu : {false, true}) {
            for (bool attempt : {false, true}) {
                int us = extractionSliceUs(cfg(true, AnalysisMode::Offline, false, lowCpu), facts(attempt, true, false, false), target);
                CHECK(us > 0);
                CHECK(us <= 8000);
                CHECK(static_cast<double>(us) <= target * 250.0 + 1e-9);
                if (attempt) CHECK(us <= (lowCpu ? 500 : 1000));
            }
        }
    }

    SECTION("v0.12.2 analysis-cpu: parse / names (unknown -> low, the old default)");
    CHECK(parseCpu("low") == CpuTier::Low);
    CHECK(parseCpu("normal") == CpuTier::Normal);
    CHECK(parseCpu("fast") == CpuTier::Fast);
    CHECK(parseCpu("fastest") == CpuTier::Fastest);
    CHECK(parseCpu("") == CpuTier::Low);
    CHECK(parseCpu("Fast") == CpuTier::Low);
    CHECK(parseCpu("turbo") == CpuTier::Low);
    CHECK(parseCpu(nullptr) == CpuTier::Low);
    for (auto t : {CpuTier::Low, CpuTier::Normal, CpuTier::Fast, CpuTier::Fastest}) CHECK(parseCpu(name(t)) == t);
    CHECK(std::string(requiredPlan(CpuTier::Low)).empty() && std::string(requiredPlan(CpuTier::Normal)).empty());
    CHECK(std::string(requiredPlan(CpuTier::Fast)) == "GPRL Plus");
    CHECK(std::string(requiredPlan(CpuTier::Fastest)) == "GPRL Pro");

    SECTION("v0.12.2 resolveCpu: fast needs the `faster` entitlement, fastest the `fastest` one; low / normal never change");
    constexpr CpuTier kTiers[] = {CpuTier::Low, CpuTier::Normal, CpuTier::Fast, CpuTier::Fastest};
    constexpr SpeedAllowance kAllow[] = {SpeedAllowance::Normal, SpeedAllowance::Faster, SpeedAllowance::Fastest};
    for (auto a : kAllow) {
        for (auto t : kTiers) {
            auto c = resolveCpu(t, a);
            std::string note = std::string(name(t)) + " with " + name(a);
            CHECK_MSG(c.requested == t, note);
            CHECK_MSG(static_cast<int>(c.effective) <= static_cast<int>(maxTier(a)), note + ": never above the plan");
            CHECK_MSG(static_cast<int>(c.effective) <= static_cast<int>(t), note + ": never above the setting");
            CHECK_MSG(c.limited == (static_cast<int>(t) > static_cast<int>(maxTier(a))), note + ": limited");
            // Free is never worse than before v0.12.2: low and normal run as asked under every plan
            if (t == CpuTier::Low || t == CpuTier::Normal) CHECK_MSG(c.effective == t && !c.limited, note);
            // above the plan: the best tier the plan allows (normal for Free / Supporter)
            if (c.limited) CHECK_MSG(c.effective == maxTier(a), note);
        }
    }
    CHECK(resolveCpu(CpuTier::Fast, SpeedAllowance::Normal).effective == CpuTier::Normal);
    CHECK(resolveCpu(CpuTier::Fastest, SpeedAllowance::Normal).effective == CpuTier::Normal);
    CHECK(resolveCpu(CpuTier::Fast, SpeedAllowance::Faster).effective == CpuTier::Fast);
    CHECK(resolveCpu(CpuTier::Fastest, SpeedAllowance::Faster).effective == CpuTier::Fast);
    CHECK(resolveCpu(CpuTier::Fastest, SpeedAllowance::Fastest).effective == CpuTier::Fastest);
    static_assert(resolveCpu(CpuTier::Fastest, SpeedAllowance::Normal).effective == CpuTier::Normal, "constexpr rule");

    SECTION("v0.12.2 workerBurst: low / normal keep the 0.12.0 numbers, fast / fastest only lengthen the burst");
    CHECK(workerBurst(CpuTier::Low).burstMs == 8.0 && workerBurst(CpuTier::Low).restMs == 8.0);
    CHECK(workerBurst(CpuTier::Normal).burstMs == 16.0 && workerBurst(CpuTier::Normal).restMs == 1.0);
    CHECK(workerBurst(CpuTier::Fast).burstMs == 32.0 && workerBurst(CpuTier::Fast).restMs == 1.0);
    CHECK(workerBurst(CpuTier::Fastest).burstMs == 64.0 && workerBurst(CpuTier::Fastest).restMs == 1.0);
    for (int i = 1; i < 4; ++i) CHECK(workerBurst(kTiers[i]).burstMs > workerBurst(kTiers[i - 1]).burstMs);
    for (auto t : kTiers) CHECK(workerBurst(t).restMs > 0.0);   // every burst still yields

    SECTION("v0.12.2 the game thread is unchanged: fast / fastest extract like normal; pressure / Record-Safe still stop every tier");
    for (auto t : {CpuTier::Normal, CpuTier::Fast, CpuTier::Fastest}) {
        ModeConfig c = cfg(true, AnalysisMode::Full, false, false);
        c.cpu = t;
        CHECK(extractionSliceUs(c, facts(true, true, false, false), k60) == 1000);
        CHECK(extractionSliceUs(c, facts(true, true, false, true), k60) == 0);
        ModeConfig rs = cfg(true, AnalysisMode::Full, true, false);
        rs.cpu = t;
        CHECK(!simAllowedNow(rs, facts(true, true, false, false)));   // Record-Safe attempt
        CHECK(!simAllowedNow(c, facts(true, true, false, true)));     // frame pressure
        CHECK(clonesAllowed(c) && !clonesAllowed(rs));                 // the live solver does not care about the tier
    }

    SECTION("v0.12.2 speedLine: the Session tab says why");
    CHECK(speedLine(resolveCpu(CpuTier::Low, SpeedAllowance::Normal)) == "Analysis speed: Low");
    CHECK(speedLine(resolveCpu(CpuTier::Normal, SpeedAllowance::Fastest)) == "Analysis speed: Normal");
    CHECK(speedLine(resolveCpu(CpuTier::Fast, SpeedAllowance::Normal)) == "Analysis speed: Normal (Fast needs GPRL Plus)");
    CHECK(speedLine(resolveCpu(CpuTier::Fastest, SpeedAllowance::Normal)) == "Analysis speed: Normal (Fastest needs GPRL Pro)");
    CHECK(speedLine(resolveCpu(CpuTier::Fast, SpeedAllowance::Faster)) == "Analysis speed: Fast (GPRL Plus)");
    CHECK(speedLine(resolveCpu(CpuTier::Fastest, SpeedAllowance::Faster)) == "Analysis speed: Fast (Fastest needs GPRL Pro)");
    CHECK(speedLine(resolveCpu(CpuTier::Fastest, SpeedAllowance::Fastest)) == "Analysis speed: Fastest (GPRL Pro)");

    SECTION("summary: the Account tab line names the mode and Record-Safe");
    CHECK(std::string(summary(cfg(true, AnalysisMode::Offline, true))) == "Analysis: offline simulation (Record-Safe: live solver off)");
    CHECK(std::string(summary(cfg(true, AnalysisMode::Offline, false))) == "Analysis: full (live solver + level simulation)");
    CHECK(std::string(summary(cfg(false, AnalysisMode::Full, false))) == "Analysis: off");
    CHECK(std::string(summary(cfg(true, AnalysisMode::Full, true))).find("live solver off") != std::string::npos);
    CHECK(std::string(summary(cfg(true, AnalysisMode::Passive, false))) == "Analysis: live solver, no level simulation");
    for (auto m : kModes) CHECK_MSG(std::string(summary(cfg(true, m, false))).find("live solver") != std::string::npos, "every mode measures");
    for (auto m : kModes) {
        for (bool rs : {false, true}) {
            std::string s = summary(cfg(true, m, rs));
            CHECK(s.rfind("Analysis: ", 0) == 0);
            if (rs) CHECK_MSG(s.find("Record-Safe") != std::string::npos, s);
            for (char ch : s) CHECK_MSG(static_cast<unsigned char>(ch) < 0x80, "ASCII only (GD fonts)");
        }
    }

    return gprl::test::finish("sim_modes_tests");
}
