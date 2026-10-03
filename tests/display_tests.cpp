// core/display host tests (geode v0.5.1, stream M1): the HUD history lines, the level-analysis
// coverage block, the "Not a rated demon" line, the session level hints and the attempt clock
// behind attempt_end.activeMs / practiceMs / startPosMs.
#include "test_util.hpp"

#include "../core/display.hpp"
#include "../core/json.hpp"

#include <cmath>
#include <string>

using namespace gprl;
using namespace gprl::display;

namespace {

json::Value parseJson(char const* text) {
    json::Value v;
    json::ParseError err;
    bool ok = json::parse(text, v, &err);
    CHECK_MSG(ok, "json parse: " + err.message);
    return v;
}

HistoryEntry measured(bool down, double width, double early, double late, bool miss = false) {
    HistoryEntry e;
    e.ok = true;
    e.down = down;
    e.widthMs = width;
    e.earlyMs = early;
    e.lateMs = late;
    e.miss = miss;
    return e;
}

void testHistoryText() {
    SECTION("historyText: the four line shapes of the spec + open sides + bot prefix");
    CHECK(historyText(measured(true, 4.166666, -2.083333, 2.083333)) == "PRESS 4.17 ms [-2.08 +2.08]");
    CHECK(historyText(measured(false, 2.5, -1.25, 1.25)) == "REL 2.50 ms [-1.25 +1.25]");
    // a miss keeps its window (both edges on one side of the actual input)
    CHECK(historyText(measured(true, 4.17, -6.25, -2.08, true)) == "PRESS miss 4.17 ms [-6.25 -2.08]");
    HistoryEntry dropped;
    dropped.ok = false;
    dropped.down = true;
    dropped.reason = "mismatch";
    CHECK(historyText(dropped) == "PRESS dropped: mismatch");
    dropped.down = false;
    dropped.reason = "cut by restart";
    CHECK(historyText(dropped) == "REL dropped: cut by restart");
    dropped.reason.clear();
    CHECK(historyText(dropped) == "REL dropped: unknown");
    // v0.7.0: the timing_result's short form follows once it arrived
    auto withV2 = measured(true, 16.666667, -6.25, 10.416667);
    withV2.suffix = "local 4.00 f / seq 11.00 f ok";
    CHECK(historyText(withV2) == "PRESS 16.67 ms [-6.25 +10.42] | local 4.00 f / seq 11.00 f ok");
    dropped.suffix = "unresolved";
    CHECK(historyText(dropped) == "REL dropped: unknown | unresolved");
    dropped.suffix.clear();
    // open sides keep the "<" / ">" markers of the old single-line readout
    auto open = measured(true, 41.67, -41.67, 0.0);
    open.boundedEarly = false;
    CHECK(historyText(open) == "PRESS 41.67 ms [<-41.67 +0.00]");
    open.boundedEarly = true;
    open.boundedLate = false;
    open.earlyMs = -0.0;
    CHECK(historyText(open) == "PRESS 41.67 ms [+0.00 +0.00>]");
    // bot playback: level-only evidence, said so on the line
    auto bot = measured(true, 4.17, -2.08, 2.08);
    bot.levelOnly = true;
    CHECK(historyText(bot) == "bot PRESS 4.17 ms [-2.08 +2.08]");
    HistoryEntry nan = measured(true, NAN, NAN, 1.0);
    CHECK(historyText(nan) == "PRESS ? ms [? +1.00]");
}

void testHistoryTonesAndList() {
    SECTION("historyTone / historyOpacity / historyLines: colours, dimming, cap at 8, newest first");
    CHECK(historyTone(measured(true, 1, -1, 0)) == Tone::Measured);
    CHECK(historyTone(measured(true, 1, -1, 0, true)) == Tone::Miss);
    HistoryEntry d;
    d.ok = false;
    CHECK(historyTone(d) == Tone::Dropped);
    CHECK(historyOpacity(0) == 255);
    CHECK(historyOpacity(1) == 237);
    CHECK(historyOpacity(7) == 129);
    CHECK(historyOpacity(8) == 111);
    CHECK(historyOpacity(9) == 110);    // floor
    CHECK(historyOpacity(1000) == 110);
    DisplayParams p;
    p.opacityStep = 0;
    CHECK(historyOpacity(5, p) == 255);
    std::vector<HistoryEntry> entries;
    for (int i = 0; i < 12; ++i) entries.push_back(measured(i % 2 == 0, 1.0 + i, -0.5, 0.5, i == 3));
    entries[5].ok = false;
    entries[5].reason = "pool";
    auto lines = historyLines(entries);
    CHECK(lines.size() == 8);
    CHECK(lines[0].text == "PRESS 1.00 ms [-0.50 +0.50]");
    CHECK(lines[0].opacity == 255 && lines[0].tone == Tone::Measured);
    CHECK(lines[1].text == "REL 2.00 ms [-0.50 +0.50]");
    CHECK(lines[3].tone == Tone::Miss && lines[3].text == "REL miss 4.00 ms [-0.50 +0.50]");
    CHECK(lines[5].tone == Tone::Dropped && lines[5].text == "REL dropped: pool");
    CHECK(lines[7].opacity == 129);
    CHECK(historyLines({}).empty());
    p = DisplayParams{};
    p.historyLines = 3;
    CHECK(historyLines(entries, p).size() == 3);
    p.historyLines = -1;
    CHECK(historyLines(entries, p).empty());
}

// The core API's shape (shared/src/api/contracts.ts V1LevelAnalysisResponse), trimmed.
char const* kPartial = R"({"levelId":"lvl-1","gdLevelId":68848817,"levelName":"Deadlocked – v2","levelCounts":true,
"levelVersion":{"id":"v1","levelHash":"abc"},"versions":[],"status":"partial",
"coverage":{"percent":83.4,"coveragePercent":83.4,"displayPercent":83,"complete":false,"observedPercent":97.2,
"covered":[{"fromPercent":0,"toPercent":61.2},{"fromPercent":64.7,"toPercent":91},{"fromPercent":94.3,"toPercent":100}],
"missing":[{"fromPercent":61.2,"toPercent":64.7},{"fromPercent":91,"toPercent":94.3}],
"missingDisplay":["61.2–64.7%","91.0–94.3%"]},"analysis":null})";

char const* kComplete = R"({"levelId":"lvl-2","levelName":"Bloodbath","levelCounts":true,"status":"complete",
"coverage":{"percent":100,"displayPercent":100,"complete":true,"observedPercent":100,"covered":[{"fromPercent":0,"toPercent":100}],
"missing":[],"missingDisplay":[]},"analysis":{"version":"level-analysis/1"}})";

char const* kNotDemon = R"({"levelId":"lvl-3","levelName":"Stereo Madness","levelCounts":false,"status":"none",
"coverage":{"percent":0,"displayPercent":0,"complete":false,"observedPercent":0,"covered":[],"missing":[{"fromPercent":0,"toPercent":100}],
"missingDisplay":["0.0–100.0%"]},"analysis":null})";

char const* kOldProducer = R"({"levelId":"lvl-4","levelName":"x","coverage":{"coveragePercent":42.25,"complete":false,
"missing":[{"fromPercent":42.25,"toPercent":100}]}})";

void testCoverage() {
    SECTION("parseLevelCoverage / coverageLine: the MASTER \xc2\xa7" "20 block on one line");
    LevelCoverage c;
    std::string err;
    CHECK_MSG(parseLevelCoverage(parseJson(kPartial), c, &err), err);
    CHECK(c.levelId == "lvl-1");
    CHECK(c.levelName == "Deadlocked - v2");
    CHECK(c.levelCounts);
    CHECK(c.status == "partial");
    CHECK_NEAR(c.percent, 83.4, 1e-9);
    CHECK(c.displayPercent == 83);
    CHECK(!c.complete);
    CHECK_NEAR(c.observedPercent, 97.2, 1e-9);
    CHECK(c.missing.size() == 2 && c.missing[0] == "61.2-64.7%" && c.missing[1] == "91.0-94.3%");
    CHECK(!c.hasAnalysis);
    CHECK(coverageLine(c) == "Level Analysis Coverage: 83% / Missing: - 61.2-64.7% - 91.0-94.3%");

    CHECK_MSG(parseLevelCoverage(parseJson(kComplete), c, &err), err);
    CHECK(c.complete && c.displayPercent == 100 && c.missing.empty() && c.hasAnalysis);
    CHECK(coverageLine(c) == "Level Analysis Coverage: 100% / Complete");

    CHECK_MSG(parseLevelCoverage(parseJson(kNotDemon), c, &err), err);
    CHECK(!c.levelCounts);
    CHECK(coverageLine(c) == "Level Analysis Coverage: 0% / Missing: - 0.0-100.0% (not a rated demon: not analyzed)");

    // older producer: coveragePercent only, no displayPercent / missingDisplay -> formatted here
    CHECK_MSG(parseLevelCoverage(parseJson(kOldProducer), c, &err), err);
    CHECK(c.displayPercent == 42);
    CHECK(c.missing.size() == 1 && c.missing[0] == "42.2-100.0%");
    CHECK(c.levelCounts);   // absent = counts (the route always sends it)
    CHECK(coverageLine(c) == "Level Analysis Coverage: 42% / Missing: - 42.2-100.0%");

    // nothing missing listed and not complete (a producer that sends neither): honest 0-100
    LevelCoverage empty;
    CHECK(coverageLine(empty) == "Level Analysis Coverage: 0% / Missing: - 0.0-100.0%");

    // not a level-analysis response
    CHECK(!parseLevelCoverage(parseJson(R"({"levelId":"x"})"), c, &err) && err.find("coverage") != std::string::npos);
    CHECK(!parseLevelCoverage(parseJson(R"([1,2])"), c, &err));
    CHECK(!parseLevelCoverage(parseJson(R"({"coverage":5})"), c, &err));
    // hostile numbers are clamped, never trusted
    CHECK(parseLevelCoverage(parseJson(R"({"coverage":{"percent":1e300,"displayPercent":-5,"observedPercent":250}})"), c, &err));
    CHECK(c.percent == 100.0 && c.displayPercent == 0 && c.observedPercent == 100.0);
}

void testAsciiDash() {
    SECTION("asciiDash: en / em dash and minus become '-', other non-ASCII dropped");
    CHECK(asciiDash("61.2\xe2\x80\x93" "64.7%") == "61.2-64.7%");
    CHECK(asciiDash("a\xe2\x80\x94" "b") == "a-b");
    CHECK(asciiDash("\xe2\x88\x92" "3") == "-3");
    CHECK(asciiDash("caf\xc3\xa9") == "caf");
    CHECK(asciiDash("\xf0\x9f\x98\x80 hi") == " hi");
    CHECK(asciiDash("plain-ascii") == "plain-ascii");
    CHECK(asciiDash("").empty());
    // a truncated lead byte at the end never reads past the string
    CHECK(asciiDash("x\xe2\x80") == "x");
}

void testLevelCountsLine() {
    SECTION("levelCountsLine: the spec text, the server's extra facts");
    CHECK(levelCountsLine(true, "").empty());
    CHECK(levelCountsLine(true, "whatever").empty());
    // v0.8.4: every level feeds calibration; the line only says "not on the levels list"
    CHECK(levelCountsLine(false, "") == "Not a rated demon: not on the levels list (still feeds your calibration)");
    CHECK(levelCountsLine(false, "Not a rated demon: not counted.") == "Not a rated demon: not on the levels list (still feeds your calibration)");
    CHECK(levelCountsLine(false, "Main level Stereo Madness: not a demon, not counted.")
          == "Not a rated demon: not on the levels list (still feeds your calibration) - Main level Stereo Madness: not a demon, not counted.");
    CHECK(levelCountsLine(false, "Local or editor level (id 0): never counted.")
          == "Not a rated demon: not on the levels list (still feeds your calibration) - Local or editor level (id 0): never counted.");
    CHECK(levelCountsLine(false, "Level rating unknown: not counted.") == "Not a rated demon: not on the levels list (still feeds your calibration) - Level rating unknown: not counted.");
}

void testUnratedSessionNotice() {
    SECTION("unratedSessionNotice: the server's ratableReason in the player's words (v0.14.6)");
    // the live reason of the owner's friend (sessions.trust_reason, 2026-10-03)
    CHECK(unratedSessionNotice("mod menu without a state adapter: Mega Hack (absolllute.megahack)")
          == "GPRL: this session is NOT rated. Mega Hack is loaded and GPRL cannot see whether its cheats are on. Disable Mega Hack while you play to "
             "calibrate (the Eclipse menu works).");
    // several reasons: the mod-menu one wins wherever it stands
    CHECK(unratedSessionNotice("client-reported integrity state \"flagged\"; mod menu without a state adapter: OpenHack (prevter.openhack); noclip active")
              .find("OpenHack is loaded") != std::string::npos);
    CHECK(unratedSessionNotice("mod menu without a state adapter: Mega Hack; physics modified (tps 480)").find("Disable Mega Hack while") != std::string::npos);
    CHECK(unratedSessionNotice("unknown gameplay-affecting mod some.hack; noclip active")
          == "GPRL: this session is NOT rated: unknown gameplay mod some.hack is loaded. Disable it to calibrate.");
    CHECK(unratedSessionNotice("physics modified (tps 480, tps bypass)") == "GPRL: this session is NOT rated: physics modified (tps 480, tps bypass)");
    CHECK(unratedSessionNotice("") == "GPRL: this session is NOT rated.");
    // bounded and ASCII (GD's bitmap fonts)
    CHECK(unratedSessionNotice(std::string(500, 'x')).size() == 160);
    CHECK(unratedSessionNotice("physics \xE2\x80\x93 modified") == "GPRL: this session is NOT rated: physics - modified");
    CHECK(std::string(kNoclipNotice).find("Turn noclip off to calibrate") != std::string::npos);
}

void testLevelHints() {
    SECTION("demonDifficultyName / levelNameHint: GJGameLevel -> session hints");
    CHECK(demonDifficultyName(0, true) == "hard");
    CHECK(demonDifficultyName(3, true) == "easy");
    CHECK(demonDifficultyName(4, true) == "medium");
    CHECK(demonDifficultyName(5, true) == "insane");
    CHECK(demonDifficultyName(6, true) == "extreme");
    CHECK(demonDifficultyName(1, true) == "hard");
    CHECK(demonDifficultyName(2, true) == "hard");
    CHECK(demonDifficultyName(99, true) == "hard");
    CHECK(demonDifficultyName(6, false).empty());
    CHECK(levelNameHint("Deadlocked") == "Deadlocked");
    CHECK(levelNameHint("  Bloodbath  ") == "Bloodbath");
    CHECK(levelNameHint("caf\xc3\xa9 \xe2\x80\x93 x") == "caf  x");
    CHECK(levelNameHint("tab\there\nnew") == "tabherenew");
    CHECK(levelNameHint("").empty());
    CHECK(levelNameHint("   ").empty());
    CHECK(levelNameHint("\xc3\xa9\xc3\xa9").empty());
    std::string longName(80, 'a');
    CHECK(levelNameHint(longName).size() == 64);
    std::string spacedCut = std::string(63, 'b') + "   tail";
    CHECK(levelNameHint(spacedCut) == std::string(63, 'b'));
    DisplayParams p;
    p.levelNameMax = 3;
    CHECK(levelNameHint("abcdef", p) == "abc");
}

void testAttemptClock() {
    SECTION("AttemptClock: unpaused wall-clock ms, disjoint parts, hitch / pause guard");
    AttemptClock c;
    CHECK(c.activeMs() == 0.0 && c.practiceMs() == 0.0 && c.startPosMs() == 0.0);
    for (int i = 0; i < 60; ++i) c.frame(16.5, false, false, false);   // ~1 s from 0
    CHECK_NEAR(c.activeMs(), 990.0, 1e-9);
    CHECK(c.practiceMs() == 0.0 && c.startPosMs() == 0.0);
    CHECK(c.countedFrames() == 60 && c.discardedFrames() == 0);
    for (int i = 0; i < 10; ++i) c.frame(16.5, false, true, true);     // practice wins over StartPos
    CHECK_NEAR(c.activeMs(), 1155.0, 1e-9);
    CHECK_NEAR(c.practiceMs(), 165.0, 1e-9);
    CHECK(c.startPosMs() == 0.0);
    for (int i = 0; i < 10; ++i) c.frame(16.5, false, false, true);    // StartPos, practice off
    CHECK_NEAR(c.startPosMs(), 165.0, 1e-9);
    CHECK_NEAR(c.activeMs(), 1320.0, 1e-9);
    // the parts never exceed the whole (schema invariant)
    CHECK(c.practiceMs() + c.startPosMs() <= c.activeMs() + 1e-9);
    // paused frames, hitches, negative and non-finite deltas are discarded
    c.frame(16.5, true, false, false);
    c.frame(2500.0, false, false, false);   // the first frame after the pause menu closed
    c.frame(-5.0, false, false, false);
    c.frame(NAN, false, false, false);
    c.frame(INFINITY, false, false, false);
    CHECK_NEAR(c.activeMs(), 1320.0, 1e-9);
    CHECK(c.discardedFrames() == 5 && c.countedFrames() == 80);
    // exactly the limit counts, one above does not
    c.frame(500.0, false, false, false);
    c.frame(500.001, false, false, false);
    CHECK_NEAR(c.activeMs(), 1820.0, 1e-9);
    CHECK(c.discardedFrames() == 6);
    c.reset();
    CHECK(c.activeMs() == 0.0 && c.countedFrames() == 0 && c.discardedFrames() == 0);
    DisplayParams p;
    p.maxFrameMs = 100.0;
    AttemptClock strict(p);
    strict.frame(150.0, false, false, false);
    CHECK(strict.activeMs() == 0.0 && strict.discardedFrames() == 1);
    CHECK(std::string(kDisplay.version) == "gprl-display/1");
}

}  // namespace

void testLiveTag() {
    SECTION("liveNow: LIVE only while the server is accepting this level's data (owner request 2026-10-02)");
    using gprl::display::LiveFacts;
    using gprl::display::liveNow;
    LiveFacts f;
    f.enabled = true;
    f.connected = true;
    f.sessionOpen = true;
    f.remote = true;
    f.nowMs = 100000;
    f.lastOkMs = 98000;
    f.eventsPending = 40;
    CHECK(liveNow(f));
    // no accepted batch yet this level: no tag (a receipt, never a promise)
    { auto g = f; g.lastOkMs = 0; CHECK(!liveNow(g)); }
    // a batch failed (or was spooled) after the last accepted one
    { auto g = f; g.lastFailMs = 99000; CHECK(!liveNow(g)); }
    { auto g = f; g.lastFailMs = 97000; CHECK(liveNow(g)); }
    // events waiting and nothing accepted for more than 10 s: the network is stuck
    { auto g = f; g.nowMs = f.lastOkMs + gprl::display::kLiveFreshMs + 1; CHECK(!liveNow(g)); }
    // paused: nothing to send, everything delivered -> still LIVE
    { auto g = f; g.nowMs = f.lastOkMs + 600000; g.eventsPending = 0; CHECK(liveNow(g)); }
    // no network path at all
    { auto g = f; g.localOnly = true; CHECK(!liveNow(g)); }
    { auto g = f; g.connected = false; CHECK(!liveNow(g)); }
    { auto g = f; g.sessionOpen = false; CHECK(!liveNow(g)); }
    { auto g = f; g.remote = false; CHECK(!liveNow(g)); }   // Local / Unsent: spooled to disk
    { auto g = f; g.enabled = false; CHECK(!liveNow(g)); }
}

int main() {
    testHistoryText();
    testHistoryTonesAndList();
    testCoverage();
    testAsciiDash();
    testLevelCountsLine();
    testUnratedSessionNotice();
    testLevelHints();
    testAttemptClock();
    testLiveTag();
    return gprl::test::finish("display_tests");
}
