#include "telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "solver/timing_status.hpp"

namespace gprl::telemetry {

using json::Value;

namespace {

constexpr char const* kKindNames[kEventKindCount] = {
    "attempt_start", "input", "state_sample", "gamemode_change", "death", "timing_window", "progress", "attempt_end", "environment",
    "clip_available", "sequence_window", "timing_result",
};

/// validate.ts CLIP_ID_PATTERN / SHA256_HEX_PATTERN
bool clipIdOk(std::string const& id) {
    if (id.size() < 8 || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool sha256HexOk(std::string const& hex) {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool setErr(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

Value nullableNumber(std::optional<double> const& v) { return v ? Value(*v) : Value(nullptr); }

std::optional<double> optionalFromJson(Value const& v, std::string_view key) {
    auto* p = v.find(key);
    if (!p || !p->isNumber()) return std::nullopt;
    return p->asNumber();
}

std::optional<bool> optionalBoolFromJson(Value const& v, std::string_view key) {
    auto* p = v.find(key);
    if (!p || !p->isBool()) return std::nullopt;
    return p->asBool();
}

/// `key?: number | null` (schema.ts holdMinMs / holdMaxMs): absent, explicit null or a number,
/// preserved as read so the canonical bytes survive a round trip. Anything else is rejected like
/// validate.ts optNum does.
bool nullableFromJson(Value const& v, std::string_view key, Nullable<double>& out, std::string* err, char const* what) {
    auto* p = v.find(key);
    if (!p) { out = Nullable<double>::absent(); return true; }
    if (p->isNull()) { out = Nullable<double>::nullValue(); return true; }
    if (p->isNumber()) { out = Nullable<double>(p->asNumber()); return true; }
    return setErr(err, std::string("bad ") + what + " (expected a number or null)");
}

/// Caller checks !isAbsent() first: null stays null, a value prints as a number.
Value nullableToJson(Nullable<double> const& n) { return n.hasValue() ? Value(n.value()) : Value(nullptr); }

template <typename Enum>
bool readEnum(Value const& v, std::string_view key, Enum& out, std::string* err, char const* what) {
    auto* p = v.find(key);
    if (!p || !p->isString() || !parse(p->asString(), out)) return setErr(err, std::string("bad or missing ") + what);
    return true;
}

// ---- validation helpers (validate.ts) ----

struct Ctx {
    std::string* err;
    bool ok = true;
    void fail(std::string const& path, std::string const& msg) {
        if (ok && err) *err = path + ": " + msg;
        ok = false;
    }
};

bool finite(double d) { return std::isfinite(d); }

void num(Ctx& c, std::string const& path, double v, bool integer = false, std::optional<double> min = std::nullopt,
         std::optional<double> max = std::nullopt) {
    if (!finite(v)) return c.fail(path, "expected finite number");
    if (integer && std::floor(v) != v) c.fail(path, "expected integer");
    if (min && v < *min) c.fail(path, "expected >= " + json::formatNumber(*min));
    if (max && v > *max) c.fail(path, "expected <= " + json::formatNumber(*max));
}

void str(Ctx& c, std::string const& path, std::string const& v) {
    if (v.empty()) c.fail(path, "expected non-empty string");
    if (v.size() > kMaxStringLength) c.fail(path, "string longer than " + std::to_string(kMaxStringLength));
}

void slot(Ctx& c, std::string const& path, int player) {
    if (player != 1 && player != 2) c.fail(path, "expected 1 or 2");
}

// ---- timing_result (v0.7.0, telemetry revision 4) ----

constexpr char const* kEdgeStops[] = {"fail", "range", "neighbour", "history", "attempt_start", "untested", "undecided"};
constexpr char const* kEdgeCauses[] = {"self", "downstream", "extension"};
constexpr char const* kPlacements[] = {"tick", "cbf"};
constexpr char const* kAdaptations[] = {"pair", "chain2", "chain3", "comp1", "comp2", "comp3"};   // schema.ts SA_ADAPTATIONS (comp*: revision 6)
constexpr char const* kHoldBases[] = {"sequence", "local"};
constexpr char const* kEdgeProofs[] = {"local", "rejoined", "survived", "compensated"};   // schema.ts TIMING_EDGE_PROOFS (Fable D3b; compensated: revision 6)
constexpr size_t kMaxCompensationOffsets = 3;   // schema.ts: at most maxFollowers offsets per side

/// schema.ts TIMING_CLUSTER_ID_PATTERN ^[A-Za-z0-9_-]{1,64}:[0-9]{1,6}$
bool timingClusterIdOk(std::string const& id) {
    auto colon = id.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 64) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    size_t digits = id.size() - colon - 1;
    if (digits < 1 || digits > 6) return false;
    for (size_t i = colon + 1; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return true;
}

template <size_t N>
bool inVocab(std::string const& v, char const* const (&vocab)[N]) {
    for (auto const* w : vocab) {
        if (v == w) return true;
    }
    return false;
}

Value optNumber(std::optional<double> const& v) { return v ? Value(*v) : Value(nullptr); }
Value optInt(std::optional<int64_t> const& v) { return v ? Value(*v) : Value(nullptr); }
Value optBool(std::optional<bool> const& v) { return v ? Value(*v) : Value(nullptr); }

Value edgeToJson(TimingEdgeV2Payload const& e) {
    Value o = Value::object();
    o.set("passMs", e.passMs);
    o.set("failMs", optNumber(e.failMs));
    o.set("stop", e.stop);
    o.set("cause", e.cause ? Value(*e.cause) : Value(nullptr));
    o.set("laterInputs", optInt(e.laterInputs));
    o.set("failAfterMs", optNumber(e.failAfterMs));
    o.set("failObjectId", optInt(e.failObjectId));
    o.set("placement", e.placement);
    // optional Fable D3b keys: written only when present (older payloads stay byte-identical)
    if (e.proof) o.set("proof", *e.proof);
    if (e.provenPassMs) o.set("provenPassMs", *e.provenPassMs);
    return o;
}

void windowFields(Value& o, TimingWindowV2Payload const& w) {
    o.set("earliestMs", w.earliestMs);
    o.set("latestMs", w.latestMs);
    o.set("early", edgeToJson(w.early));
    o.set("late", edgeToJson(w.late));
    o.set("resolutionMs", w.resolutionMs);
    o.set("trials", w.trials);
}

Value windowToJson(TimingWindowV2Payload const& w) {
    Value o = Value::object();
    windowFields(o, w);
    return o;
}

/// A key that must be present with a number or null (schema.ts `x: number | null`, always written).
bool requiredNullableNumber(Value const& v, std::string_view key, std::optional<double>& out, std::string* err, char const* what) {
    auto* p = v.find(key);
    if (!p) return setErr(err, std::string("missing ") + what + " (number or null required)");
    if (p->isNull()) { out.reset(); return true; }
    if (!p->isNumber()) return setErr(err, std::string("bad ") + what + " (expected a number or null)");
    out = p->asNumber();
    return true;
}

bool requiredNullableInt(Value const& v, std::string_view key, std::optional<int64_t>& out, std::string* err, char const* what) {
    auto* p = v.find(key);
    if (!p) return setErr(err, std::string("missing ") + what + " (integer or null required)");
    if (p->isNull()) { out.reset(); return true; }
    if (!p->isNumber() || std::floor(p->asNumber()) != p->asNumber()) return setErr(err, std::string("bad ") + what + " (expected an integer or null)");
    out = p->asInt();
    return true;
}

/// A key that must be present with a boolean (validate.ts `bool()`): getBool's default would turn
/// a missing / mistyped key into `false` and hide it from the validator.
bool requiredBool(Value const& v, std::string_view key, bool& out, std::string* err, char const* what) {
    auto* p = v.find(key);
    if (!p || !p->isBool()) return setErr(err, std::string("bad or missing ") + what + " (expected a boolean)");
    out = p->asBool();
    return true;
}

bool requiredNullableBool(Value const& v, std::string_view key, std::optional<bool>& out, std::string* err, char const* what) {
    auto* p = v.find(key);
    if (!p) return setErr(err, std::string("missing ") + what + " (boolean or null required)");
    if (p->isNull()) { out.reset(); return true; }
    if (!p->isBool()) return setErr(err, std::string("bad ") + what + " (expected a boolean or null)");
    out = p->asBool();
    return true;
}

bool edgeFromJson(Value const* v, TimingEdgeV2Payload& e, std::string* err, std::string const& what) {
    if (!v || !v->isObject()) return setErr(err, "bad or missing " + what + " (expected an object)");
    e.passMs = v->getNumber("passMs", std::nan(""));
    if (!requiredNullableNumber(*v, "failMs", e.failMs, err, (what + ".failMs").c_str())) return false;
    e.stop = v->getString("stop");
    auto* c = v->find("cause");
    if (!c) return setErr(err, "missing " + what + ".cause (string or null required)");
    if (c->isNull()) e.cause.reset();
    else if (c->isString()) e.cause = c->asString();
    else return setErr(err, "bad " + what + ".cause (expected a string or null)");
    if (!requiredNullableInt(*v, "laterInputs", e.laterInputs, err, (what + ".laterInputs").c_str())) return false;
    if (!requiredNullableNumber(*v, "failAfterMs", e.failAfterMs, err, (what + ".failAfterMs").c_str())) return false;
    if (!requiredNullableInt(*v, "failObjectId", e.failObjectId, err, (what + ".failObjectId").c_str())) return false;
    e.placement = v->getString("placement");
    // optional Fable D3b keys: absent = not sent; present = strictly typed (validate.ts checks the same)
    if (auto* pr = v->find("proof")) {
        if (!pr->isString()) return setErr(err, "bad " + what + ".proof (expected a string)");
        e.proof = pr->asString();
    }
    if (auto* pp = v->find("provenPassMs")) {
        if (!pp->isNumber()) return setErr(err, "bad " + what + ".provenPassMs (expected a number)");
        e.provenPassMs = pp->asNumber();
    }
    return true;
}

bool windowFromJson(Value const* v, TimingWindowV2Payload& w, std::string* err, std::string const& what) {
    if (!v || !v->isObject()) return setErr(err, "bad " + what + " (expected an object)");
    w.earliestMs = v->getNumber("earliestMs", std::nan(""));
    w.latestMs = v->getNumber("latestMs", std::nan(""));
    if (!edgeFromJson(v->find("early"), w.early, err, what + ".early")) return false;
    if (!edgeFromJson(v->find("late"), w.late, err, what + ".late")) return false;
    w.resolutionMs = v->getNumber("resolutionMs", std::nan(""));
    auto* t = v->find("trials");
    if (!t || !t->isNumber() || std::floor(t->asNumber()) != t->asNumber()) return setErr(err, "bad or missing " + what + ".trials (expected an integer)");
    w.trials = t->asInt();
    return true;
}

}  // namespace

char const* kindName(EventKind k) { return kKindNames[static_cast<size_t>(k)]; }

bool parseKind(std::string_view s, EventKind& out) {
    for (int i = 0; i < kEventKindCount; ++i) {
        if (s == kKindNames[i]) { out = static_cast<EventKind>(i); return true; }
    }
    return false;
}

// ---- snapshot (schema.ts PlayerStateSnapshot, key order of the fixture) ----

Value snapshotToJson(PlayerStateSnapshot const& s) {
    Value o = Value::object();
    o.set("player", s.player);
    o.set("tick", s.tick);
    o.set("levelTime", s.levelTime);
    o.set("subTick", s.subTick);
    o.set("gamemode", name(s.gamemode));
    o.set("speed", name(s.speed));
    o.set("gravityFlipped", s.gravityFlipped);
    o.set("mini", s.mini);
    o.set("platformer", s.platformer);
    o.set("x", s.x);
    o.set("y", s.y);
    o.set("yVel", s.yVel);
    o.set("xVel", s.xVel);
    o.set("rotation", s.rotation);
    o.set("isOnGround", s.isOnGround);
    o.set("isOnSlope", s.isOnSlope);
    o.set("isHolding", s.isHolding);
    o.set("isDashing", s.isDashing);
    o.set("hasJumped", s.hasJumped);
    o.set("touchedRing", s.touchedRing);
    o.set("jumpBuffered", s.jumpBuffered);
    o.set("ringJumpPending", s.ringJumpPending);
    o.set("nearPortal", s.nearPortal);
    o.set("lastPortalObjectId", s.lastPortalObjectId);
    o.set("geometryHash", s.geometryHash);
    return o;
}

bool snapshotFromJson(Value const& v, PlayerStateSnapshot& s, std::string* err) {
    if (!v.isObject()) return setErr(err, "snapshot is not an object");
    s = PlayerStateSnapshot{};
    s.player = static_cast<int>(v.getInt("player", 1));
    s.tick = v.getInt("tick");
    s.levelTime = v.getNumber("levelTime");
    s.subTick = v.getNumber("subTick");
    if (!readEnum(v, "gamemode", s.gamemode, err, "snapshot.gamemode")) return false;
    if (!readEnum(v, "speed", s.speed, err, "snapshot.speed")) return false;
    s.gravityFlipped = v.getBool("gravityFlipped");
    s.mini = v.getBool("mini");
    s.platformer = v.getBool("platformer");
    s.x = v.getNumber("x");
    s.y = v.getNumber("y");
    s.yVel = v.getNumber("yVel");
    s.xVel = v.getNumber("xVel");
    s.rotation = v.getNumber("rotation");
    s.isOnGround = v.getBool("isOnGround");
    s.isOnSlope = v.getBool("isOnSlope");
    s.isHolding = v.getBool("isHolding");
    s.isDashing = v.getBool("isDashing");
    s.hasJumped = v.getBool("hasJumped");
    s.touchedRing = v.getBool("touchedRing");
    s.jumpBuffered = v.getBool("jumpBuffered");
    s.ringJumpPending = v.getBool("ringJumpPending");
    s.nearPortal = v.getBool("nearPortal");
    s.lastPortalObjectId = static_cast<int>(v.getInt("lastPortalObjectId"));
    double gh = v.getNumber("geometryHash");
    s.geometryHash = gh >= 0.0 && gh <= 4294967295.0 ? static_cast<uint32_t>(gh) : 0u;
    return true;
}

// ---- fingerprint (schema.ts TimingFingerprint order) ----

Value fingerprintToJson(TimingFingerprint const& f) {
    Value o = Value::object();
    o.set("gamemode", name(f.gamemode));
    o.set("speed", name(f.speed));
    o.set("gravity", name(f.gravity));
    o.set("kind", name(f.kind));
    o.set("mini", f.mini);
    o.set("windowMs", f.windowMs);
    o.set("yVelocity", f.yVelocity);
    o.set("trajectory", name(f.trajectory));
    o.set("horizontalState", name(f.horizontalState));
    o.set("prevInputGapMs", nullableNumber(f.prevInputGapMs));
    o.set("nextInputGapMs", nullableNumber(f.nextInputGapMs));
    o.set("holdMs", nullableNumber(f.holdMs));
    o.set("portalTransition", name(f.portalTransition));
    o.set("inputDirection", name(f.inputDirection));
    o.set("geometryHash", f.geometryHash);
    return o;
}

bool fingerprintFromJson(Value const& v, TimingFingerprint& f, std::string* err) {
    if (!v.isObject()) return setErr(err, "fingerprint is not an object");
    f = TimingFingerprint{};
    if (!readEnum(v, "gamemode", f.gamemode, err, "fingerprint.gamemode")) return false;
    if (!readEnum(v, "speed", f.speed, err, "fingerprint.speed")) return false;
    if (!readEnum(v, "gravity", f.gravity, err, "fingerprint.gravity")) return false;
    if (!readEnum(v, "kind", f.kind, err, "fingerprint.kind")) return false;
    f.mini = v.getBool("mini");
    f.windowMs = v.getNumber("windowMs");
    f.yVelocity = v.getNumber("yVelocity");
    if (!readEnum(v, "trajectory", f.trajectory, err, "fingerprint.trajectory")) return false;
    if (!readEnum(v, "horizontalState", f.horizontalState, err, "fingerprint.horizontalState")) return false;
    f.prevInputGapMs = optionalFromJson(v, "prevInputGapMs");
    f.nextInputGapMs = optionalFromJson(v, "nextInputGapMs");
    f.holdMs = optionalFromJson(v, "holdMs");
    if (!readEnum(v, "portalTransition", f.portalTransition, err, "fingerprint.portalTransition")) return false;
    if (!readEnum(v, "inputDirection", f.inputDirection, err, "fingerprint.inputDirection")) return false;
    f.geometryHash = v.getString("geometryHash");
    return true;
}

// ---- events ----

Value toJson(Event const& e) {
    Value o = Value::object();
    o.set("kind", kindName(e.kind()));
    o.set("t", e.t);
    o.set("tick", e.tick);
    o.set("seq", e.seq);
    o.set("attemptId", e.attemptId);
    std::visit([&](auto const& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, AttemptStartPayload>) {
            o.set("attemptNo", p.attemptNo);
            o.set("fromPercent", p.fromPercent);
            o.set("practice", p.practice);
            o.set("startPosTick", p.startPosTick ? Value(*p.startPosTick) : Value(nullptr));
            o.set("noclip", p.noclip);
            o.set("sessionAttemptCount", p.sessionAttemptCount);
            if (p.gdAttemptCount) o.set("gdAttemptCount", *p.gdAttemptCount);
        }
        else if constexpr (std::is_same_v<T, InputPayload>) {
            o.set("player", p.player);
            o.set("button", name(p.button));
            o.set("down", p.down);
            o.set("tSubTick", p.tSubTick);
        }
        else if constexpr (std::is_same_v<T, StateSamplePayload>) {
            o.set("state", snapshotToJson(p.state));
        }
        else if constexpr (std::is_same_v<T, GamemodeChangePayload>) {
            o.set("player", p.player);
            o.set("from", name(p.from));
            o.set("to", name(p.to));
            o.set("portalObjectId", p.portalObjectId);
        }
        else if constexpr (std::is_same_v<T, DeathPayload>) {
            o.set("percent", p.percent);
            o.set("x", p.x);
            o.set("objectId", p.objectId);
            o.set("wouldBe", p.wouldBe);
        }
        else if constexpr (std::is_same_v<T, TimingWindowPayload>) {
            o.set("inputSeq", p.inputSeq);
            o.set("inputKind", name(p.inputKind));
            o.set("earliestMs", p.earliestMs);
            o.set("latestMs", p.latestMs);
            o.set("actualMs", p.actualMs);
            o.set("boundedEarly", p.boundedEarly);
            o.set("boundedLate", p.boundedLate);
            o.set("resolutionMs", p.resolutionMs);
            // `holdMinMs?: number | null`: an absent key stays absent, an explicit null stays
            // null (canonical.ts keeps null, the signature covers it)
            if (!p.holdMinMs.isAbsent()) o.set("holdMinMs", nullableToJson(p.holdMinMs));
            if (!p.holdMaxMs.isAbsent()) o.set("holdMaxMs", nullableToJson(p.holdMaxMs));
            o.set("scope", p.scope);
            o.set("solverVersion", p.solverVersion);
            o.set("fingerprint", fingerprintToJson(p.fingerprint));
            if (p.evidenceHint) o.set("evidenceHint", *p.evidenceHint);
        }
        else if constexpr (std::is_same_v<T, ProgressPayload>) {
            o.set("percent", p.percent);
            o.set("best", p.best);
        }
        else if constexpr (std::is_same_v<T, AttemptEndPayload>) {
            o.set("reason", name(p.reason));
            o.set("percent", p.percent);
            o.set("completed", p.completed);
            o.set("legit", p.legit);
            if (p.noclipSeen) o.set("noclipSeen", *p.noclipSeen);
            if (p.activeMs) o.set("activeMs", *p.activeMs);
            if (p.practiceMs) o.set("practiceMs", *p.practiceMs);
            if (p.startPosMs) o.set("startPosMs", *p.startPosMs);
        }
        else if constexpr (std::is_same_v<T, EnvironmentPayload>) {
            Value mods = Value::array();
            for (auto const& m : p.mods) {
                Value mo = Value::object();
                mo.set("id", m.id);
                mo.set("version", m.version);
                if (m.gameplayAffecting) mo.set("gameplayAffecting", *m.gameplayAffecting);
                mods.push(std::move(mo));
            }
            o.set("mods", std::move(mods));
            if (p.modules) {
                Value modules = Value::array();
                for (auto const& m : *p.modules) {
                    Value mo = Value::object();
                    mo.set("name", m.name);
                    mo.set("size", m.size);
                    mo.set("hash", m.hash);
                    modules.push(std::move(mo));
                }
                o.set("modules", std::move(modules));
            }
            Value hashes = Value::object();
            hashes.set("gd", p.hashes.gd);
            hashes.set("geode", p.hashes.geode);
            hashes.set("gprl", p.hashes.gprl);
            hashes.set("level", p.hashes.level);
            o.set("hashes", std::move(hashes));
            o.set("cbf", p.cbf);
            if (p.tpsBypass) o.set("tpsBypass", *p.tpsBypass);
            o.set("tps", p.tps);
            o.set("fps", p.fps);
            o.set("integrity", name(p.integrity));
            if (p.trust) o.set("trust", name(*p.trust));
            if (p.noclip) o.set("noclip", *p.noclip);
            if (p.bot) o.set("bot", *p.bot);
            if (p.gdVersion) o.set("gdVersion", *p.gdVersion);
            if (p.droppedEvents) o.set("droppedEvents", *p.droppedEvents);
        }
        else if constexpr (std::is_same_v<T, ClipAvailablePayload>) {
            o.set("clipId", p.clipId);
            o.set("durationMs", p.durationMs);
            o.set("sha256", p.sha256);
        }
        else if constexpr (std::is_same_v<T, SequenceWindowPayload>) {
            Value seqs = Value::array();
            for (int64_t s : p.inputSeqs) seqs.push(Value(s));
            o.set("inputSeqs", std::move(seqs));
            Value widths = Value::array();
            for (double w : p.localWidthsMs) widths.push(Value(w));
            o.set("localWidthsMs", std::move(widths));
            o.set("jointFeasibleShare", p.jointFeasibleShare);
            o.set("samples", p.samples);
            o.set("resolutionMs", p.resolutionMs);
            o.set("solverVersion", p.solverVersion);
        }
        else if constexpr (std::is_same_v<T, TimingResultPayload>) {
            // key order = shared/src/fixtures/telemetry.ts FIXTURE_TELEMETRY_BATCH_TIMING_RESULT
            // (JSON.stringify layout); the canonical bytes sort the keys anyway
            o.set("inputSeq", p.inputSeq);
            o.set("inputKind", name(p.inputKind));
            o.set("attemptInputIndex", p.attemptInputIndex);
            o.set("x", p.x);
            o.set("percentAtInput", p.percentAtInput);
            o.set("subTickMs", p.subTickMs);
            if (p.engineSubTickMs) o.set("engineSubTickMs", *p.engineSubTickMs);   // optional (Fable D11)
            o.set("gamemode", name(p.gamemode));
            o.set("speed", name(p.speed));
            o.set("gamemodeAfter", p.gamemodeAfter ? Value(name(*p.gamemodeAfter)) : Value(nullptr));
            o.set("status", p.status);
            Value reasons = Value::array();
            for (auto const& r : p.statusReasons) reasons.push(Value(r));
            o.set("statusReasons", std::move(reasons));
            o.set("stateReplayValid", p.stateReplayValid);
            o.set("miss", p.miss);
            o.set("actualMs", p.actualMs);
            if (p.local) o.set("local", windowToJson(*p.local));
            if (p.sequence) {
                Value sq = Value::object();
                windowFields(sq, p.sequence->window);
                sq.set("solverVersion", p.sequence->solverVersion);
                sq.set("decided", p.sequence->decided);
                Value used = Value::array();
                for (auto const& a : p.sequence->adaptationUsed) used.push(Value(a));
                sq.set("adaptationUsed", std::move(used));
                sq.set("undecidedShifts", p.sequence->undecidedShifts);
                if (p.sequence->compensation) {
                    // v0.14.0 optional (revision 6): written last in the sequence object, so older
                    // payloads stay byte-identical
                    Value cp = Value::object();
                    Value early = Value::array();
                    for (double v : p.sequence->compensation->earlyOffsetsMs) early.push(Value(v));
                    Value late = Value::array();
                    for (double v : p.sequence->compensation->lateOffsetsMs) late.push(Value(v));
                    cp.set("earlyOffsetsMs", std::move(early));
                    cp.set("lateOffsetsMs", std::move(late));
                    sq.set("compensation", std::move(cp));
                }
                o.set("sequence", std::move(sq));
            }
            if (p.pair) o.set("pair", windowToJson(*p.pair));
            if (p.hold) {
                Value h = Value::object();
                h.set("pressSeq", p.hold->pressSeq);
                h.set("minMs", p.hold->minMs);
                h.set("maxMs", p.hold->maxMs);
                h.set("basis", p.hold->basis);
                h.set("localMinMs", p.hold->localMinMs);
                h.set("localMaxMs", p.hold->localMaxMs);
                o.set("hold", std::move(h));
            }
            Value cl = Value::object();
            cl.set("id", p.cluster.id);
            cl.set("index", p.cluster.index);
            cl.set("connectedPrev", optBool(p.cluster.connectedPrev));
            cl.set("connectedNext", optBool(p.cluster.connectedNext));
            o.set("cluster", std::move(cl));
            o.set("boundarySimulations", p.boundarySimulations);
            o.set("controlSimulations", p.controlSimulations);
            o.set("solverVersion", p.solverVersion);
            if (p.dual) {
                // v0.8.0 optional (revision 5): written last, so older payloads stay byte-identical
                Value d = Value::object();
                d.set("p2Gamemode", name(p.dual->p2Gamemode));
                d.set("p2GravityFlipped", p.dual->p2GravityFlipped);
                d.set("p2Mini", p.dual->p2Mini);
                d.set("sameGravity", p.dual->sameGravity);
                o.set("dual", std::move(d));
            }
        }
    }, e.payload);
    return o;
}

bool fromJson(Value const& v, Event& e, std::string* err) {
    if (!v.isObject()) return setErr(err, "event is not an object");
    EventKind kind;
    auto* kindV = v.find("kind");
    if (!kindV || !kindV->isString() || !parseKind(kindV->asString(), kind)) return setErr(err, "bad or missing event kind");
    e.t = v.getNumber("t");
    e.tick = v.getInt("tick");
    e.seq = v.getInt("seq");
    e.attemptId = v.getString("attemptId");
    switch (kind) {
        case EventKind::AttemptStart: {
            AttemptStartPayload p;
            p.attemptNo = static_cast<int>(v.getInt("attemptNo"));
            p.fromPercent = v.getNumber("fromPercent");
            p.practice = v.getBool("practice");
            if (auto* sp = v.find("startPosTick"); sp && sp->isNumber()) p.startPosTick = sp->asInt();
            p.noclip = v.getBool("noclip");
            p.sessionAttemptCount = static_cast<int>(v.getInt("sessionAttemptCount"));
            if (auto* g = v.find("gdAttemptCount"); g && g->isNumber()) p.gdAttemptCount = g->asInt();
            e.payload = p;
            break;
        }
        case EventKind::Input: {
            InputPayload p;
            p.player = static_cast<int>(v.getInt("player", 1));
            if (!readEnum(v, "button", p.button, err, "input.button")) return false;
            p.down = v.getBool("down");
            p.tSubTick = v.getNumber("tSubTick");
            e.payload = p;
            break;
        }
        case EventKind::StateSample: {
            StateSamplePayload p;
            if (!snapshotFromJson(v["state"], p.state, err)) return false;
            e.payload = p;
            break;
        }
        case EventKind::GamemodeChange: {
            GamemodeChangePayload p;
            p.player = static_cast<int>(v.getInt("player", 1));
            if (!readEnum(v, "from", p.from, err, "gamemode_change.from")) return false;
            if (!readEnum(v, "to", p.to, err, "gamemode_change.to")) return false;
            p.portalObjectId = static_cast<int>(v.getInt("portalObjectId"));
            e.payload = p;
            break;
        }
        case EventKind::Death: {
            DeathPayload p;
            p.percent = v.getNumber("percent");
            p.x = v.getNumber("x");
            p.objectId = static_cast<int>(v.getInt("objectId"));
            p.wouldBe = v.getBool("wouldBe");
            e.payload = p;
            break;
        }
        case EventKind::TimingWindow: {
            TimingWindowPayload p;
            p.inputSeq = v.getInt("inputSeq");
            if (!readEnum(v, "inputKind", p.inputKind, err, "timing_window.inputKind")) return false;
            p.earliestMs = v.getNumber("earliestMs");
            p.latestMs = v.getNumber("latestMs");
            p.actualMs = v.getNumber("actualMs");
            p.boundedEarly = v.getBool("boundedEarly");
            p.boundedLate = v.getBool("boundedLate");
            p.resolutionMs = v.getNumber("resolutionMs");
            if (!nullableFromJson(v, "holdMinMs", p.holdMinMs, err, "timing_window.holdMinMs")) return false;
            if (!nullableFromJson(v, "holdMaxMs", p.holdMaxMs, err, "timing_window.holdMaxMs")) return false;
            p.scope = v.getString("scope", "local");
            p.solverVersion = v.getString("solverVersion");
            if (!fingerprintFromJson(v["fingerprint"], p.fingerprint, err)) return false;
            if (auto* h = v.find("evidenceHint"); h && h->isString()) p.evidenceHint = h->asString();
            e.payload = p;
            break;
        }
        case EventKind::Progress: {
            ProgressPayload p;
            p.percent = v.getNumber("percent");
            p.best = v.getBool("best");
            e.payload = p;
            break;
        }
        case EventKind::AttemptEnd: {
            AttemptEndPayload p;
            if (!readEnum(v, "reason", p.reason, err, "attempt_end.reason")) return false;
            p.percent = v.getNumber("percent");
            p.completed = v.getBool("completed");
            p.legit = v.getBool("legit", true);
            p.noclipSeen = optionalBoolFromJson(v, "noclipSeen");
            p.activeMs = optionalFromJson(v, "activeMs");
            p.practiceMs = optionalFromJson(v, "practiceMs");
            p.startPosMs = optionalFromJson(v, "startPosMs");
            e.payload = p;
            break;
        }
        case EventKind::Environment: {
            EnvironmentPayload p;
            for (auto const& m : v["mods"].asArray()) {
                p.mods.push_back({m.getString("id"), m.getString("version"), optionalBoolFromJson(m, "gameplayAffecting")});
            }
            if (auto* modules = v.find("modules"); modules && modules->isArray()) {
                std::vector<EnvironmentModule> list;
                for (auto const& m : modules->asArray()) list.push_back({m.getString("name"), m.getInt("size"), m.getString("hash")});
                p.modules = std::move(list);
            }
            auto const& h = v["hashes"];
            p.hashes = {h.getString("gd"), h.getString("geode"), h.getString("gprl"), h.getString("level")};
            p.cbf = v.getBool("cbf");
            p.tpsBypass = optionalBoolFromJson(v, "tpsBypass");
            p.tps = v.getNumber("tps", 240.0);
            p.fps = v.getNumber("fps");
            if (!readEnum(v, "integrity", p.integrity, err, "environment.integrity")) return false;
            if (auto* t = v.find("trust"); t && t->isString()) {
                TrustState ts;
                if (!parse(t->asString(), ts)) return setErr(err, "bad environment.trust");
                p.trust = ts;
            }
            p.noclip = optionalBoolFromJson(v, "noclip");
            p.bot = optionalBoolFromJson(v, "bot");
            if (auto* g = v.find("gdVersion"); g && g->isString()) p.gdVersion = g->asString();
            if (auto* d = v.find("droppedEvents"); d && d->isNumber()) p.droppedEvents = d->asInt();
            e.payload = p;
            break;
        }
        case EventKind::ClipAvailable: {
            ClipAvailablePayload p;
            p.clipId = v.getString("clipId");
            p.durationMs = v.getNumber("durationMs");
            p.sha256 = v.getString("sha256");
            e.payload = p;
            break;
        }
        case EventKind::SequenceWindow: {
            SequenceWindowPayload p;
            // validate.ts rejects anything but arrays of numbers here; a reader that dropped a
            // bad element would hide it from validateBatch, so the parse itself refuses
            auto* seqs = v.find("inputSeqs");
            auto* widths = v.find("localWidthsMs");
            if (!seqs || !seqs->isArray()) return setErr(err, "bad or missing sequence_window.inputSeqs (expected an array)");
            if (!widths || !widths->isArray()) return setErr(err, "bad or missing sequence_window.localWidthsMs (expected an array)");
            for (auto const& s : seqs->asArray()) {
                if (!s.isNumber() || std::floor(s.asNumber()) != s.asNumber()) return setErr(err, "bad sequence_window.inputSeqs element (expected an integer)");
                p.inputSeqs.push_back(s.asInt());
            }
            for (auto const& w : widths->asArray()) {
                if (!w.isNumber()) return setErr(err, "bad sequence_window.localWidthsMs element (expected a number)");
                p.localWidthsMs.push_back(w.asNumber());
            }
            p.jointFeasibleShare = v.getNumber("jointFeasibleShare", std::nan(""));
            p.samples = v.getInt("samples");
            p.resolutionMs = v.getNumber("resolutionMs");
            p.solverVersion = v.getString("solverVersion");
            e.payload = p;
            break;
        }
        case EventKind::TimingResult: {
            // strict like sequence_window: a reader that dropped a malformed field would hide it
            // from validateBatch, and a nullable key is always written by both sides
            TimingResultPayload p;
            auto intField = [&](char const* key, int64_t& out) {
                auto* f = v.find(key);
                if (!f || !f->isNumber() || std::floor(f->asNumber()) != f->asNumber()) return setErr(err, std::string("bad or missing timing_result.") + key + " (expected an integer)");
                out = f->asInt();
                return true;
            };
            if (!intField("inputSeq", p.inputSeq)) return false;
            if (!readEnum(v, "inputKind", p.inputKind, err, "timing_result.inputKind")) return false;
            if (!intField("attemptInputIndex", p.attemptInputIndex)) return false;
            p.x = v.getNumber("x", std::nan(""));
            p.percentAtInput = v.getNumber("percentAtInput", std::nan(""));
            p.subTickMs = v.getNumber("subTickMs", std::nan(""));
            if (auto* es = v.find("engineSubTickMs")) {   // optional (Fable D11)
                if (!es->isNumber()) return setErr(err, "bad timing_result.engineSubTickMs (expected a number)");
                p.engineSubTickMs = es->asNumber();
            }
            if (!readEnum(v, "gamemode", p.gamemode, err, "timing_result.gamemode")) return false;
            if (!readEnum(v, "speed", p.speed, err, "timing_result.speed")) return false;
            {
                auto* g = v.find("gamemodeAfter");
                if (!g) return setErr(err, "missing timing_result.gamemodeAfter (gamemode or null required)");
                if (!g->isNull()) {
                    Gamemode gm;
                    if (!g->isString() || !parse(g->asString(), gm)) return setErr(err, "bad timing_result.gamemodeAfter");
                    p.gamemodeAfter = gm;
                }
            }
            p.status = v.getString("status");
            {
                auto* r = v.find("statusReasons");
                if (!r || !r->isArray()) return setErr(err, "bad or missing timing_result.statusReasons (expected an array)");
                for (auto const& x : r->asArray()) {
                    if (!x.isString()) return setErr(err, "bad timing_result.statusReasons element (expected a string)");
                    p.statusReasons.push_back(x.asString());
                }
            }
            if (!requiredBool(v, "stateReplayValid", p.stateReplayValid, err, "timing_result.stateReplayValid")) return false;
            if (!requiredBool(v, "miss", p.miss, err, "timing_result.miss")) return false;
            p.actualMs = v.getNumber("actualMs", std::nan(""));
            if (auto* l = v.find("local")) {
                TimingWindowV2Payload w;
                if (!windowFromJson(l, w, err, "timing_result.local")) return false;
                p.local = w;
            }
            if (auto* sq = v.find("sequence")) {
                SequenceWindowV2Payload w;
                if (!windowFromJson(sq, w.window, err, "timing_result.sequence")) return false;
                w.solverVersion = sq->getString("solverVersion");
                if (!requiredBool(*sq, "decided", w.decided, err, "timing_result.sequence.decided")) return false;
                auto* used = sq->find("adaptationUsed");
                if (!used || !used->isArray()) return setErr(err, "bad or missing timing_result.sequence.adaptationUsed (expected an array)");
                for (auto const& a : used->asArray()) {
                    if (!a.isString()) return setErr(err, "bad timing_result.sequence.adaptationUsed element (expected a string)");
                    w.adaptationUsed.push_back(a.asString());
                }
                auto* u = sq->find("undecidedShifts");
                if (!u || !u->isNumber() || std::floor(u->asNumber()) != u->asNumber()) return setErr(err, "bad or missing timing_result.sequence.undecidedShifts");
                w.undecidedShifts = u->asInt();
                if (auto* cp = sq->find("compensation")) {
                    if (!cp->isObject()) return setErr(err, "bad timing_result.sequence.compensation (expected an object)");
                    CompensationPayload c;
                    std::pair<char const*, std::vector<double>*> const parts[] = {{"earlyOffsetsMs", &c.earlyOffsetsMs}, {"lateOffsetsMs", &c.lateOffsetsMs}};
                    for (auto const& [key, dst] : parts) {
                        auto* arr = cp->find(key);
                        if (!arr || !arr->isArray()) return setErr(err, std::string("bad or missing timing_result.sequence.compensation.") + key + " (expected an array)");
                        for (auto const& x : arr->asArray()) {
                            if (!x.isNumber()) return setErr(err, std::string("bad timing_result.sequence.compensation.") + key + " element (expected a number)");
                            dst->push_back(x.asNumber());
                        }
                    }
                    w.compensation = c;
                }
                p.sequence = w;
            }
            if (auto* pr = v.find("pair")) {
                TimingWindowV2Payload w;
                if (!windowFromJson(pr, w, err, "timing_result.pair")) return false;
                p.pair = w;
            }
            if (auto* h = v.find("hold")) {
                if (!h->isObject()) return setErr(err, "bad timing_result.hold (expected an object)");
                HoldRangeV2Payload hr;
                auto* ps = h->find("pressSeq");
                if (!ps || !ps->isNumber() || std::floor(ps->asNumber()) != ps->asNumber()) return setErr(err, "bad or missing timing_result.hold.pressSeq");
                hr.pressSeq = ps->asInt();
                hr.minMs = h->getNumber("minMs", std::nan(""));
                hr.maxMs = h->getNumber("maxMs", std::nan(""));
                hr.basis = h->getString("basis");
                hr.localMinMs = h->getNumber("localMinMs", std::nan(""));
                hr.localMaxMs = h->getNumber("localMaxMs", std::nan(""));
                p.hold = hr;
            }
            {
                auto* cl = v.find("cluster");
                if (!cl || !cl->isObject()) return setErr(err, "bad or missing timing_result.cluster (expected an object)");
                p.cluster.id = cl->getString("id");
                auto* ix = cl->find("index");
                if (!ix || !ix->isNumber() || std::floor(ix->asNumber()) != ix->asNumber()) return setErr(err, "bad or missing timing_result.cluster.index");
                p.cluster.index = ix->asInt();
                if (!requiredNullableBool(*cl, "connectedPrev", p.cluster.connectedPrev, err, "timing_result.cluster.connectedPrev")) return false;
                if (!requiredNullableBool(*cl, "connectedNext", p.cluster.connectedNext, err, "timing_result.cluster.connectedNext")) return false;
            }
            if (!intField("boundarySimulations", p.boundarySimulations)) return false;
            if (!intField("controlSimulations", p.controlSimulations)) return false;
            p.solverVersion = v.getString("solverVersion");
            if (auto* d = v.find("dual")) {
                // v0.8.0 optional block (revision 5): never null, every key required and typed
                if (!d->isObject()) return setErr(err, "bad timing_result.dual (expected an object)");
                TimingResultDualPayload dual;
                if (!readEnum(*d, "p2Gamemode", dual.p2Gamemode, err, "timing_result.dual.p2Gamemode")) return false;
                if (!requiredBool(*d, "p2GravityFlipped", dual.p2GravityFlipped, err, "timing_result.dual.p2GravityFlipped")) return false;
                if (!requiredBool(*d, "p2Mini", dual.p2Mini, err, "timing_result.dual.p2Mini")) return false;
                if (!requiredBool(*d, "sameGravity", dual.sameGravity, err, "timing_result.dual.sameGravity")) return false;
                p.dual = dual;
            }
            e.payload = p;
            break;
        }
    }
    return true;
}

// ---- batch ----

Value toJson(Batch const& b) {
    Value o = Value::object();
    o.set("schema", b.schema);
    o.set("sessionId", b.sessionId);
    o.set("seq", b.seq);
    o.set("nonce", b.nonce);
    o.set("clientBuild", b.clientBuild);
    Value events = Value::array();
    for (auto const& e : b.events) events.push(toJson(e));
    o.set("events", std::move(events));
    return o;
}

bool fromJson(Value const& v, Batch& b, std::string* err) {
    if (!v.isObject()) return setErr(err, "batch is not an object");
    b.schema = v.getString("schema");
    if (b.schema != kSchema) return setErr(err, "/schema: expected " + std::string(kSchema));
    b.sessionId = v.getString("sessionId");
    b.seq = v.getInt("seq");
    b.nonce = v.getString("nonce");
    b.clientBuild = v.getString("clientBuild");
    b.events.clear();
    auto* events = v.find("events");
    if (!events || !events->isArray()) return setErr(err, "/events: expected array");
    size_t i = 0;
    for (auto const& ev : events->asArray()) {
        Event e;
        std::string inner;
        if (!fromJson(ev, e, &inner)) return setErr(err, "/events/" + std::to_string(i) + ": " + inner);
        b.events.push_back(std::move(e));
        ++i;
    }
    return true;
}

std::string canonicalBody(Batch const& b) { return json::canonical(toJson(b)); }

std::string serializeBatch(Batch const& b, bool pretty) {
    Value v = toJson(b);
    return pretty ? json::stringifyPretty(v) : json::stringify(v);
}

bool parseBatch(std::string_view text, Batch& out, std::string* err) {
    Value v;
    json::ParseError pe;
    if (!json::parse(text, v, &pe)) return setErr(err, "json: " + pe.message);
    return fromJson(v, out, err);
}

namespace {

/// validate.ts validateEdgeV2 / validateWindowV2 (collecting into Ctx). `sequence`: an edge of the
/// sequence-adjusted window, which may carry the local bracket on a side that did not end at a fail
/// of its own (Fable review D1; checked against the local edge by checkTimingResult).
void checkEdge(Ctx& c, std::string const& path, TimingEdgeV2Payload const& e, bool sequence = false) {
    num(c, path + "/passMs", e.passMs);
    if (e.failMs) num(c, path + "/failMs", *e.failMs);
    if (!inVocab(e.stop, kEdgeStops)) c.fail(path + "/stop", "expected one of fail|range|neighbour|history|attempt_start|untested|undecided");
    if (e.cause && !inVocab(*e.cause, kEdgeCauses)) c.fail(path + "/cause", "expected one of self|downstream|extension");
    if (e.laterInputs) num(c, path + "/laterInputs", static_cast<double>(*e.laterInputs), true, 0.0);
    if (e.failAfterMs) num(c, path + "/failAfterMs", *e.failAfterMs);
    if (e.failObjectId) num(c, path + "/failObjectId", static_cast<double>(*e.failObjectId), true, -1.0);
    if (!inVocab(e.placement, kPlacements)) c.fail(path + "/placement", "expected one of tick|cbf");
    // optional Fable D3b keys (their consistency with the local edge is the evidence gate's:
    // solver::checkTimingResultPayload `proof_inconsistent`)
    if (e.proof && !inVocab(*e.proof, kEdgeProofs)) c.fail(path + "/proof", "expected one of local|rejoined|survived|compensated");
    if (e.provenPassMs) num(c, path + "/provenPassMs", *e.provenPassMs);
    bool isFail = e.stop == "fail";
    if (e.cause.has_value() != isFail) c.fail(path + "/cause", "cause must be present exactly when stop is fail");
    if (e.failMs.has_value() != isFail && !(sequence && e.failMs.has_value()))
        c.fail(path + "/failMs", "failMs must be present exactly when stop is fail");
}

double edgeOffset(TimingEdgeV2Payload const& e) { return e.failMs ? 0.5 * (e.passMs + *e.failMs) : e.passMs; }

void checkWindow(Ctx& c, std::string const& path, TimingWindowV2Payload const& w, double actualMs, bool sequence = false) {
    num(c, path + "/earliestMs", w.earliestMs);
    num(c, path + "/latestMs", w.latestMs);
    checkEdge(c, path + "/early", w.early, sequence);
    checkEdge(c, path + "/late", w.late, sequence);
    num(c, path + "/resolutionMs", w.resolutionMs, false, 0.0);
    if (w.resolutionMs == 0.0) c.fail(path + "/resolutionMs", "expected > 0");
    num(c, path + "/trials", static_cast<double>(w.trials), true, 0.0);
    if (w.latestMs < w.earliestMs) c.fail(path + "/latestMs", "latestMs < earliestMs");
    if (w.early.passMs > w.late.passMs) c.fail(path + "/early/passMs", "early.passMs > late.passMs");
    if (w.early.failMs && !(*w.early.failMs < w.early.passMs)) c.fail(path + "/early/failMs", "early.failMs must lie before early.passMs");
    if (w.late.failMs && !(*w.late.failMs > w.late.passMs)) c.fail(path + "/late/failMs", "late.failMs must lie after late.passMs");
    if (std::isfinite(actualMs)) {
        if (std::fabs(w.earliestMs - (actualMs + edgeOffset(w.early))) > kTimingEdgeToleranceMs)
            c.fail(path + "/earliestMs", "earliestMs != actualMs + the early edge (midpoint when bounded)");
        if (std::fabs(w.latestMs - (actualMs + edgeOffset(w.late))) > kTimingEdgeToleranceMs)
            c.fail(path + "/latestMs", "latestMs != actualMs + the late edge (midpoint when bounded)");
    }
}

void checkTimingResult(Ctx& c, std::string const& path, TimingResultPayload const& p) {
    namespace st = solver::status;
    num(c, path + "/inputSeq", static_cast<double>(p.inputSeq), true, 0.0);
    num(c, path + "/attemptInputIndex", static_cast<double>(p.attemptInputIndex), true, 1.0);
    num(c, path + "/x", p.x);
    num(c, path + "/percentAtInput", p.percentAtInput, false, 0.0, 100.0);
    num(c, path + "/subTickMs", p.subTickMs, false, 0.0);
    if (std::isfinite(p.subTickMs) && !(p.subTickMs < kTickMs)) c.fail(path + "/subTickMs", "expected < one tick (1000/240 ms)");
    if (p.engineSubTickMs) {   // optional (Fable D11): the engine's own sub-tick, same range
        num(c, path + "/engineSubTickMs", *p.engineSubTickMs, false, 0.0);
        if (std::isfinite(*p.engineSubTickMs) && !(*p.engineSubTickMs < kTickMs)) c.fail(path + "/engineSubTickMs", "expected < one tick (1000/240 ms)");
    }
    st::TimingStatus status = st::TimingStatus::Ok;
    bool statusKnown = st::parseStatus(p.status, status);
    if (!statusKnown) c.fail(path + "/status", "expected one of the timing statuses");
    if (p.statusReasons.size() > kMaxStatusReasons) c.fail(path + "/statusReasons", "expected at most 12 reasons");
    std::vector<st::Reason> reasons;
    bool reasonsKnown = true;
    for (size_t i = 0; i < p.statusReasons.size(); ++i) {
        st::Reason r;
        if (!st::parseReason(p.statusReasons[i], r)) {
            reasonsKnown = false;
            c.fail(path + "/statusReasons/" + std::to_string(i), "expected one of the timing status reasons");
            continue;
        }
        for (auto x : reasons) {
            if (x == r) c.fail(path + "/statusReasons/" + std::to_string(i), "duplicate status reason");
        }
        reasons.push_back(r);
    }
    if (statusKnown && reasonsKnown && p.statusReasons.size() <= kMaxStatusReasons) {
        std::string why;
        if (!st::statusConsistent(status, reasons, &why)) c.fail(path + "/status", why);
    }
    num(c, path + "/actualMs", p.actualMs);
    if (p.local) checkWindow(c, path + "/local", *p.local, p.actualMs);
    if (p.sequence) {
        auto const& sq = *p.sequence;
        std::string sp = path + "/sequence";
        checkWindow(c, sp, sq.window, p.actualMs, true);
        str(c, sp + "/solverVersion", sq.solverVersion);
        num(c, sp + "/undecidedShifts", static_cast<double>(sq.undecidedShifts), true, 0.0);
        for (size_t i = 0; i < sq.adaptationUsed.size(); ++i) {
            if (!inVocab(sq.adaptationUsed[i], kAdaptations)) c.fail(sp + "/adaptationUsed/" + std::to_string(i), "expected one of pair|chain2|chain3|comp1|comp2|comp3");
            for (size_t j = 0; j < i; ++j) {
                if (sq.adaptationUsed[j] == sq.adaptationUsed[i]) c.fail(sp + "/adaptationUsed/" + std::to_string(i), "duplicate adaptation");
            }
        }
        if (sq.compensation) {
            // v0.14.0 (revision 6): finite offsets, at most kMaxCompensationOffsets per side
            std::pair<char const*, std::vector<double> const*> const parts[] = {{"earlyOffsetsMs", &sq.compensation->earlyOffsetsMs},
                                                                               {"lateOffsetsMs", &sq.compensation->lateOffsetsMs}};
            for (auto const& [key, arr] : parts) {
                std::string ap = sp + "/compensation/" + key;
                if (arr->size() > kMaxCompensationOffsets) c.fail(ap, "expected at most 3 offsets");
                for (size_t i = 0; i < arr->size(); ++i) num(c, ap + "/" + std::to_string(i), (*arr)[i]);
            }
        }
        auto open = [](std::string const& s) { return s == "undecided" || s == "untested"; };
        bool bothDecided = !open(sq.window.early.stop) && !open(sq.window.late.stop);
        if (sq.decided != bothDecided) c.fail(sp + "/decided", "decided must be true exactly when neither side is undecided / untested");
        if (!p.local) c.fail(sp, "a sequence window needs the local window");
        else {
            // validate.ts: every local pass is an SA pass; on a side that is not undecided the
            // reported edges too (the full Fable D1 rule is the evidence gate's:
            // solver::checkTimingResultPayload `sequence_not_containing_local`)
            auto const& lw = *p.local;
            double tol = kTimingEdgeToleranceMs;
            if (sq.window.early.stop == "undecided") {
                if (sq.window.early.passMs > lw.early.passMs + tol) c.fail(sp + "/early/passMs", "the sequence window must contain the local window");
            }
            else if (sq.window.earliestMs > lw.earliestMs + tol) c.fail(sp + "/earliestMs", "the sequence window must contain the local window");
            if (sq.window.late.stop == "undecided") {
                if (sq.window.late.passMs < lw.late.passMs - tol) c.fail(sp + "/late/passMs", "the sequence window must contain the local window");
            }
            else if (sq.window.latestMs < lw.latestMs - tol) c.fail(sp + "/latestMs", "the sequence window must contain the local window");
            // Fable review D1: a side without a fail of its own may only carry the LOCAL bracket
            // (the inherited edge)
            struct SideRef {
                char const* name;
                TimingEdgeV2Payload const& s;
                TimingEdgeV2Payload const& l;
            };
            for (auto const& side : {SideRef{"early", sq.window.early, lw.early}, SideRef{"late", sq.window.late, lw.late}}) {
                if (side.s.stop == "fail" || !side.s.failMs) continue;
                bool same = side.l.failMs && std::fabs(side.s.passMs - side.l.passMs) <= tol && std::fabs(*side.s.failMs - *side.l.failMs) <= tol;
                if (!same) c.fail(sp + "/" + side.name + "/failMs", "a sequence side without a fail of its own may only carry the local bracket (inherited edge)");
            }
        }
    }
    if (p.pair) checkWindow(c, path + "/pair", *p.pair, p.actualMs);
    if (p.hold) {
        std::string hp = path + "/hold";
        num(c, hp + "/pressSeq", static_cast<double>(p.hold->pressSeq), true, 0.0);
        num(c, hp + "/minMs", p.hold->minMs);
        num(c, hp + "/maxMs", p.hold->maxMs);
        if (!inVocab(p.hold->basis, kHoldBases)) c.fail(hp + "/basis", "expected one of sequence|local");
        num(c, hp + "/localMinMs", p.hold->localMinMs);
        num(c, hp + "/localMaxMs", p.hold->localMaxMs);
        if (p.hold->maxMs < p.hold->minMs) c.fail(hp + "/maxMs", "maxMs < minMs");
        if (p.hold->localMaxMs < p.hold->localMinMs) c.fail(hp + "/localMaxMs", "localMaxMs < localMinMs");
        if (p.inputKind != InputKind::Release) c.fail(hp, "a hold range belongs to a release");
    }
    if (!timingClusterIdOk(p.cluster.id)) c.fail(path + "/cluster/id", "expected <attemptId>:<input index> (A-Z a-z 0-9 _ - up to 64, then 1-6 digits)");
    num(c, path + "/cluster/index", static_cast<double>(p.cluster.index), true, 1.0);
    num(c, path + "/boundarySimulations", static_cast<double>(p.boundarySimulations), true, 0.0);
    num(c, path + "/controlSimulations", static_cast<double>(p.controlSimulations), true, 0.0);
    str(c, path + "/solverVersion", p.solverVersion);
    if (statusKnown && status == st::TimingStatus::Ok) {
        if (!p.local) c.fail(path + "/status", "ok needs a local window");
        if (!p.stateReplayValid) c.fail(path + "/status", "ok needs stateReplayValid");
        if (p.miss) c.fail(path + "/status", "a miss is never ok");
        if (!p.sequence || !p.sequence->decided) c.fail(path + "/status", "ok needs a decided sequence window");
    }
    if (statusKnown && status == st::TimingStatus::StateReplayFailed && p.stateReplayValid)
        c.fail(path + "/stateReplayValid", "state_replay_failed needs stateReplayValid false");
    // v0.8.0 (revision 5, docs/LIVE_ISOLATION_DESIGN.md §3.3): a sample aborted by the live-state
    // invariant is never a measurement - no window, no hold, no miss, the replay not valid
    if (statusKnown && status == st::TimingStatus::LiveMutationDetected) {
        if (p.stateReplayValid) c.fail(path + "/stateReplayValid", "live_mutation_detected needs stateReplayValid false");
        if (p.miss) c.fail(path + "/miss", "live_mutation_detected is never a miss");
        if (p.local) c.fail(path + "/local", "live_mutation_detected carries no window");
        if (p.sequence) c.fail(path + "/sequence", "live_mutation_detected carries no window");
        if (p.pair) c.fail(path + "/pair", "live_mutation_detected carries no window");
        if (p.hold) c.fail(path + "/hold", "live_mutation_detected carries no hold range");
    }
    // v0.8.0: `dual_pair` names the pair simulator; its player-2 facts travel in the `dual` block
    if (!p.dual && std::find(p.statusReasons.begin(), p.statusReasons.end(), "dual_pair") != p.statusReasons.end())
        c.fail(path + "/dual", "dual_pair needs the dual block");
}

}  // namespace

bool validateTimingResult(TimingResultPayload const& p, std::string const& path, std::string* err) {
    Ctx c{err};
    checkTimingResult(c, path, p);
    return c.ok;
}

bool validateBatch(Batch const& b, std::string* err) {
    Ctx c{err};
    if (b.schema != kSchema) c.fail("/schema", std::string("expected ") + kSchema);
    str(c, "/sessionId", b.sessionId);
    num(c, "/seq", static_cast<double>(b.seq), true, 0.0);
    str(c, "/nonce", b.nonce);
    str(c, "/clientBuild", b.clientBuild);
    if (b.events.size() > kMaxEventsPerBatch) c.fail("/events", "more than " + std::to_string(kMaxEventsPerBatch) + " events");
    for (size_t i = 0; i < b.events.size(); ++i) {
        auto const& e = b.events[i];
        std::string path = "/events/" + std::to_string(i);
        num(c, path + "/t", e.t, false, 0.0);
        num(c, path + "/tick", static_cast<double>(e.tick), true, 0.0);
        num(c, path + "/seq", static_cast<double>(e.seq), true, 0.0);
        str(c, path + "/attemptId", e.attemptId);
        std::visit([&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, AttemptStartPayload>) {
                num(c, path + "/attemptNo", p.attemptNo, true, 0.0);
                num(c, path + "/fromPercent", p.fromPercent, false, 0.0, 100.0);
                if (p.startPosTick) num(c, path + "/startPosTick", static_cast<double>(*p.startPosTick), true, 0.0);
                num(c, path + "/sessionAttemptCount", p.sessionAttemptCount, true, 1.0);
                if (p.gdAttemptCount) num(c, path + "/gdAttemptCount", static_cast<double>(*p.gdAttemptCount), true, 0.0);
            }
            else if constexpr (std::is_same_v<T, InputPayload>) {
                slot(c, path + "/player", p.player);
                num(c, path + "/tSubTick", p.tSubTick, false, 0.0, 1.0);
                if (p.tSubTick == 1.0) c.fail(path + "/tSubTick", "expected < 1");
            }
            else if constexpr (std::is_same_v<T, StateSamplePayload>) {
                std::string sp = path + "/state";
                auto const& s = p.state;
                slot(c, sp + "/player", s.player);
                num(c, sp + "/tick", static_cast<double>(s.tick), true, 0.0);
                num(c, sp + "/levelTime", s.levelTime, false, 0.0);
                num(c, sp + "/subTick", s.subTick, false, 0.0, 1.0);
                if (s.subTick == 1.0) c.fail(sp + "/subTick", "expected < 1");
                for (double d : {s.x, s.y, s.yVel, s.xVel, s.rotation}) {
                    if (!finite(d)) c.fail(sp, "expected finite number");
                }
                num(c, sp + "/lastPortalObjectId", s.lastPortalObjectId, true, 0.0);
            }
            else if constexpr (std::is_same_v<T, GamemodeChangePayload>) {
                slot(c, path + "/player", p.player);
                num(c, path + "/portalObjectId", p.portalObjectId, true, 0.0);
            }
            else if constexpr (std::is_same_v<T, DeathPayload>) {
                num(c, path + "/percent", p.percent, false, 0.0, 100.0);
                num(c, path + "/x", p.x);
                num(c, path + "/objectId", p.objectId, true, 0.0);
            }
            else if constexpr (std::is_same_v<T, TimingWindowPayload>) {
                num(c, path + "/inputSeq", static_cast<double>(p.inputSeq), true, 0.0);
                num(c, path + "/earliestMs", p.earliestMs);
                num(c, path + "/latestMs", p.latestMs);
                num(c, path + "/actualMs", p.actualMs);
                if (p.latestMs < p.earliestMs) c.fail(path + "/latestMs", "latestMs < earliestMs");
                num(c, path + "/resolutionMs", p.resolutionMs, false, 0.0);
                if (p.holdMinMs.hasValue()) num(c, path + "/holdMinMs", p.holdMinMs.value());
                if (p.holdMaxMs.hasValue()) num(c, path + "/holdMaxMs", p.holdMaxMs.value());
                if (p.scope != "local" && p.scope != "sequence") c.fail(path + "/scope", "expected one of local|sequence");
                str(c, path + "/solverVersion", p.solverVersion);
                num(c, path + "/fingerprint/windowMs", p.fingerprint.windowMs, false, 0.0);
                num(c, path + "/fingerprint/yVelocity", p.fingerprint.yVelocity);
                if (p.fingerprint.geometryHash.size() > kMaxStringLength) c.fail(path + "/fingerprint/geometryHash", "string too long");
                if (p.evidenceHint && *p.evidenceHint != kEvidencePlayer && *p.evidenceHint != kEvidenceLevelOnly)
                    c.fail(path + "/evidenceHint", "expected one of player|level_only");
            }
            else if constexpr (std::is_same_v<T, ProgressPayload>) {
                num(c, path + "/percent", p.percent, false, 0.0, 100.0);
            }
            else if constexpr (std::is_same_v<T, AttemptEndPayload>) {
                num(c, path + "/percent", p.percent, false, 0.0, 100.0);
                // validate.ts: each capture field >= 0, and the two parts never exceed the whole
                if (p.activeMs) num(c, path + "/activeMs", *p.activeMs, false, 0.0);
                if (p.practiceMs) num(c, path + "/practiceMs", *p.practiceMs, false, 0.0);
                if (p.startPosMs) num(c, path + "/startPosMs", *p.startPosMs, false, 0.0);
                if (p.activeMs && p.practiceMs.value_or(0.0) + p.startPosMs.value_or(0.0) > *p.activeMs + 1e-6)
                    c.fail(path + "/activeMs", "practiceMs + startPosMs exceed activeMs");
            }
            else if constexpr (std::is_same_v<T, EnvironmentPayload>) {
                if (p.mods.size() > kMaxModsPerEnvironment) c.fail(path + "/mods", "too many mods");
                for (size_t k = 0; k < p.mods.size(); ++k) {
                    str(c, path + "/mods/" + std::to_string(k) + "/id", p.mods[k].id);
                    str(c, path + "/mods/" + std::to_string(k) + "/version", p.mods[k].version);
                }
                if (p.modules) {
                    if (p.modules->size() > kMaxModulesPerEnvironment) c.fail(path + "/modules", "too many modules");
                    for (size_t k = 0; k < p.modules->size(); ++k) {
                        std::string mp = path + "/modules/" + std::to_string(k);
                        str(c, mp + "/name", (*p.modules)[k].name);
                        num(c, mp + "/size", static_cast<double>((*p.modules)[k].size), true, 0.0);
                        str(c, mp + "/hash", (*p.modules)[k].hash);
                    }
                }
                str(c, path + "/hashes/gd", p.hashes.gd);
                str(c, path + "/hashes/geode", p.hashes.geode);
                str(c, path + "/hashes/gprl", p.hashes.gprl);
                str(c, path + "/hashes/level", p.hashes.level);
                num(c, path + "/tps", p.tps, false, 1.0);
                num(c, path + "/fps", p.fps, false, 0.0);
                if (p.gdVersion) str(c, path + "/gdVersion", *p.gdVersion);
                if (p.droppedEvents) num(c, path + "/droppedEvents", static_cast<double>(*p.droppedEvents), true, 0.0);
            }
            else if constexpr (std::is_same_v<T, ClipAvailablePayload>) {
                if (!clipIdOk(p.clipId)) c.fail(path + "/clipId", "expected 8-64 characters of A-Z a-z 0-9 _ -");
                num(c, path + "/durationMs", p.durationMs, false, 0.0);
                if (p.durationMs == 0.0) c.fail(path + "/durationMs", "expected > 0");
                if (!sha256HexOk(p.sha256)) c.fail(path + "/sha256", "expected 64 lowercase hex characters");
            }
            else if constexpr (std::is_same_v<T, SequenceWindowPayload>) {
                // validate.ts `sequence_window`: 2-3 inputs, strictly increasing seqs, one positive
                // local width per input, share in [0,1], samples >= 1, resolution > 0
                if (p.inputSeqs.size() < kSequenceMinInputs || p.inputSeqs.size() > kSequenceMaxInputs)
                    c.fail(path + "/inputSeqs", "expected " + std::to_string(kSequenceMinInputs) + " to " + std::to_string(kSequenceMaxInputs) + " input seqs");
                for (size_t k = 0; k < p.inputSeqs.size(); ++k) {
                    std::string sp = path + "/inputSeqs/" + std::to_string(k);
                    num(c, sp, static_cast<double>(p.inputSeqs[k]), true, 0.0);
                    if (k > 0 && p.inputSeqs[k] <= p.inputSeqs[k - 1]) c.fail(sp, "expected strictly increasing input seqs");
                }
                if (p.localWidthsMs.size() != p.inputSeqs.size()) c.fail(path + "/localWidthsMs", "expected one local width per input seq");
                for (size_t k = 0; k < p.localWidthsMs.size(); ++k) {
                    std::string wp = path + "/localWidthsMs/" + std::to_string(k);
                    num(c, wp, p.localWidthsMs[k], false, 0.0);
                    if (p.localWidthsMs[k] == 0.0) c.fail(wp, "expected > 0");
                }
                num(c, path + "/jointFeasibleShare", p.jointFeasibleShare, false, 0.0, 1.0);
                num(c, path + "/samples", static_cast<double>(p.samples), true, 1.0);
                num(c, path + "/resolutionMs", p.resolutionMs, false, 0.0);
                if (p.resolutionMs == 0.0) c.fail(path + "/resolutionMs", "expected > 0");
                str(c, path + "/solverVersion", p.solverVersion);
            }
            else if constexpr (std::is_same_v<T, TimingResultPayload>) {
                // validate.ts validateTimingResult (revision 4, docs/TIMING_SOLVER_V2.md §4.1)
                checkTimingResult(c, path, p);
            }
        }, e.payload);
    }
    return c.ok;
}

bool checkBatchInvariants(Batch const& b, int64_t previousEventSeq, std::set<std::string> const& knownAttemptIds, std::string* err) {
    Ctx c{err};
    int64_t lastSeq = previousEventSeq;
    std::set<std::string> attempts = knownAttemptIds;
    std::map<std::string, std::pair<double, int64_t>> lastT;
    for (size_t i = 0; i < b.events.size(); ++i) {
        auto const& e = b.events[i];
        std::string path = "/events/" + std::to_string(i);
        if (e.seq <= lastSeq) c.fail(path + "/seq", "seq " + std::to_string(e.seq) + " not greater than previous " + std::to_string(lastSeq));
        lastSeq = std::max(lastSeq, e.seq);
        if (e.kind() == EventKind::Environment) continue;   // session-scoped
        if (e.kind() == EventKind::AttemptStart) {
            attempts.insert(e.attemptId);
            lastT[e.attemptId] = {e.t, e.tick};
            continue;
        }
        if (!attempts.count(e.attemptId)) c.fail(path + "/attemptId", "event before attempt_start");
        if (e.kind() == EventKind::TimingWindow) continue;   // deferred solver output: t/tick refer to the input
        if (e.kind() == EventKind::ClipAvailable) continue;  // deferred: t/tick refer to the attempt's end
        if (e.kind() == EventKind::SequenceWindow) continue; // deferred solver output: t/tick refer to the group's first input
        if (e.kind() == EventKind::TimingResult) continue;   // deferred solver output: t/tick refer to the input
        auto it = lastT.find(e.attemptId);
        if (it != lastT.end()) {
            if (e.t < it->second.first) c.fail(path + "/t", "t " + json::formatNumber(e.t) + " decreased from " + json::formatNumber(it->second.first));
            if (e.tick < it->second.second) c.fail(path + "/tick", "tick " + std::to_string(e.tick) + " decreased from " + std::to_string(it->second.second));
            it->second = {std::max(e.t, it->second.first), std::max(e.tick, it->second.second)};
        }
        else lastT[e.attemptId] = {e.t, e.tick};
    }
    return c.ok;
}

ReopenSplit splitForReopenedSession(std::vector<Event> events, std::set<std::string>& startedAttempts) {
    ReopenSplit out;
    for (auto& e : events) {
        EventKind k = e.kind();
        if (k == EventKind::AttemptStart) startedAttempts.insert(e.attemptId);
        bool sendable = k == EventKind::Environment || startedAttempts.count(e.attemptId) > 0;
        (sendable ? out.send : out.orphaned).push_back(std::move(e));
    }
    return out;
}

}  // namespace gprl::telemetry
