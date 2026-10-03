#pragma once
// A detached copy of a player's physics state (docs/SOLVER_DESIGN.md §1.1-§1.2): the frame-perfect-
// counter PlayerState (PlayerFields.inc, the touched rings, the four collision logs) extended with
// the hitbox block copyState copies beyond CopyFields.inc, so the history ring can hold ~2 s of
// steps as records instead of hidden PlayerObjects (D3). PlayerFields.inc / CopyFields.inc are
// verbatim copies of FPC's generated field lists; never hand-edit them.
//
// Invariant (checked in game by CloneEngine's snapshot round-trip self-test, §4.3):
//   applyState(b, capture(a)) leaves b such that statesMatch(a, b) and hitboxEqual(a, b).
#include <Geode/Geode.hpp>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gprl::clone {

#include "PlayerFields.inc"

/// GameObject::getObjectRect builds the hitbox from these (width / height, sprite scale factors,
/// object scale, flips, box offsets, position), so a copy must carry them all.
struct Hitbox {
    float width = 0.f;
    float height = 0.f;
    float spriteWidthScale = 1.f;
    float spriteHeightScale = 1.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float customScaleX = 1.f;
    float customScaleY = 1.f;
    float pixelScaleX = 1.f;
    float pixelScaleY = 1.f;
    bool isFlipX = false;
    bool isFlipY = false;
    bool isMirroredByScale = false;
    bool isRotationAligned = false;
    cocos2d::CCPoint customBoxOffset;
    cocos2d::CCPoint boxOffset;
    bool boxOffsetCalculated = false;
    bool hasExtendedCollision = false;
    double positionX = 0.0;
    double positionY = 0.0;
    float unmodifiedPositionX = 0.f;
    float unmodifiedPositionY = 0.f;
    cocos2d::CCRect objectRect;
    float objectRadius = 0.f;
};

struct PlayerState {
    PlayerFields f{};
    cocos2d::CCPoint pos;
    float rot = 0.f;
    float sx = 1.f;
    float sy = 1.f;
    cocos2d::CCPoint position;       // PlayerObject::m_position
    cocos2d::CCPoint lastPosition;   // m_lastPosition
    cocos2d::CCPoint shipRotation;   // the game stores the previous step's position here
    float unkUnused3 = 0.f;          // and the previous step's rotation here
    Hitbox box;
    std::vector<geode::Ref<cocos2d::CCObject>> rings;
    std::vector<std::pair<intptr_t, geode::Ref<cocos2d::CCObject>>> logs[4];
};

void captureState(PlayerState& out, PlayerObject* p);
void applyState(PlayerObject* p, PlayerState const& s);
/// Field-by-field copy between two live player objects (FPC copyState, verbatim).
void copyState(PlayerObject* dst, PlayerObject* src);
/// The hitbox fields of two players compare equal (self-test); `why` names the first difference.
bool hitboxEqual(PlayerObject* a, PlayerObject* b, std::string& why);
std::string rectStr(PlayerObject* p);

}  // namespace gprl::clone
