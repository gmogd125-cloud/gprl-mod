#pragma once
// Live updates (owner decision 2026-10-01: "data on GD is prioritized over the data on the
// website" - the game is the source of truth and the website follows it live;
// docs/contracts/calibration.md "Live updates").
//
// The server used to recalculate a player only at the session end, so the website and the HUD sat
// at the previous values for a whole session. While a Remote session is open (on any level: every
// level feeds the calibration since 2026-10-01, the levels list is not a condition) and at least
// one timing_window / timing_result event has been SENT (accepted by the server) since the last
// live recalculation, the telemetry worker calls POST /v1/me/recalc every
// kLiveRecalcIntervalSeconds (45 s) of wall clock and stores the answer exactly like
// GET /v1/me/calibration (src/Telemetry.cpp, Command::Kind::LiveRecalc). The server allows one
// recalculation per player per 30 s (api/src/processing/config.ts liveRecalc.cooldownSeconds) and
// answers the stored calibration inside it; the interval here is never below that. After an error
// the worker waits 5 minutes (one WARN per session).
//
// Pure C++20, no Geode: the schedule, the answer's extra fields and the log line are host-tested
// (tests/live_recalc_tests.cpp).
#include <cstdint>
#include <optional>
#include <string>

#include "calibration.hpp"
#include "json.hpp"

namespace gprl::live {

/// processing.liveRecalc.intervalSeconds: how often the mod asks while it sends windows.
inline constexpr int64_t kLiveRecalcIntervalSeconds = 45;
/// The server's per-player cooldown (api/src/processing/config.ts LIVE_RECALC_0_1.cooldownSeconds).
inline constexpr int64_t kServerCooldownSeconds = 30;
/// After any error: wait this long before the next try (and WARN once per session).
inline constexpr int64_t kErrorBackoffSeconds = 300;
static_assert(kLiveRecalcIntervalSeconds >= kServerCooldownSeconds, "never ask faster than the server's cooldown");

struct LiveRecalcParams {
    std::string version = "live-recalc/0.1";
    int64_t intervalMs = kLiveRecalcIntervalSeconds * 1000;
    int64_t serverCooldownMs = kServerCooldownSeconds * 1000;
    int64_t errorBackoffMs = kErrorBackoffSeconds * 1000;
};

/// The interval actually used: never below the server's cooldown (nor below 1 s).
int64_t effectiveIntervalMs(LiveRecalcParams const& p);
/// The error back-off actually used: never below the interval.
int64_t effectiveBackoffMs(LiveRecalcParams const& p);

/// What the worker knows when it asks `due`.
struct LiveRecalcGate {
    bool enabled = true;       // setting `live-recalc` (and the mod enabled, not local-only)
    bool sessionOpen = false;  // a level session is open
    bool remote = false;       // ... in SessionMode::Remote (connected, server session)
    bool levelCounts = false;  // the level is on the levels list (V1CreateSessionResponse.levelCounts); not a condition of `due`
};

/// The interval / back-off state machine. Times are milliseconds of one monotonic clock (the
/// worker passes std::chrono::steady_clock): wall clock, so a paused game still counts.
class LiveRecalcSchedule {
public:
    explicit LiveRecalcSchedule(LiveRecalcParams params = {});

    /// A level session opened: the first live recalculation comes one interval later (the session
    /// start already fetched the calibration), or after a running error back-off, whichever is
    /// later. Clears the windows count and the once-per-session WARN.
    void sessionStarted(int64_t nowMs);
    /// The session ended: nothing is due until the next one starts.
    void sessionEnded();
    /// A batch the server accepted carried `count` timing_window / timing_result events.
    void windowsSent(int64_t count);

    /// Whether POST /v1/me/recalc is due now.
    bool due(int64_t nowMs, LiveRecalcGate const& gate) const;

    /// The worker posted the command: no second one until an outcome arrives.
    void markQueued() { m_queued = true; }
    bool queued() const { return m_queued; }
    /// The command was dropped without a request (the gate closed in between): due again later.
    void dropQueued() { m_queued = false; }

    /// The server answered. `recalculated` true: the windows sent so far are in the stored rows,
    /// the count starts over. False (the server's cooldown, or no session there): the windows stay
    /// pending and the next try is one interval later. Either way any error back-off ends.
    void onAnswer(int64_t nowMs, bool recalculated);
    /// The request failed (transport, HTTP error, unparseable answer): the next try waits the
    /// error back-off. Returns true for the first error of this session (log a WARN), else false.
    bool onError(int64_t nowMs);

    int64_t pendingWindows() const { return m_pendingWindows; }
    bool backingOff() const { return m_backingOff; }
    /// Earliest time the next request may go out (ms of the caller's clock).
    int64_t nextAttemptMs() const { return m_nextAttemptMs; }
    LiveRecalcParams const& params() const { return m_params; }

private:
    LiveRecalcParams m_params;
    bool m_session = false;
    bool m_queued = false;
    bool m_backingOff = false;
    bool m_warnedThisSession = false;
    int64_t m_pendingWindows = 0;
    int64_t m_nextAttemptMs = 0;
    int64_t m_backoffUntilMs = 0;
};

/// The fields POST /v1/me/recalc adds to the GET /v1/me/calibration body (shared
/// V1LiveRecalcResponse). The calibration part is read by the same parsers as the GET
/// (calibrationStateFromJson on `overall`, calibrationDisplayFromJson on the body).
struct LiveRecalcAnswer {
    bool recalculated = false;
    std::string computedAt;      // "" = null (not recalculated)
    std::string nextAllowedAt;
    int64_t cooldownSeconds = 0;
    std::string skipped;         // "" = null; "cooldown" | "no_sessions"
    std::optional<int64_t> ratableSamples;   // rated samples behind this recalculation; absent when it did not run
    std::optional<int64_t> sampleCount;
};

/// Reads the live fields. False (and `out` untouched) when `recalculated` is not a boolean: not
/// a POST /v1/me/recalc answer.
bool parseLiveRecalcAnswer(json::Value const& body, LiveRecalcAnswer& out);

/// The one INFO line per live recalculation (without the "GPRL: " prefix the caller adds):
///   "live recalc -> calibration 35.7% (46 rated, 31 effective), display locked, private -"
///   "live recalc -> calibration 35.7% (server cooldown, stored; 31 effective), display locked, private yes"
///   "live recalc -> calibration 92.0% (512 rated, 700 effective), display visible 123.4, private yes"
/// Never a private figure, and no sigma/s figure while the display is locked (logs get shared);
/// visible / ranked name the public headline figure the HUD shows anyway.
std::string liveRecalcLogLine(LiveRecalcAnswer const& answer, CalibrationState const& overall, CalibrationDisplay const& display);

}  // namespace gprl::live
