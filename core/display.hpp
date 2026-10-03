#pragma once
// Pure display rules and level hints of geode v0.5.1 (stream M1, docs/FINISH_PLAN.md "Mod
// program" M1). No Geode / cocos includes: compiled by the mod AND by tests/display_tests.cpp.
//
//   HUD history      the middle-right panel lists the last 8 presses / releases
//                    ("PRESS 4.17 ms [-2.08 +2.08]", "REL ...", "PRESS miss ...", "PRESS dropped:
//                    mismatch"); tone = colour, opacity dims with age (newest first)
//   Level coverage   GET /v1/levels/:id/analysis -> "Level Analysis Coverage: 83% / Missing:
//                    - 61.2-64.7% - 91.0-94.3%" (MASTER §20 block on one line; GD's bitmap fonts
//                    have no en dash, so the server's "61.2–64.7%" is rewritten with '-')
//   Level counts     "Not a rated demon: not counted" from the session response (MASTER C6)
//   Level hints      GJGameLevel fields -> the optional POST /v1/client/sessions hints (never
//                    trusted server side): demon difficulty name, cleaned level name
//   Attempt clock    activeMs / practiceMs / startPosMs (MASTER §11): unpaused wall-clock time,
//                    frame deltas above `maxFrameMs` discarded as pauses / hitches
//
// Every constant lives in DisplayParams (one place, versioned).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace gprl::display {

struct DisplayParams {
    char const* version = "gprl-display/1";
    int historyLines = 8;              // HUD history length
    double msPerFrame240 = 1000.0 / 240.0;
    // opacity of history line i (0 = newest): newestOpacity - i * opacityStep, never below minOpacity
    int newestOpacity = 255;
    int opacityStep = 18;
    int minOpacity = 110;
    // attempt clock: a frame delta above this is a pause / alt-tab / hitch and is not counted
    double maxFrameMs = 500.0;
    // level name hint: server limit (api/src/routes/client.ts `levelName` max 64)
    size_t levelNameMax = 64;
};
constexpr DisplayParams kDisplay{};

// ---- HUD history ----

enum class Tone : uint8_t { Measured, Miss, Dropped, Idle };

/// One finished measurement as the HUD shows it (a copy of GdOracle's LastWindow fields).
struct HistoryEntry {
    bool ok = false;         // emitted (false: dropped, see reason)
    bool down = true;        // press / release
    bool miss = false;       // the real player died on this input
    bool levelOnly = false;  // measured while a bot played: level-only evidence
    double widthMs = 0.0;
    double earlyMs = 0.0;    // earliest - actual (<= 0)
    double lateMs = 0.0;     // latest - actual (>= 0)
    bool boundedEarly = true, boundedLate = true;
    std::string reason;      // drop reason when !ok
    std::string suffix;      // v0.7.0: "local 4.00 f / seq 10.75 f ok" once the timing_result arrived
    // v0.15.0 (docs/SHIP_SOLVER.md §11.7): a connected-control input (Ship): the line's figure is
    // the LOCAL window - this input moved with every other input FROZEN - and says so; the
    // compensated window follows in the suffix ("compensated 15.0 f ok")
    bool frozen = false;
};

struct HistoryLine {
    std::string text;
    Tone tone = Tone::Idle;
    int opacity = 255;
};

/// "PRESS 4.17 ms [-2.08 +2.08]" / "REL 2.50 ms [<-4.17 +0.00]" (an open side keeps its "<" / ">")
/// / "PRESS miss 4.17 ms [-6.25 -2.08]" / "PRESS dropped: mismatch" / "REL dropped: cut by restart".
/// A level-only window (bot playback) is prefixed "bot ". v0.7.0: the timing_result's short form is
/// appended as " | local 4.00 f / seq 10.75 f ok" once it arrived (the status word when there is no window).
std::string historyText(HistoryEntry const& e, DisplayParams const& p = kDisplay);
Tone historyTone(HistoryEntry const& e);
/// Opacity of the i-th line (0 = newest).
int historyOpacity(size_t index, DisplayParams const& p = kDisplay);
/// The whole list, newest first, at most `p.historyLines` lines.
std::vector<HistoryLine> historyLines(std::vector<HistoryEntry> const& newestFirst, DisplayParams const& p = kDisplay);

// ---- level analysis coverage (GET /v1/levels/:id/analysis, V1LevelAnalysisResponse) ----

struct LevelCoverage {
    std::string levelId;               // GPRL level id
    std::string levelName;
    bool levelCounts = true;           // rated demon (deep analysis runs only for those)
    std::string status;                // LevelAnalysisStatus as served (e.g. "partial", "complete")
    double percent = 0.0;              // coverage.percent (1 decimal)
    int displayPercent = 0;            // coverage.displayPercent (whole percent, 100 only when complete)
    bool complete = false;
    double observedPercent = 0.0;      // any attempt, windows or not
    std::vector<std::string> missing;  // coverage.missingDisplay with '-' instead of the en dash
    bool hasAnalysis = false;          // `analysis` present (not null)
};

/// Parses the response body. False with `err` when it is not a level-analysis response
/// (missing `coverage` object). Tolerant of absent optional members.
bool parseLevelCoverage(json::Value const& v, LevelCoverage& out, std::string* err = nullptr);

/// The MASTER §20 block on one line: "Level Analysis Coverage: 83% / Missing: - 61.2-64.7% - 91.0-94.3%".
/// Complete: "Level Analysis Coverage: 100% / Complete". Not a rated demon: "... (not a rated demon:
/// not analyzed)" appended. Nothing observed yet: "Level Analysis Coverage: 0% / Missing: - 0.0-100.0%".
std::string coverageLine(LevelCoverage const& c);

/// Rewrites U+2013 (en dash) / U+2014 as '-' and drops other non-ASCII bytes: GD's bitmap fonts.
std::string asciiDash(std::string const& s);

// ---- which levels count (V1CreateSessionResponse.levelCounts) ----

/// "" when the level counts, else "Not a rated demon: not counted" (the spec text; the server's
/// own `levelCountsReason` is appended after " - " when it says more, ASCII-only).
std::string levelCountsLine(bool levelCounts, std::string const& reason);

// ---- why a session does not feed the rating (V1CreateSessionResponse.ratable / ratableReason) ----

/// v0.14.6 (owner report 2026-10-03: "0 % calibrating for my friend" = Mega Hack loaded, every
/// session stored unrated, nothing on screen said why): the top-right notice for a session the
/// server answered `ratable: false` to, from its `ratableReason` (api/src/processing/trust.ts):
///   "mod menu without a state adapter: Mega Hack (absolllute.megahack)"
///       -> "GPRL: this session is NOT rated. Mega Hack is loaded and GPRL cannot see whether its
///           cheats are on. Disable Mega Hack while you play to calibrate (the Eclipse menu works)."
///   "unknown gameplay-affecting mod some.id"
///       -> "GPRL: this session is NOT rated: unknown gameplay mod some.id is loaded. Disable it to calibrate."
///   anything else -> "GPRL: this session is NOT rated: <reason>" (ASCII, at most 160 characters)
///   ""            -> "GPRL: this session is NOT rated."
/// Several reasons are joined by "; " on the wire: the first mod-menu reason wins.
std::string unratedSessionNotice(std::string const& ratableReason);

/// The same once per game launch for noclip (the server still stores a noclip session, but
/// `noclip-evidence/1` keeps only the inputs before an attempt's first would-be death, at reduced
/// weight and at most a quarter of a gamemode's evidence).
inline constexpr char const* kNoclipNotice =
    "GPRL: noclip is on. Noclip attempts barely count (only the inputs before the first would-be death, at reduced weight). Turn noclip off to calibrate.";

// ---- level hints (POST /v1/client/sessions, never trusted server side) ----

/// GJGameLevel::m_demonDifficulty -> V1DemonDifficulty name for a demon: 3 easy, 4 medium,
/// 5 insane, 6 extreme, anything else (0 = GD's default) hard. "" when the level is not a demon.
std::string demonDifficultyName(int gdDemonDifficulty, bool isDemon);

/// GJGameLevel::m_levelName -> the `levelName` hint: printable ASCII only (GD's own charset),
/// trimmed, at most `p.levelNameMax` characters. "" when nothing printable is left.
std::string levelNameHint(std::string const& name, DisplayParams const& p = kDisplay);

// ---- attempt clock (attempt_end.activeMs / practiceMs / startPosMs) ----

/// Accumulates the unpaused wall-clock time of one attempt. The caller feeds every rendered
/// frame's real elapsed ms with the frame's state; deltas above `maxFrameMs` (pause menu, alt-tab,
/// a hitch) and negative deltas are discarded and counted in `discardedFrames`.
class AttemptClock {
public:
    // not explicit: `Attempt a;` / `State{}` in Tracker.cpp copy-list-initialise it from `{}`
    AttemptClock(DisplayParams const& p = kDisplay) : m_params(p) {}
    void reset() { *this = AttemptClock(m_params); }
    /// One frame: `elapsedMs` since the previous frame, and the level state during it.
    void frame(double elapsedMs, bool paused, bool practice, bool startPos);
    double activeMs() const { return m_active; }
    double practiceMs() const { return m_practice; }
    double startPosMs() const { return m_startPos; }
    int discardedFrames() const { return m_discarded; }
    int countedFrames() const { return m_counted; }

private:
    DisplayParams m_params;
    double m_active = 0.0, m_practice = 0.0, m_startPos = 0.0;
    int m_discarded = 0, m_counted = 0;
};

// ---- v0.12.0 LIVE tag (owner request 2026-10-02: "if LIVE isn't there it's not being sent") ----

/// What the HUD knows about the upload path right now (client::Status + settings).
struct LiveFacts {
    bool enabled = false;          // the mod's `enabled`
    bool localOnly = false;        // local-only mode / placeholder API: nothing networks
    bool connected = false;        // a usable device token
    bool sessionOpen = false;      // a telemetry session is open
    bool remote = false;           // ... and it is a Remote one (Local / Unsent spool to disk)
    int64_t nowMs = 0;             // steady clock (client::steadyNowMs)
    int64_t lastOkMs = 0;          // last batch the server ACCEPTED (0 = none this run)
    int64_t lastFailMs = 0;        // last batch that failed or was spooled instead of sent
    int64_t eventsPending = 0;     // events waiting for the next flush
};

/// A batch accepted within this many ms keeps the tag while events are still flowing (the worker
/// flushes every 2 s while there are events).
constexpr int64_t kLiveFreshMs = 10000;

/// LIVE = data is reaching the server: a remote session is open on a connected, networking client,
/// the server accepted a batch of it, no batch failed since, and either that acceptance is recent
/// or nothing is waiting to be sent (a pause sends nothing and stays LIVE). Before the first
/// accepted batch of a level there is no tag: LIVE is a delivery receipt, never a promise.
inline bool liveNow(LiveFacts const& f) {
    if (!f.enabled || f.localOnly || !f.connected || !f.sessionOpen || !f.remote) return false;
    if (f.lastOkMs <= 0) return false;
    if (f.lastFailMs > f.lastOkMs) return false;
    if (f.eventsPending <= 0) return true;
    return f.nowMs - f.lastOkMs <= kLiveFreshMs;
}

}  // namespace gprl::display
