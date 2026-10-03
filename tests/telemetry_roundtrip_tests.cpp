// core/telemetry + core/json host tests against the golden fixture
// tests/fixtures/telemetry/batch-basic.json / batch-basic.expected.json (ARCHITECTURE §5):
//   parse -> validate -> invariants -> re-canonicalise == expected.canonical
//   SHA-256 / HMAC of the canonical bytes == expected, and the pretty printer reproduces the file.
// Also covers the JSON writer's number formatting, the validators' negative cases and the
// playtest spool fixture tests/fixtures/playtest/local-spool (18 batches, 891 events, TEST-key
// signatures, per-session invariants, byte-exact re-serialisation of every line).
// argv[1] = repository root (D:\GPRL).
#include "test_util.hpp"
#include "fixture_util.hpp"

#include "../core/crypto.hpp"
#include "../core/json.hpp"
#include "../core/solver/pass_planner.hpp"
#include "../core/solver/timing_result_event.hpp"
#include "../core/solver/timing_status.hpp"
#include "../core/solver/window_event.hpp"
#include "../core/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace gprl;
using namespace gprl::telemetry;

namespace {

std::string g_root;

std::string fixture(char const* rel) {
    std::string path = g_root + "/tests/fixtures/" + rel;
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    return text;
}

void testNumberFormatting() {
    SECTION("JSON numbers print like ECMAScript Number#toString");
    CHECK(json::formatNumber(0.0) == "0");
    CHECK(json::formatNumber(-0.0) == "0");
    CHECK(json::formatNumber(1.0) == "1");
    CHECK(json::formatNumber(240.0) == "240");
    CHECK(json::formatNumber(2.05) == "2.05");
    CHECK(json::formatNumber(0.1 + 0.2) == "0.30000000000000004");
    CHECK(json::formatNumber(2045.8333333333333) == "2045.8333333333333");
    CHECK(json::formatNumber(8.333333333333334) == "8.333333333333334");
    CHECK(json::formatNumber(1e21) == "1e+21");
    CHECK(json::formatNumber(1e20) == "100000000000000000000");
    CHECK(json::formatNumber(1e-7) == "1e-7");
    CHECK(json::formatNumber(0.000001) == "0.000001");
    CHECK(json::formatNumber(-1.5e-10) == "-1.5e-10");
    CHECK(json::formatNumber(123456789012345680000.0) == "123456789012345680000");
    CHECK(json::formatNumber(std::numeric_limits<double>::infinity()) == "null");
    CHECK(json::formatNumber(std::nan("")) == "null");
}

void testJsonStrings() {
    SECTION("JSON string escaping + parsing");
    json::Value v(std::string("a\"b\\c\n\t\x01" "\xc3\xa9"));
    CHECK(json::stringify(v) == "\"a\\\"b\\\\c\\n\\t\\u0001\xc3\xa9\"");
    json::Value parsed;
    CHECK(json::parse("\"\\u00e9\\ud83d\\ude00\\/x\"", parsed));
    CHECK(parsed.asString() == "\xc3\xa9\xf0\x9f\x98\x80/x");
    json::ParseError err;
    CHECK(!json::parse("{\"a\":1,}", parsed, &err) && !err.ok);
    CHECK(!json::parse("[1 2]", parsed, &err));
    CHECK(json::parse("  {\"k\": [1, 2.5, -3e2, true, null, {}]}  ", parsed));
    CHECK(json::canonical(parsed) == "{\"k\":[1,2.5,-300,true,null,{}]}");
    // canonical sorts keys, stringify keeps insertion order
    json::Value o = json::Value::object();
    o.set("b", 1).set("a", 2);
    CHECK(json::stringify(o) == "{\"b\":1,\"a\":2}");
    CHECK(json::canonical(o) == "{\"a\":2,\"b\":1}");
}

void testGoldenBatch() {
    SECTION("golden batch: parse, validate, invariants, canonical bytes, SHA-256, HMAC");
    std::string text = fixture("telemetry/batch-basic.json");
    std::string expectedText = fixture("telemetry/batch-basic.expected.json");
    if (text.empty() || expectedText.empty()) return;
    json::Value expected;
    CHECK(json::parse(expectedText, expected));

    Batch batch;
    std::string err;
    bool parsed = parseBatch(text, batch, &err);
    CHECK_MSG(parsed, err);
    if (!parsed) return;
    CHECK(batch.events.size() == 12);
    CHECK(batch.sessionId == "sess-fixture-0001");
    CHECK(batch.seq == 3);

    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    // previous seq below the first event passes, equal to it fails
    CHECK(checkBatchInvariants(batch, 99, {}, &err));
    CHECK(!checkBatchInvariants(batch, 100, {}, &err));

    std::string canonical = canonicalBody(batch);
    CHECK(canonical.size() == static_cast<size_t>(expected.getInt("canonicalLength")));
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ");
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    CHECK(expected["hmac"].getString("header") == "X-GPRL-Signature");

    // the pretty printer reproduces the fixture file (LF, 2-space, trailing newline), and the
    // structs re-serialise to the same JSON value as the file
    json::Value original;
    CHECK(json::parse(text, original));
    CHECK(toJson(batch) == original);
    std::string pretty = serializeBatch(batch, true) + "\n";
    std::string normalized = text;
    std::string::size_type pos = 0;
    while ((pos = normalized.find("\r\n", pos)) != std::string::npos) normalized.replace(pos, 2, "\n");
    CHECK_MSG(pretty == normalized, "pretty layout differs from the fixture");

    // spot checks of decoded payloads
    auto const& env = std::get<EnvironmentPayload>(batch.events[0].payload);
    CHECK(env.integrity == Integrity::Clean && env.cbf && env.tps == 240.0 && env.mods.size() == 2);
    CHECK(env.mods[0].gameplayAffecting.has_value() && !*env.mods[0].gameplayAffecting);
    CHECK(!env.modules && !env.trust && !env.noclip);
    auto const& sample = std::get<StateSamplePayload>(batch.events[2].payload);
    CHECK(sample.state.tick == 480 && sample.state.levelTime == 2.0 && sample.state.isOnGround && sample.state.x == 623.16);
    auto const& press = std::get<TimingWindowPayload>(batch.events[4].payload);
    CHECK(!press.holdMinMs && press.fingerprint.prevInputGapMs && *press.fingerprint.prevInputGapMs == 612.5);
    auto const& release = std::get<TimingWindowPayload>(batch.events[7].payload);
    CHECK(release.holdMinMs && *release.holdMinMs == 41.6 && !release.boundedLate);
    CHECK(!release.fingerprint.nextInputGapMs.has_value());
    auto const& end = std::get<AttemptEndPayload>(batch.events[10].payload);
    CHECK(end.reason == AttemptEndReason::Death && !end.completed && end.legit);
}

Event baseEvent(int64_t seq, double t, int64_t tick, std::string attempt = "att-x") {
    Event e;
    e.seq = seq;
    e.t = t;
    e.tick = tick;
    e.attemptId = std::move(attempt);
    return e;
}

void testOwnBatchRoundTrip() {
    SECTION("a batch built by the mod round-trips with every optional field");
    Batch b;
    b.sessionId = "s";
    b.seq = 7;
    b.nonce = "n";
    b.clientBuild = "gprl-geode test";
    {
        Event e = baseEvent(1, 0, 0);
        EnvironmentPayload env;
        env.mods = {{"geode.loader", "5.6.1", false}, {"eclipse.eclipse-menu", "1.9.0", true}, {"x.unknown", "1.0.0", std::nullopt}};
        env.modules = std::vector<EnvironmentModule>{{"GeometryDash.exe", 12345, "abc"}};
        env.hashes = {"g", "e", "p", "l"};
        env.cbf = false;
        env.tpsBypass = true;
        env.tps = 480.0;
        env.fps = 144.0;
        env.integrity = Integrity::Flagged;
        env.trust = TrustState::PhysicsChanged;
        env.noclip = false;
        env.bot = false;
        env.gdVersion = "2.2081";
        env.droppedEvents = 3;
        e.payload = env;
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(2, 0, 0);
        AttemptStartPayload p;
        p.attemptNo = 5;
        p.fromPercent = 42.5;
        p.practice = true;
        p.startPosTick = 1234;
        p.sessionAttemptCount = 3;
        e.payload = p;
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(3, 0.5, 120);
        StateSamplePayload p;
        p.state.player = 2;
        p.state.tick = 120;
        p.state.levelTime = 5.5;
        p.state.subTick = 0.5;
        p.state.gamemode = Gamemode::Wave;
        p.state.speed = Speed::Fastest;
        p.state.gravityFlipped = true;
        p.state.mini = true;
        p.state.platformer = true;
        p.state.x = -1.5;
        p.state.y = 2.25;
        p.state.yVel = -3.75;
        p.state.xVel = 4.0;
        p.state.rotation = 90.0;
        p.state.isOnSlope = true;
        p.state.isHolding = true;
        p.state.ringJumpPending = true;
        p.state.nearPortal = true;
        p.state.lastPortalObjectId = 13;
        p.state.geometryHash = 0xffffffffu;
        e.payload = p;
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(4, 0.5, 120);
        InputPayload p{2, Button::Right, false, 0.75};
        e.payload = p;
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(5, 0.6, 144);
        GamemodeChangePayload p{1, Gamemode::Cube, Gamemode::Ship, 13};
        e.payload = p;
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(6, 0.5, 120);   // deferred solver output: earlier than the last live event
        TimingWindowPayload p;
        p.inputSeq = 4;
        p.inputKind = InputKind::Release;
        p.earliestMs = 499.0;
        p.latestMs = 501.5;
        p.actualMs = 500.0;
        p.boundedEarly = true;
        p.resolutionMs = 0.05;
        p.holdMaxMs = 30.0;
        p.solverVersion = "local-window/0.1.0+boundary-search/0.1.0";
        p.fingerprint.gamemode = Gamemode::Wave;
        p.fingerprint.kind = InputKind::Release;
        p.fingerprint.windowMs = 2.5;
        p.fingerprint.holdMs = 12.0;
        e.payload = p;
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(7, 0.7, 168);
        e.payload = ProgressPayload{7.0, true};
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(8, 0.8, 192);
        e.payload = DeathPayload{7.0, 300.0, 8, true};
        b.events.push_back(e);
    }
    {
        Event e = baseEvent(9, 0.8, 192);
        e.payload = AttemptEndPayload{AttemptEndReason::Complete, 100.0, true, false};
        b.events.push_back(e);
    }
    std::string err;
    CHECK_MSG(validateBatch(b, &err), err);
    CHECK_MSG(checkBatchInvariants(b, 0, {}, &err), err);

    std::string text = serializeBatch(b, false);
    Batch back;
    CHECK_MSG(parseBatch(text, back, &err), err);
    CHECK(toJson(back) == toJson(b));
    CHECK(canonicalBody(back) == canonicalBody(b));
    auto const& env = std::get<EnvironmentPayload>(back.events[0].payload);
    CHECK(env.modules && env.modules->size() == 1 && (*env.modules)[0].size == 12345);
    CHECK(env.trust && *env.trust == TrustState::PhysicsChanged);
    CHECK(env.droppedEvents && *env.droppedEvents == 3);
    CHECK(!env.mods[2].gameplayAffecting.has_value());
    CHECK(text.find("\"gameplayAffecting\"") != std::string::npos);
    CHECK(text.find("\"holdMinMs\"") == std::string::npos);   // absent stays absent
    auto const& st = std::get<StateSamplePayload>(back.events[2].payload).state;
    CHECK(st == std::get<StateSamplePayload>(b.events[2].payload).state);
    CHECK(st.geometryHash == 0xffffffffu);

    // `holdMinMs?: number | null` (schema.ts): an explicit null is a third wire form that
    // canonical.ts keeps, so it must survive the round trip distinct from "absent"
    // (tools/fixture-gen writes null, the golden fixture omits the key)
    {
        Batch n = b;
        auto& w = std::get<TimingWindowPayload>(n.events[5].payload);
        w.holdMinMs = Nullable<double>::nullValue();
        w.holdMaxMs = Nullable<double>::absent();
        CHECK_MSG(validateBatch(n, &err), err);
        std::string nt = serializeBatch(n, false);
        CHECK(nt.find("\"holdMinMs\":null") != std::string::npos);
        CHECK(nt.find("\"holdMaxMs\"") == std::string::npos);
        CHECK(canonicalBody(n).find("\"holdMinMs\":null") != std::string::npos);
        CHECK(canonicalBody(n) != canonicalBody(b));   // the signature would differ, so the form matters
        Batch nb;
        CHECK_MSG(parseBatch(nt, nb, &err), err);
        auto const& wb = std::get<TimingWindowPayload>(nb.events[5].payload);
        CHECK(wb.holdMinMs.isNull() && !wb.holdMinMs && wb.holdMaxMs.isAbsent());
        CHECK(canonicalBody(nb) == canonicalBody(n));
        // a present key with neither a number nor null is rejected (validate.ts optNum)
        std::string bad = nt;
        bad.replace(bad.find("\"holdMinMs\":null"), 16, "\"holdMinMs\":\"x\"");
        CHECK(!parseBatch(bad, nb, &err) && err.find("holdMinMs") != std::string::npos);
    }
}

void testValidationNegatives() {
    SECTION("validators reject what validate.ts rejects");
    Batch b;
    b.sessionId = "s";
    b.nonce = "n";
    b.clientBuild = "c";
    std::string err;
    CHECK(validateBatch(b, &err));
    b.schema = "gprl.telemetry/2";
    CHECK(!validateBatch(b, &err) && err.find("/schema") == 0);
    b.schema = kSchema;
    {
        Event e = baseEvent(1, 0, 0);
        e.payload = InputPayload{3, Button::Jump, true, 0.0};
        b.events = {e};
        CHECK(!validateBatch(b, &err) && err.find("/events/0/player") == 0);
        e.payload = InputPayload{1, Button::Jump, true, 1.0};
        b.events = {e};
        CHECK(!validateBatch(b, &err) && err.find("tSubTick") != std::string::npos);
        e.payload = ProgressPayload{101.0, false};
        b.events = {e};
        CHECK(!validateBatch(b, &err));
        e.attemptId.clear();
        e.payload = ProgressPayload{1.0, false};
        b.events = {e};
        CHECK(!validateBatch(b, &err) && err.find("attemptId") != std::string::npos);
    }
    // invariants
    Event start = baseEvent(1, 0, 0);
    start.payload = AttemptStartPayload{};
    Event later = baseEvent(2, 1.0, 240);
    later.payload = ProgressPayload{1.0, false};
    Event earlier = baseEvent(3, 0.5, 120);
    earlier.payload = ProgressPayload{2.0, false};
    b.events = {start, later, earlier};
    CHECK(!checkBatchInvariants(b, -1, {}, &err) && err.find("/events/2/t") == 0);
    Event dupSeq = later;
    b.events = {start, later, dupSeq};
    CHECK(!checkBatchInvariants(b, -1, {}, &err) && err.find("/seq") != std::string::npos);
    Event orphan = baseEvent(1, 0, 0, "unknown-attempt");
    orphan.payload = ProgressPayload{};
    b.events = {orphan};
    CHECK(!checkBatchInvariants(b, -1, {}, &err) && err.find("before attempt_start") != std::string::npos);
    CHECK(checkBatchInvariants(b, -1, {"unknown-attempt"}, &err));
    // environment events never need an attempt; timing windows may go back in time
    Event env = baseEvent(1, 0, 0, "never-started");
    env.payload = EnvironmentPayload{};
    Event window = baseEvent(3, 0.5, 120);
    window.payload = TimingWindowPayload{};
    b.events = {env, start, later, window};
    b.events[1].seq = 2;
    b.events[2].seq = 4;
    b.events[3].seq = 5;
    CHECK_MSG(checkBatchInvariants(b, -1, {}, &err), err);
}

/// First JSON-pointer path where two values differ (empty when equal) - diagnostics for the
/// lossless round-trip checks.
std::string firstDifference(json::Value const& a, json::Value const& b, std::string const& path = "") {
    if (a.isObject() && b.isObject()) {
        for (auto const& [key, value] : a.asObject()) {
            if (!b.has(key)) return path + "/" + key + " (missing after round trip)";
            std::string d = firstDifference(value, b[key], path + "/" + key);
            if (!d.empty()) return d;
        }
        for (auto const& [key, value] : b.asObject())
            if (!a.has(key)) return path + "/" + key + " (added by round trip)";
        return {};
    }
    if (a.isArray() && b.isArray()) {
        if (a.asArray().size() != b.asArray().size()) return path + " (array length)";
        for (size_t i = 0; i < a.asArray().size(); ++i) {
            std::string d = firstDifference(a.asArray()[i], b.asArray()[i], path + "/" + std::to_string(i));
            if (!d.empty()) return d;
        }
        return {};
    }
    if (a == b) return {};
    return path + " (" + json::stringify(a) + " -> " + json::stringify(b) + ")";
}

/// One accepted spool line (tools/playtest-export/read-spool.mjs SpoolBatch).
struct SpoolLine {
    std::string file;
    int line = 0;
    bool envelope = false;   // {"kind","at","status","signature","batch"} vs a bare batch
    double atMs = 0.0;
    std::string signature;
    Batch batch;
};

/// The bytes of the JSON object starting at `begin` (brace matching that skips strings and
/// escapes); empty when `begin` is not an object or it is unterminated.
std::string extractObject(std::string const& text, size_t begin) {
    if (begin >= text.size() || text[begin] != '{') return {};
    int depth = 0;
    bool inString = false;
    for (size_t i = begin; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return text.substr(begin, i - begin + 1);
    }
    return {};
}

/// Splits a JSONL file into trimmed lines (CRLF tolerant, like read-spool.mjs).
std::vector<std::string> jsonlLines(std::string const& text) {
    std::vector<std::string> out;
    std::string::size_type pos = 0;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
        std::string::size_type start = 0;
        while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t')) ++start;
        out.push_back(raw.substr(start));
    }
    return out;
}

// tests/fixtures/playtest/local-spool/*.jsonl is the mod's own LocalStore format (spool envelopes
// plus one session of bare batches), generated by tools/fixture-gen. Mirrors what
// tests/fixture-checks/playtest.mjs + tools/playtest-export/read-spool.mjs check on the TS side:
// every line parses and validates, the TEST-key signature is HMAC-SHA256(key, canonical), the
// canonical form is idempotent, the per-session ordering invariants hold with the running event
// seq + known attempts, and the counts equal local-spool.expected.json `summary.input`. On top of
// that the C++ structs must be lossless: re-serialising the parsed batch reproduces the line's
// compact JSON byte for byte (LocalStore writes exactly this layout).
void testPlaytestSpool() {
    SECTION("playtest local spool: parse, validate, sign, re-serialise, per-session invariants, counts");
    std::string expectedText = fixture("playtest/local-spool.expected.json");
    if (expectedText.empty()) return;
    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string key;
    CHECK(crypto::fromHex(expected["signatures"].getString("keyHex"), key));
    CHECK(expected["signatures"].getString("algorithm") == "HMAC-SHA256");
    auto const& input = expected["summary"]["input"];

    // PLAYTEST_EXPORT_CONFIG.fallbackEpochIso = "2026-01-01T00:00:00.000Z": bare lines get
    // fallbackEpoch + n seconds so the session order stays deterministic.
    double const fallbackEpochMs = test::isoToUnixMs("2026-01-01T00:00:00.000Z");
    CHECK(fallbackEpochMs == 1767225600000.0);
    int bareIndex = 0;
    int signedCount = 0;
    int bareCount = 0;
    std::vector<SpoolLine> accepted;

    for (auto const& f : input["files"].asArray()) {
        std::string name = f.getString("file");
        std::string text = fixture(("playtest/local-spool/" + name).c_str());
        if (text.empty()) continue;
        int lines = 0;
        int batches = 0;
        int lineNo = 0;
        for (auto const& raw : jsonlLines(text)) {
            ++lineNo;
            if (raw.empty()) continue;
            ++lines;
            std::string where = name + ":" + std::to_string(lineNo);
            json::Value v;
            json::ParseError perr;
            bool ok = json::parse(raw, v, &perr);
            CHECK_MSG(ok, where + ": " + perr.message);
            if (!ok) continue;

            SpoolLine sl;
            sl.file = name;
            sl.line = lineNo;
            json::Value const* batchValue = &v;
            std::string batchText = raw;
            if (v.has("batch")) {
                sl.envelope = true;
                std::string kind = v.getString("kind");
                CHECK_MSG(kind == "local" || kind == "unsent" || kind == "failed", where + ": unknown spool kind " + kind);
                CHECK_MSG(v["at"].isNumber() && v["status"].isNumber() && v["signature"].isString(), where + ": envelope fields");
                sl.atMs = v.getNumber("at");
                sl.signature = v.getString("signature");
                batchValue = &v["batch"];
                // the batch object's own bytes inside the envelope ({"kind",...,"batch":{...},...};
                // the fixture generator appends a "$comment" member after it)
                static char const kBatchKey[] = ",\"batch\":";
                auto at = raw.find(kBatchKey);
                CHECK_MSG(at != std::string::npos, where + ": envelope layout");
                if (at != std::string::npos) batchText = extractObject(raw, at + sizeof(kBatchKey) - 1);
                CHECK_MSG(!batchText.empty(), where + ": batch object not found in the envelope");
            } else {
                sl.atMs = fallbackEpochMs + 1000.0 * bareIndex++;
                CHECK_MSG(v.getString("schema") == kSchema, where + ": bare line is not a gprl.telemetry/1 batch");
            }

            std::string err;
            ok = fromJson(*batchValue, sl.batch, &err);
            CHECK_MSG(ok, where + ": " + err);
            if (!ok) continue;
            CHECK_MSG(validateBatch(sl.batch, &err), where + ": " + err);
            CHECK_MSG(toJson(sl.batch) == *batchValue, where + ": toJson(parsed) differs from the spool line at " + firstDifference(*batchValue, toJson(sl.batch)));
            {
                std::string compact = serializeBatch(sl.batch, false);
                size_t i = 0;
                while (i < compact.size() && i < batchText.size() && compact[i] == batchText[i]) ++i;
                CHECK_MSG(compact == batchText, where + ": compact re-serialisation differs from the spool line at byte " + std::to_string(i) +
                                                    " (sizes " + std::to_string(compact.size()) + " vs " + std::to_string(batchText.size()) + "): ours '" +
                                                    compact.substr(i > 20 ? i - 20 : 0, 60) + "' line '" + batchText.substr(i > 20 ? i - 20 : 0, 60) + "'");
            }
            std::string canonical = canonicalBody(sl.batch);
            json::Value re;
            CHECK_MSG(json::parse(canonical, re) && json::canonical(re) == canonical, where + ": canonical JSON is not idempotent");
            if (sl.envelope) {
                CHECK_MSG(crypto::toHex(crypto::hmacSha256(key, canonical)) == sl.signature,
                          where + ": signature is not HMAC-SHA256(test key, canonicalJson(batch))");
                ++signedCount;
            } else {
                ++bareCount;
            }
            ++batches;
            accepted.push_back(std::move(sl));
        }
        CHECK_MSG(lines == f.getInt("lines"), name + ": " + std::to_string(lines) + " lines, expected " + std::to_string(f.getInt("lines")));
        CHECK_MSG(batches == f.getInt("batches"), name + ": " + std::to_string(batches) + " batches, expected " + std::to_string(f.getInt("batches")));
        CHECK(f.getInt("rejected") == 0);
    }
    CHECK(static_cast<int64_t>(accepted.size()) == input.getInt("acceptedBatches"));
    CHECK(signedCount == input.getInt("signedBatches"));
    CHECK(bareCount == input.getInt("bareBatches"));
    CHECK(input.getInt("rejectedLines") == 0);

    // Per-session ordering exactly like read-spool.mjs orderAndCheckSessions (and the server):
    // batches by seq (then spool time), no duplicate seq, invariants with the running event seq
    // and the attempts started so far in the session.
    std::map<std::string, std::vector<SpoolLine const*>> bySession;
    for (auto const& sl : accepted) bySession[sl.batch.sessionId].push_back(&sl);
    int64_t events = 0, attempts = 0, deaths = 0, windows = 0;
    for (auto& [sessionId, list] : bySession) {
        std::stable_sort(list.begin(), list.end(), [](SpoolLine const* a, SpoolLine const* b) {
            if (a->batch.seq != b->batch.seq) return a->batch.seq < b->batch.seq;
            return a->atMs < b->atMs;
        });
        int64_t previousEventSeq = -1;
        std::set<std::string> knownAttemptIds;
        std::set<int64_t> seenSeq;
        for (auto const* sl : list) {
            std::string where = sl->file + ":" + std::to_string(sl->line);
            CHECK_MSG(!seenSeq.count(sl->batch.seq), where + ": duplicate batch seq in session " + sessionId);
            std::string err;
            CHECK_MSG(checkBatchInvariants(sl->batch, previousEventSeq, knownAttemptIds, &err), where + ": " + err);
            seenSeq.insert(sl->batch.seq);
            for (auto const& e : sl->batch.events) {
                previousEventSeq = std::max(previousEventSeq, e.seq);
                switch (e.kind()) {
                    case EventKind::AttemptStart: knownAttemptIds.insert(e.attemptId); ++attempts; break;
                    case EventKind::Death: ++deaths; break;
                    case EventKind::TimingWindow: {
                        ++windows;
                        // a miss is an input outside [earliestMs, latestMs], so only the window
                        // itself is checked here; hit/miss is derived downstream
                        auto const& w = std::get<TimingWindowPayload>(e.payload);
                        CHECK_MSG(w.earliestMs <= w.latestMs && w.resolutionMs > 0.0, where + ": window bounds");
                        CHECK(w.scope == "local");
                        CHECK(w.fingerprint.windowMs > 0.0 && w.fingerprint.kind == w.inputKind);
                        break;
                    }
                    case EventKind::Environment: {
                        // the generator plants recognisable devfx-* anti-cheat values (leak scans);
                        // one session is deliberately physics-modified, so tps / integrity vary
                        auto const& env = std::get<EnvironmentPayload>(e.payload);
                        CHECK(env.hashes.gd.rfind("devfx-", 0) == 0 && env.hashes.level.rfind("devfx-", 0) == 0);
                        CHECK(env.tps > 0.0 && env.fps > 0.0 && !env.mods.empty());
                        break;
                    }
                    default: break;
                }
                ++events;
            }
        }
    }
    CHECK(static_cast<int64_t>(bySession.size()) == input.getInt("sessions"));
    CHECK_MSG(events == input.getInt("events"), std::to_string(events) + " events, expected " + std::to_string(input.getInt("events")));
    CHECK_MSG(attempts == input.getInt("attempts"), std::to_string(attempts) + " attempts, expected " + std::to_string(input.getInt("attempts")));
    CHECK_MSG(deaths == input.getInt("deaths"), std::to_string(deaths) + " deaths, expected " + std::to_string(input.getInt("deaths")));
    // `samples` = every timing_window (the export derives one TimingSample per window; the
    // ratable subset - noclip / physics filtering - is the export's business, not the wire's)
    CHECK_MSG(windows == input.getInt("samples"), std::to_string(windows) + " timing windows, expected " + std::to_string(input.getInt("samples")));
}

void testNoclipSeen() {
    SECTION("attempt_end.noclipSeen (schema.ts optional): written when set, absent otherwise, round-trips");
    Batch b;
    b.sessionId = "s";
    b.seq = 0;
    b.nonce = "n";
    b.clientBuild = "gprl-geode test";
    Event start = baseEvent(0, 0, 0, "a1");
    start.payload = AttemptStartPayload{};
    b.events.push_back(start);
    Event end = baseEvent(1, 1.0, 240, "a1");
    AttemptEndPayload p{AttemptEndReason::Complete, 100.0, true, false};
    p.noclipSeen = true;
    end.payload = p;
    b.events.push_back(end);
    std::string err;
    CHECK_MSG(validateBatch(b, &err), err);
    std::string text = serializeBatch(b, false);
    CHECK(text.find("\"legit\":false,\"noclipSeen\":true") != std::string::npos);
    CHECK(canonicalBody(b).find("\"noclipSeen\":true") != std::string::npos);
    Batch back;
    CHECK_MSG(parseBatch(text, back, &err), err);
    auto const& e = std::get<AttemptEndPayload>(back.events[1].payload);
    CHECK(e.noclipSeen.has_value() && *e.noclipSeen);
    CHECK(canonicalBody(back) == canonicalBody(b));

    // absent (older clients, every golden fixture): no key, and parsing leaves it absent
    std::get<AttemptEndPayload>(b.events[1].payload).noclipSeen.reset();
    text = serializeBatch(b, false);
    CHECK(text.find("noclipSeen") == std::string::npos);
    CHECK_MSG(parseBatch(text, back, &err), err);
    CHECK(!std::get<AttemptEndPayload>(back.events[1].payload).noclipSeen.has_value());
    // an explicit false is kept (the mod always writes the key)
    std::get<AttemptEndPayload>(b.events[1].payload).noclipSeen = false;
    text = serializeBatch(b, false);
    CHECK(text.find("\"noclipSeen\":false") != std::string::npos);
}

void testReopenSplit() {
    SECTION("session_invalid reopen: a replacement session only receives what its invariants accept");
    auto env = [](int64_t seq, char const* attempt) {
        Event e = baseEvent(seq, 0.25, 60, attempt);
        EnvironmentPayload p;
        p.hashes = {"g", "e", "p", "l"};
        p.fps = 60.0;
        p.integrity = Integrity::Warnings;
        p.trust = TrustState::NoclipModified;
        p.noclip = true;
        e.payload = p;
        return e;
    };
    auto input = [](int64_t seq, double t, char const* attempt) {
        Event e = baseEvent(seq, t, static_cast<int64_t>(t * 240.0), attempt);
        e.payload = InputPayload{1, Button::Jump, true, 0.5};
        return e;
    };
    auto start = [](int64_t seq, char const* attempt) {
        Event e = baseEvent(seq, 0, 0, attempt);
        AttemptStartPayload p;
        p.attemptNo = 3;
        e.payload = p;
        return e;
    };
    auto endOf = [](int64_t seq, double t, char const* attempt) {
        Event e = baseEvent(seq, t, static_cast<int64_t>(t * 240.0), attempt);
        e.payload = AttemptEndPayload{AttemptEndReason::Death, 12.0, false, true};
        return e;
    };

    // the rejected batch: the tail of attempt a1 (started in the invalidated session), then a2;
    // the worker puts the last accepted environment event (seq 40, a1) in front of it
    std::vector<Event> requeued = {env(40, "a1"), input(50, 1.0, "a1"), endOf(51, 1.2, "a1"), start(52, "a2"),
                                   input(53, 0.3, "a2"), env(54, "a2"), endOf(55, 0.9, "a2")};
    std::set<std::string> started;
    auto split = splitForReopenedSession(requeued, started);
    CHECK(split.send.size() == 5);
    CHECK(split.orphaned.size() == 2);
    CHECK(started == std::set<std::string>{"a2"});
    std::vector<int64_t> sentSeqs, orphanSeqs;
    for (auto const& e : split.send) sentSeqs.push_back(e.seq);
    for (auto const& e : split.orphaned) orphanSeqs.push_back(e.seq);
    CHECK((sentSeqs == std::vector<int64_t>{40, 52, 53, 54, 55}));
    CHECK((orphanSeqs == std::vector<int64_t>{50, 51}));

    // what goes to the new session passes validation + invariants as its FIRST batch (no known
    // attempts, no previous event seq) - exactly what the API checks (telemetry.ts)
    Batch fresh;
    fresh.sessionId = "new";
    fresh.seq = 0;
    fresh.nonce = "n2";
    fresh.clientBuild = "gprl-geode test";
    fresh.events = split.send;
    std::string err;
    CHECK_MSG(validateBatch(fresh, &err), err);
    CHECK_MSG(checkBatchInvariants(fresh, -1, {}, &err), err);
    // the orphans could not have been sent there: their attempt started in the old session
    Batch orphan = fresh;
    orphan.events = split.orphaned;
    CHECK(!checkBatchInvariants(orphan, -1, {}, &err) && err.find("attempt_start") != std::string::npos);
    // re-labelling nothing: orphans keep their attempt id and seq
    CHECK(split.orphaned[0].attemptId == "a1" && split.orphaned[1].attemptId == "a1");

    // later events: a2 is known now, a1 stays orphaned, a new attempt a3 is accepted
    std::vector<Event> later = {input(60, 1.5, "a1"), start(61, "a3"), input(62, 0.1, "a3")};
    auto split2 = splitForReopenedSession(later, started);
    CHECK(split2.send.size() == 2 && split2.orphaned.size() == 1);
    CHECK(started.count("a3") == 1);
    Batch second = fresh;
    second.seq = 1;
    second.events = split2.send;
    std::set<std::string> known = {"a2"};
    CHECK_MSG(checkBatchInvariants(second, 55, known, &err), err);

    // re-splitting already-split events is a no-op (flush re-filters halves it put back)
    std::set<std::string> again = started;
    auto split3 = splitForReopenedSession(split.send, again);
    CHECK(split3.send.size() == split.send.size() && split3.orphaned.empty());
}


/// geode v0.14.0 (docs/SHIP_SOLVER.md §4, telemetry revision 6): the NEW golden
/// tests/fixtures/telemetry/batch-timing-result-v0140.json (a Ship press decided by the lockstep
/// compensation planner: adaptation `comp1`, late proof `compensated`, the `compensation` offsets
/// block; a Ship release whose sequence window came from the uniform pair with a `sequence` hold
/// basis; a `sequence_dependent` press without a sequence block) must parse, validate, pass the
/// invariants and re-canonicalise to the recorded bytes; the compensation block round-trips
/// through the C++ reader / writer key for key; the older timing_result goldens carry none of it.
void testTimingResultFixture0140() {
    SECTION("timing_result golden v0.14.0 (lockstep compensation): comp1, compensated, the compensation offsets block");
    std::string text = fixture("telemetry/batch-timing-result-v0140.json");
    std::string expectedText = fixture("telemetry/batch-timing-result-v0140.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.clientBuild == "gprl-geode 0.14.0+win");
    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(static_cast<int64_t>(canonical.size()) == expected.getInt("canonicalLength"));
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the v0.14.0 timing_result fixture");

    int comp1 = 0, pairHold = 0, seqDependent = 0;
    for (auto const& e : batch.events) {
        if (e.kind() != EventKind::TimingResult) continue;
        auto const& p = std::get<TimingResultPayload>(e.payload);
        CHECK(p.solverVersion == "gprl-clone/5");
        if (p.sequence) {
            CHECK(p.sequence->solverVersion == "gprl-clone-sa/4");
            bool hasComp1 = std::find(p.sequence->adaptationUsed.begin(), p.sequence->adaptationUsed.end(), "comp1") != p.sequence->adaptationUsed.end();
            bool hasPair = std::find(p.sequence->adaptationUsed.begin(), p.sequence->adaptationUsed.end(), "pair") != p.sequence->adaptationUsed.end();
            if (hasComp1) {
                ++comp1;
                CHECK(p.status == "ok" && p.sequence->decided);
                CHECK(p.sequence->window.late.proof == std::optional<std::string>("compensated"));
                CHECK(p.sequence->window.late.provenPassMs && *p.sequence->window.late.provenPassMs == p.sequence->window.late.passMs);
                CHECK(p.sequence->compensation.has_value());
                if (p.sequence->compensation) {
                    CHECK(p.sequence->compensation->earlyOffsetsMs.empty());
                    CHECK(p.sequence->compensation->lateOffsetsMs == std::vector<double>{12.5});
                }
                // the gate mirror accepts it (W_local ⊆ W_SA, the proof keys)
                auto gate = gprl::solver::checkTimingResultPayload(p, e.t, p.subTickMs, nullptr);
                std::string why;
                for (auto const& r : gate.reasons) why += r + " ";
                CHECK_MSG(gate.accepted, "gate: " + why);
            }
            if (hasPair) {
                ++pairHold;
                CHECK(p.hold && p.hold->basis == "sequence");
                CHECK(p.sequence->compensation.has_value() && !p.sequence->compensation->lateOffsetsMs.empty());
            }
        }
        else if (p.status == "sequence_dependent") ++seqDependent;
    }
    CHECK(comp1 == 1 && pairHold == 1 && seqDependent == 1);

    // the writer puts the block last in the sequence object and omits it when absent
    for (auto const& e : batch.events) {
        if (e.kind() != EventKind::TimingResult) continue;
        auto p = std::get<TimingResultPayload>(e.payload);
        if (!p.sequence || !p.sequence->compensation) continue;
        CHECK(json::canonical(toJson(e)).find("\"compensation\":{\"earlyOffsetsMs\":[") != std::string::npos);
        Event own = e;
        p.sequence->compensation.reset();
        own.payload = p;
        CHECK(json::canonical(toJson(own)).find("compensation") == std::string::npos);
    }
    // revision 6 constants; the older goldens carry none of the vocabulary
    CHECK(kTelemetryRevision == 6 && kCompensationRevision == 6);
    for (char const* older : {"telemetry/batch-timing-result.json", "telemetry/batch-timing-result-v071.json", "telemetry/batch-timing-result-v080.json"}) {
        std::string t = fixture(older);
        CHECK(t.find("\"comp1\"") == std::string::npos && t.find("compensated") == std::string::npos && t.find("\"compensation\"") == std::string::npos);
    }
}

}  // namespace

/// geode v0.5.1 capture fields (docs/FINISH_PLAN.md): the NEW golden tests/fixtures/telemetry/
/// batch-capture.json must parse, validate, pass the invariants and re-canonicalise to the
/// recorded bytes / SHA-256 / HMAC; the optional keys survive a round trip in both forms
/// (absent = older client); the C++ validator mirrors validate.ts on the bad values.
void testCaptureFixture() {
    SECTION("capture golden (v0.5.1): gdAttemptCount, evidenceHint, activeMs / practiceMs / startPosMs");
    std::string text = fixture("telemetry/batch-capture.json");
    std::string expectedText = fixture("telemetry/batch-capture.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.events.size() == 11);
    CHECK(batch.clientBuild == "gprl-geode 0.5.1+win");
    auto const& start1 = std::get<AttemptStartPayload>(batch.events[1].payload);
    CHECK(start1.gdAttemptCount.has_value() && *start1.gdAttemptCount == 50000);
    CHECK(start1.startPosTick.has_value() && *start1.startPosTick == 9000);
    auto const& win1 = std::get<TimingWindowPayload>(batch.events[3].payload);
    CHECK(win1.evidenceHint.has_value() && *win1.evidenceHint == kEvidenceLevelOnly);
    auto const& win2 = std::get<TimingWindowPayload>(batch.events[8].payload);
    CHECK(win2.evidenceHint.has_value() && *win2.evidenceHint == kEvidencePlayer);
    auto const& end1 = std::get<AttemptEndPayload>(batch.events[4].payload);
    CHECK(end1.activeMs && *end1.activeMs == 4120.5);
    CHECK(end1.practiceMs && *end1.practiceMs == 0.0);
    CHECK(end1.startPosMs && *end1.startPosMs == 4120.5);
    auto const& env0 = std::get<EnvironmentPayload>(batch.events[0].payload);
    CHECK(env0.bot && *env0.bot && env0.trust && *env0.trust == TrustState::Botting);

    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(static_cast<int64_t>(canonical.size()) == expected.getInt("canonicalLength"));
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    // the pretty printer reproduces the fixture file byte for byte (LF, 2-space, trailing newline)
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the capture fixture");

    // the new keys are absent from the frozen basic golden and stay absent after a round trip
    Batch basic;
    CHECK_MSG(parseBatch(fixture("telemetry/batch-basic.json"), basic, &err), err);
    for (auto const& e : basic.events) {
        if (auto* s = std::get_if<AttemptStartPayload>(&e.payload)) CHECK(!s->gdAttemptCount);
        if (auto* w = std::get_if<TimingWindowPayload>(&e.payload)) CHECK(!w->evidenceHint);
        if (auto* a = std::get_if<AttemptEndPayload>(&e.payload)) CHECK(!a->activeMs && !a->practiceMs && !a->startPosMs);
    }
    std::string basicText = serializeBatch(basic, false);
    CHECK(basicText.find("gdAttemptCount") == std::string::npos && basicText.find("evidenceHint") == std::string::npos
          && basicText.find("activeMs") == std::string::npos);

    // validator mirror of validate.ts (shared/test/telemetry.test.ts "rejects bad values")
    Batch bad = batch;
    std::get<AttemptStartPayload>(bad.events[1].payload).gdAttemptCount = -1;
    CHECK(!validateBatch(bad, &err) && err.find("/events/1/gdAttemptCount") != std::string::npos);
    bad = batch;
    std::get<TimingWindowPayload>(bad.events[3].payload).evidenceHint = "bot";
    CHECK(!validateBatch(bad, &err) && err.find("/events/3/evidenceHint") != std::string::npos);
    bad = batch;
    std::get<AttemptEndPayload>(bad.events[4].payload).practiceMs = 5000.0;   // 5000 + 4120.5 > 4120.5
    CHECK(!validateBatch(bad, &err) && err.find("/events/4/activeMs") != std::string::npos);
    bad = batch;
    std::get<AttemptEndPayload>(bad.events[10].payload).startPosMs = -0.5;
    CHECK(!validateBatch(bad, &err) && err.find("/events/10/startPosMs") != std::string::npos);
    // the parts may add up to exactly the whole; a part without the whole is fine
    Batch ok = batch;
    std::get<AttemptEndPayload>(ok.events[4].payload).practiceMs = 1000.0;
    std::get<AttemptEndPayload>(ok.events[4].payload).startPosMs = 3120.5;
    std::get<AttemptEndPayload>(ok.events[10].payload).activeMs.reset();
    CHECK_MSG(validateBatch(ok, &err), err);
    // a non-number gdAttemptCount / evidenceHint on the wire parses as absent (tolerant reader),
    // exactly like the other optional keys
    Batch odd;
    CHECK(parseBatch(R"({"schema":"gprl.telemetry/1","sessionId":"s","seq":0,"nonce":"n","clientBuild":"b","events":[
        {"kind":"attempt_start","t":0,"tick":0,"seq":0,"attemptId":"a","attemptNo":1,"fromPercent":0,"practice":false,"startPosTick":null,"noclip":false,"sessionAttemptCount":1,"gdAttemptCount":"x"}]})",
                     odd, &err));
    CHECK(!std::get<AttemptStartPayload>(odd.events[0].payload).gdAttemptCount);
}

/// geode v0.6.0 (stream M3, telemetry revision 2): the NEW golden tests/fixtures/telemetry/
/// batch-clip.json with the `clip_available` event must parse, validate, pass the invariants and
/// re-canonicalise to the recorded bytes / SHA-256 / HMAC; the validator mirrors validate.ts on bad
/// clip ids, hashes and durations; the two older goldens never contain the kind.
void testClipFixture() {
    SECTION("clip golden (v0.6.0): clip_available {clipId, attemptId, durationMs, sha256}");
    // the kind arrived with revision 2; later revisions only add kinds (3 = sequence_window, v0.6.1)
    CHECK(kTelemetryRevision >= 2 && kClipAvailableRevision == 2);
    CHECK(kEventKindCount >= 10);
    CHECK(std::string(kindName(EventKind::ClipAvailable)) == "clip_available");
    EventKind parsed = EventKind::Input;
    CHECK(parseKind("clip_available", parsed) && parsed == EventKind::ClipAvailable);
    // the variant index IS the kind: appended last, the nine older kinds keep their positions
    CHECK(static_cast<int>(EventKind::Environment) == 8 && static_cast<int>(EventKind::ClipAvailable) == 9);

    std::string text = fixture("telemetry/batch-clip.json");
    std::string expectedText = fixture("telemetry/batch-clip.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.events.size() == 6);
    CHECK(batch.clientBuild == "gprl-geode 0.6.0+win");
    CHECK(batch.events[5].kind() == EventKind::ClipAvailable);
    CHECK(batch.events[5].attemptId == "clp-a1" && batch.events[5].t == 96.5 && batch.events[5].tick == 23160);
    auto const& clip = std::get<ClipAvailablePayload>(batch.events[5].payload);
    CHECK(clip.clipId == "clip-19a0b1c2d3e-5f6a7b8c");
    CHECK(clip.durationMs == 100500.0);
    CHECK(clip.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(clip.sha256 == crypto::toHex(crypto::sha256("abc")));

    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(static_cast<int64_t>(canonical.size()) == expected.getInt("canonicalLength"));
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the clip fixture");

    // an event the mod builds itself serialises to exactly the fixture's event
    Event own;
    own.t = 96.5;
    own.tick = 23160;
    own.seq = 5;
    own.attemptId = "clp-a1";
    own.payload = ClipAvailablePayload{"clip-19a0b1c2d3e-5f6a7b8c", 100500.0, clip.sha256};
    CHECK(json::canonical(toJson(own)) == json::canonical(toJson(batch.events[5])));
    CHECK(json::stringify(toJson(own))
          == "{\"kind\":\"clip_available\",\"t\":96.5,\"tick\":23160,\"seq\":5,\"attemptId\":\"clp-a1\",\"clipId\":\"clip-19a0b1c2d3e-5f6a7b8c\","
             "\"durationMs\":100500,\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}");

    // the frozen goldens of revision 1 never contain the kind
    for (char const* older : {"telemetry/batch-basic.json", "telemetry/batch-capture.json"}) {
        Batch old;
        CHECK_MSG(parseBatch(fixture(older), old, &err), err);
        for (auto const& e : old.events) CHECK(e.kind() != EventKind::ClipAvailable);
        CHECK(serializeBatch(old, false).find("clip_available") == std::string::npos);
    }

    // validator mirror of validate.ts (shared/test/telemetry.test.ts "rejects bad clip ids, hashes and durations")
    auto withClip = [&](ClipAvailablePayload p) {
        Batch b = batch;
        b.events[5].payload = std::move(p);
        return b;
    };
    struct Bad {
        ClipAvailablePayload payload;
        char const* path;
    };
    const std::string sha = clip.sha256;
    const Bad bads[] = {
        {{"short", 100500.0, sha}, "/events/5/clipId"},
        {{"has space in it", 100500.0, sha}, "/events/5/clipId"},
        {{std::string(65, 'x'), 100500.0, sha}, "/events/5/clipId"},
        {{"", 100500.0, sha}, "/events/5/clipId"},
        {{"clip-19a0b1c2d3e-5f6a7b8c", 100500.0, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"}, "/events/5/sha256"},
        {{"clip-19a0b1c2d3e-5f6a7b8c", 100500.0, "ba7816bf"}, "/events/5/sha256"},
        {{"clip-19a0b1c2d3e-5f6a7b8c", 100500.0, ""}, "/events/5/sha256"},
        {{"clip-19a0b1c2d3e-5f6a7b8c", 0.0, sha}, "/events/5/durationMs"},
        {{"clip-19a0b1c2d3e-5f6a7b8c", -1.0, sha}, "/events/5/durationMs"},
        {{"clip-19a0b1c2d3e-5f6a7b8c", std::nan(""), sha}, "/events/5/durationMs"},
    };
    for (auto const& bad : bads) {
        Batch b = withClip(bad.payload);
        CHECK_MSG(!validateBatch(b, &err) && err.find(bad.path) != std::string::npos, std::string(bad.path) + " got: " + err);
    }
    CHECK_MSG(validateBatch(withClip({std::string(64, 'x'), 0.5, sha}), &err), err);   // 64 characters and a sub-ms clip are legal

    // ordering: it must name a started attempt; like timing_window its t / tick are exempt
    Batch orphan = batch;
    orphan.events[5].attemptId = "never-started";
    CHECK(!checkBatchInvariants(orphan, -1, {}, &err) && err.find("/events/5/attemptId") != std::string::npos);
    Batch later;
    later.sessionId = batch.sessionId;
    later.seq = 1;
    later.nonce = batch.nonce;
    later.clientBuild = batch.clientBuild;
    later.events = {batch.events[5]};
    std::set<std::string> known = {"clp-a1"};
    CHECK_MSG(checkBatchInvariants(later, 4, known, &err), err);
    CHECK(!checkBatchInvariants(later, 4, {}, &err));
    Batch early = batch;                       // cut while the attempt was still running: t = 0 / tick = 0
    early.events[5].t = 0.0;
    early.events[5].tick = 0;
    CHECK_MSG(checkBatchInvariants(early, -1, {}, &err), err);
    early.events[5].seq = 4;                   // its seq still has to increase
    CHECK(!checkBatchInvariants(early, -1, {}, &err) && err.find("/events/5/seq") != std::string::npos);

    // session_invalid reopen: a clip of an attempt of the invalidated session is an orphan (spooled)
    std::set<std::string> started;
    auto split = splitForReopenedSession({batch.events[5]}, started);
    CHECK(split.send.empty() && split.orphaned.size() == 1);
    started = {"clp-a1"};
    split = splitForReopenedSession({batch.events[5]}, started);
    CHECK(split.send.size() == 1 && split.orphaned.empty());
}

/// geode v0.6.1 (stream M4, telemetry revision 3): the NEW golden tests/fixtures/telemetry/
/// batch-sequence.json with two `sequence_window` events (a pair, and a triple emitted after the
/// attempt ended) must parse, validate, pass the invariants and re-canonicalise to the recorded
/// bytes / SHA-256 / HMAC; the validator mirrors validate.ts on malformed groups; the three older
/// goldens never contain the kind.
void testSequenceFixture() {
    SECTION("sequence golden (v0.6.1): sequence_window {inputSeqs[], localWidthsMs[], jointFeasibleShare, samples, resolutionMs, solverVersion}");
    CHECK(kTelemetryRevision >= 3 && kSequenceWindowRevision == 3);
    CHECK(kEventKindCount >= 11);
    CHECK(kSequenceMinInputs == 2 && kSequenceMaxInputs == 3);
    CHECK(std::string(kindName(EventKind::SequenceWindow)) == "sequence_window");
    EventKind parsed = EventKind::Input;
    CHECK(parseKind("sequence_window", parsed) && parsed == EventKind::SequenceWindow);
    // the variant index IS the kind: appended last, the ten older kinds keep their positions
    CHECK(static_cast<int>(EventKind::ClipAvailable) == 9 && static_cast<int>(EventKind::SequenceWindow) == 10);

    std::string text = fixture("telemetry/batch-sequence.json");
    std::string expectedText = fixture("telemetry/batch-sequence.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.events.size() == 12);
    CHECK(batch.clientBuild == "gprl-geode 0.6.1+win");
    CHECK(batch.events[8].kind() == EventKind::SequenceWindow && batch.events[11].kind() == EventKind::SequenceWindow);
    auto const& pair = std::get<SequenceWindowPayload>(batch.events[8].payload);
    CHECK((pair.inputSeqs == std::vector<int64_t>{2, 3}));
    CHECK((pair.localWidthsMs == std::vector<double>{12.5, 12.5}));
    CHECK(pair.jointFeasibleShare == 0.7777777777777778 && pair.samples == 4);
    CHECK(pair.resolutionMs == 4.166666666666667 && pair.solverVersion == "gprl-clone-seq/1");
    // t / tick are the first input's (events[2]), not the emission time
    CHECK(batch.events[8].t == batch.events[2].t && batch.events[8].tick == batch.events[2].tick);
    auto const& triple = std::get<SequenceWindowPayload>(batch.events[11].payload);
    CHECK((triple.inputSeqs == std::vector<int64_t>{2, 3, 4}) && triple.localWidthsMs.size() == 3 && triple.samples == 12);
    CHECK(batch.events[10].kind() == EventKind::AttemptEnd);   // the triple follows attempt_end: deferred output

    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(static_cast<int64_t>(canonical.size()) == expected.getInt("canonicalLength"));
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the sequence fixture");

    // an event the mod builds itself serialises to exactly the fixture's event
    Event own;
    own.t = 1.5;
    own.tick = 360;
    own.seq = 8;
    own.attemptId = "seq-a1";
    own.payload = SequenceWindowPayload{{2, 3}, {12.5, 12.5}, 7.0 / 9.0, 4, 1000.0 / 240.0, "gprl-clone-seq/1"};
    CHECK(json::canonical(toJson(own)) == json::canonical(toJson(batch.events[8])));
    CHECK(json::stringify(toJson(own))
          == "{\"kind\":\"sequence_window\",\"t\":1.5,\"tick\":360,\"seq\":8,\"attemptId\":\"seq-a1\",\"inputSeqs\":[2,3],\"localWidthsMs\":[12.5,12.5],"
             "\"jointFeasibleShare\":0.7777777777777778,\"samples\":4,\"resolutionMs\":4.166666666666667,\"solverVersion\":\"gprl-clone-seq/1\"}");

    // the frozen goldens of revisions 1 and 2 never contain the kind
    for (char const* older : {"telemetry/batch-basic.json", "telemetry/batch-capture.json", "telemetry/batch-clip.json"}) {
        Batch old;
        CHECK_MSG(parseBatch(fixture(older), old, &err), err);
        for (auto const& e : old.events) CHECK(e.kind() != EventKind::SequenceWindow);
        CHECK(serializeBatch(old, false).find("sequence_window") == std::string::npos);
    }

    // validator mirror of validate.ts (shared/test/telemetry.test.ts "rejects malformed groups ...")
    struct Bad {
        SequenceWindowPayload payload;
        char const* path;
    };
    const Bad bads[] = {
        {{{2}, {12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/inputSeqs"},
        {{{2, 3, 4, 5}, {1.0, 1.0, 1.0, 1.0}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/inputSeqs"},
        {{{3, 2}, {12.5, 12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/inputSeqs/1"},
        {{{2, 2}, {12.5, 12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/inputSeqs/1"},
        {{{-1, 3}, {12.5, 12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/inputSeqs/0"},
        {{{2, 3}, {12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/localWidthsMs"},
        {{{2, 3}, {12.5, 12.5, 12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/localWidthsMs"},
        {{{2, 3}, {12.5, 0.0}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/localWidthsMs/1"},
        {{{2, 3}, {-1.0, 12.5}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/localWidthsMs/0"},
        {{{2, 3}, {12.5, std::nan("")}, 0.5, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/localWidthsMs/1"},
        {{{2, 3}, {12.5, 12.5}, 1.01, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/jointFeasibleShare"},
        {{{2, 3}, {12.5, 12.5}, -0.1, 4, 4.0, "gprl-clone-seq/1"}, "/events/8/jointFeasibleShare"},
        {{{2, 3}, {12.5, 12.5}, std::nan(""), 4, 4.0, "gprl-clone-seq/1"}, "/events/8/jointFeasibleShare"},
        {{{2, 3}, {12.5, 12.5}, 0.5, 0, 4.0, "gprl-clone-seq/1"}, "/events/8/samples"},
        {{{2, 3}, {12.5, 12.5}, 0.5, 4, 0.0, "gprl-clone-seq/1"}, "/events/8/resolutionMs"},
        {{{2, 3}, {12.5, 12.5}, 0.5, 4, -1.0, "gprl-clone-seq/1"}, "/events/8/resolutionMs"},
        {{{2, 3}, {12.5, 12.5}, 0.5, 4, 4.0, ""}, "/events/8/solverVersion"},
    };
    for (auto const& bad : bads) {
        Batch b = batch;
        b.events[8].payload = bad.payload;
        CHECK_MSG(!validateBatch(b, &err) && err.find(bad.path) != std::string::npos, std::string(bad.path) + " got: " + err);
    }
    for (SequenceWindowPayload const& ok : {SequenceWindowPayload{{2, 3}, {12.5, 12.5}, 0.0, 1, 0.065, "gprl-clone-seq/1"},
                                            SequenceWindowPayload{{2, 3, 4}, {0.5, 12.5, 8.0}, 1.0, 64, 4.0, "gprl-clone-seq/1"}}) {
        Batch b = batch;
        b.events[8].payload = ok;
        CHECK_MSG(validateBatch(b, &err), err);
    }
    // a wire value the validator would reject is refused by the parser too (never silently dropped)
    Batch odd;
    CHECK(!parseBatch(R"({"schema":"gprl.telemetry/1","sessionId":"s","seq":0,"nonce":"n","clientBuild":"b","events":[
        {"kind":"sequence_window","t":0,"tick":0,"seq":0,"attemptId":"a","inputSeqs":[2,"3"],"localWidthsMs":[1,1],"jointFeasibleShare":1,"samples":1,"resolutionMs":1,"solverVersion":"x"}]})",
                      odd, &err));
    CHECK(!parseBatch(R"({"schema":"gprl.telemetry/1","sessionId":"s","seq":0,"nonce":"n","clientBuild":"b","events":[
        {"kind":"sequence_window","t":0,"tick":0,"seq":0,"attemptId":"a","inputSeqs":[2,3],"jointFeasibleShare":1,"samples":1,"resolutionMs":1,"solverVersion":"x"}]})",
                      odd, &err));

    // ordering: it must name a started attempt; like timing_window its t / tick are exempt
    Batch orphan = batch;
    orphan.events[11].attemptId = "never-started";
    CHECK(!checkBatchInvariants(orphan, -1, {}, &err) && err.find("/events/11/attemptId") != std::string::npos);
    Batch later;
    later.sessionId = batch.sessionId;
    later.seq = 1;
    later.nonce = batch.nonce;
    later.clientBuild = batch.clientBuild;
    later.events = {batch.events[11]};
    std::set<std::string> known = {"seq-a1"};
    CHECK_MSG(checkBatchInvariants(later, 10, known, &err), err);
    CHECK(!checkBatchInvariants(later, 10, {}, &err));
    CHECK(!checkBatchInvariants(later, 11, known, &err) && err.find("/events/0/seq") != std::string::npos);   // its seq still has to increase

    // session_invalid reopen: a sequence window of an attempt of the invalidated session is an orphan (spooled)
    std::set<std::string> started;
    auto split = splitForReopenedSession({batch.events[11]}, started);
    CHECK(split.send.empty() && split.orphaned.size() == 1);
    started = {"seq-a1"};
    split = splitForReopenedSession({batch.events[11]}, started);
    CHECK(split.send.size() == 1 && split.orphaned.empty());
}

/// geode v0.7.0 (docs/TIMING_SOLVER_V2.md §4.1, telemetry revision 4): the NEW golden
/// tests/fixtures/telemetry/batch-timing-result.json with four `timing_result` events (ok + decided
/// sequence + pair, a release with hold, sequence_dependent with an undecided side, unresolved
/// without a local window after attempt_end) must parse, validate, pass the invariants and
/// re-canonicalise to the recorded bytes / SHA-256 / HMAC; the payload the MOD builds from planner
/// results serialises to exactly the fixture's event; the validator mirrors validate.ts; the four
/// older goldens never contain the kind (they stay byte-identical: golden-fixtures.test.ts).
void testTimingResultFixture() {
    SECTION("timing_result golden (v0.7.0): status + local / sequence / pair / hold + cluster + counts");
    // v0.8.0: the schema is at revision 5; the kind arrived with 4, a 0.8.0 result needs 5
    CHECK(kTelemetryRevision == 6 && kTimingResultRevision == 4 && kIsolationRevision == 5 && kCompensationRevision == 6);
    CHECK(kEventKindCount == 12);
    CHECK(std::string(kindName(EventKind::TimingResult)) == "timing_result");
    EventKind parsed = EventKind::Input;
    CHECK(parseKind("timing_result", parsed) && parsed == EventKind::TimingResult);
    CHECK(static_cast<int>(EventKind::SequenceWindow) == 10 && static_cast<int>(EventKind::TimingResult) == 11);

    std::string text = fixture("telemetry/batch-timing-result.json");
    std::string expectedText = fixture("telemetry/batch-timing-result.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.events.size() == 15);
    CHECK(batch.clientBuild == "gprl-geode 0.7.0+win");
    int results = 0;
    for (auto const& e : batch.events) if (e.kind() == EventKind::TimingResult) ++results;
    CHECK(results == 4);
    auto const& p1 = std::get<TimingResultPayload>(batch.events[8].payload);
    CHECK(p1.status == "ok" && p1.local && p1.sequence && p1.pair && !p1.hold);
    CHECK(p1.sequence->decided && p1.sequence->solverVersion == "gprl-clone-sa/1");   // the v0.7.0 golden's own version
    CHECK(p1.local->early.cause && *p1.local->early.cause == "downstream" && p1.local->early.laterInputs && *p1.local->early.laterInputs == 2);
    CHECK(p1.cluster.id == "tr-a1:1" && p1.cluster.connectedPrev == std::optional<bool>(false) && p1.cluster.connectedNext == std::optional<bool>(true));
    auto const& r = std::get<TimingResultPayload>(batch.events[10].payload);
    CHECK(r.inputKind == InputKind::Release && r.hold && r.hold->basis == "sequence" && r.hold->pressSeq == 2);
    auto const& p2 = std::get<TimingResultPayload>(batch.events[11].payload);
    CHECK(p2.status == "sequence_dependent" && p2.sequence && !p2.sequence->decided && p2.sequence->window.late.stop == "undecided");
    CHECK(!p2.sequence->window.late.failMs && !p2.sequence->window.late.cause && !p2.cluster.connectedNext);
    auto const& r2 = std::get<TimingResultPayload>(batch.events[14].payload);
    CHECK(r2.status == "unresolved" && !r2.local && !r2.sequence && batch.events[13].kind() == EventKind::AttemptEnd);

    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(static_cast<int64_t>(canonical.size()) == expected.getInt("canonicalLength"));
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the timing_result fixture");

    // the payload the mod BUILDS from planner results is exactly the fixture's P1 event
    {
        using namespace gprl::solver;
        PlannerConfig cfg;
        PassPlanner planner(cfg, kNaN);
        auto shifts = planner.nextPass();
        std::vector<ShiftOutcome> outs;
        for (double s : shifts) {
            ShiftOutcome o;
            o.nominalFrames = o.appliedFrames = s;
            if (s >= -2.0 && s <= 2.0) o.kind = ShiftKind::Survived;
            else {
                o.kind = ShiftKind::Died;
                o.deathAfterFrames = s < 0 ? 83.0 : 44.0;
                o.laterFixed = s < 0 ? 2 : 1;
            }
            outs.push_back(o);
        }
        planner.ingest(outs, true, false);
        WindowResult w = planner.result(1500.0);
        CHECK(w.valid);
        SAResult sa;
        sa.valid = true;
        sa.sequence.present = true;
        sa.sequence.early = {-5.0, -6.0, EdgeStop::Fail, EdgeCause::Self, 0, 18.0, 1717, kNaN, false};
        sa.sequence.late = {6.0, 7.0, EdgeStop::Fail, EdgeCause::Self, 0, 30.0, -1, kNaN, false};
        // v0.7.1 (Fable D3b): a wave pair re-joins the recorded run - both sides rejoin-proven
        sa.sequence.early.proof = sa.sequence.late.proof = SAProof::Rejoined;
        sa.sequence.early.provenPassFrames = -5.0;
        sa.sequence.late.provenPassFrames = 6.0;
        sa.sequence.resolutionFrames = 1.0;
        sa.sequence.trials = 9;
        sa.decided = true;
        sa.sideDecided[0] = sa.sideDecided[1] = true;
        sa.adaptationUsed = {SAAdaptation::Pair};
        sa.pair.present = true;
        sa.pair.early = {-5.0, -6.0, EdgeStop::Fail, EdgeCause::Self, 0, 18.0, 1717, kNaN, false};
        sa.pair.late = {6.0, 7.0, EdgeStop::Fail, EdgeCause::Downstream, 1, 58.0, -1, kNaN, false};
        sa.pair.resolutionFrames = 1.0;
        sa.pair.trials = 13;
        LocalEvidence ev;
        ev.window = &w;
        ev.outcomes = &planner.outcomes();
        ev.frame = 360.0;
        ev.nextFrame = 378.0;
        ev.nextFollows = true;
        ev.horizonFrame = 360.0 + 130.0;
        auto lf = localFacts(ev);
        CHECK(lf.present && lf.downstream[0] && lf.downstream[1] && !lf.noEffect);
        auto st = status::statusOf({}, lf, saFacts(&sa), {});
        CHECK(st.status == status::TimingStatus::Ok);
        TimingResultContext ctx;
        ctx.inputSeq = 2;
        ctx.kind = InputKind::Press;
        ctx.attemptInputIndex = 1;
        ctx.x = 4614.25;
        ctx.percentAtInput = 12.653214;
        ctx.subTickMs = 0.0;
        ctx.gamemode = Gamemode::Wave;
        ctx.speed = Speed::Normal;
        ctx.eventT = 1.5;
        ctx.cluster = {"tr-a1:1", 1, cluster::Tri::No, cluster::Tri::Yes};
        ctx.controlSimulations = 3;
        ctx.boundarySimulations = 42;
        auto built = buildTimingResultEvent(ctx, ev, &sa, st, true);
        CHECK_MSG(built.ok, built.error);
        // v0.7.1 adds the OPTIONAL Fable keys (proof / provenPassMs on the sequence edges, D3b);
        // the v0.7.0 golden predates them and stays byte-identical: compare without them (the
        // batch-timing-result-v071 golden carries them)
        CHECK(built.payload.sequence && built.payload.sequence->window.early.proof == std::optional<std::string>("rejoined"));
        auto withoutOptional = built.payload;
        for (auto* e : {&withoutOptional.sequence->window.early, &withoutOptional.sequence->window.late}) {
            e->proof.reset();
            e->provenPassMs.reset();
        }
        // the golden was written by v0.7.0 (gprl-clone/2, gprl-clone-sa/1) and stays byte-identical;
        // the builder now stamps v0.8.0's versions, so the comparison pins the golden's strings
        // (the current constants are checked in window_event_tests / tuning_tests)
        auto const& goldenP = std::get<TimingResultPayload>(batch.events[8].payload);
        withoutOptional.solverVersion = goldenP.solverVersion;
        if (withoutOptional.sequence && goldenP.sequence) withoutOptional.sequence->solverVersion = goldenP.sequence->solverVersion;
        Event own;
        own.t = 1.5;
        own.tick = 361;
        own.seq = 8;
        own.attemptId = "tr-a1";
        own.payload = withoutOptional;
        CHECK_MSG(json::canonical(toJson(own)) == json::canonical(toJson(batch.events[8])), json::canonical(toJson(own)));
        // the local window equals the timing_window's edges bit for bit (server cross-check)
        auto const& win = std::get<TimingWindowPayload>(batch.events[5].payload);
        auto check = checkTimingResultPayload(built.payload, 1.5, 0.0, &win);
        CHECK_MSG(check.accepted, check.reasons.empty() ? "" : check.reasons[0]);
    }

    // validator mirror of validate.ts (shared/test/timing-result.test.ts runs the same cases)
    struct Bad {
        char const* what;
        std::function<void(TimingResultPayload&)> mutate;
        char const* path;
    };
    const std::vector<Bad> bads = {
        {"ok with a sequence_dependent reason", [](TimingResultPayload& p) { p.statusReasons.push_back("sa_undecided"); }, "/events/8/status"},
        {"duplicate reason", [](TimingResultPayload& p) { p.statusReasons.push_back("local_edge_downstream"); }, "/events/8/statusReasons/1"},
        {"unknown reason", [](TimingResultPayload& p) { p.statusReasons = {"because"}; }, "/events/8/statusReasons/0"},
        {"13 reasons", [](TimingResultPayload& p) {
             p.statusReasons = {"isolated", "open_early", "open_late", "limited_by_neighbour", "limited_by_history", "attempt_start", "transition_input",
                                "local_edge_downstream", "local_edge_adaptable", "miss", "frozen_world_extension", "untested_gap_in_bracket", "half_tick_input"};
             p.status = "low_confidence";
         }, "/events/8/statusReasons"},
        {"edge not at actual + midpoint", [](TimingResultPayload& p) { p.local->earliestMs += 0.01; }, "/events/8/local/earliestMs"},
        {"cause without a fail", [](TimingResultPayload& p) { p.local->late.stop = "range"; p.local->late.failMs.reset(); }, "/events/8/local/late/cause"},
        {"fail without failMs", [](TimingResultPayload& p) { p.local->early.failMs.reset(); }, "/events/8/local/early/failMs"},
        {"bad stop", [](TimingResultPayload& p) { p.local->early.stop = "wall"; }, "/events/8/local/early/stop"},
        {"bad placement", [](TimingResultPayload& p) { p.local->early.placement = "frame"; }, "/events/8/local/early/placement"},
        {"resolution 0", [](TimingResultPayload& p) { p.local->resolutionMs = 0.0; }, "/events/8/local/resolutionMs"},
        {"sequence narrower than local", [](TimingResultPayload& p) {
             auto& w = p.sequence->window;
             w.early.passMs = -1.0 * (1000.0 / 240.0);
             w.early.failMs = -2.0 * (1000.0 / 240.0);
             w.earliestMs = p.actualMs + 0.5 * (w.early.passMs + *w.early.failMs);
         }, "/events/8/sequence/earliestMs"},
        {"decided with an undecided side", [](TimingResultPayload& p) {
             auto& w = p.sequence->window;
             w.late.stop = "undecided";
             w.late.failMs.reset();
             w.late.cause.reset();
             w.late.laterInputs.reset();
             w.late.failAfterMs.reset();
             w.late.failObjectId.reset();
             w.latestMs = p.actualMs + w.late.passMs;
         }, "/events/8/sequence/decided"},
        {"ok without a sequence", [](TimingResultPayload& p) { p.sequence.reset(); }, "/events/8/status"},
        {"sequence without local", [](TimingResultPayload& p) { p.local.reset(); p.status = "low_confidence"; p.statusReasons = {"miss"}; }, "/events/8/sequence"},
        {"hold on a press", [](TimingResultPayload& p) { p.hold = HoldRangeV2Payload{2, 10.0, 20.0, "local", 10.0, 20.0}; }, "/events/8/hold"},
        {"cluster id", [](TimingResultPayload& p) { p.cluster.id = "tr a1:1"; }, "/events/8/cluster/id"},
        {"cluster index 0", [](TimingResultPayload& p) { p.cluster.index = 0; }, "/events/8/cluster/index"},
        {"sub-tick of a whole tick", [](TimingResultPayload& p) { p.subTickMs = 1000.0 / 240.0; }, "/events/8/subTickMs"},
        {"percent over 100", [](TimingResultPayload& p) { p.percentAtInput = 100.5; }, "/events/8/percentAtInput"},
        {"input index 0", [](TimingResultPayload& p) { p.attemptInputIndex = 0; }, "/events/8/attemptInputIndex"},
        {"a miss is never ok", [](TimingResultPayload& p) { p.miss = true; }, "/events/8/status"},
        {"state_replay_failed with a valid replay", [](TimingResultPayload& p) { p.status = "state_replay_failed"; p.statusReasons = {"control_mismatch"}; }, "/events/8/stateReplayValid"},
        {"adaptation vocabulary", [](TimingResultPayload& p) { p.sequence->adaptationUsed = {"triple"}; }, "/events/8/sequence/adaptationUsed/0"},
        {"negative simulations", [](TimingResultPayload& p) { p.boundarySimulations = -1; }, "/events/8/boundarySimulations"},
        {"empty solver version", [](TimingResultPayload& p) { p.solverVersion.clear(); }, "/events/8/solverVersion"},
    };
    for (auto const& bad : bads) {
        Batch b = batch;
        auto& p = std::get<TimingResultPayload>(b.events[8].payload);
        bad.mutate(p);
        CHECK_MSG(!validateBatch(b, &err) && err.find(bad.path) != std::string::npos, std::string(bad.what) + ": " + bad.path + " got: " + err);
    }
    // Fable review D1 (shared/test/timing-result.test.ts runs the same two cases): an SA side that
    // ended undecided at the local edge carries the LOCAL bracket (valid); any other bracket on a
    // side without a fail of its own is refused
    for (int variant = 0; variant < 2; ++variant) {
        Batch b = batch;
        auto& p = std::get<TimingResultPayload>(b.events[8].payload);
        p.status = "sequence_dependent";
        p.statusReasons = {"sa_undecided", "local_edge_downstream"};
        auto& w = p.sequence->window;
        w.late = p.local->late;
        w.late.stop = "undecided";
        w.late.cause.reset();
        w.late.laterInputs.reset();
        w.late.failAfterMs.reset();
        w.late.failObjectId.reset();
        if (variant == 1) *w.late.failMs += 1000.0 / 240.0;
        w.latestMs = variant == 0 ? p.local->latestMs : p.actualMs + 0.5 * (w.late.passMs + *w.late.failMs);
        p.sequence->decided = false;
        if (variant == 0) CHECK_MSG(validateBatch(b, &err), err);
        else CHECK_MSG(!validateBatch(b, &err) && err.find("/events/8/sequence/late/failMs") != std::string::npos, err);
    }
    // the variants the solver produces validate: the release with its hold, the undecided press,
    // the unresolved result without windows (each on its own copy of the batch)
    for (size_t idx : {size_t(10), size_t(11), size_t(14)}) {
        Batch b = batch;
        CHECK_MSG(validateBatch(b, &err), err);
        CHECK(b.events[idx].kind() == EventKind::TimingResult);
    }
    // a wire value the validator would reject is refused by the parser too (never silently dropped):
    // a missing nullable key, and a non-string reason
    Batch odd;
    std::string missingFail = text;
    auto at = missingFail.find("\"failMs\": null");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        missingFail.replace(at, std::string("\"failMs\": null,").size(), "");
        CHECK(!parseBatch(missingFail, odd, &err) && err.find("failMs") != std::string::npos);
    }
    std::string numericReason = text;
    auto rr = numericReason.find("\"cut_by_restart\"");
    CHECK(rr != std::string::npos);
    if (rr != std::string::npos) {
        numericReason.replace(rr, std::string("\"cut_by_restart\"").size(), "7");
        CHECK(!parseBatch(numericReason, odd, &err));
    }
    // the booleans are required like in validate.ts `bool()`: a missing / mistyped key must not
    // read as `false` (the unresolved R2 and the undecided P2 would otherwise still validate)
    {
        std::string noMiss = text;
        auto m = noMiss.rfind("\"miss\": false,");
        CHECK(m != std::string::npos);
        if (m != std::string::npos) {
            noMiss.replace(m, std::string("\"miss\": false,").size(), "");
            CHECK_MSG(!parseBatch(noMiss, odd, &err) && err.find("timing_result.miss") != std::string::npos, err);
        }
        std::string textReplay = text;
        auto s = textReplay.rfind("\"stateReplayValid\": true");
        CHECK(s != std::string::npos);
        if (s != std::string::npos) {
            textReplay.replace(s, std::string("\"stateReplayValid\": true").size(), "\"stateReplayValid\": \"true\"");
            CHECK_MSG(!parseBatch(textReplay, odd, &err) && err.find("timing_result.stateReplayValid") != std::string::npos, err);
        }
        std::string numericDecided = text;
        auto d = numericDecided.find("\"decided\": false");
        CHECK(d != std::string::npos);
        if (d != std::string::npos) {
            numericDecided.replace(d, std::string("\"decided\": false").size(), "\"decided\": 0");
            CHECK_MSG(!parseBatch(numericDecided, odd, &err) && err.find("timing_result.sequence.decided") != std::string::npos, err);
        }
    }

    // ordering: deferred output (t / tick exempt) that must name a started attempt
    Batch orphan = batch;
    orphan.events[14].attemptId = "never-started";
    CHECK(!checkBatchInvariants(orphan, -1, {}, &err) && err.find("/events/14/attemptId") != std::string::npos);
    Batch later;
    later.sessionId = batch.sessionId;
    later.seq = 1;
    later.nonce = batch.nonce;
    later.clientBuild = batch.clientBuild;
    later.events = {batch.events[14]};
    std::set<std::string> known = {"tr-a1"};
    CHECK_MSG(checkBatchInvariants(later, 13, known, &err), err);
    CHECK(!checkBatchInvariants(later, 14, known, &err));   // its seq still has to increase

    // the frozen goldens of revisions 1-3 never contain the kind
    for (char const* older : {"telemetry/batch-basic.json", "telemetry/batch-capture.json", "telemetry/batch-clip.json", "telemetry/batch-sequence.json"}) {
        Batch old;
        CHECK_MSG(parseBatch(fixture(older), old, &err), err);
        for (auto const& e : old.events) CHECK(e.kind() != EventKind::TimingResult);
        CHECK(serializeBatch(old, false).find("timing_result") == std::string::npos);
    }
}

void testTimingResultFixture071() {
    SECTION("timing_result golden v0.7.1 (the Fable review): engineSubTickMs, proof / provenPassMs, the inherited edge, the new reasons");
    std::string text = fixture("telemetry/batch-timing-result-v071.json");
    std::string expectedText = fixture("telemetry/batch-timing-result-v071.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.clientBuild == "gprl-geode 0.7.1+win");
    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the v0.7.1 timing_result fixture");
    std::map<int64_t, TimingResultPayload const*> results;
    std::map<int64_t, TimingWindowPayload const*> windows;
    std::map<int64_t, std::pair<double, double>> inputs;   // seq -> (t, tSubTick ms)
    for (auto const& e : batch.events) {
        if (e.kind() == EventKind::TimingResult) {
            auto const& p = std::get<TimingResultPayload>(e.payload);
            results[p.inputSeq] = &p;
        }
        if (e.kind() == EventKind::TimingWindow) {
            auto const& w = std::get<TimingWindowPayload>(e.payload);
            windows[w.inputSeq] = &w;
        }
        if (e.kind() == EventKind::Input) inputs[e.seq] = {e.t, std::get<InputPayload>(e.payload).tSubTick * 1000.0 / 240.0};
    }
    CHECK(results.size() == 5);
    auto const& p1 = *results[2];
    CHECK(p1.engineSubTickMs && *p1.engineSubTickMs == 0.0);
    CHECK(p1.sequence && p1.sequence->window.early.proof == std::optional<std::string>("rejoined") && p1.sequence->window.early.provenPassMs == p1.sequence->window.early.passMs);
    auto const& r1 = *results[3];
    CHECK(r1.sequence->window.late.stop == "undecided" && !r1.sequence->window.late.cause && r1.sequence->window.late.failMs == r1.local->late.failMs);
    CHECK(r1.sequence->window.latestMs == r1.local->latestMs);
    CHECK(r1.sequence->window.early.proof == std::optional<std::string>("survived") && r1.sequence->window.early.provenPassMs);
    auto const& p2 = *results[4];
    CHECK(p2.engineSubTickMs && std::fabs(*p2.engineSubTickMs - 1.1) < 1e-12 && p2.actualMs == 1650.0 + 1.1);
    CHECK(std::find(p2.statusReasons.begin(), p2.statusReasons.end(), "open_range") != p2.statusReasons.end());
    CHECK(results[11]->status == "sequence_dependent" && !results[11]->miss && windows.count(11) == 0);
    CHECK(results[12]->miss && results[12]->statusReasons.size() == 2 && results[12]->statusReasons[1] == "speed_change_in_lookahead");
    // the mod's mirror of the server gate accepts every result against its own input event (and
    // its timing_window, when one was emitted): containment on pass AND reported edges (D1), the
    // proof keys (D3b), actualMs from the engine's sub-tick within 0.5 ms of the tracker's (D11)
    for (auto const& [seq, r] : results) {
        auto const& in = inputs[seq];
        auto w = windows.count(seq) ? windows[seq] : nullptr;
        auto gate = solver::checkTimingResultPayload(*r, in.first, in.second, w);
        CHECK_MSG(gate.accepted, "input " + std::to_string(seq) + ": " + (gate.reasons.empty() ? std::string() : gate.reasons[0]));
    }
    // the same malformed cases as shared/test/timing-result.test.ts: a wrong TYPE is refused by the
    // reader, a wrong value by the validator (consistency is the gate's, proofConsistent)
    size_t r1Index = 0;
    for (size_t i = 0; i < batch.events.size(); ++i) {
        if (batch.events[i].kind() == EventKind::TimingResult && std::get<TimingResultPayload>(batch.events[i].payload).inputSeq == 3) r1Index = i;
    }
    std::string const r1Path = "/events/" + std::to_string(r1Index);
    {
        Batch b = batch;
        std::get<TimingResultPayload>(b.events[r1Index].payload).sequence->window.early.proof = "guessed";
        CHECK_MSG(!validateBatch(b, &err) && err.find(r1Path + "/sequence/early/proof") != std::string::npos, err);
    }
    for (double bad : {1000.0 / 240.0, -0.1}) {
        Batch b = batch;
        std::get<TimingResultPayload>(b.events[r1Index].payload).engineSubTickMs = bad;
        CHECK_MSG(!validateBatch(b, &err) && err.find(r1Path + "/engineSubTickMs") != std::string::npos, err);
    }
    {
        Batch odd;
        std::string s = text;
        auto at = s.find("\"provenPassMs\": ");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            s.insert(at + std::string("\"provenPassMs\": ").size(), "\"1\", \"x\": ");
            CHECK(!parseBatch(s, odd, &err) && err.find("provenPassMs") != std::string::npos);
        }
        std::string e = text;
        auto en = e.find("\"engineSubTickMs\": 0");
        CHECK(en != std::string::npos);
        if (en != std::string::npos) {
            e.replace(en, std::string("\"engineSubTickMs\": 0").size(), "\"engineSubTickMs\": \"0\"");
            CHECK(!parseBatch(e, odd, &err) && err.find("engineSubTickMs") != std::string::npos);
        }
    }
    // the v0.7.0 golden carries none of the optional keys (older payloads stay byte-identical)
    CHECK(fixture("telemetry/batch-timing-result.json").find("engineSubTickMs") == std::string::npos);
    CHECK(fixture("telemetry/batch-timing-result.json").find("provenPassMs") == std::string::npos);
}

/// geode v0.8.0 (docs/LIVE_ISOLATION_DESIGN.md §6, telemetry revision 5): the NEW golden
/// tests/fixtures/telemetry/batch-timing-result-v080.json (an `ok` dual PAIR result with `dual_pair` +
/// the `dual` block, a `live_mutation_detected` release without any window / hold / timing_window,
/// a `state_replay_failed (control_invalid_layer_sync)` press) must parse, validate, pass the
/// invariants and re-canonicalise to the recorded bytes; the payloads the MOD builds for the
/// breach and the layer-sync result are exactly the fixture's events; the validator rules of
/// revision 5 mirror validate.ts (shared/test/timing-result.test.ts runs the same cases); the older
/// timing_result goldens carry none of it (byte-identical).
void testTimingResultFixture080() {
    SECTION("timing_result golden v0.8.0 (live isolation): live_mutation_detected, control_invalid_layer_sync, dual_pair + dual block");
    std::string text = fixture("telemetry/batch-timing-result-v080.json");
    std::string expectedText = fixture("telemetry/batch-timing-result-v080.expected.json");
    Batch batch;
    std::string err;
    CHECK_MSG(parseBatch(text, batch, &err), err);
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);
    CHECK(batch.clientBuild == "gprl-geode 0.8.0+win");
    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    std::string canonical = canonicalBody(batch);
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ from the golden");
    CHECK(static_cast<int64_t>(canonical.size()) == expected.getInt("canonicalLength"));
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));
    std::string normalized;
    for (char c : text) if (c != '\r') normalized.push_back(c);
    CHECK_MSG(serializeBatch(batch, true) + "\n" == normalized, "pretty layout differs from the v0.8.0 timing_result fixture");

    std::map<int64_t, size_t> resultIndex;
    std::set<int64_t> windowInputs;
    for (size_t i = 0; i < batch.events.size(); ++i) {
        auto const& e = batch.events[i];
        if (e.kind() == EventKind::TimingResult) resultIndex[std::get<TimingResultPayload>(e.payload).inputSeq] = i;
        if (e.kind() == EventKind::TimingWindow) windowInputs.insert(std::get<TimingWindowPayload>(e.payload).inputSeq);
    }
    CHECK(resultIndex.size() == 3);
    size_t const ia = resultIndex[2], ib = resultIndex[5], ic = resultIndex[7];
    auto const& a = std::get<TimingResultPayload>(batch.events[ia].payload);
    // The v0.8.0 golden carries the solver versions of its day (gprl-clone/3, gprl-clone-sa/2); the
    // round trip must preserve them, not rewrite them to the current solver.
    CHECK((a.status == "ok" && a.statusReasons == std::vector<std::string>{"local_edge_downstream", "dual_pair"} && a.solverVersion == "gprl-clone/3"));
    CHECK(a.sequence && a.sequence->solverVersion == "gprl-clone-sa/2" && a.sequence->decided);
    CHECK(a.dual && a.dual->p2Gamemode == Gamemode::Ship && a.dual->p2GravityFlipped && !a.dual->p2Mini && !a.dual->sameGravity);
    auto const& b = std::get<TimingResultPayload>(batch.events[ib].payload);
    CHECK(b.status == "live_mutation_detected" && !b.stateReplayValid && !b.miss && b.inputKind == InputKind::Release);
    CHECK(!b.local && !b.sequence && !b.pair && !b.hold && windowInputs.count(5) == 0 && b.dual);
    auto const& c = std::get<TimingResultPayload>(batch.events[ic].payload);
    CHECK((c.status == "state_replay_failed" && c.statusReasons == std::vector<std::string>{"control_invalid_layer_sync"} && !c.dual));

    // the payloads the MOD builds: the breach (a pair sample) and the layer-sync control, byte for
    // byte (the solver string is window_event's solverVersionFor - the ENGINE's constant - so it is
    // compared on its own and the event with the fixture's string)
    {
        using namespace gprl::solver;
        auto build = [&](TimingResultContext const& ctx, status::StatusResult const& st, bool replayValid, size_t index, char const* what) {
            auto built = buildTimingResultEvent(ctx, LocalEvidence{}, nullptr, st, replayValid);
            CHECK_MSG(built.ok, std::string(what) + ": " + built.error);
            CHECK(built.payload.solverVersion == std::string(solverVersionFor(false)));
            Event own = batch.events[index];
            auto payload = built.payload;
            payload.solverVersion = std::get<TimingResultPayload>(own.payload).solverVersion;
            own.payload = payload;
            CHECK_MSG(json::canonical(toJson(own)) == json::canonical(toJson(batch.events[index])), std::string(what) + ": " + json::canonical(toJson(own)));
            return built;
        };
        TimingResultContext cb;
        cb.inputSeq = 5;
        cb.kind = InputKind::Release;
        cb.attemptInputIndex = 2;
        cb.x = 6170.25;
        cb.percentAtInput = 31.657402;
        cb.subTickMs = 0.0;
        cb.engineSubTickMs = 0.0;
        cb.gamemode = Gamemode::Ship;
        cb.speed = Speed::Normal;
        cb.eventT = 2.1;
        cb.pressMs = 2000.0;   // a known press: the breach still drops the hold
        cb.pressSeq = 2;
        cb.cluster = {"t80-a1:1", 2, cluster::Tri::Yes, cluster::Tri::Unknown};
        cb.controlSimulations = 1;
        cb.boundarySimulations = 6;
        cb.dual = a.dual;
        status::ContextFacts breach;
        breach.liveMutation = true;
        breach.dualPair = true;
        // the caller's replay flag is true: the breach forces it false
        auto bb = build(cb, status::statusOf({}, {}, {}, breach), true, ib, "live_mutation_detected");
        auto gate = checkTimingResultPayload(bb.payload, 2.1, 0.0);
        CHECK(!gate.accepted && gate.reasons.size() == 1 && gate.reasons[0] == "live_mutation_detected");
        TimingResultContext cc;
        cc.inputSeq = 7;
        cc.kind = InputKind::Press;
        cc.attemptInputIndex = 3;
        cc.x = 6318.75;
        cc.percentAtInput = 32.419882;
        cc.engineSubTickMs = 0.0;
        cc.gamemode = Gamemode::Cube;
        cc.eventT = 2.4;
        cc.cluster = {"t80-a1:3", 1, cluster::Tri::Unknown, cluster::Tri::Unknown};
        cc.controlSimulations = 1;
        auto bc = build(cc, status::statusOf({{status::Reason::ControlInvalidLayerSync}}, {}, {}, {}), false, ic, "control_invalid_layer_sync");
        CHECK(bc.status.status == status::TimingStatus::StateReplayFailed && !bc.payload.stateReplayValid);
        // the gate mirror accepts A against its own timing_window and C (a stored, non-usable status)
        auto const& win = std::get<TimingWindowPayload>(batch.events[ia - 1].payload);
        auto ga = checkTimingResultPayload(a, 2.0, 0.0, &win);
        CHECK_MSG(ga.accepted, ga.reasons.empty() ? "" : ga.reasons[0]);
        CHECK(checkTimingResultPayload(c, 2.4, 0.0).accepted);
    }

    // validator mirror of validate.ts (shared/test/timing-result.test.ts runs the same cases)
    struct Bad {
        char const* what;
        size_t index;
        std::function<void(TimingResultPayload&)> mutate;
        char const* suffix;
    };
    TimingWindowV2Payload aLocal = *a.local;
    const std::vector<Bad> bads = {
        {"live_mutation_detected with a valid replay", ib, [](TimingResultPayload& p) { p.stateReplayValid = true; }, "/stateReplayValid"},
        {"live_mutation_detected as a miss", ib, [](TimingResultPayload& p) { p.miss = true; }, "/miss"},
        {"live_mutation_detected with a local window", ib, [aLocal](TimingResultPayload& p) {
             TimingWindowV2Payload w = aLocal;
             w.earliestMs = p.actualMs + 0.5 * (w.early.passMs + *w.early.failMs);
             w.latestMs = p.actualMs + 0.5 * (w.late.passMs + *w.late.failMs);
             p.local = w;
         }, "/local"},
        {"live_mutation_detected with a hold range", ib, [](TimingResultPayload& p) { p.hold = HoldRangeV2Payload{2, 90.0, 110.0, "local", 90.0, 110.0}; }, "/hold"},
        {"live_mutation_detected without its reason", ib, [](TimingResultPayload& p) { p.statusReasons = {"dual_pair"}; }, "/status"},
        {"the reason without the status", ic, [](TimingResultPayload& p) { p.statusReasons = {"control_invalid_layer_sync", "live_mutation_detected"}; }, "/status"},
        {"dual_pair without the dual block", ia, [](TimingResultPayload& p) { p.dual.reset(); }, "/dual"},
        {"control_invalid_unisolated with a valid replay", ic, [](TimingResultPayload& p) {
             p.statusReasons = {"control_invalid_unisolated"};
             p.stateReplayValid = true;
         }, "/stateReplayValid"},
    };
    for (auto const& bad : bads) {
        Batch bb = batch;
        bad.mutate(std::get<TimingResultPayload>(bb.events[bad.index].payload));
        std::string path = "/events/" + std::to_string(bad.index) + bad.suffix;
        CHECK_MSG(!validateBatch(bb, &err) && err.find(path) != std::string::npos, std::string(bad.what) + ": " + path + " got: " + err);
    }
    // the variants the isolated engine produces validate
    {
        Batch v = batch;
        std::get<TimingResultPayload>(v.events[ia].payload).statusReasons = {"local_edge_downstream"};   // the block alone is legal
        auto& pb = std::get<TimingResultPayload>(v.events[ib].payload);
        pb.statusReasons = {"live_mutation_detected", "control_mismatch", "dual_pair"};
        std::get<TimingResultPayload>(v.events[ic].payload).statusReasons = {"control_invalid_unisolated"};
        CHECK_MSG(validateBatch(v, &err), err);
    }
    // the reader is strict on the block: null, a missing key, a mistyped key, an unknown gamemode
    {
        Batch odd;
        auto at = normalized.find("\"dual\": {");   // CRLF checkouts: compare on the LF text
        CHECK(at != std::string::npos);
        auto mutateText = [&](std::string const& from, std::string const& to) {
            std::string t = normalized;
            auto pos = t.find(from, at);
            CHECK(pos != std::string::npos);
            if (pos != std::string::npos) t.replace(pos, from.size(), to);
            return t;
        };
        std::string nullDual = normalized;
        std::string block = "\"dual\": {\n        \"p2Gamemode\": \"ship\",\n        \"p2GravityFlipped\": true,\n        \"p2Mini\": false,\n        \"sameGravity\": false\n      }";
        auto bpos = nullDual.find(block);
        CHECK(bpos != std::string::npos);
        if (bpos != std::string::npos) {
            nullDual.replace(bpos, block.size(), "\"dual\": null");
            CHECK_MSG(!parseBatch(nullDual, odd, &err) && err.find("timing_result.dual") != std::string::npos, err);
        }
        CHECK(!parseBatch(mutateText("\"p2Mini\": false", "\"p2Mini\": 0"), odd, &err) && err.find("timing_result.dual.p2Mini") != std::string::npos);
        CHECK(!parseBatch(mutateText("\"p2Gamemode\": \"ship\"", "\"p2Gamemode\": \"jetpack\""), odd, &err) && err.find("timing_result.dual.p2Gamemode") != std::string::npos);
        CHECK(!parseBatch(mutateText(",\n        \"sameGravity\": false", ""), odd, &err) && err.find("timing_result.dual.sameGravity") != std::string::npos);
    }
    // the older timing_result goldens carry none of the revision-5 vocabulary or the block
    for (char const* older : {"telemetry/batch-timing-result.json", "telemetry/batch-timing-result-v071.json"}) {
        std::string t = fixture(older);
        for (char const* word : {"live_mutation_detected", "control_invalid_", "dual_pair", "\"dual\""}) CHECK_MSG(t.find(word) == std::string::npos, std::string(older) + " " + word);
    }
}

/// tests/fixtures/telemetry/batch-death-detector.json (geode 0.14.9, noclip-death-detector/2): the
/// optional death fields parse, validate, re-serialise to the same bytes and re-canonicalise to the
/// TypeScript reference; older goldens stay free of them.
void testDeathDetectorFixture() {
    SECTION("death detector golden: detector / source / player / hazardType / contactId / attemptGeneration");
    std::string text = fixture("telemetry/batch-death-detector.json");
    std::string expectedText = fixture("telemetry/batch-death-detector.expected.json");
    CHECK_MSG(!text.empty() && !expectedText.empty(), "batch-death-detector golden missing");
    if (text.empty() || expectedText.empty()) return;
    json::Value expected;
    CHECK(json::parse(expectedText, expected));
    Batch batch;
    std::string err;
    bool parsed = parseBatch(text, batch, &err);
    CHECK_MSG(parsed, err);
    if (!parsed) return;
    CHECK(batch.events.size() == 8);
    CHECK(batch.clientBuild == "gprl-geode 0.14.9+win");
    CHECK_MSG(validateBatch(batch, &err), err);
    CHECK_MSG(checkBatchInvariants(batch, -1, {}, &err), err);

    std::string canonical = canonicalBody(batch);
    CHECK(canonical.size() == static_cast<size_t>(expected.getInt("canonicalLength")));
    CHECK_MSG(canonical == expected.getString("canonical"), "canonical bytes differ");
    CHECK(crypto::toHex(crypto::sha256(canonical)) == expected.getString("canonicalSha256"));
    std::string key;
    CHECK(crypto::fromHex(expected["hmac"].getString("keyHex"), key));
    CHECK(crypto::toHex(crypto::hmacSha256(key, canonical)) == expected["hmac"].getString("signatureHex"));

    json::Value original;
    CHECK(json::parse(text, original));
    CHECK(toJson(batch) == original);
    std::string pretty = serializeBatch(batch, true) + "\n";
    std::string normalized = text;
    std::string::size_type pos = 0;
    while ((pos = normalized.find("\r\n", pos)) != std::string::npos) normalized.replace(pos, 2, "\n");
    CHECK_MSG(pretty == normalized, "pretty layout differs from the fixture");

    // decoded payloads: one would-be death per lethal contact, each with its real player
    auto const& spike = std::get<DeathPayload>(batch.events[1].payload);
    CHECK(spike.wouldBe && spike.detector == "noclip-death-detector/2" && spike.source == "live_gd_death");
    CHECK(spike.player == 1 && spike.hazardType == 2 && spike.contactId == 1 && spike.attemptGeneration == 41 && spike.objectId == 8);
    auto const& crush = std::get<DeathPayload>(batch.events[2].payload);
    CHECK(crush.wouldBe && crush.player == 2 && crush.hazardType == -1 && crush.contactId == 2 && crush.objectId == 0);
    auto const& saw = std::get<DeathPayload>(batch.events[3].payload);
    CHECK(saw.player == 1 && saw.contactId == 3 && saw.objectId == 1705);
    auto const& real = std::get<DeathPayload>(batch.events[6].payload);
    CHECK(!real.wouldBe && real.contactId == 0 && real.attemptGeneration == 42 && real.source == "live_gd_death");

    // the validator: only an accepted death of a real live player is ever sent
    Batch bad = batch;
    std::get<DeathPayload>(bad.events[1].payload).source = "rejected_clone";
    CHECK(!validateBatch(bad, &err));
    bad = batch;
    std::get<DeathPayload>(bad.events[1].payload).source = "external_kill";   // a would-be death is a live_gd_death
    CHECK(!validateBatch(bad, &err));
    bad = batch;
    std::get<DeathPayload>(bad.events[6].payload).source = "external_kill";   // a real death may be
    CHECK_MSG(validateBatch(bad, &err), err);
    bad = batch;
    std::get<DeathPayload>(bad.events[2].payload).player = 0;
    CHECK(!validateBatch(bad, &err));
    bad = batch;
    std::get<DeathPayload>(bad.events[3].payload).contactId = 0;
    CHECK(!validateBatch(bad, &err));
    bad = batch;
    std::get<DeathPayload>(bad.events[6].payload).attemptGeneration = 0;
    CHECK(!validateBatch(bad, &err));

    // an older build's death (no detector) writes none of the keys, and the older goldens have none
    Batch old = batch;
    std::get<DeathPayload>(old.events[1].payload).detector.clear();
    std::string oldText = serializeBatch(old, false);
    Batch back;
    CHECK_MSG(parseBatch(oldText, back, &err), err);
    auto const& legacy = std::get<DeathPayload>(back.events[1].payload);
    CHECK(legacy.detector.empty() && legacy.source.empty() && legacy.player == 0 && legacy.hazardType == -1 && legacy.contactId == 0);
    for (char const* older : {"telemetry/batch-basic.json", "telemetry/batch-capture.json", "telemetry/batch-timing-result-v0140.json"}) {
        std::string olderText = fixture(older);
        CHECK_MSG(olderText.find("\"detector\"") == std::string::npos && olderText.find("contactId") == std::string::npos, older);
    }
}

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "D:/GPRL";
    testNumberFormatting();
    testJsonStrings();
    testGoldenBatch();
    testOwnBatchRoundTrip();
    testValidationNegatives();
    testPlaytestSpool();
    testNoclipSeen();
    testReopenSplit();
    testCaptureFixture();
    testClipFixture();
    testSequenceFixture();
    testTimingResultFixture();
    testTimingResultFixture071();
    testTimingResultFixture080();
    testTimingResultFixture0140();
    testDeathDetectorFixture();
    return gprl::test::finish("telemetry_roundtrip_tests");
}
