#include "GdOracle.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>
#include <unordered_map>

#include "../../core/solver/isolation.hpp"
#include "../../core/solver/portal_model.hpp"
#include "../../core/fingerprint_build.hpp"
#include "../../core/json.hpp"
#include "../../core/solver/sequence.hpp"
#include "../../core/solver/sequence_adjusted.hpp"
#include "../../core/solver/timing_result_event.hpp"
#include "../../core/solver/timing_status.hpp"
#include "../../core/solver/timing_units.hpp"
#include "../../core/solver/trace_view.hpp"
#include "../../core/solver/window_event.hpp"
#include "../../core/telemetry.hpp"
#include "../Hud.hpp"
#include "../Settings.hpp"
#include "../Telemetry.hpp"
#include "../Tracker.hpp"
#include "../analyzer/Modes.hpp"
#include "CloneEngine.hpp"

using namespace geode::prelude;

namespace gprl::solver::oracle {

namespace {

using clone::CloneEngine;

// Portals a clone passes through with the game's own code (its one-shot activation flags are
// isolated per clone by the engine); everything else a clone touches is swallowed.
// v0.7.1 (Fable review D7b, zero-cost safety): the speed portals 200-203 / 1334 are no longer on
// the list. The clones mirror the real player's speed from the ring anyway (CloneEngine simStep),
// so the touch gives a clone nothing, and it was the only path by which a clone shifted `s` ticks
// AHEAD of the real player could write the level-wide speed queue (m_gameState.m_timeModRelated,
// docs/GD_PHYSICS_NOTES.md "Speed portals") `s` ticks early for the REAL player. Whether GD routes
// speed portals through playerTouchedTrigger at all is not verifiable offline (the notes say they
// are not collisions); removing them costs nothing either way. Not run in-game.

CalibrationTracker s_calibration;
std::string s_state = "off";
LastWindow s_lastWindow;
std::chrono::steady_clock::time_point s_lastWindowAt;
constexpr size_t kRecentWindows = 12;
std::deque<std::pair<LastWindow, std::chrono::steady_clock::time_point>> s_recent;   // newest first

void noteLastWindow(LastWindow w) {
    s_lastWindow = std::move(w);
    s_lastWindow.any = true;
    s_lastWindowAt = std::chrono::steady_clock::now();
    s_recent.emplace_front(s_lastWindow, s_lastWindowAt);
    while (s_recent.size() > kRecentWindows) s_recent.pop_back();
}
bool s_platformer = false;
int s_pendingJob = 0;
int s_windowsThisLevel = 0;
// v0.5.1: the gate is open under bot playback for LEVEL-ONLY evidence (MASTER §13 / §20). Each
// job remembers whether a bot was playing when its input happened (the trust state at that
// moment), so the window it produces later is tagged `evidenceHint: level_only` even if the bot
// stopped in between. Never counted for the player: the environment event says bot = true and the
// server classifies from it; the local calibration excludes bot samples too.
bool s_levelOnlyGate = false;
std::unordered_map<int, bool> s_jobLevelOnly;
int s_levelOnlyWindows = 0;
// v0.6.1 (MASTER §5 sequence difficulty, docs/SOLVER_DESIGN.md §13): sequence windows of this level
int s_sequencesEmitted = 0;
int s_sequencesUnsent = 0;
// v0.7.0 (docs/TIMING_SOLVER_V2.md §4.1): timing_results of this level
int s_resultsSent = 0;
int s_resultsUnsent = 0;
int s_traceSerialShown = 0;   // the engine trace serial last handed to the HUD overlay

double unixMs() {
    using namespace std::chrono;
    return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

int verbosity() {
    auto const& s = settings::get();
    if (s.debugLog) return 2;
    if (s.solverDebug == "off") return 0;
    if (s.solverDebug == "verbose") return 2;
    return 1;
}

clone::EngineConfig engineConfig(bool cbfActive) {
    auto const& s = settings::get();
    clone::EngineConfig c;
    c.subtick = cbfActive && s.solverSubtick != "off";
    if (s.solverSubtick == "1/64 tick") {
        c.resolutionFrames = 1.0 / 64.0;
        c.maxRefinePasses = 2;
    }
    else {
        c.resolutionFrames = 0.125;
        c.maxRefinePasses = 1;
    }
    c.maxShiftTicks = std::clamp(s.maxShiftTicks, 1, 10);
    // v0.11.0 settled look-ahead (core/solver/settle.hpp): the jobs plan with the settle maximum
    // while it is on; the plain horizon setting is the look-ahead with it off
    c.settle.enabled = s.settleLookahead;
    c.settle.maxSeconds = std::clamp(s.settleMaxSeconds, 1.0, 10.0);
    c.settle.groundTicks = std::clamp(s.settleGroundTicks, 6, 240);
    c.horizonSeconds = solver::settle::lookAheadSeconds(c.settle, std::clamp(s.horizonSeconds, 0.25, 2.0));
    c.verbosity = verbosity();
    c.traceMaxTicks = std::clamp(s.traceMaxTicks, 0, solver::budget::kTraceCaps.maxTraceTicksSetting);
    // v0.8.2 (docs/LIVE_ISOLATION_DESIGN.md §3.2): the live-state invariant per clone step with the
    // setting or the debug log; every analysis block otherwise (release default)
    c.isolationEveryStep = s.isolationCheck == "every clone step" || s.debugLog;
    c.dualShadow = s.solverDual != "off";   // v0.8.3 (docs/LIVE_ISOLATION_DESIGN.md §2.7): the pair shadow in dual sections
    return c;
}

char const* kindWord(bool down) { return down ? "press" : "release"; }

std::string boundsText(telemetry::TimingWindowPayload const& p) {
    return fmt::format("[{:+.2f},{:+.2f}]{}", p.earliestMs - p.actualMs, p.latestMs - p.actualMs,
        p.boundedEarly && p.boundedLate ? " bounded" : p.boundedEarly ? " open late" : p.boundedLate ? " open early" : " open");
}

/// The engine finished a job: build the event, emit, feed the local calibration, log.
bool onJobResult(clone::JobResult const& r) {
    auto& e = CloneEngine::get();
    int v = e.config().verbosity;
    // level-only (bot playback) as remembered when the input happened; a job the map does not
    // know (should not happen) falls back to the gate's current state
    bool levelOnly = s_levelOnlyGate;
    if (auto it = s_jobLevelOnly.find(r.jobId); it != s_jobLevelOnly.end()) {
        levelOnly = it->second;
        s_jobLevelOnly.erase(it);
    }
    if (r.missDownstream) {
        // v0.7.1 (Fable D6): the control died with the real player, but a LATER input of the same
        // run could still have saved it (that later input is the miss) - no timing_window; the
        // input's timing_result says sequence_dependent (miss_downstream)
        if (v >= 1) {
            log::info("GPRL solver: input #{} {} (seq {}, job {}): MISS downstream - a later input of the died run is the miss; no timing_window, timing_result "
                      "only{}{}",
                      r.attemptInputIndex, kindWord(r.down), r.inputSeq, r.jobId, r.extension ? " [ext]" : "", v >= 2 ? " | " + r.detail : "");
        }
        tracker::noteWindow(false, false);
        LastWindow lw;
        lw.ok = false;
        lw.down = r.down;
        lw.levelOnly = levelOnly;
        lw.reason = "miss downstream";
        lw.jobId = r.jobId;
        noteLastWindow(std::move(lw));
        return false;
    }
    if (!r.ok) {
        std::string why = r.dropReason;
        if (r.dropReason == "mismatch" && !r.mismatch.empty()) why += " (" + r.mismatch + ")";
        else if (!r.window.invalidReason.empty()) why += " (" + r.window.invalidReason + ")";
        else if (!r.invalidWhy.empty()) why += " (" + r.invalidWhy + ")";   // v0.7.0: `DROPPED invalid` names why (teleport / dual portal)
        // Not-windowable inputs are not solver failures: say so in the log and on the HUD.
        std::string hudReason = r.dropReason;
        if (r.dropReason == "no_pass") {
            why = "no window: the death within the horizon was not caused by this input (no shift in range survives)";
            hudReason = "death unrelated";
        }
        else if (r.dropReason == "reset") {
            why = "no window: restart before the look-ahead ended";
            hudReason = "cut by restart";
        }
        else if (r.dropReason == "level_end") {
            why = "no window: the level ended before the look-ahead";
            hudReason = "cut by level end";
        }
        if (r.dropReason == "no_shift_tested") {
            why = "no window: no shift could be tested within the limits (attempt start / neighbours)";
            hudReason = "nothing to test";
        }
        if (v >= 1) {
            // v0.7.0 (RC-minor 4): the input by its index in this attempt, the job id in brackets
            log::info("GPRL solver: input #{} {} (seq {}, job {}): DROPPED {}{}", r.attemptInputIndex, kindWord(r.down), r.inputSeq, r.jobId, why,
                      r.detail.empty() ? "" : " | " + r.detail);
        }
        tracker::noteWindow(false, false);
        LastWindow lw;
        lw.ok = false;
        lw.down = r.down;
        lw.levelOnly = levelOnly;
        lw.reason = hudReason;
        lw.jobId = r.jobId;
        noteLastWindow(std::move(lw));
        return false;
    }
    FingerprintContext fc;
    fc.kind = r.down ? InputKind::Press : InputKind::Release;
    fc.prevInputGapMs = r.prevGapMs;
    fc.nextInputGapMs = r.nextGapMs;
    fc.holdMs = r.holdMs;
    fc.portalObjectId = r.firstPortalId;
    fc.geometryHash = r.geometryHash;
    fc.ceilingTouch = r.ceilingTouch;
    TimingFingerprint fp = buildFingerprint(r.pre, fc);
    WindowEventContext wc;
    wc.inputSeq = r.inputSeq;
    wc.kind = fc.kind;
    // v0.7.0 (RC-minor 6): the canonical time includes the sub-tick part (0 without CBF); the
    // timing_result's `local` window uses exactly the same doubles (bit-exact cross-check).
    // v0.7.1 (Fable D11): the ENGINE's sub-tick - every shift is measured from its frame of the input
    wc.actualMs = solver::units::actualMs(r.eventT, r.engineSubTickMs);
    wc.pressMs = r.pressMs;
    wc.refined = r.refined;
    auto built = buildWindowEvent(r.window, wc, fp);
    if (!built.ok) {
        if (v >= 1)
            log::info("GPRL solver: input #{} {} (seq {}, job {}): DROPPED invalid payload ({}) | {}", r.attemptInputIndex, kindWord(r.down), r.inputSeq, r.jobId,
                      built.error, r.detail);
        tracker::noteWindow(false, false);
        return false;
    }
    // v0.5.1: the client's evidence hint (schema.ts timing_window.evidenceHint), always written
    built.payload.evidenceHint = levelOnly ? telemetry::kEvidenceLevelOnly : telemetry::kEvidencePlayer;
    auto check = checkWindowPayload(built.payload, r.eventT);
    if (!check.accepted && v >= 1) {
        std::string reasons;
        for (auto const& s : check.reasons) reasons += (reasons.empty() ? "" : ",") + s;
        log::warn("GPRL solver: input #{} {} (seq {}, job {}): payload would fail the server gate ({}); emitted anyway as evidence", r.attemptInputIndex,
                  kindWord(r.down), r.inputSeq, r.jobId, reasons);
    }
    telemetry::Event ev;
    ev.t = r.eventT;
    ev.tick = r.tick;
    ev.attemptId = r.attemptId;
    ev.payload = built.payload;
    int64_t droppedBefore = client::droppedEvents();
    client::push(std::move(ev));
    int64_t seq = client::lastAssignedSeq();
    bool dropped = client::droppedEvents() > droppedBefore;

    // local calibration sample (SOLVER_DESIGN §3.7)
    auto const& p = built.payload;
    CalibrationSample sample;
    sample.id = r.attemptId + ":" + std::to_string(r.inputSeq);
    sample.levelId = tracker::session().levelId;
    sample.windowMs = built.widthMs;
    sample.hit = built.hit;
    sample.kind = fc.kind;
    sample.gamemode = r.pre.gamemode;
    sample.speed = r.pre.speed;
    sample.fingerprint = p.fingerprint;
    sample.recordedAtMs = unixMs();
    TrustState trust = tracker::trust();
    // a window measured under bot playback is never a player sample (MASTER §13): bot = true keeps
    // it out of the local calibration whatever the trust state is by now
    sample.bot = levelOnly || trust == TrustState::Botting;
    sample.wouldBeDeath = r.wouldBeDeathAfter;
    sample.physicsModified = trust == TrustState::PhysicsChanged || trust == TrustState::UnknownMod;
    if (!dropped) s_calibration.add(sample, std::max(1, tracker::session().attempts));
    {
        LastWindow lw;
        lw.ok = true;
        lw.down = r.down;
        lw.widthMs = built.widthMs;
        lw.earlyMs = p.earliestMs - p.actualMs;
        lw.lateMs = p.latestMs - p.actualMs;
        lw.boundedEarly = p.boundedEarly;
        lw.boundedLate = p.boundedLate;
        lw.refined = r.refined;
        lw.miss = r.miss;
        lw.levelOnly = levelOnly;
        lw.jobId = r.jobId;
        noteLastWindow(std::move(lw));
    }

    if (v >= 1) {
        std::string control = r.miss ? fmt::format("MISS (control died with the real player{})", r.extension ? ", finished in the death pause" : "") : "control=exact";
        // v0.7.0 (RC-minor 4): `input #` is the input's index in this attempt, the job id in brackets
        log::info("GPRL solver: input #{} {} (seq {}, job {}) t={:.4f}: window {:.2f} ms {} {} res {:.2f} ms {}{}{} -> {} seq {} ({}){}{}{}", r.attemptInputIndex,
                  kindWord(r.down), r.inputSeq, r.jobId, r.eventT, built.widthMs, boundsText(p), r.refined ? "cbf" : "240tps", p.resolutionMs, control,
                  r.window.nonMonotonic ? " islands" : "", built.widthBelowResolution ? " width<res" : "", dropped ? "DROPPED by the ring" : "emitted", seq,
                  p.solverVersion, r.extension ? " [ext]" : "", levelOnly ? " evidence=level_only (bot playback, never the player's)" : "",
                  v >= 2 ? " | " + r.detail : "");
    }
    if (!dropped) {
        ++s_windowsThisLevel;
        if (levelOnly) ++s_levelOnlyWindows;
        tracker::noteWindow(true, r.miss);
    }
    else tracker::noteWindow(false, false);
    return !dropped;
}

/// The engine finished a sequence job with a valid result: build the `sequence_window` event and
/// push it, unless the session's server does not know the kind yet (telemetry revision < 3): an
/// unknown kind would make its validator reject the WHOLE batch, so the result is only logged.
clone::SeqEmit onSequenceResult(clone::SeqResult const& r) {
    auto built = buildSequenceEvent(r.result, r.inputSeqs);
    if (!built.ok) return {-1, "REFUSED: invalid payload (" + built.error + ")"};
    auto check = checkSequencePayload(built.payload);
    std::string gate;
    if (!check.accepted) {
        for (auto const& reason : check.reasons) gate += (gate.empty() ? "" : ",") + reason;
        gate = " [would fail the server gate: " + gate + "]";
    }
    // only a status that describes THIS level session counts (the worker publishes a session a
    // moment after the main thread began it and shows the previous one until then)
    auto cs = client::status();
    bool current = cs.sessionGen != 0 && cs.sessionGen == client::sessionGeneration();
    if (!client::sessionOpen() || !current || cs.mode == client::SessionMode::None) {
        ++s_sequencesUnsent;
        return {0, "NOT SENT: the telemetry session is not open yet" + gate};
    }
    if (cs.telemetryRevision < telemetry::kSequenceWindowRevision) {
        ++s_sequencesUnsent;
        return {0, fmt::format("NOT SENT: this server validates telemetry revision {} (sequence_window needs {}){}", cs.telemetryRevision,
                               telemetry::kSequenceWindowRevision, gate)};
    }
    telemetry::Event ev;
    ev.t = r.eventT;          // the FIRST input's t / tick (deferred solver output, like timing_window)
    ev.tick = r.tick;
    ev.attemptId = r.attemptId;
    ev.payload = built.payload;
    int64_t droppedBefore = client::droppedEvents();
    client::push(std::move(ev));
    if (client::droppedEvents() > droppedBefore) return {-1, "DROPPED by the telemetry ring" + gate};
    ++s_sequencesEmitted;
    return {1, fmt::format("emitted seq {}{}", client::lastAssignedSeq(), gate)};
}

/// v0.7.0 (docs/TIMING_SOLVER_V2.md §4.1, §6 items 2-3): the engine finished an input's
/// `timing_result`. Pushed only when the session's server validates telemetry revision >= 4 (an
/// unknown kind would make an older validator reject the WHOLE batch); logged either way as the
/// `GPRL timing:` line and the canonical `GPRL timing json:` line tools/timing-report reads.
/// Returns true when the event was pushed to the telemetry session.
bool onTimingResult(clone::TimingResultOut const& out) {
    auto& e = CloneEngine::get();
    auto const& p = out.build.payload;
    std::string sent;
    auto cs = client::status();
    bool current = cs.sessionGen != 0 && cs.sessionGen == client::sessionGeneration();
    int64_t seq = -1;
    bool pushed = false;
    if (!client::sessionOpen() || !current || cs.mode == client::SessionMode::None) {
        ++s_resultsUnsent;
        sent = "NOT SENT (the telemetry session is not open)";
    }
    else if (cs.telemetryRevision < telemetry::kCompensationRevision) {
        ++s_resultsUnsent;
        // v0.14.0: the compensation vocabulary (comp1..3, compensated, the compensation block) needs
        // revision 6; v0.8.2: the gprl-timing-status/2 vocabulary (live_mutation_detected, ...) needed 5;
        // an older validator would reject the WHOLE batch for one unknown status
        sent = fmt::format("NOT SENT (this server validates telemetry revision {}, a timing_result of this mod needs {})", cs.telemetryRevision, telemetry::kCompensationRevision);
    }
    else {
        telemetry::Event ev;
        ev.t = out.eventT;   // the INPUT's t / tick: deferred solver output, like timing_window
        ev.tick = out.tick;
        ev.attemptId = out.attemptId;
        ev.payload = p;
        int64_t droppedBefore = client::droppedEvents();
        client::push(std::move(ev));
        if (client::droppedEvents() > droppedBefore) {
            ++s_resultsUnsent;
            sent = "DROPPED by the telemetry ring";
        }
        else {
            ++s_resultsSent;
            seq = client::lastAssignedSeq();
            sent = fmt::format("emitted seq {}", seq);
            pushed = true;
        }
    }
    int v = e.config().verbosity;
    if (v >= 1) log::info("GPRL timing: {} -> {}", out.line, sent);
    {
        // v0.7.1: the server gate's rules the mod can check itself (containment on pass AND reported
        // edges - Fable D1, proof consistency - D3b, engine vs tracker sub-tick - D11); a refusal is
        // logged, the result is still sent as evidence (the server stores it unusable)
        auto gate = solver::checkTimingResultPayload(p, out.eventT, p.subTickMs);
        if (!gate.accepted) {
            std::string reasons;
            for (auto const& s : gate.reasons) reasons += (reasons.empty() ? "" : ",") + s;
            log::warn("GPRL timing: input #{} (job {}) would fail the server gate ({}); sent anyway as evidence", out.attemptInputIndex, out.jobId, reasons);
        }
    }
    {
        // the canonical payload, one per job (tools/timing-report reads these lines, never the network)
        telemetry::Event ev;
        ev.t = out.eventT;
        ev.tick = out.tick;
        ev.seq = seq;
        ev.attemptId = out.attemptId;
        ev.payload = p;
        log::info("GPRL timing json: {}", json::stringify(telemetry::toJson(ev)));
    }
    // HUD: the input's history line gains "local 4.00 f / seq 10.75 f ok" (or the status word)
    std::string v2;
    if (std::isfinite(out.localWidthMs)) v2 = "local " + solver::units::framesText(out.localWidthMs);
    if (std::isfinite(out.seqWidthMs)) v2 += (v2.empty() ? "" : " / ") + std::string("seq ") + solver::units::framesText(out.seqWidthMs);
    v2 += (v2.empty() ? "" : " ") + p.status;
    for (auto& [w, at] : s_recent) {
        (void)at;
        if (w.jobId == out.jobId && out.jobId != 0) {
            w.v2 = v2;
            break;
        }
    }
    if (s_lastWindow.jobId == out.jobId && out.jobId != 0) s_lastWindow.v2 = v2;
    // debug view (§2.12, M9): the engine printed a new `GPRL trace:` block for this input just
    // before handing the result over - the overlay (src/Hud) draws the last one
    auto const& tv = e.lastTrace();
    if (tv.serial != 0 && tv.serial != s_traceSerialShown) {
        s_traceSerialShown = tv.serial;
        hud::showTrace(trace_view::fromEngine(tv));
    }
    return pushed;
}

}  // namespace

void applySettings() {
    auto& e = CloneEngine::get();
    if (!e.active()) return;
    // v0.12.0 review fix (docs/BACKGROUND_ANALYZER_DESIGN.md §5): a visit whose engine was stopped
    // stays off until the level is left, whatever the settings say now
    if (e.offForVisit()) {
        s_state = analyzer::modes::clonesAllowed() ? std::string("live solver off until the next level (setting changed mid-level)")
                                                   : analyzer::modes::liveSolverOffText();
        return;
    }
    // `analysis-mode` / `record-safe` changed in the pause menu so that hidden clones are no longer
    // allowed: the gate closes and the open results end like on a level exit, but the engine is NOT
    // torn down mid-level (no clone PlayerObject is removed while the level runs): the idle hidden
    // clones are removed by the normal teardown at onQuit / goEdit (or forgotten by forgetLayer)
    if (!analyzer::modes::clonesAllowed()) {
        s_state = analyzer::modes::liveSolverOffText();
        e.stopForVisit(s_state + " (setting analysis-mode / record-safe changed mid-level)");
        s_pendingJob = 0;
        s_levelOnlyGate = false;
        return;
    }
    auto menus = env::readMenus();
    e.configure(engineConfig(menus.cbfActive));
    refreshGate(tracker::trust(), menus);
}

void setup(PlayLayer* pl) {
    auto& e = CloneEngine::get();
    s_windowsThisLevel = 0;
    s_pendingJob = 0;
    clearRecentWindows();   // the HUD history is per level
    s_jobLevelOnly.clear();
    s_levelOnlyWindows = 0;
    s_levelOnlyGate = false;
    s_sequencesEmitted = 0;
    s_sequencesUnsent = 0;
    s_resultsSent = 0;
    s_resultsUnsent = 0;
    s_traceSerialShown = 0;
    // Owner decision 2026-10-02: measuring cannot be switched off (`measure-windows` was removed);
    // a disabled mod never gets here (Hooks.cpp setupHasCompleted), this is only the safety net
    if (!settings::get().enabled) {
        s_state = "off";
        e.teardown();
        return;
    }
    // v0.12.0 (docs/BACKGROUND_ANALYZER_DESIGN.md §5, AN-D2 / AN-D4): the one gate of the live clone
    // solver. Record-Safe Mode creates no hidden clone at all (no pool, no shadow, no self-test
    // step); every analysis mode (passive / offline / full) keeps the live solver.
    if (!analyzer::modes::clonesAllowed()) {
        s_state = analyzer::modes::liveSolverOffText();
        e.teardown();
        log::info("GPRL solver: {} - no hidden clones on this level ({})", s_state, analyzer::modes::summary());
        return;
    }
    auto menus = env::readMenus();
    auto cfg = engineConfig(menus.cbfActive);
    e.setSink(onJobResult);
    e.setSeqSink(onSequenceResult);
    e.setResultSink(onTimingResult);
    e.setSequences(settings::get().measureJointShare);
    e.setSequenceAdjusted(settings::get().measureSequenceAdjusted);
    if (!e.setup(pl, cfg)) {
        s_state = "failed: " + e.setupError();
        log::error("GPRL solver: not available on this level: {}", e.setupError());
        return;
    }
    s_platformer = pl->m_levelSettings && pl->m_levelSettings->m_platformerMode;
    refreshGate(tracker::trust(), menus);
    bool fpc = Loader::get()->isModLoaded("gmo12.frame_perfect_counter");
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §6 item 1): the versions this level runs with
    log::info("GPRL solver: ready - solver {}, sa {}, status {}, pool {} warm (max {}, +{} per frame), jobs max {}, budget={} ({} ms/frame target, {} steps/frame to start), "
              "history {} steps ({:.1f} MB), cbf={} subtick={}, {} level objects, gate={}, sa={}, joint-share={}, trace <= {} ticks, snapshot roundtrip {}, "
              "activation slot: real uid {} clone uid {} (mirrored){}",
              solver::solverVersionFor(false), solver::kSASolverVersion, solver::status::kTimingStatusVersion, e.poolWarmed(), clone::kMaxClones,
              solver::budget::kBudget.cloneCreatesPerFrame, clone::kMaxJobs, solver::budget::kBudget.version, solver::budget::kBudget.targetSimMsPerFrame,
              e.stepBudget(), clone::kHistorySteps, e.ringMegabytes(), menus.cbfActive ? 1 : 0, cfg.subtick ? settings::get().solverSubtick : std::string("off"),
              pl->m_objects ? pl->m_objects->count() : 0u, s_state, e.sequenceAdjustedOn() ? "on" : "off", e.sequencesOn() ? "on" : "off", cfg.traceMaxTicks,
              e.ringOk() ? "OK" : "FAILED", e.realPlayerUid(), e.firstCloneUid(), fpc ? " | FPC detected: both engines run their own clones" : "");
    // v0.8.2 (docs/LIVE_ISOLATION_DESIGN.md §7.2): what the isolation runs with on this level
    log::info("GPRL isolation: ready - {}, portal model {}, live state {}, invariant check {} (setting isolation-check{}), census {} object types "
              "(gate-model 8 portals replayed on the clone, gate-invalid 3, filter 5, replace 11 orbs), activation overlay on, dual = {}",
              solver::isolation::kIsolationConfig.version, solver::portal::kVersion, solver::live::kVersion, solver::isolation::name(e.isolationGuard().mode()),
              settings::get().debugLog ? " / debug log" : "", solver::isolation::kObjectTypeCount,
              cfg.dualShadow ? "pair shadow (P1 + P2 clones in GD's dual order, compared with both real players; inputs in dual not measured yet)" : "off (no clone steps in dual sections, setting solver-dual)");
    auto const& sa = solver::kSA;
    auto const& sab = solver::budget::kSABudget;
    log::info("GPRL sa: {} - {} (sequence-adjusted windows: earlier inputs fixed, the next inputs of the cluster may follow as pair / chain2 / chain3): "
              "<= {} trials per input, <= {} inputs per job spanning <= {:g} ticks, narrowest first (local <= {:g} ticks), queue {}; only when idle: <= {:.0f}% of the "
              "step budget and {:g} ms per frame, {} clones at once, {} idle clones always left for the local jobs; every job proves itself with two exact controls{}",
              e.sequenceAdjustedOn() ? "on" : "off (measure-sequence-adjusted setting)", sa.version, sa.maxTrialsPerInput, sa.maxInputsPerJob, sa.maxJobSpanTicks,
              sa.priorityMaxLocalWidthTicks, sa.queueMax, sab.frameShare * 100.0, sab.targetMsPerFrame, sab.parallelClones, sab.poolReserve,
              sa.requireNegativeControl ? " and one known death" : "");
    auto const& sq = solver::kSequence;
    log::info("GPRL sequence: {} - {} (joint windows of neighbouring inputs, level evidence only): inputs <= {:g} ticks apart (triples <= {:g}), "
              "{} / {} nodes per axis and <= {} / {} samples for a pair / triple, <= {} jobs per attempt ({} triples), {} per place; only when idle: <= {:.0f}% of the step "
              "budget and {:g} ms per frame while the local jobs + sequence stay under {:.0f}%, {} clones at once, {} idle clones always left for the local jobs; every job proves itself with two exact controls{}",
              e.sequencesOn() ? "on" : "off (measure-joint-share setting)", sq.version, sq.maxGapTicks, sq.maxTripleSpanTicks, sq.nodesPerAxisPair, sq.nodesPerAxisTriple,
              sq.maxSamplesPair, sq.maxSamplesTriple, sq.maxJobsPerAttempt, sq.maxTriplesPerAttempt, sq.maxPerPosition, sq.frameShare * 100.0, sq.targetMsPerFrame,
              sq.idleLoadShare * 100.0,
              sq.parallelClones, sq.poolReserve, sq.requireNegativeControl ? " and one known death" : "");
}

void teardown() {
    auto& e = CloneEngine::get();
    if (!e.active()) return;
    e.teardown();
    s_state = "off";
}

void forgetLayer(PlayLayer* pl) { CloneEngine::get().forgetLayer(pl); }

void reset(PlayLayer* pl) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.layer() != pl) return;
    e.reset();
    s_pendingJob = 0;
}

void refreshGate(TrustState trust, env::MenuState const& menus) {
    auto& e = CloneEngine::get();
    if (!e.active()) return;
    if (e.offForVisit()) return;   // stopForVisit (setting changed mid-level): the gate stays closed until the level is left
    bool on = true;
    // Physics changes are checked from the menus themselves, BEFORE the trust state: trust ranks
    // botting above physics (env::classify), and a bot at a wrong TPS must still pause.
    bool tpsWrong = menus.tpsBypass && std::fabs(menus.tps - 240.0) > 1e-6;
    bool speedhack = menus.speedhack && std::fabs(menus.speedhackValue - 1.0) > 1e-3;
    bool bot = trust == TrustState::Botting || menus.botState == 2;
    bool wasLevelOnly = s_levelOnlyGate;
    s_levelOnlyGate = false;
    // v0.12.0: the Record-Safe gate (applySettings normally stops the engine for the visit first;
    // this keeps the gate closed should a refresh run before it)
    if (!analyzer::modes::clonesAllowed()) { on = false; s_state = analyzer::modes::liveSolverOffText(); }
    else if (!e.ringOk()) { on = false; s_state = "failed: " + e.setupError(); }
    else if (s_platformer) { on = false; s_state = "unsupported: platformer level"; }
    else if (tpsWrong) { on = false; s_state = fmt::format("paused: Eclipse Physics Bypass at {:g} TPS (set 240 or turn it off)", menus.tps); }
    else if (speedhack) { on = false; s_state = "paused: speedhack"; }
    else if (trust == TrustState::PhysicsChanged) { on = false; s_state = "paused: physics modified"; }
    // Record-Safe Mode uses no bot / replay playback as evidence at all - but Record-Safe already
    // closed the gate above: modes::clonesAllowed() == modes::botPlaybackEvidenceAllowed() in every
    // configuration (core/sim/modes.hpp, sim_modes_tests), so a separate "bot in Record-Safe"
    // branch here could never run and was removed
    else if (bot) {
        // v0.5.1 (MASTER §13 / §20): keep measuring under bot playback - the windows describe the
        // LEVEL (evidenceHint level_only) and never the player (environment bot = true)
        s_levelOnlyGate = true;
        s_state = "measuring (bot playback: level-only evidence, not counted for you)";
    }
    else s_state = "measuring";
    if (s_levelOnlyGate != wasLevelOnly) {
        log::info("GPRL solver: bot playback {} - windows {} tagged evidence=level_only (level analysis only, never the player's sigma/s)",
                  s_levelOnlyGate ? "started" : "stopped", s_levelOnlyGate ? "are now" : "are no longer");
    }
    auto cfg = engineConfig(menus.cbfActive);
    auto const& cur = e.config();
    if (cfg.subtick != cur.subtick || cfg.verbosity != cur.verbosity || cfg.maxShiftTicks != cur.maxShiftTicks || cfg.horizonSeconds != cur.horizonSeconds
        || cfg.resolutionFrames != cur.resolutionFrames || cfg.maxRefinePasses != cur.maxRefinePasses || cfg.traceMaxTicks != cur.traceMaxTicks
        || cfg.settle.enabled != cur.settle.enabled || cfg.settle.maxSeconds != cur.settle.maxSeconds || cfg.settle.groundTicks != cur.settle.groundTicks) {
        e.configure(cfg);
    }
    e.setMeasuring(on, on ? std::string() : s_state);
    e.setSequences(settings::get().measureJointShare);
    e.setSequenceAdjusted(settings::get().measureSequenceAdjusted);
}

void stepBegin(GJBaseGameLayer* layer, float dt, bool halfTick) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.offForVisit() || layer != static_cast<GJBaseGameLayer*>(e.layer())) return;
    e.stepBegin(dt, halfTick);
}

void playerSubUpdate(PlayerObject* p, float dt) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.stepping() || !e.layer() || p != e.layer()->m_player1) return;
    e.playerSubUpdate(dt);
}

void onPushButton(PlayerObject* p, int button, bool down) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.offForVisit() || e.stepping() || !e.layer() || p != e.layer()->m_player1) return;
    if (button != static_cast<int>(PlayerButton::Jump)) return;
    auto pl = e.layer();
    PlayerStateSnapshot pre = tracker::snapshotOf(pl, p, 1);
    bool ceiling = p->m_isUpsideDown ? p->m_lastCollisionBottom != -1 : p->m_lastCollisionTop != -1;
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §2.2): the progress AT the input, full float (3 decimals on display)
    float pct = pl->getCurrentPercent();
    double percent = std::isfinite(pct) ? std::clamp(static_cast<double>(pct), 0.0, 100.0) : 0.0;
    s_pendingJob = e.input(down, pre, ceiling, percent);
    if (s_pendingJob > 0) {
        s_jobLevelOnly[s_pendingJob] = s_levelOnlyGate;
        // bounded: a job that never reports (cancelled without a result) must not leak
        if (s_jobLevelOnly.size() > 4 * static_cast<size_t>(clone::kMaxJobs)) s_jobLevelOnly.clear();
    }
}

void bindLastInput(int64_t seq, double t, int64_t tick, std::string const& attemptId, bool practice, bool dropped, double tSubTick) {
    auto& e = CloneEngine::get();
    if (!e.active()) return;
    if (!s_pendingJob) {
        // v0.7.0: a skipped input still has an input event (a release's hold names its press by seq)
        if (!dropped) e.noteLastInputSeq(seq);
        return;
    }
    int job = s_pendingJob;
    s_pendingJob = 0;
    if (dropped) {
        e.cancelJob(job, "input event dropped by the ring");
        return;
    }
    e.bindJob(job, seq, t, tick, attemptId, practice, tSubTick);
}

bool claimDestroy(PlayLayer* pl, PlayerObject* player, GameObject* object) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.layer() != pl) return false;
    // GD's anti-cheat spike: PlayLayer::updateVerifyDamage keeps moving it onto the real player, and
    // PlayLayer::destroyPlayer only notes the hit for it (nobody dies). The clones run a few steps
    // behind the real player and touch it as they catch up; that is not a death for them either.
    if (object && pl->m_anticheatSpike && (object == pl->m_anticheatSpike || object->m_uniqueID == pl->m_anticheatSpike->m_uniqueID)
        && (e.stepping() || e.isClone(player))) {
        e.noteAnticheatTouch();
        return true;
    }
    // Any death raised while a clone is being stepped is that clone's (the game may pass the
    // main player's pointer here), and a death of a clone outside a step is still the clone's.
    if (e.stepping() || e.isClone(player)) {
        e.cloneDied(player, object);
        return true;
    }
    return false;
}

void onRealDestroy(PlayLayer* pl, PlayerObject* player, GameObject* object, bool wasDead, bool isDead) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.layer() != pl || !player) return;
    if (player != pl->m_player1 && player != pl->m_player2) return;
    if (wasDead) return;
    if (object && pl->m_anticheatSpike && object == pl->m_anticheatSpike) return;
    e.onRealDeath(object, !isDead);
}


void onRealPortal(PlayerObject* p, int objectId) {
    auto& e = CloneEngine::get();
    if (!e.active() || !e.layer() || p != e.layer()->m_player1) return;
    e.onRealPortal(objectId);
}

void onLevelComplete(PlayLayer* pl) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.layer() != pl) return;
    e.onLevelComplete();
}

void frameEnd(PlayLayer* pl, float dt) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.offForVisit() || e.layer() != pl) return;
    e.frameEnd(dt);
}

bool stepping() { return CloneEngine::get().stepping(); }
bool isClone(PlayerObject* p) { return CloneEngine::get().isClone(p); }
PlayerObject* simClone() { return CloneEngine::get().simClone(); }
void cloneInvalid(PlayerObject* p, char const* why) { CloneEngine::get().cloneInvalid(p, why); }
uint8_t overlayGet(EnhancedGameObject* o) { return CloneEngine::get().overlayGet(o); }
void overlaySet(EnhancedGameObject* o, uint8_t logical) { CloneEngine::get().overlaySet(o, logical); }
uint8_t simSelfBit() { return CloneEngine::get().simSelfBit(); }
PlayerObject* simPartner() { return CloneEngine::get().simPartner(); }
bool simHalfTick() { return CloneEngine::get().simHalfTick(); }
void noteTripwire(char const* what) { CloneEngine::get().noteTripwire(what); }
void notePortalModelled(int objectType) { CloneEngine::get().notePortalModelled(objectType); }

uint32_t geometryHashAt(float x) {
    auto& e = CloneEngine::get();
    if (!e.active()) return 0;
    return e.geometryHashAt(x, nullptr);
}

void tripIsolationBreaker(char const* who) {
    auto& e = CloneEngine::get();
    if (!e.active() || e.stepping()) return;
    e.tripIsolationBreaker(who);
}

std::vector<LastWindow> recentWindows(size_t max) {
    std::vector<LastWindow> out;
    auto now = std::chrono::steady_clock::now();
    for (auto const& [w, at] : s_recent) {
        if (out.size() >= max) break;
        LastWindow c = w;
        c.ageSeconds = std::chrono::duration<double>(now - at).count();
        out.push_back(std::move(c));
    }
    return out;
}

void clearRecentWindows() {
    s_recent.clear();
    s_lastWindow = LastWindow{};
}

void clearLocalData() {
    clearRecentWindows();
    s_calibration.reset();
    log::info("GPRL solver: local data cleared after Reset data (HUD history + local calibration tracker)");
}

LastWindow lastWindow() {
    LastWindow w = s_lastWindow;
    if (w.any) w.ageSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_lastWindowAt).count();
    return w;
}

Status status() {
    auto& e = CloneEngine::get();
    Status s;
    s.active = e.active();
    s.measuring = e.active() && e.measuring();
    s.state = e.active() ? s_state : std::string("off (no level)");
    // v0.12.0: the Record-Safe gate names itself even without an engine
    if (!e.active() && !analyzer::modes::clonesAllowed()) s.state = analyzer::modes::liveSolverOffText();
    s.levelOnly = e.active() && s_levelOnlyGate;
    s.localSamples = s_calibration.sampleCount();
    if (e.active()) {
        auto const& c = e.counters();
        s.emitted = c.emitted;
        s.misses = c.misses;
        s.dropped = c.dropped;
        s.dropMismatch = c.dropMismatch;
        s.dropInvalid = c.dropInvalid;
        s.dropPool = c.dropPool;
        s.dropBudget = c.dropBudget;
        s.dropNoPass = c.dropNoPass;
        s.dropReset = c.dropReset;
        s.dropLevelEnd = c.dropLevelEnd;
        s.dropHistory = c.dropHistory;
        s.dropBlocked = c.dropBlocked;
        s.dropUnbound = c.dropUnbound;
        s.dropOther = c.dropOther;
        s.notWindowable = e.notWindowable();
        s.skipped = c.skipped;
        s.skippedDual = c.skippedDual;
        s.skippedDead = c.skippedDead;
        s.skippedPaused = c.skippedPaused;
        s.skippedThrottle = c.skippedThrottle;
        s.skippedFrame = c.skippedFrame;
        s.skippedJobs = c.skippedJobs;
        s.skippedOther = c.skippedOther;
        s.deferredSpawns = c.deferredSpawns;
        s.deferredExpired = c.deferredExpired;
        s.clonesCreated = c.clonesCreated;
        s.poolSize = e.poolSize();
        for (auto const& [k, n] : c.mismatchKinds) s.mismatchKinds += fmt::format("{}{} {}", s.mismatchKinds.empty() ? "" : ", ", k, n);
        s.coverage = e.coveragePercent();
        s.shadowSteps = c.shadowSteps;
        s.shadowMismatches = c.shadowMismatches;
        s.simMs = e.simMsPerFrame();
        s.simPeakMs = e.simPeakMs();
        s.stepCostUs = e.stepCostUs();
        s.stepBudget = e.stepBudget();
        s.throttled = e.throttled();
        s.clones = e.runningClones();
        s.jobs = e.openJobs();
        auto const& q = e.seqCounters();
        s.sequencesOn = e.sequencesOn();
        s.seqGroups = q.groups;
        s.seqJobs = q.jobs;
        s.seqMeasured = q.measured;
        s.seqEmitted = q.emitted;
        s.seqUnsent = q.unsent;
        s.seqTrivial = q.trivial;
        s.seqDropped = q.dropped();
        s.seqNotStarted = q.notStarted();
        s.seqWaiting = e.seqWaiting() + (e.seqRunning() ? 1 : 0);
        // v0.7.0
        s.results = c.results;
        s.resultsSent = s_resultsSent;
        s.resultsUnsent = s_resultsUnsent;
        for (int i = 0; i < 6; ++i) s.statusCount[i] = c.statusCount[i];
        auto const& a = e.saCounters();
        s.saOn = e.sequenceAdjustedOn();
        s.saCandidates = a.candidates;
        s.saMeasured = a.measured;
        s.saWaiting = e.saWaiting();
        s.saDropped = a.dropped();
        s.saNotStarted = a.notStarted();
        s.replayBroken = e.replayBroken();
    }
    if (!s.active) s.line = s.state.rfind("live solver off", 0) == 0 ? "Timing windows: " + s.state : "Timing windows: solver " + s.state;
    else if (s.measuring) {
        s.line = fmt::format("Timing windows: measuring live ({}, sim {:.1f} ms/frame, {:.1f} us/step, budget {} steps/frame{})", kSolverVersion, s.simMs,
                             s.stepCostUs, s.stepBudget, s.throttled ? ", throttled" : "");
    }
    else s.line = "Timing windows: " + s.state;
    // One line for the Session tab (it is width-limited): the numbers that answer "why no window";
    // the 5 s summary in the Geode log carries every reason.
    s.line2 = fmt::format("Windows: {} emitted ({} misses, {} local{}), coverage {:.0f}%, dropped {} (mismatch {}{}, invalid {}, pool {}, other {}), not windowable {}, "
                          "skipped {} (dead {}, dual {}, jobs {}, other {}) | shadow {}/{}",
        s.emitted, s.misses, s.localSamples, s_levelOnlyWindows > 0 ? fmt::format(", {} level-only", s_levelOnlyWindows) : std::string(), s.coverage, s.dropped,
        s.dropMismatch, s.mismatchKinds.empty() ? std::string() : " [" + s.mismatchKinds + "]",
        s.dropInvalid, s.dropPool, s.dropBudget + s.dropHistory + s.dropBlocked + s.dropUnbound + s.dropOther, s.notWindowable, s.skipped, s.skippedDead,
        s.skippedDual, s.skippedJobs, s.skippedPaused + s.skippedThrottle + s.skippedFrame + s.skippedOther, s.shadowMismatches, s.shadowSteps);
    // v0.6.1: joint-share windows (measure-joint-share, off by default in v0.7.0), short
    if (s.active && s.sequencesOn && s.seqGroups > 0) s.line2 += fmt::format(" | joint {}/{} of {}", s.seqEmitted, s.seqMeasured, s.seqGroups);
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §3.4 item 10): the timing_result statuses + the SA jobs
    using TS = solver::status::TimingStatus;
    auto sc = [&](TS t) { return s.statusCount[static_cast<int>(t)]; };
    s.line3 = fmt::format("Results: {} (ok {}, low {}, seq-dep {}, no effect {}, unresolved {}, replay failed {}; sent {}, not sent {}) | SA {}: {} candidates, "
                          "{} measured, {} waiting, {} dropped, {} not started{}",
        s.results, sc(TS::Ok), sc(TS::LowConfidence), sc(TS::SequenceDependent), sc(TS::NoEffect), sc(TS::Unresolved), sc(TS::StateReplayFailed), s.resultsSent,
        s.resultsUnsent, s.saOn ? "on" : "off", s.saCandidates, s.saMeasured, s.saWaiting, s.saDropped, s.saNotStarted,
        s.replayBroken ? " | replay broken this attempt" : "");
    return s;
}

bool hasLocalSamples() { return s_calibration.sampleCount() > 0; }

CalibrationState localCalibration() { return s_calibration.report().state; }

trace::View const& lastTrace() {
    static trace::View const kNone{};
    auto const& e = CloneEngine::get();
    return e.active() ? e.lastTrace() : kNone;
}

}  // namespace gprl::solver::oracle
