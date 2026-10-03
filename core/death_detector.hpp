#pragma once
// noclip-death-detector/2 (docs/NOCLIP_DEATH_DETECTOR.md): the one place that decides whether a
// kill the game raised is a real death, a would-be death (a lethal contact of the REAL live player
// that something suppressed), a continuation of the current lethal contact, or nothing at all.
// Pure: plain values in, plain values out (no Geode / cocos includes), so
// tests/death_detector_tests.cpp runs the whole rule set on the host. src/DeathPath.cpp is the
// game-side caller: it reads the fields named below and hands the plain values in here.
//
// Source of truth (GD 2.2081 win, disassembly notes in the doc §2): every kill of a player is
// raised inside `GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage)`, which only
// `GJBaseGameLayer::update` calls, and ends in the virtual `destroyPlayer(player, object)`:
//
//   checkCollisions            hazard loop (object = the hazard)          returns 1
//   checkCollisions            out of bounds two ticks in a row (object = nullptr)   returns 1
//   PlayerObject::collidedWithObjectInternal / collidedWithSlopeInternal / postCollision
//                              block crush, slope, squeeze (object = nullptr, via GameManager's PlayLayer)
//
// `PlayLayer::destroyPlayer` itself returns without killing when player 1 is locked (end
// animation), when `m_playerDied` is already set, and for the anti-cheat spike (compared by
// unique id); a null player means player 1. GPRL never reconstructs a collision itself: a
// would-be death is a kill GD raised for the real player in its own physics step, which GD's own
// rules would have executed, after which the player is still alive.
//
// What is NOT a would-be death, each with its own reason (the debug record names it):
//   - a hidden solver clone, or anything raised while a clone is being stepped   rejected_clone
//   - a PlayerObject that is not the active PlayLayer's player 1 / player 2       rejected_unknown_player
//   - a kill that belongs to another attempt / session generation, to no open attempt, or that
//     was raised while the level resets                                          rejected_stale_attempt
//   - a kill GD itself does not execute (anti-cheat spike, locked player, already dead), and a
//     collision check that ran with `ignoreDamage` (GD raises no kill then; Click Between Frames
//     probes the real player this way between its sub-steps)                       rejected_not_lethal
//   - a swallowed kill for the real player raised OUTSIDE the game's own physics step (another
//     mod's simulation on the real player, a direct destroyPlayer call), or anything from the
//     background analyzer / a replay / a practice simulation                      simulated_collision
//
// One continuous lethal contact = one would-be death, per player: a suppressed kill on the same
// tick or the next one continues the contact (whatever the object is and however many kills arrive
// in the tick). GD's out-of-bounds kill needs `m_isOutOfBounds` on two consecutive ticks and clears
// it when it fires, so it can only fire every second tick while the player stays out of bounds:
// two kills WITHOUT an object two ticks apart are the same contact as well.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gprl::death {

/// The semantics version of every death / would-be death this build emits (death.detector).
inline constexpr char const* kDetectorVersion = "noclip-death-detector/2";

/// Who raised the candidate.
enum class Origin : uint8_t {
    LivePlayLayer,   // the real PlayLayer's death path (PlayLayer::destroyPlayer / checkCollisions)
    TimingSolver,    // the isolated timing solver (hidden clones)
    Analyzer,        // the background level analyzer's simulation / reconstructed hitboxes
    Replay,          // a replay / macro clone
    PracticeSim,     // a practice / checkpoint simulation copy
};
char const* name(Origin o);

/// Which game signal carried it.
enum class Via : uint8_t {
    DestroyHook,       // PlayLayer::destroyPlayer reached GPRL's hook
    CollisionReturn,   // checkCollisions returned 1 for the player (a hazard / out-of-bounds contact), but no
                       // destroyPlayer call reached the hook: another mod's hook swallowed it before GPRL's
                       // (hook order). With `ignoreDamage` the check raised no kill at all: never a death
};
char const* name(Via v);

/// death.source vocabulary (docs/TELEMETRY.md §12). Only the first two are ever sent.
enum class Source : uint8_t {
    LiveGdDeath,            // "live_gd_death": GD's live collision / death path for the real current player
    ExternalKill,           // "external_kill": the real player died, but not in GD's collision step (another mod killed it)
    SimulatedCollision,     // "simulated_collision"
    RejectedClone,          // "rejected_clone"
    RejectedStaleAttempt,   // "rejected_stale_attempt"
    RejectedUnknownPlayer,  // "rejected_unknown_player"
    RejectedNotLethal,      // "rejected_not_lethal"
};
char const* name(Source s);

enum class Decision : uint8_t {
    Death,              // the real player died: the attempt ends
    WouldBeDeath,       // a NEW continuous lethal contact whose kill was suppressed: one would-be death
    ContinuesContact,   // the same lethal contact goes on: no new death
    Rejected,           // not the live player's death path: nothing changes
};
char const* name(Decision d);

enum class Reason : uint8_t {
    RealDeath,               // Death: killed in GD's collision step
    ExternalKillDeath,       // Death: dead after a destroyPlayer raised outside GD's collision step
    NewLethalContact,        // WouldBeDeath
    SameContact,             // ContinuesContact
    NotLiveOrigin,           // analyzer / replay / practice simulation
    SolverClone,             // the PlayerObject is one of GPRL's hidden clones (or the origin is the solver)
    SolverStep,              // raised while the solver steps a clone
    UnknownPlayer,           // not player 1 / player 2 of the active PlayLayer
    StaleSession,            // another level session
    StaleAttempt,            // another attempt generation
    NoOpenAttempt,           // no attempt is open (ended, level left)
    DuringReset,             // raised while the level resets
    AnticheatSpike,          // GD's own probe: PlayLayer::destroyPlayer only notes it
    AlreadyDead,             // the player was dead before the call
    LockedPlayer,            // GD ignores kills while player 1 is locked (end animation)
    PlayerDiedFlag,          // GD ignores kills once m_playerDied is set
    IgnoreDamage,            // checkCollisions ran with ignoreDamage: GD raised no kill (a sub-step probe, GD's own option)
    OutsideLiveStep,         // a swallowed kill for the real player raised outside GD's own physics step
};
char const* name(Reason r);

/// One kill the game (or a simulation) raised, as plain values.
struct Candidate {
    // --- generation, captured WHEN THE KILL WAS RAISED (before the game's death path ran) ---
    uint32_t sessionGen = 0;
    uint32_t attemptGen = 0;
    bool duringReset = false;      // raised inside PlayLayer::resetLevel
    int64_t tick = 0;              // classify::tickFromProgress(m_currentProgress)
    // --- who ---
    Origin origin = Origin::LivePlayLayer;
    Via via = Via::DestroyHook;
    int playerSlot = 0;            // 1 / 2: pointer-equal to the ACTIVE PlayLayer's m_player1 / m_player2; 0: anything else
    bool solverClone = false;      // one of the solver's hidden PlayerObjects
    bool solverStepping = false;   // the solver is stepping a clone right now
    // --- GD's live physics step ---
    bool liveStepKnown = true;     // the checkCollisions scope hooks are observed working (false: the scope rule is off)
    bool inLiveStep = false;       // inside GJBaseGameLayer::update -> checkCollisions(THIS PlayerObject)
    bool ignoreDamage = false;     // that checkCollisions call's ignoreDamage argument
    // --- the hazard ---
    bool hasObject = false;        // false: block crush / slope / squeeze / out of bounds (GD passes nullptr)
    bool anticheatSpike = false;   // same unique id as PlayLayer::m_anticheatSpike
    // --- GD's own state before the death path ran ---
    bool wasDeadBefore = false;    // the player's m_isDead
    bool playerLocked = false;     // m_player1->m_isLocked
    bool layerPlayerDied = false;  // PlayLayer::m_playerDied
    // --- after the death path returned ---
    bool gdDeathFired = false;     // the game's own PlayLayer::destroyPlayer was reached (no hook swallowed the call)
    bool deadAfter = false;        // the player's m_isDead
};

struct Verdict {
    Decision decision = Decision::Rejected;
    Reason reason = Reason::UnknownPlayer;
    Source source = Source::RejectedUnknownPlayer;
    int player = 0;            // 1 / 2 for a real live player, 0 = none
    uint32_t contactId = 0;    // continuous_contact_id of the lethal contact (WouldBeDeath / ContinuesContact), else 0
    bool accepted() const { return decision != Decision::Rejected; }
    /// A death event is sent for exactly these two.
    bool emitsEvent() const { return decision == Decision::Death || decision == Decision::WouldBeDeath; }
};

/// Per-attempt counters (the debug summary line and the tests).
struct Counters {
    int deaths = 0;
    std::array<int, 3> wouldBeDeaths{};     // [1] / [2] per player, [0] unused
    int continued = 0;                      // suppressed kills that continued a contact
    int rejectedClone = 0;
    int rejectedStale = 0;
    int rejectedUnknownPlayer = 0;
    int rejectedNotLethal = 0;
    int simulated = 0;
    int totalWouldBe() const { return wouldBeDeaths[1] + wouldBeDeaths[2]; }
};

class Detector {
public:
    /// A new level session: nothing of an earlier session is current any more.
    void beginSession(uint32_t sessionGen);
    /// A new attempt: every lethal contact and every counter starts fresh.
    void beginAttempt(uint32_t attemptGen);
    /// The attempt ended (death / restart / completion / exit): later candidates are stale.
    void endAttempt();
    /// The level was left.
    void endSession();

    Verdict onCandidate(Candidate const& c);

    uint32_t sessionGen() const { return m_sessionGen; }
    uint32_t attemptGen() const { return m_attemptGen; }
    bool attemptOpen() const { return m_open; }
    Counters const& counters() const { return m_counters; }
    /// The tick of the first accepted would-be death of the attempt, or -1 (the noclip evidence
    /// rule's "clean state ends here", docs/NOCLIP_EVIDENCE_DESIGN.md).
    int64_t firstWouldBeTick() const { return m_firstWouldBeTick; }

private:
    struct Contact {
        bool any = false;
        int64_t lastTick = 0;
        bool lastObjectless = false;
        uint32_t id = 0;
    };
    Verdict reject(Reason r, Source s, int player = 0);

    uint32_t m_sessionGen = 0;
    uint32_t m_attemptGen = 0;
    bool m_sessionOpen = false;
    bool m_open = false;
    std::array<Contact, 3> m_contact{};
    uint32_t m_nextContactId = 0;
    int64_t m_firstWouldBeTick = -1;
    Counters m_counters;
};

// ---- debug records (PROMPT §1, §8: bounded, only while the death-debug setting is on) ----

struct Box {
    bool valid = false;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;   // origin (min corner) + size, world units
};

/// Everything known about one candidate. The game side fills it; `format` makes the log line.
struct DebugRecord {
    std::string sessionId;
    std::string attemptId;
    uint32_t sessionGen = 0;
    uint32_t attemptGen = 0;
    int64_t tick = 0;
    int64_t lastTick = 0;          // the last tick folded into this record (DebugHistory)
    double percent = 0.0;
    uint64_t playerPtr = 0;        // the PlayerObject pointer the game passed (0 = null: GD means player 1)
    bool nullPlayerArg = false;
    float playerX = 0.f, playerY = 0.f;
    Box playerBox;
    bool hasObject = false;
    int objectId = 0;              // GD object id (m_objectID)
    int objectUid = 0;             // m_uniqueID
    int objectType = -1;           // GameObjectType
    float objectX = 0.f, objectY = 0.f;
    float objectRotation = 0.f;
    float objectScaleX = 1.f, objectScaleY = 1.f;
    float objectRadius = 0.f;      // > 0: GD tests a circle (saws)
    Box objectBox;
    bool megahackLoaded = false;
    bool noclipObserved = false;   // a lethal kill of the live player was suppressed (observed behaviour)
    bool menuNoclip = false;       // a readable menu (Eclipse) says noclip is on
    bool deadAfterGd = false;      // m_isDead right after the game's own function returned (innermost hook)
    int collisionDepth = 0;        // nested checkCollisions calls at the time
    int repeats = 0;               // identical records collapsed into this one (rate limit)
    Candidate candidate;
    Verdict verdict;
};

/// "GPRL death: ..." - one compact line with every field of §1.
std::string format(DebugRecord const& r);
/// The overlay's short label: source, reason, tick, object.
std::string overlayLabel(DebugRecord const& r);
/// True when the two boxes overlap (the overlay's "was there a hitbox collision at all").
bool overlaps(Box const& a, Box const& b);
/// The centre of the overlap of the two boxes (the collision marker), or false.
bool overlapCentre(Box const& a, Box const& b, float& x, float& y);

/// Bounded history + rate limit: every accepted death / new contact and every FIRST rejection of a
/// kind is kept; an identical rejection or continuation on the following ticks only counts up
/// `repeats` on the kept record (a 470-tick contact is one line, a dying clone pool is one line).
class DebugHistory {
public:
    explicit DebugHistory(size_t capacity = 48) : m_capacity(capacity) {}
    /// Returns true when the record was stored as a new entry (the caller logs it); false when it
    /// was folded into the previous entry of the same kind.
    bool add(DebugRecord const& r);
    void clear();
    std::vector<DebugRecord> const& entries() const { return m_entries; }
    size_t dropped() const { return m_dropped; }
    uint64_t revision() const { return m_revision; }   // bumps on every stored / folded record

private:
    static bool sameKind(DebugRecord const& a, DebugRecord const& b);
    size_t m_capacity;
    std::vector<DebugRecord> m_entries;
    size_t m_dropped = 0;
    uint64_t m_revision = 0;
};

}  // namespace gprl::death
