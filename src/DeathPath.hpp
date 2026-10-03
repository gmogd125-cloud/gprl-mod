#pragma once
// noclip-death-detector/2, game side (docs/NOCLIP_DEATH_DETECTOR.md; rules + tests in
// core/death_detector). Everything GPRL knows about a death / would-be death of the real player
// comes through here:
//
//   PlayLayer::destroyPlayer (src/Hooks.cpp, outermost)  begin() -> the game's death path -> finish()
//   PlayLayer::destroyPlayer (innermost, this file)       "the game's own function was reached"
//   GJBaseGameLayer::update / checkCollisions (this file) the scope "GD's own physics step is checking
//                                                         THIS PlayerObject" + the return-value signal
//   PlayLayer::resetLevel (src/Hooks.cpp)                 resetBegin() / resetEnd()
//
// GPRL never tests a hitbox itself here: the hitboxes in the debug record / overlay are read for
// display AFTER the game raised the kill, and only while the `death-debug` setting is on.
#include <Geode/Geode.hpp>

#include "../core/death_detector.hpp"

namespace gprl::deathpath {

/// THE identity rule (PROMPT §3): 1 / 2 when `p` is, by pointer, player 1 / player 2 of `pl`, `pl`
/// is the active PlayLayer of the tracked level session, and `p` is not one of the solver's hidden
/// clones; 0 for everything else (clones, replay / trajectory copies, other mods' PlayerObjects).
int liveSlot(PlayLayer* pl, PlayerObject* p);
inline bool isRealLivePlayer(PlayLayer* pl, PlayerObject* p) { return liveSlot(pl, p) != 0; }

/// Facts captured BEFORE the game's death path runs (generation, identity, GD's own state).
struct Pending {
    death::Candidate candidate;
    PlayerObject* player = nullptr;   // the PlayerObject GD means (a null argument is player 1)
    GameObject* object = nullptr;
    bool nullPlayerArg = false;
    int token = 0;                    // nesting token of the innermost-hook flag
};

/// PlayLayer::destroyPlayer, before the original. `player` / `object` exactly as the game passed them.
Pending begin(PlayLayer* pl, PlayerObject* player, GameObject* object);
/// After the original returned: verdict, tracker / analyzer / solver, debug record.
void finish(PlayLayer* pl, Pending& pending);
/// A kill the solver claimed (a hidden clone's, or one raised while a clone was stepped): counted
/// and shown in the debug record as rejected_clone, never anything else.
void cloneKill(PlayLayer* pl, PlayerObject* player, GameObject* object);

void resetBegin(PlayLayer* pl);
void resetEnd(PlayLayer* pl);

/// Level left / layer gone: drop every pointer.
void forget();
/// Once per rendered frame (PlayLayer::postUpdate): the debug overlay.
void frame(PlayLayer* pl, float dt);

/// "live checks N, kills seen N, ..." - the Level page's Details and the debug summary.
std::string summary();

}  // namespace gprl::deathpath
