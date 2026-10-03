#pragma once
// Mirror of the telemetry schema `gprl.telemetry/1` (ARCHITECTURE §5, docs/TELEMETRY.md).
//
// REFERENCE: shared/src/telemetry/schema.ts (types), validate.ts (structural + ordering checks),
// canonical.ts (the bytes the signature covers). This file reproduces the wire shape exactly;
// tests/fixtures/telemetry/batch-basic.json must parse, validate, pass the invariants and
// re-canonicalise to the bytes recorded in batch-basic.expected.json
// (tests/telemetry_roundtrip_tests.cpp checks the canonical string, its SHA-256 and the HMAC).
//
// Batch  { schema: "gprl.telemetry/1", sessionId, seq, nonce, clientBuild, events: [...] }
// Event  flat object { kind, t, tick, seq, attemptId, ...payload }
//   t    = level time in seconds since the attempt start,  tick = 240 TPS tick index,
//   seq  = event counter within the session (strictly increasing across batches)
// Payload keys (schema.ts):
//   attempt_start   attemptNo, fromPercent, practice, startPosTick (number|null), noclip, sessionAttemptCount,
//                   gdAttemptCount? (v0.5.1: the GD save's total, UNTRUSTED context, MASTER §10)
//   input           player (1|2), button (jump|left|right), down, tSubTick in [0,1)
//   state_sample    state: PlayerStateSnapshot (core/snapshot.hpp, all fields)
//   gamemode_change player, from, to, portalObjectId
//   death           percent, x, objectId, wouldBe, + noclip-death-detector/2 (geode >= 0.14.9, all
//                   optional): detector, source, player, hazardType, contactId, attemptGeneration
//   timing_window   inputSeq, inputKind, earliestMs, latestMs, actualMs, boundedEarly, boundedLate,
//                   resolutionMs, holdMinMs?, holdMaxMs? (number | null | omitted; the wire form is
//                   preserved because canonical.ts keeps null), scope, solverVersion, fingerprint,
//                   evidenceHint? (v0.5.1: "player" | "level_only", a hint the server may ignore)
//   progress        percent, best
//   attempt_end     reason (death|complete|exit|restart), percent, completed, legit, noclipSeen?,
//                   activeMs?, practiceMs?, startPosMs? (v0.5.1: unpaused wall-clock ms of the attempt)
//   environment    mods[], modules[]?, hashes{gd,geode,gprl,level}, cbf, tpsBypass?, tps, fps,
//                   integrity (clean|warnings|flagged|unknown), trust?, noclip?, bot?, gdVersion?, droppedEvents?
//   clip_available  clipId, durationMs, sha256 (v0.6.0, telemetry revision 2: a clip of the event's
//                   attempt was saved on the player's computer; never the media itself)
//   sequence_window inputSeqs[], localWidthsMs[], jointFeasibleShare, samples, resolutionMs,
//                   solverVersion (v0.6.1, telemetry revision 3: the joint window of 2-3
//                   neighbouring inputs, core/solver/sequence.hpp; level evidence only)
//   timing_result   inputSeq, inputKind, attemptInputIndex, x, percentAtInput, subTickMs, gamemode,
//                   speed, gamemodeAfter (gamemode|null), status, statusReasons[], stateReplayValid,
//                   miss, actualMs, local?, sequence?, pair?, hold?, cluster{id,index,connectedPrev,
//                   connectedNext}, boundarySimulations, controlSimulations, solverVersion
//                   (v0.7.0, telemetry revision 4: the final word on one measured input,
//                   docs/TIMING_SOLVER_V2.md §4.1; windows {earliestMs, latestMs, early, late,
//                   resolutionMs, trials}, edges {passMs, failMs|null, stop, cause|null,
//                   laterInputs|null, failAfterMs|null, failObjectId|null, placement}),
//                   dual? {p2Gamemode, p2GravityFlipped, p2Mini, sameGravity} (v0.8.0, revision 5:
//                   the status `live_mutation_detected` and the reasons of gprl-timing-status/2,
//                   docs/LIVE_ISOLATION_DESIGN.md §8)
// Optional keys (`?`) are OMITTED when absent (JSON.stringify drops undefined), so the canonical
// bytes match the TypeScript side. Windows are milliseconds (double); frames at 240 are a display
// conversion (vocab.hpp framesAt240).
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "fingerprint.hpp"
#include "json.hpp"
#include "snapshot.hpp"
#include "vocab.hpp"

namespace gprl::telemetry {

constexpr char const* kSchema = "gprl.telemetry/1";

/// TELEMETRY_LIMITS in schema.ts (enforced by the server, respected by the client).
constexpr size_t kMaxEventsPerBatch = 4000;
constexpr size_t kMaxBatchBytes = 1'000'000;
constexpr size_t kMaxModsPerEnvironment = 256;
constexpr size_t kMaxModulesPerEnvironment = 1024;
constexpr size_t kMaxStringLength = 512;

enum class EventKind : uint8_t {
    AttemptStart, Input, StateSample, GamemodeChange, Death, TimingWindow, Progress, AttemptEnd, Environment, ClipAvailable,
    SequenceWindow, TimingResult
};
constexpr int kEventKindCount = 12;

/// schema.ts TELEMETRY_REVISION: additive revisions of `gprl.telemetry/1` (the schema id stays).
///   1 = everything through geode 0.5.1;  2 = adds the `clip_available` event kind (geode 0.6.0);
///   3 = adds the `sequence_window` event kind (geode 0.6.1);
///   4 = adds the `timing_result` event kind (geode 0.7.0, docs/TIMING_SOLVER_V2.md §4.1);
///   5 = `timing_result` vocabulary of `gprl-timing-status/2` (status `live_mutation_detected`,
///       reasons `live_mutation_detected`, `control_invalid_unisolated`, `control_invalid_layer_sync`,
///       `dual_pair`) and the optional `dual` block (geode 0.8.0, docs/LIVE_ISOLATION_DESIGN.md §5.5);
///   6 = the lockstep compensation vocabulary (geode 0.14.0, docs/SHIP_SOLVER.md §4): adaptations
///       `comp1` / `comp2` / `comp3`, edge proof `compensated`, the optional `compensation` block of
///       the sequence window.
/// A server tells the mod its revision in V1CreateSessionResponse.telemetryRevision (absent = 1);
/// the mod never sends an event kind of a newer revision than the server's, because a validator
/// that does not know the kind rejects the WHOLE batch. The same holds for a status / reason a
/// validator does not know: from geode 0.8.0 the mod pushes `timing_result` only to servers that
/// validate `kIsolationRevision` or later (else the `GPRL timing:` line says `NOT SENT`).
constexpr int kTelemetryRevision = 6;
constexpr int kClipAvailableRevision = 2;
constexpr int kSequenceWindowRevision = 3;
constexpr int kTimingResultRevision = 4;   // the revision the KIND arrived with (schema.ts TIMING_RESULT_REVISION)
/// schema.ts ISOLATION_REVISION: the revision a `timing_result` built by geode >= 0.8.0 needs (its
/// statuses / reasons / `dual` block). The engine's send gate (solver/GdOracle onTimingResult) uses it.
constexpr int kIsolationRevision = 5;
/// schema.ts COMPENSATION_REVISION: the revision a `timing_result` built by geode >= 0.14.0 needs
/// (its `comp*` adaptations, `compensated` proof and `compensation` block). The send gate uses it.
constexpr int kCompensationRevision = 6;
/// schema.ts TIMING_RESULT_LIMITS
constexpr size_t kMaxStatusReasons = 12;
constexpr double kTimingEdgeToleranceMs = 1e-6;
/// schema.ts SEQUENCE_WINDOW_LIMITS: inputs per sequence_window event.
constexpr size_t kSequenceMinInputs = 2;
constexpr size_t kSequenceMaxInputs = 3;
char const* kindName(EventKind k);
bool parseKind(std::string_view s, EventKind& out);

struct AttemptStartPayload {
    int attemptNo = 0;
    double fromPercent = 0.0;
    bool practice = false;
    std::optional<int64_t> startPosTick;   // null for a level start
    bool noclip = false;
    int sessionAttemptCount = 1;
    // schema.ts `gdAttemptCount?: number` (v0.5.1, MASTER §10): GJGameLevel::m_attempts as the
    // client sees it - mutable local data, never a GPRL-observed count. Omitted when absent.
    std::optional<int64_t> gdAttemptCount;
};

struct InputPayload {
    int player = 1;
    Button button = Button::Jump;
    bool down = true;
    double tSubTick = 0.0;
};

struct StateSamplePayload {
    PlayerStateSnapshot state;
};

struct GamemodeChangePayload {
    int player = 1;
    Gamemode from = Gamemode::Cube;
    Gamemode to = Gamemode::Cube;
    int portalObjectId = 0;
};

struct DeathPayload {
    double percent = 0.0;
    double x = 0.0;
    int objectId = 0;
    bool wouldBe = false;   // SPEC §19: a death that noclip skipped
    // noclip-death-detector/2 (geode >= 0.14.9, core/death_detector, docs/TELEMETRY.md §12). Older
    // builds send none of them; this build sends them together (`detector` empty = all omitted, the
    // wire form of the frozen goldens).
    std::string detector;            // death::kDetectorVersion
    std::string source;              // live_gd_death | external_kill (the only sources ever sent)
    int player = 0;                  // 1 | 2: the real live player GD tried to kill
    int hazardType = -1;             // GameObjectType of the object GD passed; -1 = none (omitted)
    int contactId = 0;               // the attempt's continuous lethal contact; 0 = a real death (omitted)
    int64_t attemptGeneration = 0;   // the mod's attempt generation the kill was raised in
};

/// Tri-state wire field for `key?: T | null` (schema.ts `holdMinMs?: number | null`). All three
/// forms are legal and distinguishable on the wire, and canonical.ts keeps `null` while dropping
/// `undefined` (rule 5), so a lossless mirror must remember which one it read: the golden fixture
/// omits the key, tools/fixture-gen writes `null`, and the signature covers those bytes.
/// Windows the mod computes itself use Absent for "not computed" (= the golden fixture).
template <class T>
class Nullable {
public:
    enum class State : uint8_t { Absent, Null, Value };

    Nullable() = default;
    Nullable(T v) : m_state(State::Value), m_value(std::move(v)) {}
    static Nullable absent() { return {}; }
    static Nullable nullValue() { Nullable n; n.m_state = State::Null; return n; }

    State state() const { return m_state; }
    bool isAbsent() const { return m_state == State::Absent; }
    bool isNull() const { return m_state == State::Null; }
    bool hasValue() const { return m_state == State::Value; }
    /// True only for a real value ("not computed" in either wire form is false).
    explicit operator bool() const { return hasValue(); }
    T const& operator*() const { return m_value; }
    T const& value() const { return m_value; }

    bool operator==(Nullable const&) const = default;

private:
    State m_state = State::Absent;
    T m_value{};
};

struct TimingWindowPayload {
    int64_t inputSeq = 0;
    InputKind inputKind = InputKind::Press;
    double earliestMs = 0.0;
    double latestMs = 0.0;
    double actualMs = 0.0;
    bool boundedEarly = false;
    bool boundedLate = false;
    double resolutionMs = 0.0;
    Nullable<double> holdMinMs;   // not computed = Absent (key omitted) or an explicit null, preserved as read
    Nullable<double> holdMaxMs;
    std::string scope = "local";       // "local" | "sequence"
    std::string solverVersion;
    TimingFingerprint fingerprint;
    // schema.ts `evidenceHint?: 'player' | 'level_only'` (v0.5.1, MASTER §13 / §20): "level_only"
    // for windows measured while a bot / macro played (level analysis only, never the player).
    // A hint - the server classifies from the session trust; omitted when absent.
    std::optional<std::string> evidenceHint;
};

constexpr char const* kEvidencePlayer = "player";
constexpr char const* kEvidenceLevelOnly = "level_only";

struct ProgressPayload {
    double percent = 0.0;
    bool best = false;
};

struct AttemptEndPayload {
    AttemptEndReason reason = AttemptEndReason::Exit;
    double percent = 0.0;
    bool completed = false;
    bool legit = true;   // informational: no noclip / physics change / bot seen by the client
    // schema.ts `noclipSeen?: boolean` (SPEC §19): noclip was on at any point of the attempt or a
    // would-be death happened. Omitted when absent (older clients, the golden fixtures); the mod
    // always sets it.
    std::optional<bool> noclipSeen;
    // schema.ts `activeMs? / practiceMs? / startPosMs?` (v0.5.1, MASTER §11): wall-clock ms the
    // level was UNPAUSED during the attempt (whole / practice-mode part / StartPos-and-not-practice
    // part; the parts are disjoint and never exceed the whole). Omitted when absent.
    std::optional<double> activeMs;
    std::optional<double> practiceMs;
    std::optional<double> startPosMs;
};

struct EnvironmentMod {
    std::string id;
    std::string version;
    std::optional<bool> gameplayAffecting;   // client-side classification (SPEC §21); the server re-classifies
};

/// A module loaded in the GD process (SPEC §22: file name + size only, never a path).
struct EnvironmentModule {
    std::string name;
    int64_t size = 0;
    std::string hash;   // hex SHA-256 of "name|size" in Phase 1 (contents are not read)
};

struct EnvironmentHashes {
    std::string gd;
    std::string geode;
    std::string gprl;
    std::string level;
};

struct EnvironmentPayload {
    std::vector<EnvironmentMod> mods;
    std::optional<std::vector<EnvironmentModule>> modules;
    EnvironmentHashes hashes;
    bool cbf = false;
    std::optional<bool> tpsBypass;
    double tps = 240.0;
    double fps = 0.0;
    Integrity integrity = Integrity::Unknown;
    std::optional<TrustState> trust;
    std::optional<bool> noclip;
    std::optional<bool> bot;
    std::optional<std::string> gdVersion;
    std::optional<int64_t> droppedEvents;
};

/// schema.ts ClipAvailableEvent (v0.6.0, MASTER §16): the player chose to keep a clip of the
/// attempt named by the event's `attemptId`. `t` / `tick` are the attempt's last ones (its
/// attempt_end; 0 when the clip was cut while the attempt was still running); like timing_window
/// it is deferred output, exempt from the per-attempt t / tick rule. Identity and integrity of the
/// file only: the media never travels in telemetry, and nothing is uploaded without the player's choice.
struct ClipAvailablePayload {
    std::string clipId;        // [A-Za-z0-9_-]{8,64}, client-generated (core/clip_flow makeClipId)
    double durationMs = 0.0;   // > 0
    std::string sha256;        // 64 lowercase hex: SHA-256 of the saved clip file
};

/// schema.ts SequenceWindowEvent (v0.6.1, MASTER §5 sequence difficulty, docs/SOLVER_DESIGN.md §13):
/// the joint timing window of 2-3 neighbouring inputs of the event's attempt. `t` / `tick` are the
/// FIRST input's; like timing_window it is deferred solver output, exempt from the per-attempt
/// t / tick rule. Level evidence only: nothing here is ever a player's sample.
struct SequenceWindowPayload {
    std::vector<int64_t> inputSeqs;      // seq of each input event, in time order (strictly increasing)
    std::vector<double> localWidthsMs;   // width of each input's local window (its timing_window event), same order, > 0
    double jointFeasibleShare = 0.0;     // [0,1]: feasible part of the box of local windows (1 = independent)
    int64_t samples = 0;                 // simulated shift combinations (>= 1)
    double resolutionMs = 0.0;           // > 0: how precisely the feasible region's boundary is located
    std::string solverVersion;           // "gprl-clone-seq/1"
};

/// schema.ts TimingEdgeV2 (v0.7.0): one side of a v2 window. passMs / failMs are SIGNED offsets
/// from the event's actualMs; the window's edge is actualMs + midpoint when bounded. The nullable
/// keys are ALWAYS written (null when the side is not bounded by a fail).
struct TimingEdgeV2Payload {
    double passMs = 0.0;
    std::optional<double> failMs;              // present iff stop == "fail" - or, on a sequence side that
                                               // ended without a fail of its own at the local pass edge,
                                               // the inherited LOCAL bracket (Fable review D1)
    std::string stop = "range";                // TIMING_EDGE_STOPS
    std::optional<std::string> cause;          // TIMING_EDGE_CAUSES, present iff stop == "fail"
    std::optional<int64_t> laterInputs;        // later FIXED inputs applied before that death
    std::optional<double> failAfterMs;         // death time - actualMs
    std::optional<int64_t> failObjectId;       // -1 = no object
    std::string placement = "tick";            // "tick" | "cbf"
    // v0.7.1 OPTIONAL (Fable review D3b; sequence edges only, never null, absent in older payloads):
    std::optional<std::string> proof;          // TIMING_EDGE_PROOFS: local | rejoined | survived
    std::optional<double> provenPassMs;        // last pass contiguous from the local edge proven by the local rule or a re-join

    bool operator==(TimingEdgeV2Payload const&) const = default;
};

/// schema.ts TimingWindowV2 (local / pair; the sequence window adds SequenceWindowV2Payload's fields).
struct TimingWindowV2Payload {
    double earliestMs = 0.0;
    double latestMs = 0.0;
    TimingEdgeV2Payload early;
    TimingEdgeV2Payload late;
    double resolutionMs = 0.0;   // > 0, never clamped to the width
    int64_t trials = 0;

    bool operator==(TimingWindowV2Payload const&) const = default;
};

/// schema.ts SequenceWindowV2.compensation (v0.14.0, telemetry revision 6, docs/SHIP_SOLVER.md §4):
/// the follower offsets (ms, relative to each follower's recorded time) of the compensated pass
/// that set each side; empty when that side was not set by a compensated pass. Optional on the
/// wire (older payloads omit it), never null; at most 3 entries per side.
struct CompensationPayload {
    std::vector<double> earlyOffsetsMs;
    std::vector<double> lateOffsetsMs;

    bool operator==(CompensationPayload const&) const = default;
};

struct SequenceWindowV2Payload {
    TimingWindowV2Payload window;
    std::string solverVersion;                 // "gprl-clone-sa/1"
    bool decided = false;
    std::vector<std::string> adaptationUsed;   // SA_ADAPTATIONS
    int64_t undecidedShifts = 0;
    std::optional<CompensationPayload> compensation;   // v0.14.0 (revision 6), written after undecidedShifts

    bool operator==(SequenceWindowV2Payload const&) const = default;
};

struct HoldRangeV2Payload {
    int64_t pressSeq = 0;
    double minMs = 0.0;
    double maxMs = 0.0;
    std::string basis = "local";   // "sequence" | "local"
    double localMinMs = 0.0;
    double localMaxMs = 0.0;

    bool operator==(HoldRangeV2Payload const&) const = default;
};

struct TimingClusterPayload {
    std::string id;                        // "<attemptId>:<attemptInputIndex of the first member>"
    int64_t index = 1;
    std::optional<bool> connectedPrev;     // null = unknown (always written)
    std::optional<bool> connectedNext;

    bool operator==(TimingClusterPayload const&) const = default;
};

/// schema.ts TimingResultDual (v0.8.0, telemetry revision 5, docs/LIVE_ISOLATION_DESIGN.md §2.7):
/// the player-2 facts of a result measured by the dual PAIR simulator (P1 is the event's own
/// gamemode / speed). Every key required inside the block; the block itself is optional, never null.
struct TimingResultDualPayload {
    Gamemode p2Gamemode = Gamemode::Cube;   // player 2's gamemode at the input
    bool p2GravityFlipped = false;          // player 2 upside down at the input
    bool p2Mini = false;                    // player 2 mini at the input
    bool sameGravity = true;                // players 1 and 2 had the same gravity at the input

    bool operator==(TimingResultDualPayload const&) const = default;
};

/// schema.ts TimingResultEvent (v0.7.0, telemetry revision 4, docs/TIMING_SOLVER_V2.md §4.1):
/// the final word on one measured input - status + reasons, the LOCAL window, the
/// SEQUENCE-ADJUSTED window, the PAIR window, the HOLD range, the canonical position and the
/// observation / simulation counts. Deferred output: exempt from the per-attempt t / tick rule.
struct TimingResultPayload {
    int64_t inputSeq = 0;
    InputKind inputKind = InputKind::Press;
    int64_t attemptInputIndex = 1;
    double x = 0.0;
    double percentAtInput = 0.0;
    double subTickMs = 0.0;
    Gamemode gamemode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    std::optional<Gamemode> gamemodeAfter;     // null = no transition (always written)
    std::string status = "unresolved";         // TIMING_STATUSES
    std::vector<std::string> statusReasons;    // TIMING_STATUS_REASONS, unique, <= 12, consistent
    bool stateReplayValid = false;
    bool miss = false;
    double actualMs = 0.0;
    std::optional<TimingWindowV2Payload> local;
    std::optional<SequenceWindowV2Payload> sequence;
    std::optional<TimingWindowV2Payload> pair;
    std::optional<HoldRangeV2Payload> hold;
    TimingClusterPayload cluster;
    int64_t boundarySimulations = 0;
    int64_t controlSimulations = 0;
    std::string solverVersion;                 // "gprl-clone/3" | "gprl-clone/3-cbf" (geode 0.8.0; /2 up to 0.7.x)
    // v0.7.1 OPTIONAL (Fable review D11, never null, absent in older payloads): the ENGINE's own
    // sub-tick fraction of the input (frac(job frame) x 1000/240, [0, 1000/240)); with it actualMs
    // = t x 1000 + engineSubTickMs while subTickMs stays the tracker's (the input event's tSubTick)
    std::optional<double> engineSubTickMs;
    // v0.8.0 OPTIONAL (telemetry revision 5, never null, absent in older payloads): the player-2
    // facts of a dual PAIR result; required when statusReasons contains `dual_pair`. Written LAST
    // (after solverVersion) so every older payload stays byte-identical.
    std::optional<TimingResultDualPayload> dual;

    bool operator==(TimingResultPayload const&) const = default;
};

using Payload = std::variant<AttemptStartPayload, InputPayload, StateSamplePayload, GamemodeChangePayload, DeathPayload,
                             TimingWindowPayload, ProgressPayload, AttemptEndPayload, EnvironmentPayload, ClipAvailablePayload,
                             SequenceWindowPayload, TimingResultPayload>;

struct Event {
    double t = 0.0;
    int64_t tick = 0;
    int64_t seq = 0;
    std::string attemptId;
    Payload payload;

    EventKind kind() const { return static_cast<EventKind>(payload.index()); }
};

struct Batch {
    std::string schema = kSchema;
    std::string sessionId;
    int64_t seq = 0;
    std::string nonce;
    std::string clientBuild;
    std::vector<Event> events;
};

json::Value snapshotToJson(PlayerStateSnapshot const& s);
bool snapshotFromJson(json::Value const& v, PlayerStateSnapshot& out, std::string* err = nullptr);

json::Value fingerprintToJson(TimingFingerprint const& f);
bool fingerprintFromJson(json::Value const& v, TimingFingerprint& out, std::string* err = nullptr);

/// The timing_result rules of validate.ts (structure, vocabularies, edge = actualMs + midpoint,
/// W_local ⊆ W_SA, status / reason consistency, ok / state_replay_failed preconditions; v0.8.0:
/// live_mutation_detected carries no window / hold, no miss and stateReplayValid false, and
/// `dual_pair` needs the `dual` block).
/// Returns false with the first error in `err` (path + message). Used by validateBatch and by
/// core/solver/timing_result_event's builder self-check.
bool validateTimingResult(TimingResultPayload const& p, std::string const& path, std::string* err = nullptr);

json::Value toJson(Event const& e);
bool fromJson(json::Value const& v, Event& out, std::string* err = nullptr);

json::Value toJson(Batch const& b);
bool fromJson(json::Value const& v, Batch& out, std::string* err = nullptr);

/// The canonical body (canonical.ts) - what is sent and what the HMAC signature covers.
std::string canonicalBody(Batch const& b);
/// JSON.stringify layout (compact or `null, 2`), insertion order - for fixtures and the JSONL spool.
std::string serializeBatch(Batch const& b, bool pretty = false);
bool parseBatch(std::string_view text, Batch& out, std::string* err = nullptr);

/// Structural validation (validate.ts validateTelemetryBatch): schema, value domains, limits.
/// Returns false with the first error's JSON-pointer path + message in `err`.
bool validateBatch(Batch const& b, std::string* err = nullptr);

/// Ordering invariants (validate.ts checkBatchInvariants): event seq strictly increasing (and
/// > previousEventSeq when >= 0); t / tick non-decreasing within an attempt for live events;
/// every live event belongs to a started attempt (this batch or `knownAttemptIds`).
/// `environment` events are session-scoped and exempt; `timing_window`, `clip_available`,
/// `sequence_window` and `timing_result` events are exempt from the t / tick rule (deferred
/// output: their times are the input's / the attempt end's / the group's first input's / the input's).
bool checkBatchInvariants(Batch const& b, int64_t previousEventSeq = -1, std::set<std::string> const& knownAttemptIds = {},
                          std::string* err = nullptr);

/// Events of a session that REPLACES one the server invalidated (`session_invalid`, Telemetry.cpp).
/// A new session knows none of the old session's attempts, so only events it can accept under
/// checkBatchInvariants go to it: environment events (session-scoped), attempt_start events (the
/// attempt id is added to `startedAttempts`) and events of attempts in `startedAttempts`. Every
/// other event belongs to an attempt that started in the invalidated session: `orphaned`, spooled
/// on disk, never re-labelled into the new session. Order is preserved in both lists.
struct ReopenSplit {
    std::vector<Event> send;
    std::vector<Event> orphaned;
};
ReopenSplit splitForReopenedSession(std::vector<Event> events, std::set<std::string>& startedAttempts);

}  // namespace gprl::telemetry
