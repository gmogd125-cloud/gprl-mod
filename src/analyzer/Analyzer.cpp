// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. This file only routes hook calls to the read-only extraction / recorder and posts plain
// data to the worker thread.
//
// NO EXCEPTION REACHES GD (review fix 2026-10-02, HIGH 4 main-thread half): every entry point runs
// its work inside guarded(); an allocation failure (or any other exception) in the recorder, the
// extraction hand-over, the attempt store or a worker post stops the analysis for this level visit
// with one log::error and the game goes on. The attempt / pause facts the worker's Record-Safe rule
// reads (Modes.cpp) are kept up to date for every visit of a level, analysed or not.
#include "Analyzer.hpp"

#include <chrono>
#include <exception>
#include <new>

#include "../../core/analyzer_recorder.hpp"
#include "../../core/sim/modes.hpp"
#include "../Hud.hpp"
#include "../Settings.hpp"
#include "../Telemetry.hpp"
#include "../solver/GdOracle.hpp"
#include "Cache.hpp"
#include "Extract.hpp"
#include "Modes.hpp"
#include "Recorder.hpp"
#include "Worker.hpp"

using namespace geode::prelude;

namespace gprl::analyzer {

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kCredentialsEverySeconds = 2.0;

struct Visit {
    PlayLayer* pl = nullptr;
    uint64_t id = 0;
    bool active = false;            // the simulator is part of this visit
    bool submitted = false;
    AttemptStore store;
    int attemptIndex = 0;
    int recorded = 0;
    std::string stoppedWhy;         // an exception stopped the analysis of this visit ("" = not stopped)
};

Visit s_visit;
uint64_t s_nextVisit = 0;
bool s_inited = false;
Clock::time_point s_lastCredentials{};

worker::Config workerConfig() {
    auto const& s = settings::get();
    worker::Config c;
    c.cache = s.analysisCache;
    c.upload = s.analysisUpload;
    c.debug = s.analysisDebug || s.debugLog;
    c.maxShiftTicks = s.maxShiftTicks;
    c.subTickRefine = s.solverSubtick == "off" ? 0 : (s.solverSubtick == "1/64 tick" ? 2 : 1);
    c.clientBuild = settings::clientBuild();
    return c;
}

void pushCredentials() {
    auto auth = client::uploadAuth();   // main thread only
    auto st = client::status();
    worker::Credentials c;
    c.online = !auth.localOnly && !auth.apiBaseUrl.empty();
    c.connected = auth.connected && !auth.deviceToken.empty();
    c.deviceToken = auth.deviceToken;
    c.apiBaseUrl = auth.apiBaseUrl;
    // review fix (MEDIUM 5): the session id, the level hash it was opened for and its generation are
    // published TOGETHER by the telemetry worker (client::Status), so the session of the level just
    // left is never paired with the level that is open now (the worker publishes a new session a
    // moment after the main thread began it)
    c.sessionId = st.sessionOpen && st.mode == client::SessionMode::Remote ? st.sessionId : std::string();
    c.sessionLevelHash = c.sessionId.empty() ? std::string() : st.sessionLevelHash;
    c.sessionGen = st.sessionGen;
    worker::setCredentials(std::move(c));
    s_lastCredentials = Clock::now();
}

/// The analysis of this visit ends after an exception. noexcept: every step is guarded itself.
void stopVisitAfterException(char const* where, char const* what) noexcept {
    try {
        log::error("GPRL analyzer: {} failed ({}) - the level analysis stops for this visit; the game is not affected", where ? where : "?",
                   what ? what : "unknown exception");
    } catch (...) {
    }
    try {
        recorder::abandon();
    } catch (...) {
    }
    try {
        extraction::abandon("exception");
    } catch (...) {
    }
    s_visit.store.reset();   // frees the kept attempts (an allocation failure is the likely cause)
    if (s_visit.submitted) {
        try {
            worker::levelClosed(s_visit.id);   // its job pauses as if the level was left
        } catch (...) {
        }
    }
    s_visit.active = false;
    s_visit.submitted = false;
    try {
        s_visit.stoppedWhy = std::string("stopped after an error in ") + (where ? where : "?");
    } catch (...) {
    }
}

/// Runs `f`; nothing it throws leaves this function (GD's hook frames are never unwound).
template <class F>
void guarded(char const* where, F&& f) noexcept {
    try {
        f();
    } catch (std::bad_alloc const&) {
        stopVisitAfterException(where, "out of memory");
    } catch (std::exception const& e) {
        stopVisitAfterException(where, e.what());
    } catch (...) {
        stopVisitAfterException(where, nullptr);
    }
}

/// A finished attempt goes to the worker (after the hand-over) or waits in the visit's store.
void keep(recorder::Finished f) {
    worker::AttemptIn in;
    in.meta = f.meta;
    if (s_visit.submitted) {
        if (!s_visit.store.admit(f.attempt)) return;
        in.attempt = std::move(f.attempt);
        worker::submitAttempt(s_visit.id, std::move(in));
        ++s_visit.recorded;
        return;
    }
    if (s_visit.store.offer(std::move(f.attempt), f.meta)) ++s_visit.recorded;
}

/// The open attempt ends (death, completion, restart, exit). The attempt fact is kept for every
/// visit of the level, analysed or not.
void endAttempt(PlayLayer* pl, bool died, bool completed, bool readFinal) {
    if (!pl || pl != s_visit.pl) return;
    if (s_visit.active) {
        if (auto f = recorder::onAttemptEnd(pl, died, completed, readFinal)) keep(std::move(*f));
    }
    modes::setAttemptActive(false);
}

/// After any slice: a finished walk goes to the worker with the attempts recorded so far.
void submitIfReady() {
    if (!s_visit.active || s_visit.submitted) return;
    auto h = extraction::take();
    if (!h) return;
    std::vector<worker::AttemptIn> attempts;
    for (auto& k : s_visit.store.takeAll()) {
        worker::AttemptIn in;
        in.attempt = std::move(k.attempt);
        in.meta = k.meta;
        attempts.push_back(std::move(in));
    }
    worker::submitLevel(s_visit.id, std::move(*h), std::move(attempts));
    s_visit.submitted = true;
}

void closeVisit(bool layerValid) {
    if (s_visit.active) {
        try {
            if (layerValid) endAttempt(s_visit.pl, false, false, false);
            else recorder::abandon();
            extraction::abandon("level exit");
            if (s_visit.submitted) worker::levelClosed(s_visit.id);
            pushCredentials();
        } catch (std::bad_alloc const&) {
            stopVisitAfterException("level exit", "out of memory");
        } catch (std::exception const& e) {
            stopVisitAfterException("level exit", e.what());
        } catch (...) {
            stopVisitAfterException("level exit", nullptr);
        }
    }
    s_visit = Visit{};
    modes::setInLevel(false);
    modes::setAttemptActive(false);
    modes::setDeathPause(false);
    modes::setPaused(false);
    modes::resetPressure();
}

}  // namespace

void init() {
    if (s_inited) return;
    s_inited = true;
    cache::init(Mod::get()->getSaveDir() / "analysis");
    cache::initIdentity(Mod::get()->getSaveDir() / "identity");   // level families: identity/<levelHash>.json
}

void shutdown() { worker::shutdown(); }

void applySettings() {
    guarded("settings", [] {
        modes::applySettings();
        worker::setConfig(workerConfig());
        // a mode without the simulator, chosen mid-level: this visit stops reading and recording
        if (s_visit.active && !modes::simulatorEnabled()) {
            log::info("GPRL analyzer: {} - the level analysis stops for this visit", modes::summary());
            PlayLayer* pl = s_visit.pl;
            auto facts = modes::facts();   // still in the level: the attempt / pause facts stay as they are
            closeVisit(true);
            s_visit.pl = pl;   // still in the level, nothing analysed
            modes::setInLevel(pl != nullptr);
            modes::setAttemptActive(pl != nullptr && facts.attemptActive);
            modes::setPaused(pl != nullptr && facts.paused);
        }
    });
}

void onLevelEnter(PlayLayer* pl) {
    guarded("level enter", [pl] {
        if (s_visit.pl && s_visit.pl != pl) closeVisit(true);
        s_visit = Visit{};
        s_visit.pl = pl;
        modes::setInLevel(pl != nullptr);
        modes::setPaused(false);
        modes::setDeathPause(false);
        // attempt 1 is open from setupHasCompleted on - BEFORE the first extraction slice and the
        // hand-over below (review LOW), so the worker never sees "no attempt" while it starts
        modes::setAttemptActive(pl != nullptr);
        modes::refreshTarget();
        modes::resetPressure();
        if (!pl || !modes::simulatorEnabled()) return;
        init();
        worker::start();
        worker::setConfig(workerConfig());
        pushCredentials();
        s_visit.active = true;
        s_visit.id = ++s_nextVisit;
        recorder::onAttemptStart(pl, ++s_visit.attemptIndex);   // the menus: tracker::onLevelEnter read them just before
        extraction::begin(pl);
        // the level is still loading (setupHasCompleted): the one 8 ms slice of the visit
        // (core/sim/modes.hpp kSliceLimits.setupUs; every later slice is min(8 ms, 25 % of a frame))
        extraction::slice(pl, sim::modes::kSliceLimits.setupUs);
        submitIfReady();
    });
}

void onLevelReset(PlayLayer* pl) {
    guarded("level reset", [pl] {
        if (!pl || pl != s_visit.pl) return;
        endAttempt(pl, false, false, false);   // a restart (a died attempt already ended at the death)
        modes::setDeathPause(false);
        modes::setPaused(false);
    });
}

void onAttemptStarted(PlayLayer* pl) {
    guarded("attempt start", [pl] {
        if (!pl || pl != s_visit.pl) return;
        if (s_visit.active) recorder::onAttemptStart(pl, ++s_visit.attemptIndex);
        modes::setAttemptActive(true);
    });
}

void onStepBegin(GJBaseGameLayer* layer) {
    if (!layer || !s_visit.pl || layer != static_cast<GJBaseGameLayer*>(s_visit.pl)) return;
    if (solver::oracle::stepping()) return;   // never a clone step (the real layer's processCommands only)
    // review fix (MEDIUM 7): the real level's physics steps, so the game is not paused, whatever
    // path left the pause menu - a resume that bypassed PlayLayer::resume can no longer leave the
    // Record-Safe facts "paused" (= simulator allowed during the attempt) for the rest of the visit
    modes::setPaused(false);
    if (!s_visit.active) return;
    guarded("tick record", [] { recorder::onStepBegin(s_visit.pl); });
}

void onPlayerUpdate(PlayerObject* p, float dt) {
    if (!s_visit.active) return;
    guarded("tick clock", [p, dt] { recorder::onPlayerUpdate(s_visit.pl, p, dt); });
}

void onInput(PlayerObject* p, int button, bool down) {
    if (!s_visit.active) return;
    guarded("input record", [p, button, down] { recorder::onInput(s_visit.pl, p, button, down); });
}

void onDestroyPlayer(PlayLayer* pl, PlayerObject* p, bool wasDead, bool isDead) {
    if (!pl || pl != s_visit.pl || !p) return;
    if (p != pl->m_player1 && p != pl->m_player2) return;
    if (wasDead || !isDead) return;   // a would-be death under noclip (the attempt goes on) or a repeat
    guarded("attempt end (death)", [pl] {
        endAttempt(pl, true, false, true);
        modes::setDeathPause(true);
    });
}

void onLevelComplete(PlayLayer* pl) {
    if (!pl || pl != s_visit.pl) return;
    guarded("attempt end (complete)", [pl] { endAttempt(pl, false, true, true); });
}

void onFrame(PlayLayer* pl) {
    if (!s_visit.pl || pl != s_visit.pl) return;
    guarded("frame", [pl] {
        modes::frameSample();
        if (!s_visit.active) return;
        if (extraction::progress().active) {
            int budget = modes::extractionSliceUs();   // 0 under frame pressure: no slice this frame
            if (budget > 0) extraction::slice(pl, budget);
        }
        submitIfReady();
        if (std::chrono::duration<double>(Clock::now() - s_lastCredentials).count() >= kCredentialsEverySeconds) pushCredentials();
        // level families (FA-D11): the worker queued the server's notice lines; shown here, on the
        // main thread, only while the visit they belong to is still the open one and the setting is on
        if (worker::familyNoticePending()) {
            while (auto n = worker::takeFamilyNotice()) {
                if (n->visitId != s_visit.id) continue;   // a notice of a level already left
                if (!settings::get().familyNotice) {
                    log::info("GPRL analyzer: family notice not shown (setting family-notice off): {}", fmt::join(n->lines, " / "));
                    continue;
                }
                hud::familyNotice(std::move(n->lines));
            }
        }
    });
}

void onPause(PlayLayer* pl) {
    if (pl != s_visit.pl) return;
    modes::setPaused(true);
    modes::resetPressure();
}

void onResume(PlayLayer* pl) {
    if (pl != s_visit.pl) return;
    modes::setPaused(false);
    modes::resetPressure();
    modes::refreshTarget();   // the FPS cap / VSync may have been changed in the pause menu's options
}

void onQuit(PlayLayer* pl) {
    if (pl != s_visit.pl) return;
    closeVisit(true);
}

void forgetLayer(PlayLayer* pl) {
    if (!pl || pl != s_visit.pl) return;
    closeVisit(false);
}

VisitView visitView() {
    VisitView v;
    v.open = s_visit.active;
    v.visitId = s_visit.id;
    auto p = extraction::progress();
    v.extracting = p.active;
    v.extractPercent = p.percent();
    v.submitted = s_visit.submitted;
    v.isolationStopped = p.isolationStopped;
    v.failed = p.failed;
    v.failReason = p.failReason;
    if (!s_visit.active && !s_visit.stoppedWhy.empty() && s_visit.pl) {
        // an exception stopped this visit's analysis: shown as failed until the level is left
        v.open = true;
        v.failed = true;
        v.failReason = s_visit.stoppedWhy;
    }
    v.attemptsRecorded = s_visit.recorded;
    v.attemptsDropped = s_visit.store.dropped();
    v.recording = recorder::recording();
    v.recordSkip = recorder::skipReason();
    v.extractMs = p.totalMs;
    v.maxSliceUs = p.maxSliceUs;
    v.slices = p.slices;
    return v;
}

}  // namespace gprl::analyzer
