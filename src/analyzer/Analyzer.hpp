#pragma once
// Background level analyzer: the main-thread facade the hooks call (docs/BACKGROUND_ANALYZER_
// DESIGN.md §2, §3, §5). One "visit" per level entry: the read-only extraction runs in slices from
// PlayLayer::postUpdate, the passive recorder records every eligible attempt, and both are handed
// to the worker thread (src/analyzer/Worker.cpp), which owns everything from then on.
//
//   setupHasCompleted          -> onLevelEnter: modes refresh, attempt 1 open + recorder attempt 1
//                                 (after tracker::onLevelEnter: its menus), then the extraction
//                                 starts (the one 8 ms slice of the visit: the level is still loading)
//   resetLevel (after GD)      -> onLevelReset: the open attempt ends (restart); runs BEFORE
//                                 tracker::onLevelReset (it reads the ending attempt's noclip flag)
//                              -> onAttemptStarted: the next attempt starts; runs AFTER
//                                 tracker::onLevelReset (the recorder uses the menus it just read)
//   processCommands (pre)      -> onStepBegin: the tick that just finished is recorded; the real
//                                 physics steps, so the pause flag is cleared
//   PlayerObject::update (post)-> onPlayerUpdate: the tick clock
//   pushButton / releaseButton -> onInput (pre: before GD applies it)
//   destroyPlayer (post, real) -> onDestroyPlayer: attempt end (died), the death pause starts
//   levelComplete              -> onLevelComplete: attempt end (completed)
//   postUpdate                 -> onFrame: frame-pressure sample, one extraction slice (0 under
//                                 frame pressure, else min(8 ms, 25 % of the frame target))
//   pauseGame / resume         -> onPause / onResume
//   onQuit / goEdit            -> onQuit: attempt end (exit), the job pauses (kept 10 minutes)
//   Fields destructor          -> forgetLayer (the layer is dying: nothing is read)
//
// No exception leaves these functions (Analyzer.cpp guarded()): a failure stops the analysis of the
// visit, never the game.
//
// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. GJBaseGameLayer::update is not hooked here; nothing here steps anything.
#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>

namespace gprl::analyzer {

/// $on_mod(Loaded): the result folder. The worker thread starts with the first analysed level.
void init();
/// GameEvent Exiting: stop the worker (never networks on the exit path beyond a request in flight).
void shutdown();
/// settings::load(): modes, worker config; a mode switched off mid-level stops this visit.
void applySettings();

void onLevelEnter(PlayLayer* pl);
void onLevelReset(PlayLayer* pl);
void onAttemptStarted(PlayLayer* pl);
void onStepBegin(GJBaseGameLayer* layer);
void onPlayerUpdate(PlayerObject* p, float dt);
void onInput(PlayerObject* p, int button, bool down);
void onDestroyPlayer(PlayLayer* pl, PlayerObject* p, bool wasDead, bool isDead);
void onLevelComplete(PlayLayer* pl);
void onFrame(PlayLayer* pl);
void onPause(PlayLayer* pl);
void onResume(PlayLayer* pl);
void onQuit(PlayLayer* pl);
void forgetLayer(PlayLayer* pl);

/// The open visit as the status texts need it.
struct VisitView {
    bool open = false;              // a level is open and analysed
    uint64_t visitId = 0;
    bool extracting = false;
    double extractPercent = 0.0;
    bool submitted = false;         // the extraction went to the worker
    bool isolationStopped = false;
    bool failed = false;
    std::string failReason;
    int attemptsRecorded = 0;       // finished attempts kept or handed over this visit
    int attemptsDropped = 0;
    bool recording = false;
    std::string recordSkip;         // why the current attempt is not recorded
    double extractMs = 0.0, maxSliceUs = 0.0;
    int slices = 0;
};
VisitView visitView();

}  // namespace gprl::analyzer
