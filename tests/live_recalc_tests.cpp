// core/live_recalc host tests (live updates, owner decision 2026-10-01; docs/contracts/calibration.md
// "Live updates"): the 45 s interval / 5 min error back-off schedule of POST /v1/me/recalc, the
// answer's extra fields on top of the GET /v1/me/calibration body (one parser for both), and the
// INFO log line (no sigma/s figure while locked, never the private figure).
#include "test_util.hpp"

#include "../core/calibration.hpp"
#include "../core/json.hpp"
#include "../core/live_recalc.hpp"

#include <string>

using namespace gprl;
using namespace gprl::live;

namespace {

std::string g_root;

constexpr int64_t S = 1000;   // ms per second

LiveRecalcGate open() {
    LiveRecalcGate g;
    g.enabled = true;
    g.sessionOpen = true;
    g.remote = true;
    g.levelCounts = true;
    return g;
}

json::Value parseJson(std::string const& text) {
    json::Value v;
    json::ParseError err;
    bool ok = json::parse(text, v, &err);
    CHECK_MSG(ok, "json parse: " + err.message);
    return v;
}

bool loadApi(char const* name, json::Value& out) {
    std::string path = g_root + "/tests/fixtures/api/" + name;
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    if (text.empty()) return false;
    out = parseJson(text);
    return out.isObject();
}

void testConstants() {
    SECTION("constants: 45 s interval, never below the server's 30 s cooldown, 5 min back-off");
    LiveRecalcParams p;
    CHECK(p.version == "live-recalc/0.1");
    CHECK(effectiveIntervalMs(p) == 45 * S);
    CHECK(effectiveBackoffMs(p) == 300 * S);
    LiveRecalcParams fast;
    fast.intervalMs = 5 * S;   // a mistaken faster setting is clamped to the server cooldown
    CHECK(effectiveIntervalMs(fast) == 30 * S);
    fast.serverCooldownMs = 0;
    CHECK(effectiveIntervalMs(fast) == 5 * S);
    fast.intervalMs = 0;
    CHECK(effectiveIntervalMs(fast) == 1 * S);   // never a busy loop
    LiveRecalcParams shortBackoff;
    shortBackoff.errorBackoffMs = 10 * S;
    CHECK(effectiveBackoffMs(shortBackoff) == 45 * S);   // the back-off is never shorter than the interval
}

void testGate() {
    SECTION("due: only a Remote session with sent windows, live updates on; on any level");
    LiveRecalcSchedule s;
    int64_t t0 = 1'000'000;
    CHECK(!s.due(t0 + 3600 * S, open()));   // no session started
    s.sessionStarted(t0);
    CHECK(!s.due(t0 + 60 * S, open()));     // no window sent yet
    s.windowsSent(0);
    s.windowsSent(-3);
    CHECK(s.pendingWindows() == 0);
    s.windowsSent(4);
    CHECK(s.pendingWindows() == 4);
    CHECK(!s.due(t0 + 44 * S, open()));     // one interval after the session start
    CHECK(s.due(t0 + 45 * S, open()));
    auto g = open();
    g.enabled = false;
    CHECK(!s.due(t0 + 45 * S, g));          // setting live-recalc off (or local-only / mod disabled)
    g = open();
    g.remote = false;
    CHECK(!s.due(t0 + 45 * S, g));          // Unsent / Local session
    g = open();
    g.levelCounts = false;
    // Not on the levels list: still due. Every level feeds the calibration (docs/RATING.md §11);
    // while this was a condition the owner's calibration never moved during a session on an
    // unlisted level (report 2026-10-03).
    CHECK(s.due(t0 + 45 * S, g));
    g = open();
    g.sessionOpen = false;
    CHECK(!s.due(t0 + 45 * S, g));
    s.markQueued();
    CHECK(!s.due(t0 + 50 * S, open()));     // one command in flight at a time
    s.dropQueued();
    CHECK(s.due(t0 + 50 * S, open()));
    s.sessionEnded();
    CHECK(!s.due(t0 + 50 * S, open()));
    s.windowsSent(3);                        // a late ack after the end counts for nothing
    CHECK(s.pendingWindows() == 0);
}

void testIntervalAndAnswers() {
    SECTION("interval: every 45 s of wall clock while windows keep coming; a cooldown answer keeps them pending");
    LiveRecalcSchedule s;
    int64_t t0 = 5'000;
    s.sessionStarted(t0);
    s.windowsSent(10);
    CHECK(s.due(t0 + 45 * S, open()));
    s.markQueued();
    s.onAnswer(t0 + 46 * S, true);           // recalculated: the 10 windows are in the stored rows
    CHECK(s.pendingWindows() == 0);
    CHECK(!s.queued());
    CHECK(s.nextAttemptMs() == t0 + 91 * S);
    CHECK(!s.due(t0 + 120 * S, open()));     // nothing new sent: no request at all
    s.windowsSent(1);
    CHECK(!s.due(t0 + 90 * S, open()));
    CHECK(s.due(t0 + 91 * S, open()));
    s.markQueued();
    s.onAnswer(t0 + 91 * S, false);          // the server's cooldown (e.g. a session end just ran)
    CHECK(s.pendingWindows() == 1);          // still pending: asked again one interval later
    CHECK(!s.due(t0 + 135 * S, open()));
    CHECK(s.due(t0 + 136 * S, open()));

    SECTION("a new session restarts the clock and the windows count");
    s.sessionStarted(t0 + 200 * S);
    CHECK(s.pendingWindows() == 0);
    s.windowsSent(2);
    CHECK(!s.due(t0 + 244 * S, open()));
    CHECK(s.due(t0 + 245 * S, open()));
}

void testErrors() {
    SECTION("errors: WARN once per session, back off to 5 minutes, recover on the next answer");
    LiveRecalcSchedule s;
    int64_t t0 = 0;
    s.sessionStarted(t0);
    s.windowsSent(5);
    s.markQueued();
    CHECK(s.onError(t0 + 45 * S));           // first error of the session: WARN
    CHECK(s.backingOff());
    CHECK(!s.queued());
    CHECK(s.pendingWindows() == 5);
    CHECK(!s.due(t0 + 344 * S, open()));
    CHECK(s.due(t0 + 345 * S, open()));      // 5 minutes later
    s.markQueued();
    CHECK(!s.onError(t0 + 345 * S));         // second error: no second WARN this session
    CHECK(s.nextAttemptMs() == t0 + 645 * S);
    // a new session keeps the running back-off (no hammering a failing server per level entry)
    s.sessionStarted(t0 + 400 * S);
    s.windowsSent(1);
    CHECK(!s.due(t0 + 445 * S, open()));
    CHECK(s.due(t0 + 645 * S, open()));
    s.markQueued();
    CHECK(s.onError(t0 + 645 * S));          // ... but it WARNs again once in the new session
    s.markQueued();
    s.onAnswer(t0 + 945 * S, true);          // recovered: back to the 45 s interval
    CHECK(!s.backingOff());
    CHECK(s.nextAttemptMs() == t0 + 990 * S);
    s.sessionStarted(t0 + 1000 * S);
    CHECK(s.nextAttemptMs() == t0 + 1045 * S);
}

void testParseAnswer() {
    SECTION("answer: the GET /v1/me/calibration body + the live fields, one parser for the calibration part");
    json::Value fx;
    if (!loadApi("me-calibration-private.json", fx)) return;
    // a live answer = the calibration body (pinned) + the V1LiveRecalcResponse fields
    json::Value body = fx;
    body.set("computed", true);
    body.set("recalculated", true);
    body.set("computedAt", "2026-10-01T12:00:00.000Z");
    body.set("nextAllowedAt", "2026-10-01T12:00:30.000Z");
    body.set("cooldownSeconds", 30);
    body.set("skipped", nullptr);
    body.set("ratableSamples", 46);
    body.set("sampleCount", 52);

    LiveRecalcAnswer a;
    CHECK(parseLiveRecalcAnswer(body, a));
    CHECK(a.recalculated);
    CHECK(a.computedAt == "2026-10-01T12:00:00.000Z");
    CHECK(a.nextAllowedAt == "2026-10-01T12:00:30.000Z");
    CHECK(a.cooldownSeconds == 30);
    CHECK(a.skipped.empty());
    CHECK(a.ratableSamples && *a.ratableSamples == 46);
    CHECK(a.sampleCount && *a.sampleCount == 52);
    CalibrationState overall;
    CHECK(calibrationStateFromJson(body["overall"], overall));
    CalibrationDisplay d;
    CHECK(calibrationDisplayFromJson(body, d));
    CHECK(d.state == RatingDisplay::Visible);   // since 2026-10-02 a private figure needs the public gate
    CHECK(d.privatePresent && d.privateHeadline());   // the owner's own estimate, stored like a GET

    SECTION("answer: inside the server's cooldown (recalculated false, nulls)");
    json::Value cool = fx;
    cool.set("recalculated", false);
    cool.set("computedAt", nullptr);
    cool.set("nextAllowedAt", "2026-10-01T12:00:30.000Z");
    cool.set("cooldownSeconds", 30);
    cool.set("skipped", "cooldown");
    cool.set("ratableSamples", nullptr);
    cool.set("sampleCount", nullptr);
    LiveRecalcAnswer c;
    CHECK(parseLiveRecalcAnswer(cool, c));
    CHECK(!c.recalculated && c.computedAt.empty() && c.skipped == "cooldown");
    CHECK(!c.ratableSamples && !c.sampleCount);

    SECTION("answer: not a live answer");
    LiveRecalcAnswer untouched;
    untouched.cooldownSeconds = 7;
    CHECK(!parseLiveRecalcAnswer(fx, untouched));   // a plain GET body has no `recalculated`
    CHECK(untouched.cooldownSeconds == 7);
    CHECK(!parseLiveRecalcAnswer(parseJson(R"({"recalculated":"yes"})"), untouched));
    CHECK(!parseLiveRecalcAnswer(parseJson("[]"), untouched));
    LiveRecalcAnswer neg;
    CHECK(parseLiveRecalcAnswer(parseJson(R"({"recalculated":true,"ratableSamples":-4,"sampleCount":"x"})"), neg));
    CHECK(!neg.ratableSamples && !neg.sampleCount && neg.cooldownSeconds == 0);
}

void testLogLine() {
    SECTION("log line: the owner's first live numbers, locked, no figure");
    CalibrationState overall;
    overall.percent = 0.357;
    overall.effectiveSamples = {31.0, 700.0};
    CalibrationDisplay locked;   // default = locked, no private figure
    LiveRecalcAnswer a;
    a.recalculated = true;
    a.ratableSamples = 46;
    CHECK(liveRecalcLogLine(a, overall, locked) ==
          "live recalc -> calibration 35.7% (46 rated, 31 effective), display locked, private -");

    SECTION("log line: a private estimate is named, never its number");
    CalibrationDisplay withPrivate;
    withPrivate.privatePresent = true;
    withPrivate.privatePractical = 123.4;
    withPrivate.privateRaw = 117.3;
    std::string line = liveRecalcLogLine(a, overall, withPrivate);
    CHECK(line == "live recalc -> calibration 35.7% (46 rated, 31 effective), display locked, private yes");
    CHECK(line.find("123") == std::string::npos && line.find("117") == std::string::npos);

    SECTION("log line: a locked display never names a figure even if one is (wrongly) set");
    CalibrationDisplay lockedWithFigure;
    lockedWithFigure.practicalSigma = 150.0;
    CHECK(liveRecalcLogLine(a, overall, lockedWithFigure).find("150") == std::string::npos);

    SECTION("log line: cooldown / no session / no count");
    LiveRecalcAnswer cool;
    cool.recalculated = false;
    cool.skipped = "cooldown";
    CHECK(liveRecalcLogLine(cool, overall, locked) ==
          "live recalc -> calibration 35.7% (server cooldown, stored; 31 effective), display locked, private -");
    LiveRecalcAnswer none;
    none.skipped = "no_sessions";
    CalibrationState empty;
    empty.effectiveSamples = {0.0, 700.0};
    CHECK(liveRecalcLogLine(none, empty, locked) ==
          "live recalc -> calibration 0.0% (no session on the server, stored; 0 effective), display locked, private -");
    LiveRecalcAnswer noCount;
    noCount.recalculated = true;
    CHECK(liveRecalcLogLine(noCount, overall, locked) ==
          "live recalc -> calibration 35.7% (31 effective), display locked, private -");

    SECTION("log line: visible names the public headline figure (pinned visible body)");
    json::Value fx;
    if (loadApi("me-calibration-visible.json", fx)) {
        CalibrationState o;
        CHECK(calibrationStateFromJson(fx["overall"], o));
        CalibrationDisplay d;
        CHECK(calibrationDisplayFromJson(fx, d));
        CHECK(d.state == RatingDisplay::Visible);
        std::string v = liveRecalcLogLine(a, o, d);
        CHECK_MSG(v.rfind("live recalc -> calibration ", 0) == 0 && v.find(", display visible ") != std::string::npos, v);
        CHECK_MSG(v.size() > 4 && v.substr(v.size() - 9) == "private -", v);   // the visible body carries privateSigma: null
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "D:/GPRL";
    testConstants();
    testGate();
    testIntervalAndAnswers();
    testErrors();
    testParseAnswer();
    testLogLine();
    return gprl::test::finish("live_recalc_tests");
}
