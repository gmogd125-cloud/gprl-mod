#pragma once
// Background level analyzer: the READ-ONLY world extraction (docs/BACKGROUND_ANALYZER_DESIGN.md §3,
// AN-D11, AN-D12). PlayLayer::m_objects is walked once per level visit in time slices from
// PlayLayer::postUpdate (<= modes::extractionSliceUs; 8 ms only for the first slice inside
// setupHasCompleted, measured). What an object IS for the simulator is decided by the pure
// core/analyzer_extract.hpp (host-tested); this file only READS binding members into plain
// `ObjectReading`s.
//
// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. GameObject::getObjectRect() is NOT called: the 2.2081 disassembly (win 0x1976a0 ->
// vtable +0x498 getObjectRect2 0x197850) shows it clears m_isObjectRectDirty (+0x368) and rewrites
// m_objectRect (+0x358) (and the OBB, getBoxOffset's m_boxOffset) whenever the cache is dirty.
// gprl-extract/2: every World rect is GD's exact getObjectRect formula (core/analyzer_extract.hpp
// gdObjectRect, replicated instruction by instruction from the asm) on the level's STATIC fields;
// GD's clean m_objectRect is only compared with the same formula on GD's live fields (the log's
// "estimate check 0 of N"). Every offset the formula reads is pinned by a static_assert in
// Extract.cpp.
//
// Every slice is an analysis block inside the invariant check (AN-D11): a LiveStateSnapshot
// (src/solver/LiveCapture.cpp captureLive, both players, camera, layer) before and after the
// slice, plus a fingerprint of every object of each 64-object chunk (rect cache + dirty flag,
// activation / disabled bytes, position, rotation, visibility) before and after reading it. A
// difference logs the owner's LIVE_STATE_MUTATION_DETECTED block per field, abandons the
// extraction for the level visit (status "stopped (isolation)") and trips the live clone solver's
// breaker (oracle::tripIsolationBreaker("analyzer_extraction")).
//
// Reads (binding names): PlayLayer m_objects, m_level->m_levelID, m_level->m_originalLevel,
// m_level->m_levelName, m_levelLength, m_endPortal (getPositionX), m_anticheatSpike (compared, never
// extracted: GD moves it onto player 1 every step),
// m_levelSettings (m_startMode, m_startMini, m_startSpeed, m_startDual, m_isFlipped, m_mirrorMode,
// m_reverseGameplay, m_platformerMode, m_twoPlayerMode), m_startPosObject, m_player1 (getPosition,
// logged only); GameObject m_objectID, m_uniqueID, m_objectType, m_isTrigger, m_positionX /
// m_positionY, m_startPosition, STATIC: m_startRotationX / Y, m_startScaleX / Y, m_startFlipX / Y,
// m_width, m_height, m_spriteWidthScale / m_spriteHeightScale, m_isMirroredByScale,
// m_customBoxOffset, m_objectRadius, m_isNoTouch, m_isPassable, m_slopeIsHazard, m_groups /
// m_groupCount; LIVE (the formula check / diagnostics only): m_scaleX / m_scaleY, m_rotationXOffset /
// m_rotationYOffset, m_isFlipX / m_isFlipY, m_isRotationAligned, m_shouldUseOuterOb,
// m_boxOffsetCalculated + m_boxOffset, m_isObjectRectDirty + m_objectRect, m_slopeUphill,
// isVisible(), m_isDisabled; the chunk fingerprint also reads getPosition(), getRotation(),
// m_isActivated, m_isGroupDisabled, m_isOrientedBoxDirty. EnhancedGameObject m_isMultiActivate;
// EffectGameObject m_targetGroupID; StartPosObject m_startSettings (the LevelSettingsObject fields
// above). Only STATIC properties enter the World (the gameplay hash must not depend on when an
// object was read): runtime visibility is counted, never used.
//
// Level families (docs/LEVEL_FAMILY_DESIGN.md §3, §8; core/analyzer_identity.hpp): the SAME walk
// also keeps every object the physics ignores - type 7 decoration and collectibles as an
// identity::DecoObject (rect by the gameplay rect rule on the same static fields, plus GameObject
// m_zLayer, m_zOrder, m_hasNoGlow, m_baseOrDetailBlending, m_isNoTouch, m_isPassable; no opacity:
// getOpacity() is the live fading value), harmless triggers as an identity::TriggerObject (kind by
// object id, EffectGameObject m_duration), both capped at kConstants.maxDecoObjects while
// collecting. Pure reads only; the chunk fingerprint around every chunk covers them.
//
// Errors: a failed allocation (or any exception) inside a slice stops the walk for the visit
// (Progress::failed, "out of memory"); nothing is thrown out of PlayLayer::postUpdate.
#include <Geode/Geode.hpp>

#include <optional>
#include <string>

#include "../../core/analyzer_extract.hpp"
#include "../../core/identity/identity.hpp"

namespace gprl::analyzer::extraction {

/// What the main thread hands to the worker when the walk is complete: the builder still holds
/// the raw objects; the worker sorts / caps / builds the spans (WorldBuilder::finish) and hashes
/// the World, so none of that runs on the game thread.
struct Handover {
    extract::WorldBuilder builder;
    extract::BuildParams params;
    extract::BuildCounters counters;
    int arrayObjects = 0;       // m_objects->count()
    int slices = 0;
    double mainThreadMs = 0.0;  // all slices together (captures included)
    double maxSliceUs = 0.0;
    // level families (worker-owned after the hand-over): the decoration / trigger set of the same
    // walk, the copied-from hint (GJGameLevel::m_originalLevel, GD key 30; 0 = none) and the level
    // name reduced to printable ASCII (core/display levelNameHint) - neither is a fingerprint input
    gprl::identity::DecoSet deco;
    int copiedFromGdId = 0;
    std::string nameHint;
    int anticheatSkipped = 0;   // GD's anti-cheat spike seen in m_objects and skipped
    int decoCapped = 0;         // decoration / trigger objects over the cap (not in `deco`)
};

struct Progress {
    bool active = false;        // walking
    bool done = false;          // handed over (take())
    bool failed = false;
    bool isolationStopped = false;
    std::string failReason;
    int index = 0, total = 0;
    int slices = 0;
    double lastSliceUs = 0.0, maxSliceUs = 0.0, totalMs = 0.0;
    int checks = 0;             // invariant checks (slice snapshots + chunk fingerprints)
    double percent() const { return total > 0 ? 100.0 * index / total : 0.0; }
};

/// Level enter (setupHasCompleted): a fresh walk over pl->m_objects.
void begin(PlayLayer* pl);
/// One slice of at most `budgetUs` microseconds (measured; at least one 64-object chunk). True
/// when the walk just completed (take() the result).
bool slice(PlayLayer* pl, int budgetUs);
std::optional<Handover> take();
/// Level exit / visit end: drop whatever was read.
void abandon(char const* why);
Progress progress();

}  // namespace gprl::analyzer::extraction
