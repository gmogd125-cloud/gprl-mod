// The lockstep clone engine: the frame-perfect-counter Engine subset (docs/SOLVER_DESIGN.md §1.1
// lists what is verbatim). Physics facts respected here are in docs/GD_PHYSICS_NOTES.md; when in
// doubt the FPC code path was copied rather than re-derived.
#include "CloneEngine.hpp"

#include "LiveCapture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "../../core/geometry_hash.hpp"
#include "../../core/solver/diagnostics.hpp"
#include "../../core/solver/miss_attribution.hpp"
#include "../../core/solver/timeline.hpp"
#include "../../core/solver/timing_units.hpp"

using namespace geode::prelude;

namespace gprl::clone {

namespace {

namespace activation = gprl::solver::activation;
constexpr auto const& kBudget = gprl::solver::budget::kBudget;

constexpr float FLAG_RANGE = 130.f;   // world units around a clone whose activation flags are isolated
constexpr int CONVERGE_STEPS = 16;    // steps a shifted clone must match the control clone to count as alive
constexpr double THROTTLE_ON_MS = kBudget.throttleOnMs;   // average sim ms per frame that pauses new measurements
constexpr double THROTTLE_OFF_MS = kBudget.throttleOffMs;
constexpr int SHADOW_LOG_MAX = 6;        // shadow differences written to the log per attempt
constexpr float SLOW_FRAME_DT = static_cast<float>(kBudget.slowFrameDt);
constexpr double kWouldBeDeathWindowFrames = 0.75 * 240.0;

// v0.7.0 marks (real portal frames, shadow mismatch frames) are pruned by AGE, not by count. Every
// timing_result still to be emitted belongs to an input inside the history ring (local jobs resolve
// ~140 frames after their input; SA candidates and jobs are given up once their base leaves the
// ring; the restart flushes the rest), so a mark older than two rings can no longer touch one. The
// partial build kept the last 64 marks: in a noisy attempt (the breaker allows up to 25 % shadow
// mismatches) that silently dropped a mismatch inside an input's look-ahead before its result was
// emitted, and the result could come out `ok` (AUDIT §16), or lost a portal break a queued SA job
// needs. kMaxMarks is only the hard memory bound; every push is O(marks), bounded by it.
constexpr double kMarkKeepFrames = 2.0 * static_cast<double>(kHistorySteps);
constexpr size_t kMaxMarks = 4096;
template <class Marks>
void pushMark(Marks& marks, typename Marks::value_type mark, double now) {
    std::erase_if(marks, [&](typename Marks::value_type const& m) { return m.first < now - kMarkKeepFrames; });
    if (marks.size() >= kMaxMarks) marks.erase(marks.begin());
    marks.push_back(std::move(mark));
}

// Objects whose one-shot activation state must be isolated per clone: orbs (RingObject), pads and
// portals. (Triggers, collectibles and decorations are left alone; clones never trigger them.)
constexpr int kPadPortalIds[] = {35, 67, 140, 1332, 3004, 3005,
    10, 11, 12, 13, 47, 111, 660, 745, 1331, 1933, 99, 101, 200, 201, 202, 203, 1334, 45, 46, 286, 287, 747, 2902};

int ring(int step) { return solver::timeline::ringIndex(step, kHistorySteps); }

// The physical one-shot bits of an orb / pad / portal (core/solver/activation.hpp Flags).
activation::Flags physicalFlags(EnhancedGameObject* o) {
    activation::Flags f;
    f.slot1 = o->m_activatedByPlayer1;
    f.slot2 = o->m_activatedByPlayer2;
    f.isActivated = o->m_isActivated;
    f.activated = o->m_activated;
    return f;
}




void copyDict(CCDictionary* dst, CCDictionary* src) {
    if (!dst || !src) return;
    dst->removeAllObjects();
    if (src->count() == 0) return;
    auto keys = src->allKeys();
    if (!keys) return;
    for (auto k : CCArrayExt<CCObject*>(keys)) {
        if (auto i = typeinfo_cast<CCInteger*>(k)) {
            if (auto o = src->objectForKey(static_cast<intptr_t>(i->getValue()))) dst->setObject(o, static_cast<intptr_t>(i->getValue()));
        }
        else if (auto s = typeinfo_cast<CCString*>(k)) {
            std::string key = s->getCString();
            if (auto o = src->objectForKey(key)) dst->setObject(o, key);
        }
    }
}

void copyArray(CCArray* dst, CCArray* src) {
    if (!dst || !src) return;
    dst->removeAllObjects();
    if (src->count()) dst->addObjectsFromArray(src);
}

void clearLogs(PlayerObject* p) {
    if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
    if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
    if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
    if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();
}

// PlayerObject::resetCollisionLog is inlined on Windows; same rewrite Click Between Frames uses
// between its sub-steps.
void resetLog(PlayerObject* p) {
    clearLogs(p);
    p->m_lastCollisionLeft = -1;
    p->m_lastCollisionRight = -1;
    p->m_lastCollisionBottom = -1;
    p->m_lastCollisionTop = -1;
}

// What GJBaseGameLayer::update does to each player right after processCommands and before the
// player's update (GD 2.2081): drop rings no longer overlapped, remember last step's bottom / top
// collision ids, clear the collision logs. THE per-step reset (GD_PHYSICS_NOTES "Step loop").
void gameStepReset(PlayerObject* p) {
    p->resetTouchedRings(false);
    clearLogs(p);
    p->m_unk50C = p->m_lastCollisionBottom;
    p->m_unk510 = p->m_lastCollisionTop;
    p->m_lastCollisionBottom = -1;
    p->m_lastCollisionTop = -1;
    p->m_lastCollisionLeft = -1;
    p->m_lastCollisionRight = -1;
}

char modeChar(PlayerObject* p) {
    if (p->m_isShip) return 'S';
    if (p->m_isBall) return 'B';
    if (p->m_isBird) return 'U';
    if (p->m_isDart) return 'W';
    if (p->m_isRobot) return 'R';
    if (p->m_isSpider) return 'P';
    if (p->m_isSwing) return 'G';
    return 'C';
}

std::string stateStr(PlayerObject* p) {
    int rings = p->m_touchingRings ? static_cast<int>(p->m_touchingRings->count()) : 0;
    bool hold = false;
    if (auto it = p->m_holdingButtons.find(1); it != p->m_holdingButtons.end()) hold = it->second;
    return fmt::format("({:.3f},{:.3f}) vy {:.4f} {}{}{}{}{} rings {} last({:.2f},{:.2f}) size {:.2f} spd {:.2f}",
        p->getPositionX(), p->getPositionY(), p->m_yVelocity, modeChar(p), p->m_isOnGround ? " ground" : " air",
        p->m_isUpsideDown ? " flipped" : "", p->m_isDashing ? " dash" : "", hold ? " hold" : "", rings,
        p->m_lastPosition.x, p->m_lastPosition.y, p->m_vehicleSize, p->m_playerSpeed);
}

void dictToList(std::vector<std::pair<intptr_t, Ref<CCObject>>>& out, CCDictionary* src) {
    out.clear();
    if (!src || src->count() == 0) return;
    auto keys = src->allKeys();
    if (!keys) return;
    for (auto k : CCArrayExt<CCObject*>(keys)) {
        if (auto i = typeinfo_cast<CCInteger*>(k)) {
            if (auto o = src->objectForKey(static_cast<intptr_t>(i->getValue()))) out.emplace_back(static_cast<intptr_t>(i->getValue()), Ref<CCObject>(o));
        }
    }
}

void listToDict(CCDictionary* dst, std::vector<std::pair<intptr_t, Ref<CCObject>>> const& src) {
    if (!dst) return;
    dst->removeAllObjects();
    for (auto const& [k, o] : src) {
        if (o) dst->setObject(o, k);
    }
}

// Same rule Click Between Frames uses to decide whether an input splits the physics step.
bool notBuffering(PlayerObject* p) {
    return p->m_isOnGround || (p->m_touchingRings && p->m_touchingRings->count() > 0) || p->m_isDashing
        || p->m_isDart || p->m_isBird || p->m_isShip || p->m_isSwing;
}

bool samePhysics(PlayerObject* a, PlayerObject* b) {
    if (ccpDistance(a->getPosition(), b->getPosition()) > 0.02f) return false;
    if (std::fabs(a->m_yVelocity - b->m_yVelocity) > 1e-4) return false;
    if (a->m_isOnGround != b->m_isOnGround || a->m_isUpsideDown != b->m_isUpsideDown) return false;
    if (a->m_isShip != b->m_isShip || a->m_isBall != b->m_isBall || a->m_isBird != b->m_isBird || a->m_isDart != b->m_isDart
        || a->m_isRobot != b->m_isRobot || a->m_isSpider != b->m_isSpider || a->m_isSwing != b->m_isSwing) return false;
    if (a->m_isDashing != b->m_isDashing || a->m_isOnSlope != b->m_isOnSlope || a->m_jumpBuffered != b->m_jumpBuffered) return false;
    if (a->m_vehicleSize != b->m_vehicleSize || a->m_playerSpeed != b->m_playerSpeed) return false;
    if (a->m_holdingButtons != b->m_holdingButtons) return false;
    int ra = a->m_touchingRings ? a->m_touchingRings->count() : 0;
    int rb = b->m_touchingRings ? b->m_touchingRings->count() : 0;
    return ra == rb;
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

char statusChar(CloneStatus s) {
    switch (s) {
        case CloneStatus::Alive: return 'A';
        case CloneStatus::Dead: return 'D';
        case CloneStatus::Invalid: return 'X';
        case CloneStatus::Cancelled: return 'c';
        case CloneStatus::Limit: return 'l';
        case CloneStatus::Running: return 'r';
        case CloneStatus::Idle: return 'i';
    }
    return '?';
}

}  // namespace

// ---- state copies (FPC copyState / captureState / applyState + the hitbox block) ----

void copyState(PlayerObject* dst, PlayerObject* src) {
#include "CopyFields.inc"
    dst->m_position = src->m_position;
    dst->m_lastPosition = src->m_lastPosition;
    dst->m_shipRotation = src->m_shipRotation;   // the game stores the previous step's position here
    dst->m_unkUnused3 = src->m_unkUnused3;       // and the previous step's rotation here
    dst->setPosition(src->getPosition());
    dst->setRotation(src->getRotation());
    dst->setScaleX(src->getScaleX());
    dst->setScaleY(src->getScaleY());
    copyArray(dst->m_touchingRings, src->m_touchingRings);
    copyDict(dst->m_collisionLogTop, src->m_collisionLogTop);
    copyDict(dst->m_collisionLogBottom, src->m_collisionLogBottom);
    copyDict(dst->m_collisionLogLeft, src->m_collisionLogLeft);
    copyDict(dst->m_collisionLogRight, src->m_collisionLogRight);
    // Hitbox: GameObject::getObjectRect builds the box from these fields (width/height, sprite scale
    // factors, object scale, flips, box offsets, position), so a copy must carry them all.
    dst->m_width = src->m_width;
    dst->m_height = src->m_height;
    dst->m_spriteWidthScale = src->m_spriteWidthScale;
    dst->m_spriteHeightScale = src->m_spriteHeightScale;
    dst->m_scaleX = src->m_scaleX;
    dst->m_scaleY = src->m_scaleY;
    dst->m_customScaleX = src->m_customScaleX;
    dst->m_customScaleY = src->m_customScaleY;
    dst->m_pixelScaleX = src->m_pixelScaleX;
    dst->m_pixelScaleY = src->m_pixelScaleY;
    dst->m_isFlipX = src->m_isFlipX;
    dst->m_isFlipY = src->m_isFlipY;
    dst->m_isMirroredByScale = src->m_isMirroredByScale;
    dst->m_isRotationAligned = src->m_isRotationAligned;
    dst->m_customBoxOffset = src->m_customBoxOffset;
    dst->m_boxOffset = src->m_boxOffset;
    dst->m_boxOffsetCalculated = src->m_boxOffsetCalculated;
    dst->m_hasExtendedCollision = src->m_hasExtendedCollision;
    dst->m_positionX = src->m_positionX;
    dst->m_positionY = src->m_positionY;
    dst->m_unmodifiedPositionX = src->m_unmodifiedPositionX;
    dst->m_unmodifiedPositionY = src->m_unmodifiedPositionY;
    dst->m_objectRect = src->m_objectRect;
    dst->m_objectRadius = src->m_objectRadius;
    dst->m_isObjectRectDirty = true;
    dst->setObjectRectDirty(true);
    dst->setOrientedRectDirty(true);
    dst->m_isDead = false;
    dst->m_isSecondPlayer = false;
    dst->m_playEffects = false;
    dst->m_hasGroundParticles = false;
    dst->m_hasShipParticles = false;
}

std::string rectStr(PlayerObject* p) {
    auto r = p->getObjectRect();
    return fmt::format("[{:.1f},{:.1f} {:.1f}x{:.1f}] pos({:.1f},{:.1f}) scale {:.2f} radius {:.1f} dirty {}",
        r.origin.x, r.origin.y, r.size.width, r.size.height, p->getPositionX(), p->getPositionY(), p->getScale(),
        p->m_objectRadius, p->m_isObjectRectDirty ? 1 : 0);
}

void captureState(PlayerState& out, PlayerObject* p) {
    saveFields(out.f, p);
    out.pos = p->getPosition();
    out.rot = p->getRotation();
    out.sx = p->getScaleX();
    out.sy = p->getScaleY();
    out.position = p->m_position;
    out.lastPosition = p->m_lastPosition;
    out.shipRotation = p->m_shipRotation;
    out.unkUnused3 = p->m_unkUnused3;
    auto& b = out.box;
    b.width = p->m_width;
    b.height = p->m_height;
    b.spriteWidthScale = p->m_spriteWidthScale;
    b.spriteHeightScale = p->m_spriteHeightScale;
    b.scaleX = p->m_scaleX;
    b.scaleY = p->m_scaleY;
    b.customScaleX = p->m_customScaleX;
    b.customScaleY = p->m_customScaleY;
    b.pixelScaleX = p->m_pixelScaleX;
    b.pixelScaleY = p->m_pixelScaleY;
    b.isFlipX = p->m_isFlipX;
    b.isFlipY = p->m_isFlipY;
    b.isMirroredByScale = p->m_isMirroredByScale;
    b.isRotationAligned = p->m_isRotationAligned;
    b.customBoxOffset = p->m_customBoxOffset;
    b.boxOffset = p->m_boxOffset;
    b.boxOffsetCalculated = p->m_boxOffsetCalculated;
    b.hasExtendedCollision = p->m_hasExtendedCollision;
    b.positionX = p->m_positionX;
    b.positionY = p->m_positionY;
    b.unmodifiedPositionX = p->m_unmodifiedPositionX;
    b.unmodifiedPositionY = p->m_unmodifiedPositionY;
    b.objectRect = p->m_objectRect;
    b.objectRadius = p->m_objectRadius;
    out.rings.clear();
    if (p->m_touchingRings) {
        for (auto o : CCArrayExt<CCObject*>(p->m_touchingRings)) out.rings.emplace_back(o);
    }
    dictToList(out.logs[0], p->m_collisionLogTop);
    dictToList(out.logs[1], p->m_collisionLogBottom);
    dictToList(out.logs[2], p->m_collisionLogLeft);
    dictToList(out.logs[3], p->m_collisionLogRight);
}

void applyState(PlayerObject* p, PlayerState const& s) {
    loadFields(p, s.f);
    p->m_position = s.position;
    p->m_lastPosition = s.lastPosition;
    p->m_shipRotation = s.shipRotation;
    p->m_unkUnused3 = s.unkUnused3;
    p->setPosition(s.pos);
    p->setRotation(s.rot);
    p->setScaleX(s.sx);
    p->setScaleY(s.sy);
    if (p->m_touchingRings) {
        p->m_touchingRings->removeAllObjects();
        for (auto const& r : s.rings) if (r) p->m_touchingRings->addObject(r);
    }
    listToDict(p->m_collisionLogTop, s.logs[0]);
    listToDict(p->m_collisionLogBottom, s.logs[1]);
    listToDict(p->m_collisionLogLeft, s.logs[2]);
    listToDict(p->m_collisionLogRight, s.logs[3]);
    auto const& b = s.box;
    p->m_width = b.width;
    p->m_height = b.height;
    p->m_spriteWidthScale = b.spriteWidthScale;
    p->m_spriteHeightScale = b.spriteHeightScale;
    p->m_scaleX = b.scaleX;
    p->m_scaleY = b.scaleY;
    p->m_customScaleX = b.customScaleX;
    p->m_customScaleY = b.customScaleY;
    p->m_pixelScaleX = b.pixelScaleX;
    p->m_pixelScaleY = b.pixelScaleY;
    p->m_isFlipX = b.isFlipX;
    p->m_isFlipY = b.isFlipY;
    p->m_isMirroredByScale = b.isMirroredByScale;
    p->m_isRotationAligned = b.isRotationAligned;
    p->m_customBoxOffset = b.customBoxOffset;
    p->m_boxOffset = b.boxOffset;
    p->m_boxOffsetCalculated = b.boxOffsetCalculated;
    p->m_hasExtendedCollision = b.hasExtendedCollision;
    p->m_positionX = b.positionX;
    p->m_positionY = b.positionY;
    p->m_unmodifiedPositionX = b.unmodifiedPositionX;
    p->m_unmodifiedPositionY = b.unmodifiedPositionY;
    p->m_objectRect = b.objectRect;
    p->m_objectRadius = b.objectRadius;
    p->m_isObjectRectDirty = true;
    p->setObjectRectDirty(true);
    p->setOrientedRectDirty(true);
    p->m_isDead = false;
    p->m_isSecondPlayer = false;
    p->m_playEffects = false;
    p->m_hasGroundParticles = false;
    p->m_hasShipParticles = false;
}

bool hitboxEqual(PlayerObject* a, PlayerObject* b, std::string& why) {
#define GPRL_HB(field) if (a->field != b->field) { why = #field; return false; }
    GPRL_HB(m_width) GPRL_HB(m_height) GPRL_HB(m_spriteWidthScale) GPRL_HB(m_spriteHeightScale)
    GPRL_HB(m_scaleX) GPRL_HB(m_scaleY) GPRL_HB(m_customScaleX) GPRL_HB(m_customScaleY)
    GPRL_HB(m_pixelScaleX) GPRL_HB(m_pixelScaleY) GPRL_HB(m_isFlipX) GPRL_HB(m_isFlipY)
    GPRL_HB(m_isMirroredByScale) GPRL_HB(m_isRotationAligned) GPRL_HB(m_boxOffsetCalculated)
    GPRL_HB(m_hasExtendedCollision) GPRL_HB(m_positionX) GPRL_HB(m_positionY)
    GPRL_HB(m_unmodifiedPositionX) GPRL_HB(m_unmodifiedPositionY) GPRL_HB(m_objectRadius)
#undef GPRL_HB
    if (!a->m_customBoxOffset.equals(b->m_customBoxOffset)) { why = "m_customBoxOffset"; return false; }
    if (!a->m_boxOffset.equals(b->m_boxOffset)) { why = "m_boxOffset"; return false; }
    if (!a->m_objectRect.equals(b->m_objectRect)) { why = "m_objectRect"; return false; }
    if (!a->m_shipRotation.equals(b->m_shipRotation)) { why = "m_shipRotation"; return false; }
    if (a->m_unkUnused3 != b->m_unkUnused3) { why = "m_unkUnused3"; return false; }
    if (!a->m_position.equals(b->m_position)) { why = "m_position"; return false; }
    if (a->getScaleX() != b->getScaleX() || a->getScaleY() != b->getScaleY()) { why = "node scale"; return false; }
    if (a->getRotation() != b->getRotation()) { why = "node rotation"; return false; }
    return true;
}

// ---- engine ----

CloneEngine& CloneEngine::get() {
    // Leaked on purpose: the history ring holds Ref<CCObject>s (rings, collision-log entries) and
    // a static destructor at process exit would release objects cocos has already destroyed.
    static CloneEngine* e = new CloneEngine();
    return *e;
}

void CloneEngine::slog(int level, std::string const& line) const {
    if (m_cfg.verbosity >= level) log::info("{}", line);
}

PlayerObject* CloneEngine::makeClone() {
    auto p = PlayerObject::create(1, 1, m_pl, m_pl, true);
    if (!p) return nullptr;
    p->retain();
    p->setVisible(false);
    p->m_playEffects = false;
    p->m_hasGroundParticles = false;
    p->m_hasShipParticles = false;
    if (p->m_regularTrail) {
        p->m_regularTrail->setVisible(false);
        p->m_regularTrail->unscheduleUpdate();
    }
    if (p->m_waveTrail) p->m_waveTrail->setVisible(false);
    if (p->m_ghostTrail) p->m_ghostTrail->setVisible(false);
    if (p->m_particleSystems) {
        for (auto o : CCArrayExt<CCObject*>(p->m_particleSystems)) {
            if (auto ps = typeinfo_cast<CCParticleSystem*>(o)) {
                ps->stopSystem();
                ps->setVisible(false);
            }
        }
    }
    m_pl->m_objectLayer->addChild(p, -1000);
    return p;
}

void CloneEngine::freeClone(Clone& c) {
    c.state = CloneStatus::Idle;
    c.job = -1;
    c.pass = 0;
    c.shift = 0.0;
    c.inputFrame = 0.0;
    c.inputPending = false;
    c.appliedFrame = solver::kNaN;
    c.stepDone = -1;
    c.frameDone = 0.0;
    c.deathStep = -1;
    c.deathFrame = 0.0;
    c.deathObjId = -1;
    c.deathObjX = 0.f;
    c.converged = 0;
    c.resynced = false;
    c.extension = false;
    c.invalidReason.clear();
    c.moved.clear();
    c.flags.clear();
    c.attributeAfterFrame = 1e300;
    c.laterFixed = 0;
    c.rejoinStartFrame = solver::kNaN;
    c.deathX = 0.f;
    c.tracing = false;
    c.compNoted = false;
    c.trace.steps.clear();   // keeps the capacity: a traced clone never reallocates on its next job
    c.trace.death = {};
    c.trace.passed = false;
    c.trace.capped = false;
    c.trace.shiftFrames = 0.0;
}

int CloneEngine::allocClone(bool forControl) {
    for (size_t i = 0; i < m_clones.size(); ++i) {
        if (m_clones[i].state == CloneStatus::Idle) return static_cast<int>(i);
    }
    return createClone(forControl);
}

int CloneEngine::createClone(bool forControl) {
    // Lazy pool (SOLVER_DESIGN §6) with a per-frame creation ration (§12): PlayerObject::create
    // costs ~0.7 ms (sprites, particles, trails); 21 of them in one frame were the 18-24 ms peaks.
    if (!solver::budget::mayCreateClone(static_cast<int>(m_clones.size()), m_createdThisFrame, forControl)) return -1;
    auto obj = makeClone();
    if (!obj) return -1;
    ++m_createdThisFrame;
    ++m_counters.clonesCreated;
    if (m_firstCloneUid == 0) m_firstCloneUid = obj->m_uniqueID;
    Clone c;
    c.obj = obj;
    freeClone(c);
    m_index[obj] = static_cast<int>(m_clones.size());
    m_clones.push_back(std::move(c));
    return static_cast<int>(m_clones.size()) - 1;
}

bool CloneEngine::selfTest(PlayerObject* p1, std::string& why) {
    int ia = allocClone(true);
    int ib = allocClone(true);
    if (ia < 0 || ib < 0) {
        why = "could not create two clone players";
        return false;
    }
    auto a = m_clones[ia].obj;
    auto b = m_clones[ib].obj;
    copyState(a, p1);
    PlayerState s;
    captureState(s, p1);
    applyState(b, s);
    bool ok = statesMatch(a, b, why);
    if (ok) ok = hitboxEqual(a, b, why);
    if (ok) {
        // rings and the four collision logs must round-trip too
        int ra = a->m_touchingRings ? a->m_touchingRings->count() : 0;
        int rb = b->m_touchingRings ? b->m_touchingRings->count() : 0;
        if (ra != rb) { why = "touching rings"; ok = false; }
        CCDictionary* la[4] = {a->m_collisionLogTop, a->m_collisionLogBottom, a->m_collisionLogLeft, a->m_collisionLogRight};
        CCDictionary* lb[4] = {b->m_collisionLogTop, b->m_collisionLogBottom, b->m_collisionLogLeft, b->m_collisionLogRight};
        for (int i = 0; i < 4 && ok; ++i) {
            unsigned ca = la[i] ? la[i]->count() : 0;
            unsigned cb = lb[i] ? lb[i]->count() : 0;
            if (ca != cb) { why = fmt::format("collision log {}", i); ok = false; }
        }
    }
    freeClone(m_clones[ia]);
    freeClone(m_clones[ib]);
    return ok;
}

bool CloneEngine::setup(PlayLayer* pl, EngineConfig const& cfg) {
    teardown();
    m_offForVisit = false;   // a new level visit starts clean (teardown clears it too)
    m_setupError.clear();
    m_ringOk = true;
    if (!pl || !pl->m_objectLayer || !pl->m_player1) {
        m_setupError = "no play layer";
        return false;
    }
    m_pl = pl;
    m_pl->retain();   // keeps the layer (and our clones' parent) alive until teardown even on an unusual exit path
    m_cfg = cfg;
    m_guard.setConfiguredMode(cfg.isolationEveryStep ? solver::isolation::CheckMode::EveryCloneStep : solver::isolation::CheckMode::EveryBlock);
    m_platformer = pl->m_levelSettings && pl->m_levelSettings->m_platformerMode;

    m_act.clear();
    m_actX.clear();
    m_geo.clear();
    m_geoX.clear();
    if (pl->m_objects) {
        for (auto o : CCArrayExt<CCObject*>(pl->m_objects)) {
            auto g = typeinfo_cast<GameObject*>(o);
            if (!g) continue;
            if (g->m_objectType != GameObjectType::Decoration) m_geo.push_back(g);
            auto e = typeinfo_cast<EnhancedGameObject*>(o);
            if (!e) continue;
            bool keep = typeinfo_cast<RingObject*>(e) != nullptr;
            if (!keep) {
                for (int id : kPadPortalIds) {
                    if (e->m_objectID == id) {
                        keep = true;
                        break;
                    }
                }
            }
            if (keep) m_act.push_back(e);
        }
    }
    std::sort(m_act.begin(), m_act.end(), [](EnhancedGameObject* a, EnhancedGameObject* b) { return a->getPositionX() < b->getPositionX(); });
    m_actX.reserve(m_act.size());
    for (auto e : m_act) m_actX.push_back(e->getPositionX());
    std::sort(m_geo.begin(), m_geo.end(), [](GameObject* a, GameObject* b) { return a->getPositionX() < b->getPositionX(); });
    m_geoX.reserve(m_geo.size());
    for (auto g : m_geo) m_geoX.push_back(g->getPositionX());

    m_clones.clear();
    m_index.clear();
    m_hist.clear();
    m_hist.resize(kHistorySteps);
    m_shadow = {};
    m_shadow.obj = makeClone();
    if (!m_shadow.obj) {
        m_setupError = "could not create the shadow clone";
        log::error("GPRL solver: {}; solver off for this level", m_setupError);
        teardown();
        return false;
    }
    m_anticheatTouches = 0;
    m_counters = {};
    m_attempt = {};
    m_tripwiresAttempt = 0;
    m_portalModelAttempt = 0;
    m_seqCounters = {};
    m_saCounters = {};
    m_saCands.clear();
    m_saRecent.clear();
    m_ledger.abandon();
    m_lastTrace = {};
    m_seqPlaces.clear();
    m_saPlaces.clear();          // v0.7.1 (Fable D10): per level visit
    m_pendingMisses.clear();     // v0.7.1 (Fable D6)
    m_speedMarks.clear();        // v0.7.1 (Fable D7)
    m_subTickClockMaxMs = 0.0;   // v0.7.1 (Fable D11)
    m_seqNextId = 1;
    m_simMs = 0;
    m_simEma = 0;
    m_simFrameMs = 0;
    m_simPeakMs = 0;
    m_stepCostUs = 0.0;
    m_stepBudget = solver::budget::stepBudgetForFrame(kBudget.defaultStepCostUs);
    m_simFrames = 0;
    m_cloneSteps = 0;
    m_cloneStepsThisFrame = 0;
    m_createdThisFrame = 0;
    m_poolWarmed = 0;
    m_firstCloneUid = 0;
    m_throttled = false;
    m_frameSlow = false;
    m_lastPerfLog = nowMs();
    m_lastSummaryLog = nowMs();
    // The game selects an object's one-shot activation slot by the player's unique id (== 1: slot
    // 1); the real player 1 has id 1 in 2.2081, the clones never do (activation.hpp).
    m_realUid = pl->m_player1->m_uniqueID;
    m_realUsesSlot2 = activation::usesSlot2(m_realUid);

    std::string why;
    if (!selfTest(pl->m_player1, why)) {
        m_ringOk = false;
        m_setupError = "snapshot roundtrip FAILED: " + why;
        log::error("GPRL solver: snapshot roundtrip FAILED ({}); windows are not measured on this level (PlayerState ring cannot be trusted)", why);
    }
    else slog(0, "GPRL solver: snapshot roundtrip OK");
    // Warm pool (budget.hpp): created here, inside the level's loading, so the first passes never
    // create PlayerObjects during gameplay (the 18-24 ms first-spawn peaks of v0.4.x).
    {
        double t0 = nowMs();
        // createClone, never allocClone: allocClone hands back an idle clone (selfTest leaves two)
        // without growing the pool, which made this loop spin forever. The iteration cap is a
        // second guard so a future change here can never hang the level load again.
        for (int guard = 0; static_cast<int>(m_clones.size()) < kBudget.poolWarm && guard < kBudget.poolWarm; ++guard) {
            if (createClone(true) < 0) break;
        }
        m_poolWarmed = static_cast<int>(m_clones.size());
        m_createdThisFrame = 0;
        slog(0, fmt::format("GPRL solver: pool warmed - {} clones in {:.1f} ms (max {}, then at most {} new per frame); real player uid {} -> activation slot {}, clone uids from {} -> slot 2 mirrored",
            m_poolWarmed, nowMs() - t0, kBudget.maxClones, kBudget.cloneCreatesPerFrame, m_realUid, m_realUsesSlot2 ? 2 : 1, m_firstCloneUid));
    }
    reset();
    return true;
}

void CloneEngine::teardown() {
    if (!m_pl) return;
    m_guard.setupLevel(m_guard.configuredMode());   // v0.8.2: the next level starts with fresh counters
    m_isoAbortPending = false;
    m_isoFallbackLogged = false;
    abortJobs("teardown");
    settleMisses(true);   // v0.7.1 (Fable D6): every pending miss still owes its results
    abortSequences("teardown");
    // v0.7.0: every bound job still owes its one timing_result (cut by leaving the level)
    compAbortAll(solver::status::Reason::SaCutByRestart);
    saAbortAll(solver::status::Reason::SaCutByRestart);
    flushResults(solver::status::Reason::CutByRestart, solver::status::Reason::SaCutByRestart);
    m_clusters.reset();
    m_seqLocals.clear();
    m_seqSeen.clear();
    m_seqPlaces.clear();
    m_saPlaces.clear();
    m_speedMarks.clear();
    for (auto& c : m_clones) {
        if (c.obj) {
            c.obj->removeFromParent();
            c.obj->release();
        }
    }
    if (m_shadow.obj) {
        m_shadow.obj->removeFromParent();
        m_shadow.obj->release();
    }
    m_shadow = {};
    if (m_shadow2.obj) {
        m_shadow2.obj->removeFromParent();
        m_shadow2.obj->release();
    }
    m_shadow2 = {};
    m_pairLogged = false;
    m_clones.clear();
    m_hist.clear();
    m_index.clear();
    m_act.clear();
    m_actX.clear();
    m_geo.clear();
    m_geoX.clear();
    m_jobs.clear();
    m_log.clear();
    m_pl->release();
    m_pl = nullptr;
    m_sim = nullptr;
    m_measuring = false;
    m_offForVisit = false;
}

void CloneEngine::stopForVisit(std::string const& why) {
    if (!m_pl || m_offForVisit) return;
    // Called on the main thread from a setting change (never inside a clone step); the result part
    // is teardown()'s, in its order: every bound job / pending miss / sequence / SA candidate still
    // owes its one result, cut like a level exit.
    m_isoAbortPending = false;
    abortJobs("settings");
    settleMisses(true);
    abortSequences("settings");
    compAbortAll(solver::status::Reason::SaCutByRestart);
    saAbortAll(solver::status::Reason::SaCutByRestart);
    flushResults(solver::status::Reason::CutByRestart, solver::status::Reason::SaCutByRestart);
    m_clusters.reset();
    m_seqLocals.clear();
    m_seqSeen.clear();
    m_seqPlaces.clear();
    m_saPlaces.clear();
    m_speedMarks.clear();
    m_portalMarks.clear();
    m_shadowMarks.clear();
    m_saRecent.clear();
    m_jobs.clear();
    m_log.clear();
    m_pendingJob = 0;
    // the clones become idle, hidden objects of the object layer (makeClone: setVisible(false));
    // removing them is left to teardown (onQuit / goEdit) or forgetLayer, never done mid-level
    for (auto& c : m_clones) freeClone(c);
    freeClone(m_shadow);
    freeClone(m_shadow2);
    m_pairLogged = false;
    m_sim = nullptr;
    m_measuring = false;
    m_pauseReason = why;
    m_offForVisit = true;
    log::info("GPRL solver: {} - gate closed for the rest of this level visit; open results ended like a level exit, {} hidden clone(s) left idle "
              "until the level is left (no clone object is removed mid-level)",
              why, m_clones.size() + (m_shadow.obj ? 1 : 0) + (m_shadow2.obj ? 1 : 0));
}

void CloneEngine::forgetLayer(PlayLayer* pl) {
    if (!m_pl || pl != m_pl) return;
    // The dying layer destroys m_objectLayer's children itself and is inside its own destructor:
    // only null our pointers. The retained clone objects and the ring (whose Refs may point at
    // objects the layer already destroyed) are leaked on purpose on this path.
    m_shadow = {};
    m_shadow2 = {};
    m_clones.clear();
    (void)new std::vector<Snapshot>(std::move(m_hist));
    m_hist.clear();
    m_index.clear();
    m_act.clear();
    m_actX.clear();
    m_geo.clear();
    m_geoX.clear();
    m_jobs.clear();
    m_log.clear();
    m_seqQueue.clear();
    m_seq = {};
    m_seqActive = false;
    m_seqLocals.clear();
    m_seqSeen.clear();
    m_seqPlaces.clear();
    // nothing may be pushed from inside the layer's destructor: open results are abandoned
    m_ledger.abandon();
    m_pendingMisses.clear();
    m_speedMarks.clear();
    m_saPlaces.clear();
    m_saCands.clear();
    m_saRecent.clear();
    m_clusters.reset();
    m_pl = nullptr;
    m_sim = nullptr;
    m_measuring = false;
    m_offForVisit = false;
}

void CloneEngine::configure(EngineConfig const& cfg) {
    m_cfg = cfg;
    m_guard.setConfiguredMode(cfg.isolationEveryStep ? solver::isolation::CheckMode::EveryCloneStep : solver::isolation::CheckMode::EveryBlock);
}

void CloneEngine::setMeasuring(bool on, std::string reason) {
    if (m_offForVisit) return;   // stopForVisit: the gate never re-opens during this visit
    if (!m_ringOk) on = false;
    if (on == m_measuring && reason == m_pauseReason) return;
    m_measuring = on;
    m_pauseReason = std::move(reason);
    if (on) slog(0, "GPRL solver: gate=measuring");
    else slog(0, "GPRL solver: gate paused: " + m_pauseReason);
}

void CloneEngine::reset() {
    if (!m_pl || m_offForVisit) return;   // stopForVisit already ended every open result
    // Finalise what the death pause resolved (a job between passes keeps its coarse result), abort the rest.
    if (!m_jobs.empty()) {
        for (auto& job : m_jobs) {
            if (job.planner.ingested() && job.mismatch.empty() && !job.planner.controlFailed()) {
                job.planner.finish();
                finalize(job);
            }
            else dropJob(job, "reset");
        }
        m_jobs.clear();
    }
    // v0.7.1 (Fable D6): the death pause's misses are attributed with what resolved, while the
    // attempt's input log still exists (their results need it)
    settleMisses(true);
    // sequence jobs replay this attempt's history: nothing of them survives a restart
    abortSequences("reset");
    // v0.7.0: SA candidates of this attempt end undecided (sa_cut_by_restart); every bound job of
    // the attempt gets its one timing_result NOW, before the input log is cleared
    compAbortAll(solver::status::Reason::SaCutByRestart);
    saAbortAll(solver::status::Reason::SaCutByRestart);
    flushResults(solver::status::Reason::CutByRestart, solver::status::Reason::SaCutByRestart);
    if (m_attempt.inputs > 0 || m_attempt.shadowSteps > 0) {
        slog(0, fmt::format("GPRL solver: attempt done - {} inputs, {} emitted ({} misses, {} miss downstream), {} dropped, skipped {}{}, shadow {}/{} mismatches, "
                            "{} timing results ({} fallback), input placement: {} whole-tick, {} half-tick, {} sub-tick{}",
            m_attempt.inputs, m_attempt.emitted, m_attempt.misses, m_attempt.missDownstream, m_attempt.dropped, m_attempt.skipped,
            m_attempt.skippedReplay ? fmt::format(" ({} after the replay breaker)", m_attempt.skippedReplay) : std::string(),
            m_attempt.shadowMismatches, m_attempt.shadowSteps, m_attempt.results, m_attempt.resultsFallback, m_attempt.placeWhole, m_attempt.placeHalf,
            m_attempt.placeSub, fmt::format("{}{}", m_replayBroken ? " | replay was broken" : "", m_guard.breached() ? " | isolation BREACHED" : " | isolation ok")));
    }
    m_attempt = {};
    m_tripwiresAttempt = 0;
    m_portalModelAttempt = 0;
    m_guard.resetAttempt();   // v0.8.2: the breaker is per attempt (an escalated check mode stays)
    m_isoAbortPending = false;
    m_clusters.reset();
    m_portalMarks.clear();
    m_shadowMarks.clear();
    m_speedMarks.clear();   // v0.7.1 (Fable D7): per attempt, like the portal marks
    m_saRecent.clear();
    m_breaker.reset();
    m_replayBroken = false;
    m_firstShadowWhy.clear();
    m_tracedThisAttempt = 0;
    m_seqLocals.clear();
    m_seqSeen.clear();
    m_seqJobsThisAttempt = 0;
    m_seqTriplesThisAttempt = 0;
    m_seqClosed = false;
    for (auto& c : m_clones) freeClone(c);
    m_jobs.clear();
    m_log.clear();
    for (auto& h : m_hist) h.step = -1;
    m_step = 0;
    m_frame = 0.0;
    m_elapsed = 0.f;
    m_snapAhead = false;
    m_sim = nullptr;
    m_pendingJob = 0;
    m_realDeathStep = -1;
    m_realDeathFrame = 0.0;
    m_realDeathObj = -1;
    m_realDead = false;
    m_wouldBeDeathFrames.clear();
    m_deathMarks.clear();
    freeClone(m_shadow);
    freeClone(m_shadow2);
    m_pairLogged = false;
    m_shadowLogged = 0;
}

void CloneEngine::rangeFor(float x, size_t& a, size_t& b) const {
    a = std::lower_bound(m_actX.begin(), m_actX.end(), x - FLAG_RANGE) - m_actX.begin();
    b = std::upper_bound(m_actX.begin(), m_actX.end(), x + FLAG_RANGE) - m_actX.begin();
}

// The real world's activation flags around x in the LOGICAL form (activation.hpp packReal):
// bit0 = activated by the real player 1 (its own slot), bit1 = the other slot, then the two
// "activated" booleans. Only nonzero values are stored.
void CloneEngine::captureFlags(float x, std::vector<std::pair<EnhancedGameObject*, uint8_t>>& out) {
    out.clear();
    size_t a, b;
    rangeFor(x, a, b);
    for (size_t i = a; i < b; ++i) {
        uint8_t f = activation::packReal(physicalFlags(m_act[i]), m_realUsesSlot2);
        if (f) out.emplace_back(m_act[i], f);
    }
}

// v0.8.0: the trial's activation overlay. GD's gates for a stepped clone are answered from here
// by the hooks in CloneHooks.cpp (docs/LIVE_ISOLATION_DESIGN.md §2.3 H2-H4); the live objects'
// one-shot slots are never read or written for a clone.
uint8_t CloneEngine::overlayGet(EnhancedGameObject* o) const {
    if (!m_simClone) return 0;
    auto it = m_simClone->flags.find(o);
    return it == m_simClone->flags.end() ? 0 : it->second;
}

void CloneEngine::noteTripwire(char const* what) {
    ++m_tripwiresLevel;
    if (++m_tripwiresAttempt <= 6) {
        auto c = m_simClone;
        log::info("GPRL isolation: tripwire {} (clone {} at x={:.1f} step {}): the clone is invalid, nothing live was written",
                  what ? what : "?", c && c->obj ? c->obj->m_uniqueID : 0, c && c->obj ? c->obj->getPositionX() : 0.f, m_step);
    } else if (m_tripwiresAttempt == 7) {
        log::info("GPRL isolation: further tripwires this attempt are counted only (summary every 5 s)");
    }
}

void CloneEngine::notePortalModelled(int objectType) {
    ++m_portalModelLevel;
    if (++m_portalModelAttempt <= 3 && m_cfg.verbosity >= 2) {
        auto c = m_simClone;
        slog(2, fmt::format("GPRL isolation: portal model type {} on clone {} at x={:.1f} step {} (player part replayed, layer untouched)",
                            objectType, c && c->obj ? c->obj->m_uniqueID : 0, c && c->obj ? c->obj->getPositionX() : 0.f, m_step));
    }
}

// ---- v0.8.2 live-state invariant check (docs/LIVE_ISOLATION_DESIGN.md §3; owner spec
// LIVE_STATE_MUTATION_DETECTED). A LiveStateSnapshot (core/solver/live_state.hpp) is captured
// before and after every analysis block (or every clone step); GD runs nothing in between, so
// any difference was written by the analysis. Nothing is repaired: the samples are aborted, the
// open results end `live_mutation_detected`, and the breaker stops the analysis until the
// restart. The abort itself runs at the END of the block (isoFinishBreach): a breach found
// inside simStep must not drop jobs while advance() iterates them.

void CloneEngine::isoCapture(solver::live::Snapshot& out) { captureLive(m_pl, m_act, m_actX, m_step, out); }

bool CloneEngine::isoBegin(solver::live::Snapshot& before, double& t0, bool step) {
    if (!m_pl) return false;
    if (!step && !m_guard.beginBlock()) return false;
    t0 = nowMs();
    isoCapture(before);
    return true;
}

void CloneEngine::isoEnd(solver::live::Snapshot const& before, double t0, char const* block, Clone* trial) {
    isoCapture(m_isoAfter);
    int total = 0;
    auto diffs = solver::live::compare(before, m_isoAfter, solver::isolation::kIsolationConfig.maxLoggedFields, &total);
    double const us = (nowMs() - t0) * 1000.0;
    if (m_guard.noteCheck(us) && !m_isoFallbackLogged) {
        m_isoFallbackLogged = true;
        log::info("GPRL isolation: the invariant check costs {:.1f} us per block (limit {} us): checking every {} blocks from now on (never off)",
                  m_guard.costUs(), solver::isolation::kIsolationConfig.maxCheckUsPerStep, m_guard.sampleEvery());
    }
    if (!diffs.empty()) isoBreach(diffs, total, block, trial);
}

void CloneEngine::isoBreach(std::vector<solver::live::Diff> const& diffs, int total, char const* block, Clone* trial) {
    // 1. the owner's exact block for every differing field (at most maxLoggedFields, then the rest as a count)
    for (auto const& d : diffs) log::error("{}", solver::live::ownerBlock(d));
    if (total > static_cast<int>(diffs.size())) log::error("LIVE_STATE_MUTATION_DETECTED\n... {} more field(s) differ", total - static_cast<int>(diffs.size()));
    // 2. the stepped samples are aborted now (the trial in every-clone-step mode, every running clone in block mode)
    int aborted = 0;
    auto abortClone = [&](Clone& c) {
        if (c.state != CloneStatus::Running) return;
        c.state = CloneStatus::Invalid;
        c.invalidReason = "live_mutation_detected";
        ++aborted;
    };
    if (trial) abortClone(*trial);
    else {
        for (auto& c : m_clones) abortClone(c);
        abortClone(m_shadow);
        abortClone(m_shadow2);
    }
    auto const modeBefore = m_guard.mode();
    m_guard.onBreach();
    auto const& d0 = diffs.front();
    m_isoBreachLine = fmt::format(
        "GPRL isolation: LIVE_STATE_MUTATION_DETECTED field: {} before: {} after: {}{} | step {} frame {:.1f} | block: {}{} (mode {}) | action: {} clone(s) aborted, "
        "every open result of the attempt ends live_mutation_detected, isolation breaker on until the restart{}",
        d0.field, d0.before, d0.after, total > 1 ? fmt::format(" (+{} more)", total - 1) : std::string(), m_step, m_frame, block ? block : "?",
        trial && trial->obj ? fmt::format(" clone {}", trial->obj->m_uniqueID) : std::string(), solver::isolation::name(modeBefore), aborted,
        modeBefore == solver::isolation::CheckMode::EveryBlock ? "; the check runs per clone step for the rest of this level" : "");
    m_isoAbortPending = true;
}

void CloneEngine::isoFinishBreach() {
    if (!m_isoAbortPending) return;
    m_isoAbortPending = false;
    // results that finished BEFORE the breach keep their status (§3.3 item 3): the death pause's
    // pending misses are attributed with what resolved, then every open job / replay / result ends
    settleMisses(true);
    for (auto& job : m_jobs) {
        if (!job.dropReason.empty()) continue;
        job.mismatch = "live_mutation_detected";
        dropJob(job, "live_mutation");
    }
    m_jobs.clear();
    abortSequences("live_mutation");
    compAbortAll(solver::status::Reason::LiveMutationDetected);
    saAbortAll(solver::status::Reason::LiveMutationDetected);
    flushResults(solver::status::Reason::LiveMutationDetected, solver::status::Reason::LiveMutationDetected);
    log::error("{}", m_isoBreachLine);
}

void CloneEngine::tripIsolationBreaker(char const* who) {
    if (!m_pl || m_offForVisit) return;   // nothing runs and nothing is open (the reporter logs its own block)
    int aborted = 0;
    auto abortClone = [&](Clone& c) {
        if (c.state != CloneStatus::Running) return;
        c.state = CloneStatus::Invalid;
        c.invalidReason = "live_mutation_detected";
        ++aborted;
    };
    for (auto& c : m_clones) abortClone(c);
    abortClone(m_shadow);
    abortClone(m_shadow2);
    auto const modeBefore = m_guard.mode();
    m_guard.onBreach();
    m_isoBreachLine = fmt::format(
        "GPRL isolation: LIVE_STATE_MUTATION_DETECTED reported by {} | step {} frame {:.1f} | action: {} clone(s) aborted, every open result of the attempt ends "
        "live_mutation_detected, isolation breaker on until the restart{}",
        who ? who : "?", m_step, m_frame, aborted,
        modeBefore == solver::isolation::CheckMode::EveryBlock ? "; the check runs per clone step for the rest of this level" : "");
    m_isoAbortPending = true;
    // the analyzer reports from PlayLayer::postUpdate, never inside a clone block: finish now (a
    // call that ever arrived during a step would leave it to that block's own isoFinishBreach)
    if (!m_sim) isoFinishBreach();
}

void CloneEngine::overlaySet(EnhancedGameObject* o, uint8_t logical) {
    if (!m_simClone) return;
    if (logical) m_simClone->flags[o] = logical;
    else m_simClone->flags.erase(o);
}

bool CloneEngine::frameOf(int k, double& frame) const {
    if (k < 1 || m_hist.empty()) return false;
    auto const& h = m_hist[ring(k)];
    if (h.step != k) return false;
    frame = h.frame;
    return true;
}

uint32_t CloneEngine::geometryHashAt(float x, std::string* hex) const {
    if (hex) hex->clear();
    if (!m_pl || m_geo.empty()) return 0;
    GeometryHashParams params;
    size_t a = std::lower_bound(m_geoX.begin(), m_geoX.end(), x - static_cast<float>(params.rangeBefore)) - m_geoX.begin();
    size_t b = std::upper_bound(m_geoX.begin(), m_geoX.end(), x + static_cast<float>(params.rangeAfter)) - m_geoX.begin();
    std::vector<GeometryObject> objs;
    objs.reserve(b > a ? b - a : 0);
    for (size_t i = a; i < b; ++i) objs.push_back({m_geo[i]->m_objectID, static_cast<double>(m_geoX[i]), static_cast<double>(m_geo[i]->getPositionY()), false});
    auto h = geometryHash(objs, static_cast<double>(x), params);
    if (hex) *hex = h.hex;
    return h.low32;
}

void CloneEngine::playerSubUpdate(float dt) {
    if (m_offForVisit) return;
    m_elapsed += dt;
}

void CloneEngine::stepBegin(float dt, bool halfTick) {
    if (!m_pl || m_offForVisit) return;   // stopForVisit: no ring, no shadow, no clone step for the rest of the visit
    auto p1 = m_pl->m_player1;
    if (!p1) return;
    (void)dt;
    double t0 = nowMs();

    // The step that just ended: its real delta is the sum of the player updates it ran (0 when the
    // game skipped physics, e.g. after a death). A full tick's delta is the unit of one frame.
    if (m_step >= 1) {
        auto& prev = m_hist[ring(m_step)];
        if (prev.step == m_step) {
            prev.dt = m_elapsed;
            prev.dtKnown = true;
            prev.frames = solver::timeline::closedStepFrames(prev.halfTick, m_elapsed);
            m_frame += prev.frames;
        }
        if (m_elapsed > 1e-6f && m_elapsed < 0.5f) {
            m_stepDt = m_elapsed;
            if (prev.step == m_step && !prev.halfTick) m_tickDt = m_elapsed;
            else if (m_tickDt <= 0.f) m_tickDt = prev.halfTick ? m_elapsed * 2.f : m_elapsed;
        }
    }
    m_step++;
    m_elapsed = 0.f;

    // State at the start of this step, before any input of the step is applied. If an input arrived
    // between steps (bots firing after the physics update) the snapshot was already taken right before it.
    bool ahead = m_snapAhead;
    auto& h = m_hist[ring(m_step)];
    if (!(ahead && h.step == m_step)) {
        captureState(h.state, p1);
        captureFlags(p1->getPositionX(), h.flags);
        h.dual = m_pl->m_gameState.m_isDualMode && m_pl->m_player2 != nullptr;   // v0.8.3: the pair shadow's second half
        if (h.dual) captureState(h.state2, m_pl->m_player2);
    }
    h.step = m_step;
    h.dt = m_stepDt;
    h.dtKnown = false;
    h.halfTick = halfTick;
    h.frames = halfTick ? 0.5 : 1.0;
    h.frame = m_frame;
    h.totalTime = p1->m_totalTime;
    h.levelTime = m_pl->m_gameState.m_levelTime;
    m_snapAhead = false;
    // v0.7.1 (Fable D7): the real player's speed changes (consecutive ring snapshots run different
    // speeds). The clones mirror the speed by TIME, so a shifted trial whose span crosses one ran
    // the wrong speed for |shift| ticks: its input's timing_result says speed_change_in_lookahead
    if (m_step >= 2) {
        auto const& prevSnap = m_hist[ring(m_step - 1)];
        if (prevSnap.step == m_step - 1 && prevSnap.state.f.m_playerSpeed != h.state.f.m_playerSpeed) {
            pushMark(m_speedMarks, {h.frame, h.state.f.m_playerSpeed}, h.frame);
            ++m_counters.speedChanges;
            ++m_attempt.speedChanges;
            // verbose: tells a real portal (once per pass) from a speed that toggles between snapshots
            slog(2, fmt::format("GPRL solver: real speed change at step {} frame {:.1f} x={:.1f}: {:.2f} -> {:.2f}", m_step, h.frame, h.state.pos.x,
                                prevSnap.state.f.m_playerSpeed, h.state.f.m_playerSpeed));
        }
    }

    dropUnbound();
    // v0.8.0 isolation (docs/LIVE_ISOLATION_DESIGN.md §2.8, O6): GJBaseGameLayer::checkCollisions
    // reads the REAL player 1 inline and removes a practice checkpoint when it is dead with a
    // pending checkpoint timeout; the first checkCollisions of a death pause must be the real
    // player's, never a clone's. No clone steps until the timeout clears.
    if (p1->m_isDead && p1->m_checkpointTimeout) {
        if (!m_checkpointPauseLogged) {
            m_checkpointPauseLogged = true;
            log::info("GPRL isolation: paused - checkpoint timeout pending (no clone steps while the real player's death pause owns checkCollisions)");
        }
        double spent0 = nowMs() - t0;
        m_simMs += spent0;
        m_simFrameMs += spent0;
        return;
    }
    m_checkpointPauseLogged = false;
    // v0.8.2 (docs/LIVE_ISOLATION_DESIGN.md §3.3 isolation breaker): after a live-state breach nothing
    // of the analysis runs again this attempt - no shadow, no jobs, no sequence replays
    if (m_guard.breached()) {
        double spentB = nowMs() - t0;
        m_simMs += spentB;
        m_simFrameMs += spentB;
        return;
    }
    // the analysis block (§3.2): captured before and after when the guard checks blocks
    double isoT0 = 0.0;
    bool const iso = isoBegin(m_isoBefore, isoT0);
    if (m_measuring) {
        // v0.8.3 (§2.7): in a dual section the PAIR shadow steps (or nothing, setting solver-dual off);
        // the single shadow resyncs itself when the section ends
        auto const& hs = m_hist[ring(m_step - 1)];
        bool const dualStep = hs.step == m_step - 1 && hs.dual;
        if (!dualStep) shadowStep(m_step - 1, !ahead);
        else if (m_cfg.dualShadow) pairShadowStep(m_step - 1, !ahead);
    }
    if (!m_jobs.empty()) runJobs(m_step - 1, !ahead);
    // v0.14.0: the lockstep compensation jobs of connected-control inputs (docs/SHIP_SOLVER.md §4.3)
    if (!m_compJobs.empty()) runCompensation(m_step - 1);
    // sequence jobs last: they only get what the local jobs left of the frame's step budget
    // (v0.7.0: sequence-adjusted jobs first, M4 joint-share jobs only while no SA candidate waits)
    if (m_seqActive || !m_seqQueue.empty() || !m_saCands.empty()) runSequence(m_step - 1);
    if (iso) isoEnd(m_isoBefore, isoT0, "step", nullptr);
    isoFinishBreach();
    double spent = nowMs() - t0;
    m_simMs += spent;
    m_simFrameMs += spent;
}

void CloneEngine::dropUnbound() {
    if (!m_pendingJob) return;
    Job* j = findJob(m_pendingJob);
    m_pendingJob = 0;
    if (!j || j->bound) return;
    // The input never came through GJBaseGameLayer::handleButton (a bot calling the player
    // directly, or no open attempt): there is no telemetry input event to attach a window to.
    dropJob(*j, "unbound");
    std::erase_if(m_jobs, [&](Job const& x) { return x.id == j->id; });
}

Job* CloneEngine::findJob(int id) {
    for (auto& j : m_jobs) if (j.id == id) return &j;
    return nullptr;
}

// A later player-1 jump input was logged at `frame`: it bounds the late side of every open job
// (LocalWindowSolver's same-channel neighbour rule: a shifted press must never cross the release
// that followed it). An input in the SAME step as the job's input (a tap shorter than one tick
// without CBF: press and release both at the step start) is a neighbour at distance 0, so the late
// side is blocked - it is not skipped. The limit is never negative and never touches the control.
void CloneEngine::applyLateNeighbour(double frame, bool down) {
    for (auto& job : m_jobs) {
        if (frame < job.t - 1e-9) continue;   // logged before this job's input: not a later neighbour
        if (std::isnan(job.nextGapFrames)) job.nextGapFrames = frame - job.t;
        if (job.down && !down && std::isnan(job.releaseFrame)) job.releaseFrame = frame;
        double lim = std::max(0.0, frame - job.t - job.planner.config().neighbourMarginFrames);
        if (!std::isnan(job.lateLimitFrames) && lim >= job.lateLimitFrames) continue;
        job.lateLimitFrames = lim;
        job.planner.setLateLimit(lim);
        for (int idx : job.clones) {
            auto& c = m_clones[idx];
            if (c.state == CloneStatus::Running && c.shift > 0.0 && c.shift > lim + 1e-9) c.state = CloneStatus::Limit;
        }
    }
}

int CloneEngine::input(bool down, PlayerStateSnapshot pre, bool ceilingTouch, double percent) {
    m_lastInputLogged = false;
    if (!m_pl || m_sim || m_offForVisit) return 0;
    if (m_step < 1) return 0;
    auto p1 = m_pl->m_player1;
    if (!p1) return 0;
    double now = m_pl->m_gameState.m_levelTime;

    auto& h = m_hist[ring(m_step)];
    auto place = solver::timeline::placeInput(m_step, h.frame, h.frames, h.halfTick, m_tickDt, m_stepDt, m_elapsed);
    int step = place.step;
    double t = place.t;
    ++m_counters.inputs;
    ++m_attempt.inputs;
    uint32_t id = m_nextInputId++;   // every input gets its own number in the log, logged or not

    auto skip = [&](char const* why, int& counter) {
        ++m_counters.skipped;
        ++m_attempt.skipped;
        ++counter;
        slog(1, fmt::format("GPRL solver: input #{} {} skipped: {}", id, down ? "press" : "release", why));
        return 0;
    };
    // Inputs the real player cannot act on are NOT logged (FPC Engine::input returns before its log
    // too): a press mashed during the death pause would otherwise be replayed into the clones that
    // finish a miss with synthetic ticks, and would set a late limit on every open job.
    if (m_platformer) return skip("platformer level", m_counters.skippedOther);
    if (!m_pl->m_started || m_pl->m_hasCompletedLevel) return skip("level not running", m_counters.skippedDead);
    if (p1->m_isDead || m_realDead) return skip("player dead", m_counters.skippedDead);

    if (place.betweenSteps && !m_snapAhead) {
        // The input is applied before the next step starts: park the pre-input state as that step's snapshot.
        auto& n = m_hist[ring(step)];
        n.step = step;
        n.dt = m_stepDt;
        n.dtKnown = false;
        n.halfTick = false;
        n.frames = 1.0;
        n.frame = t;
        n.totalTime = p1->m_totalTime;
        n.levelTime = now;
        captureState(n.state, p1);
        captureFlags(p1->getPositionX(), n.flags);
        n.dual = m_pl->m_gameState.m_isDualMode && m_pl->m_player2 != nullptr;
        if (n.dual) captureState(n.state2, m_pl->m_player2);
        m_snapAhead = true;
    }
    double prevGap = m_log.empty() ? solver::kNaN : t - m_log.back().t;
    double pressFrame = solver::kNaN, pressLevelTime = solver::kNaN;
    if (!down) {
        for (auto it = m_log.rbegin(); it != m_log.rend(); ++it) {
            if (it->down) { pressFrame = it->t; pressLevelTime = it->levelTime; break; }
        }
    }
    // Every real input from here on is logged, measured or not: the shadow and the other jobs'
    // clones replay it, and it bounds the neighbouring jobs' windows.
    InputEvent logged;
    logged.t = t;
    logged.down = down;
    logged.id = id;
    logged.levelTime = now;
    m_log.push_back(logged);
    m_lastInputLogged = true;
    int const attemptIndex = static_cast<int>(m_log.size());   // "Input #47 of this attempt" (1-based)
    applyLateNeighbour(t, down);
    compNoteInput(logged);   // v0.14.0: a later input of every open compensation job
    // v0.7.0 input placement (§6 item 6): without CBF GD applies inputs at tick boundaries; a
    // half-tick input without CBF is low_confidence until the lattice includes half ticks (F4)
    double const frac = t - std::floor(t);
    bool const halfTick = !m_cfg.subtick && std::fabs(frac - 0.5) < 1e-6;
    if (frac < 1e-6 || frac > 1.0 - 1e-6) { ++m_counters.placeWhole; ++m_attempt.placeWhole; }
    else if (std::fabs(frac - 0.5) < 1e-6) { ++m_counters.placeHalf; ++m_attempt.placeHalf; }
    else { ++m_counters.placeSub; ++m_attempt.placeSub; }

    if (!m_ringOk) return skip("snapshot ring failed its self-test", m_counters.skippedOther);
    if (!m_measuring) return skip(m_pauseReason.empty() ? "paused" : m_pauseReason.c_str(), m_counters.skippedPaused);
    // v0.8.2 (§3.3 item 3): the live run was changed by the analysis this attempt - no new sample
    if (m_guard.breached()) {
        ++m_attempt.skippedIsolation;
        return skip("isolation breached this attempt", m_counters.skippedIsolation);
    }
    if (m_replayBroken) {
        ++m_attempt.skippedReplay;
        return skip("replay broken this attempt (the shadow keeps mismatching)", m_counters.skippedReplay);
    }
    if (m_pl->m_gameState.m_isDualMode) return skip(m_cfg.dualShadow ? "dual mode (pair shadow only, not measured yet)" : "dual mode (solver-dual off)", m_counters.skippedDual);
    if (m_throttled) return skip("throttled (simulation load)", m_counters.skippedThrottle);
    if (m_frameSlow) return skip("slow frame (dt > 1/30 s)", m_counters.skippedFrame);
    if (static_cast<int>(m_jobs.size()) >= kMaxJobs) return skip("too many open measurements", m_counters.skippedJobs);

    Job j;
    j.id = m_nextJobId++;
    j.inputId = id;
    j.attemptInputIndex = attemptIndex;
    j.percent = percent;
    j.halfTick = halfTick;
    j.engineSubTickMs = solver::units::engineSubTickMs(t);   // v0.7.1 (Fable D11): the shifts' own origin
    m_log.back().jobId = j.id;
    j.t = t;
    j.step = step;
    j.levelTime = now;
    j.down = down;
    j.subtick = m_cfg.subtick;
    j.connected = m_saOn && connectedModeOf(p1);   // v0.14.0 (docs/SHIP_SOLVER.md §4.3)
    j.horizonFrame = solver::timeline::horizonFrame(t, m_cfg.horizonSeconds, m_cfg.maxShiftTicks);
    j.pre = pre;
    j.ceilingTouch = ceilingTouch;
    j.prevGapFrames = prevGap;
    j.pressFrame = pressFrame;
    j.pressLevelTime = pressLevelTime;
    j.createdMs = nowMs();
    std::string hex;
    uint32_t low = geometryHashAt(p1->getPositionX(), &hex);
    j.geometryHash = hex;
    j.pre.geometryHash = low;
    // early limit: the oldest snapshot still in the ring and the previous same-channel input
    double oldest = solver::kNaN;
    int oldestStep = std::max(1, m_step - kHistorySteps + 2);
    double f = 0.0;
    if (frameOf(oldestStep, f)) oldest = t - f;
    solver::PlannerConfig pc;
    pc.maxShiftTicks = m_cfg.maxShiftTicks;
    pc.subtick = m_cfg.subtick;
    pc.resolutionFrames = m_cfg.resolutionFrames;
    pc.maxRefinePasses = m_cfg.maxRefinePasses;
    if (!pc.subtick) {
        auto off = solver::timeline::gridOffsets(t);
        pc.lateOffsetFrames = off.late;
        pc.earlyOffsetFrames = off.early;
    }
    double earlyLimit = oldest;
    // v0.7.0 edge stops (§2.4): which limit the early side runs into - the previous input, the
    // attempt start (the ring still holds step 1) or the history ring
    j.earlyLimitKind = oldestStep <= 1 ? solver::LimitKind::AttemptStart : solver::LimitKind::History;
    if (!std::isnan(prevGap)) {
        double lim = std::max(0.0, prevGap - pc.neighbourMarginFrames);
        if (std::isnan(earlyLimit) || lim <= earlyLimit) j.earlyLimitKind = solver::LimitKind::Neighbour;
        earlyLimit = std::isnan(earlyLimit) ? lim : std::min(earlyLimit, lim);
    }
    j.earlyLimitFrames = earlyLimit;
    j.planner = solver::PassPlanner(pc, earlyLimit);
    j.planner.setEarlyLimitKind(j.earlyLimitKind);
    m_jobs.push_back(std::move(j));
    m_pendingJob = m_jobs.back().id;
    ++m_counters.jobs;
    ++m_attempt.jobs;
    return m_pendingJob;
}

void CloneEngine::bindJob(int jobId, int64_t seq, double eventT, int64_t tick, std::string attemptId, bool practice, double tSubTick) {
    Job* j = findJob(jobId);
    if (!j) return;
    j->bound = true;
    j->inputSeq = seq;
    j->eventT = eventT;
    j->tick = tick;
    j->attemptId = std::move(attemptId);
    j->practice = practice;
    j->subTickMs = std::isfinite(tSubTick) ? solver::units::subTickMs(std::clamp(tSubTick, 0.0, 0.999999)) : 0.0;
    // v0.7.1 (Fable D11): the tracker's clock vs the engine's for the same input (5 s summary)
    m_subTickClockMaxMs = std::max(m_subTickClockMaxMs, std::fabs(j->engineSubTickMs - j->subTickMs));
    if (auto* le = logEntry(j->inputId)) le->seq = seq;
    if (m_pendingJob == jobId) m_pendingJob = 0;
    resultOpen(*j);   // v0.7.0: a bound job owes exactly one timing_result from here on
    slog(2, fmt::format("GPRL solver: input #{} {} t={:.4f} tick={} frame={:.2f} step={} x={:.0f}: job {} (seq {}, early limit {}, late pending)",
        j->inputId, j->down ? "press" : "release", eventT, tick, j->t, j->step, m_pl && m_pl->m_player1 ? m_pl->m_player1->getPositionX() : 0.f, j->id, seq,
        std::isnan(j->earlyLimitFrames) ? std::string("max") : fmt::format("{:.2f}", j->earlyLimitFrames)));
}

void CloneEngine::cancelJob(int jobId, char const* why) {
    Job* j = findJob(jobId);
    if (!j) return;
    if (m_pendingJob == jobId) m_pendingJob = 0;
    dropJob(*j, why);
    std::erase_if(m_jobs, [&](Job const& x) { return x.id == jobId; });
}

void CloneEngine::onRealPortal(int objectId) {
    // v0.7.0: portal frames are cluster breaks (gamemode / gravity / size / dual / teleport) and
    // make an input 2 ticks before them a transition input (§2.7, §2.10)
    if (m_offForVisit) return;
    pushMark(m_portalMarks, {m_frame, objectId}, m_frame);   // pruned by age (kMarkKeepFrames)
    for (auto& job : m_jobs) {
        if (job.firstPortalId == 0 && m_frame >= job.t - 1e-9 && m_frame <= job.horizonFrame) job.firstPortalId = objectId;
    }
}

void CloneEngine::onRealDeath(GameObject* by, bool wouldBe) {
    if (!m_pl || m_offForVisit || m_step < 1) return;
    auto& h = m_hist[ring(m_step)];
    m_realDeathStep = m_step;
    m_realDeathFrame = h.step == m_step ? h.frame + h.frames : m_frame;
    m_realDeathObj = by ? by->m_objectID : -1;
    // v0.7.1 (Fable D6): the real player's x at the death - two object -1 deaths match only at the same place
    m_deathMarks.push_back({m_realDeathFrame, m_realDeathObj, m_pl->m_player1 ? m_pl->m_player1->getPositionX() : 0.f});
    if (m_deathMarks.size() > 64) m_deathMarks.erase(m_deathMarks.begin());
    if (wouldBe) m_wouldBeDeathFrames.push_back(m_realDeathFrame);
    else m_realDead = true;
    slog(2, fmt::format("GPRL solver: real player {} at step {} frame {:.1f} on #{} x={:.1f}{}", wouldBe ? "would-be death" : "death", m_step, m_realDeathFrame,
        m_realDeathObj, m_deathMarks.back().x, m_jobs.empty() ? "" : fmt::format(", {} open job(s) continue in the death pause", m_jobs.size())));
}

// ---- jobs ----

void CloneEngine::runJobs(int target, bool compareControl) {
    // oldest first: the job order is the creation order, so pending spawns and the step budget
    // (advance) go to the oldest measurement before a younger one
    for (auto& j : m_jobs) {
        if (!j.spawned || !j.pendingShifts.empty()) trySpawn(j);
    }
    advance(target, compareControl);
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (!it->dropReason.empty()) {
            it = m_jobs.erase(it);
            continue;
        }
        if (jobResolved(*it)) {
            finalizePass(*it);
            if (!it->dropReason.empty() || it->planner.done()) it = m_jobs.erase(it);
            else ++it;
        }
        else ++it;
    }
    if (!m_pendingMisses.empty()) settleMisses(false);   // v0.7.1 (Fable D6): a would-be death under noclip
}

// A smaller shift that died makes larger shifts on that side pointless beyond the island scan
// (BoundarySearch); a pending shift in that region is not worth a clone.
bool CloneEngine::prunedByDeath(Job const& job, double s) const {
    if (job.pass != 0 || s == 0.0) return false;
    auto const& pc = job.planner.config();
    double scan = static_cast<double>(pc.islandScanSteps * pc.coarseStepTicks);
    if (pc.pruneNearShifts) {
        // v0.6.x rule (comparison runs only): in a MISS it cancelled the passing run (RC3.1)
        for (int idx : job.clones) {
            auto const& c = m_clones[idx];
            if (c.state != CloneStatus::Dead) continue;
            if (c.shift > 0 && s > 0 && s > c.shift + scan + 1e-9) return true;
            if (c.shift < 0 && s < 0 && -s > -c.shift + scan + 1e-9) return true;
        }
        return false;
    }
    if (!pc.pruneBeyondHitFail) return false;
    // v0.7.1 (Fable D5): only a side whose nearest tested shift PASSED is pruned
    double lim = hitPruneLimitOf(job, s > 0.0);
    return !std::isnan(lim) && std::fabs(s) > lim + 1e-9;
}

double CloneEngine::hitPruneLimitOf(Job const& job, bool late) const {
    // probes: pass-0 clones on the side (Alive = passed, Dead = died, Running = unknown, Invalid =
    // other) and the side's shifts still waiting for a clone (unknown); Cancelled / Limit / Idle
    // clones were never tested. Bounded by the job's clones (<= 21) + pending shifts.
    std::vector<solver::PruneProbe> probes;
    for (int idx : job.clones) {
        auto const& c = m_clones[idx];
        if (c.shift == 0.0 || (c.shift > 0.0) != late) continue;
        solver::PruneProbe p;
        p.mag = std::fabs(c.shift);
        switch (c.state) {
            case CloneStatus::Alive: p.state = solver::ProbeState::Passed; break;
            case CloneStatus::Dead: p.state = solver::ProbeState::Died; break;
            case CloneStatus::Running: p.state = solver::ProbeState::Running; break;
            case CloneStatus::Invalid: p.state = solver::ProbeState::Other; break;
            default: continue;
        }
        probes.push_back(p);
    }
    for (double s : job.pendingShifts) {
        if (s == 0.0 || (s > 0.0) != late) continue;
        probes.push_back({std::fabs(s), solver::ProbeState::Running});
    }
    auto const& pc = job.planner.config();
    return solver::hitPruneLimit(probes, static_cast<double>(pc.islandScanSteps * pc.coarseStepTicks));
}

// One shift of the current pass: 1 = a clone was spawned from the ring snapshot, 0 = no clone is
// available right now (the shift stays pending), -1 = reported NotTested (no history for it).
int CloneEngine::spawnShift(Job& job, double s, bool isControl) {
    double target = job.t + s;
    int k0 = job.step;
    if (s < 0) k0 = solver::timeline::stepForFrame(job.step, target, [this](int k, double& f) { return frameOf(k, f); });
    if (k0 < 1) {
        if (s < 0) job.historyMissingEarly = true;
        job.untested.push_back({s, "no history"});
        return -1;
    }
    auto& h = m_hist[ring(k0)];
    if (h.step != k0) {
        job.untested.push_back({s, "history lost"});
        return -1;
    }
    int idx = allocClone(isControl);
    if (idx < 0) return 0;
    auto& c = m_clones[idx];
    applyState(c.obj, h.state);
    c.flags.clear();
    for (auto& [o, f] : h.flags) c.flags[o] = f;
    c.state = CloneStatus::Running;
    c.job = job.id;
    c.pass = job.pass;
    c.shift = s;
    c.inputFrame = target;
    c.inputPending = true;
    c.appliedFrame = solver::kNaN;
    c.stepDone = k0 - 1;
    c.frameDone = h.frame;
    c.deathStep = -1;
    c.deathObjId = -1;
    c.converged = 0;
    c.resynced = false;
    c.extension = false;
    // v0.7.0 attribution (§2.3): later logged inputs this trial keeps fixed and applies before a death
    c.attributeAfterFrame = job.t;
    c.laterFixed = 0;
    c.rejoinStartFrame = solver::kNaN;
    c.tracing = m_cfg.traceMaxTicks > 0 && job.pass == 0;
    c.trace.shiftFrames = s;
    if (isControl) ++job.controlSims;
    job.clones.push_back(idx);
    if (m_step > job.spawnStep) ++m_counters.deferredSpawns;
    return 1;
}

// The pass's shifts are spawned as clones become available (docs/SOLVER_DESIGN.md §12): the
// control at once (never rationed), then the shifts closest first (later / earlier alternating,
// FPC order). What the pool cannot serve this step stays pending and is spawned on the following
// steps from the same ring snapshots (the clone catches up, kCatchUpStepsPerStep); at the spawn
// deadline the rest is reported NotTested (pool). Shifts a late neighbour excluded or a nearer
// death already made pointless never get a clone.
void CloneEngine::trySpawn(Job& job) {
    if (!job.dropReason.empty()) return;
    if (job.step > m_step) return;
    if (!job.bound) return;   // the seq linkage arrives from handleButton right after pushButton
    if (!job.spawned) {
        job.passShifts = job.planner.nextPass();
        if (job.passShifts.empty()) {
            if (!job.planner.ingested()) {
                // pass 0 has nothing to test: both sides blocked (no history, neighbours on both sides)
                dropJob(job, "blocked");
                job.spawned = true;
                return;
            }
            if (job.planner.done()) {
                job.planner.finish();
                finalize(job);
                job.spawned = true;
                return;
            }
            return;   // the planner is waiting (should not happen: the pass was ingested)
        }
        double hf = 0.0;
        if (!frameOf(job.step, hf)) {
            dropJob(job, "history lost");
            job.spawned = true;
            return;
        }
        job.controlDiedWithReal = false;
        job.spawnComplete = true;
        job.untested.clear();
        job.pendingShifts.clear();
        job.pendingShifts.push_back(0.0);   // control first
        job.pendingShifts.insert(job.pendingShifts.end(), job.passShifts.begin(), job.passShifts.end());
        job.spawnStep = m_step;
        job.spawned = true;
    }
    bool first = m_step == job.spawnStep;
    bool deadline = m_step - job.spawnStep > kBudget.spawnDeadlineSteps;
    int spawnedNow = 0;
    size_t i = 0;
    while (i < job.pendingShifts.size()) {
        double s = job.pendingShifts[i];
        bool isControl = s == 0.0;
        if (!isControl && !std::isnan(job.lateLimitFrames) && s > job.lateLimitFrames + 1e-9) {
            job.untested.push_back({s, "limit"});
            job.pendingShifts.erase(job.pendingShifts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (prunedByDeath(job, s)) {
            job.untested.push_back({s, "cancelled"});
            job.pendingShifts.erase(job.pendingShifts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (deadline) {
            job.untested.push_back({s, "pool"});
            job.spawnComplete = false;
            ++m_counters.deferredExpired;
            job.pendingShifts.erase(job.pendingShifts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        int r = spawnShift(job, s, isControl);
        if (r == 0) break;   // no clone right now: this and the rest wait, in order
        if (r > 0) ++spawnedNow;
        job.pendingShifts.erase(job.pendingShifts.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (first) {
        if (job.pass == 0) {
            slog(2, fmt::format("GPRL solver: job {} pass 0 spawned {} clones ({} shifts, {} untested, {} pending{})", job.id, spawnedNow, job.passShifts.size(),
                job.untested.size(), job.pendingShifts.size(), job.historyMissingEarly ? ", early history missing" : ""));
        }
        else {
            auto const& e = job.planner.early();
            auto const& l = job.planner.late();
            slog(2, fmt::format("GPRL solver: job {} pass {} ({:g} tick) spawned {} clones: early [{:+.3f},{:+.3f}] late [{:+.3f},{:+.3f}]{}", job.id, job.pass,
                m_cfg.resolutionFrames, spawnedNow, e.bounded() ? e.failEdge : e.passEdge, e.passEdge, l.passEdge, l.bounded() ? l.failEdge : l.passEdge,
                job.pendingShifts.empty() ? "" : fmt::format(", {} pending", job.pendingShifts.size())));
        }
    }
    else if (spawnedNow > 0 || deadline) {
        slog(2, fmt::format("GPRL solver: job {} pass {} deferred spawn: {} clones {} steps late{}", job.id, job.pass, spawnedNow, m_step - job.spawnStep,
            job.pendingShifts.empty() ? (deadline ? " (deadline: the rest untested)" : " (complete)") : fmt::format(", {} still pending", job.pendingShifts.size())));
    }
}

// One physics step for a clone, in the exact order GJBaseGameLayer::update uses for the real
// player (GD 2.2081): queued inputs, resetTouchedRings, collision log reset, internal actions,
// PlayerObject::update, checkCollisions (this is where deaths happen), updateRotation, and the
// per-step position / rotation bookkeeping. With Click Between Frames the update is split around the
// input like the mod does it. Returns the number of inputs applied, -1 when no physics ran.
int CloneEngine::simStep(Clone& c, Job const& job, int k) {
    auto p = c.obj;
    auto& h = m_hist[ring(k)];
    bool haveH = h.step == k;
    if (!haveH && k <= m_step) {
        // the snapshot of this step was overwritten: the clone lagged more than the ring holds
        c.state = CloneStatus::Invalid;
        c.invalidReason = "history lost";
        c.stepDone = k;
        return -1;
    }
    bool synthetic = !haveH;   // beyond the real timeline (death pause): whole ticks in a frozen world
    float dt = haveH ? (h.dtKnown ? h.dt : m_stepDt) : (m_tickDt > 0.f ? m_tickDt : m_stepDt);
    double f0 = haveH ? h.frame : c.frameDone;
    double fr = haveH ? h.frames : 1.0;
    if (fr <= 0.0) {
        // Physics did not run for the real player in this step (death pause): nothing to simulate.
        c.stepDone = k;
        return -1;
    }

    // Inputs on the same instant keep the order they were made in (their input ids).
    // v0.7.0: a logged input replayed at its RECORDED frame is `fixed`; the measured input and the
    // moved copies are not (their `frame` is where this trial applies them).
    struct Ev { double frac; bool down; uint32_t order; bool fixed; double frame; };
    Ev evs[16];
    int n = 0;
    {
        auto it = std::lower_bound(m_log.begin(), m_log.end(), f0 - 1e-9,
            [](InputEvent const& e, double v) { return e.t < v; });
        for (; it != m_log.end() && it->t < f0 + fr - 1e-9 && n < 15; ++it) {
            if (it->id == job.inputId) continue;
            // sequence trial (v0.6.1): every input of the group is replaced by its shifted copy
            // below; a local job's clone has no moved inputs and takes the path above unchanged
            if (!c.moved.empty()) {
                bool replaced = false;
                for (auto const& m : c.moved) replaced = replaced || m.id == it->id;
                if (replaced) continue;
            }
            evs[n++] = {std::clamp((it->t - f0) / fr, 0.0, 1.0), it->down, it->id, true, it->t};
        }
    }
    if (c.inputPending && n < 16) {
        auto a = solver::timeline::applyInStep(c.inputFrame, f0, fr, job.subtick);
        if (a.now) {
            evs[n++] = {a.frac, job.down, job.inputId, false, c.inputFrame};
            c.inputPending = false;
        }
    }
    for (auto& m : c.moved) {
        if (!m.pending || n >= 16) continue;
        auto a = solver::timeline::applyInStep(m.frame, f0, fr, job.subtick);
        if (a.now) {
            evs[n++] = {a.frac, m.down, m.id, false, m.frame};
            m.pending = false;
        }
    }
    std::stable_sort(evs, evs + n, [](Ev const& a, Ev const& b) { return a.frac < b.frac || (a.frac == b.frac && a.order < b.order); });

    // v0.8.0 isolation: nothing live is written for a clone's step. The overlay (c.flags) answers
    // GD's activation gates through the hooks, and the step's recorded half-tick flag reaches
    // PlayerObject::postCollision through its hook instead of the layer's m_isBetweenSteps.
    double isoT0 = 0.0;
    bool const isoStep = m_guard.checksEveryStep() && isoBegin(m_isoStepBefore, isoT0, true);
    simBegin(c, haveH ? h.halfTick : false, nullptr);
    if (haveH) p->m_totalTime = h.totalTime;
    // 0. speed: the real players get their speed from the layer at the start of every sub-step
    //    (GJBaseGameLayer::update -> PlayerObject::updateTimeMod from the queued
    //    m_gameState.m_timeModRelated, fed by the level's speed objects, never by a collision), so
    //    a clone never sees a speed portal. The ring snapshot of this step holds the speed the
    //    real player ran it with: mirror it (updateTimeMod also sets m_yStart / m_gravity).
    if (haveH && p->m_playerSpeed != h.state.f.m_playerSpeed) p->updateTimeMod(h.state.f.m_playerSpeed, true);

    float elapsed = 0.f;
    auto apply = [&](Ev const& ev) {
        if (ev.order == job.inputId) c.appliedFrame = f0 + static_cast<double>(elapsed) * fr;
        for (auto& m : c.moved) {
            if (m.id == ev.order) m.applied = f0 + static_cast<double>(elapsed) * fr;
        }
        // v0.7.0 death attribution (§2.3): the engine knows exactly what it applied - a later
        // input it kept fixed and applied before the death makes that death "downstream"
        if (ev.fixed && ev.frame > c.attributeAfterFrame + 1e-9) ++c.laterFixed;
        if (ev.down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    };

    // 1. inputs queued for the start of the step
    int i = 0;
    for (; i < n && evs[i].frac <= 1e-6; ++i) apply(evs[i]);
    // 2. per-step player resets
    gameStepReset(p);
    if (p->m_actionManager) p->updateInternalActions(dt);
    // 3. movement (split around mid-step inputs like Click Between Frames)
    CCPoint startPos = p->getPosition();
    bool startedOnGround = p->m_isOnGround;
    bool canSplit = job.subtick && notBuffering(p);
    bool split = false;
    float lastSub = dt;
    for (; i < n; ++i) {
        auto& ev = evs[i];
        if (canSplit && ev.frac > elapsed + 1e-6 && ev.frac < 1.0 - 1e-6) {
            float sub = dt * static_cast<float>(ev.frac - elapsed);
            p->update(sub);
            if (!split && ((p->m_yVelocity < 0) != p->m_isUpsideDown)) p->m_isOnGround = startedOnGround;
            if (!p->m_isOnSlope || p->m_isDart) m_pl->checkCollisions(p, 0.f, true);
            else m_pl->checkCollisions(p, dt, true);
            p->updateRotation(sub);
            resetLog(p);
            elapsed = static_cast<float>(ev.frac);
            split = true;
            if (c.state != CloneStatus::Running) break;
        }
        apply(ev);
    }
    if (c.state == CloneStatus::Running) {
        float rest = dt * (1.f - elapsed);
        lastSub = split ? rest : dt;
        if (rest > 0.f) p->update(rest);
    }
    // 4. collisions: solids, hazards, orbs, portals; deaths arrive through the destroyPlayer hook
    if (c.state == CloneStatus::Running) m_pl->checkCollisions(p, dt, false);
    // 5. rotation and the end-of-step bookkeeping the game loop does
    if (c.state == CloneStatus::Running) {
        p->m_unkUnused3 = p->getRotation();
        p->updateRotation(lastSub);
        if (split) p->m_lastPosition = startPos;
        p->m_shipRotation = p->getPosition();
    }
    m_sim = nullptr;
    m_simClone = nullptr;
    m_simPartner = nullptr;
    m_simSelfBit = activation::kSelf;
    if (isoStep) isoEnd(m_isoStepBefore, isoT0, "clone step", &c);   // v0.8.2: pinpoints the trial
    c.stepDone = k;
    c.frameDone = f0 + fr;
    if (synthetic) c.extension = true;
    // v0.11.0 settled look-ahead (core/solver/settle.hpp): is the copy at rest after its input?
    if (m_cfg.settle.enabled && c.state == CloneStatus::Running) {
        solver::settle::StepFacts sf;
        sf.flyingMode = p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
        sf.onGround = p->m_isOnGround;
        sf.dashing = p->m_isDashing;
        sf.touchingRing = p->m_touchingRings && p->m_touchingRings->count() > 0;
        sf.frameDone = c.frameDone;
        sf.lastMovedFrame = c.inputFrame;
        for (auto const& m : c.moved) sf.lastMovedFrame = std::max(sf.lastMovedFrame, std::isnan(m.applied) ? m.frame : m.applied);
        solver::settle::update(c.settle, sf, m_cfg.settle);
    }
    if (c.state == CloneStatus::Dead) {
        c.deathStep = k;
        c.deathFrame = f0 + fr;
        c.deathX = p->getPositionX();
    }
    if (c.tracing) traceRecord(c);
    m_cloneSteps++;
    m_cloneStepsThisFrame++;
    return n;
}

void CloneEngine::advance(int target, bool compareControl) {
    auto p1 = m_pl->m_player1;
    for (auto& job : m_jobs) {
        if (!job.spawned || !job.dropReason.empty()) continue;
        Clone* control = nullptr;
        for (int idx : job.clones) {
            if (m_clones[idx].shift == 0.0) control = &m_clones[idx];
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (int idx : job.clones) {
                auto& c = m_clones[idx];
                bool isControl = (&c == control);
                if ((pass == 0) != isControl) continue;
                // A clone far behind the real timeline (a refinement pass spawned from snapshots
                // ~0.5 s old, or a clone the per-frame cap left behind) catches up a few steps per
                // real step instead of in one burst: the ring keeps its snapshots for kHistorySteps.
                int budget = solver::budget::catchUpBudget(target - c.stepDone);
                while (c.state == CloneStatus::Running && c.stepDone < target && budget > 0 && m_cloneStepsThisFrame < m_stepBudget) {
                    if (simStep(c, job, c.stepDone + 1) >= 0) --budget;
                }
            }
        }
        // v0.6.x: a shift that died made every larger shift on that side pointless beyond the island
        // scan (Cancelled, never reported as tested). v0.7.0 (V2-D8, RC3.1): OFF - in a MISS the
        // passing run lies away from 0 and alive clones next to it were cancelled; the path is kept
        // behind PlannerConfig::pruneNearShifts for comparison runs. v0.7.1 (Fable D5): back for
        // HIT sides only - a side whose nearest tested shift passed (hitPruneLimitOf) loses the
        // running clones beyond its first fail + the island scan; a side whose nearest shift died
        // or is still running is never pruned.
        if (job.pass == 0 && !job.planner.config().pruneNearShifts && job.planner.config().pruneBeyondHitFail) {
            for (int side = 0; side < 2; ++side) {
                double lim = hitPruneLimitOf(job, side == 1);
                if (std::isnan(lim)) continue;
                for (int idx : job.clones) {
                    auto& c = m_clones[idx];
                    if (c.state != CloneStatus::Running || c.shift == 0.0 || (c.shift > 0.0) != (side == 1)) continue;
                    if (std::fabs(c.shift) > lim + 1e-9) c.state = CloneStatus::Cancelled;
                }
            }
        }
        if (job.pass == 0 && job.planner.config().pruneNearShifts) {
            double scan = static_cast<double>(job.planner.config().islandScanSteps * job.planner.config().coarseStepTicks);
            double deadLate = 1e9, deadEarly = 1e9;
            for (int idx : job.clones) {
                auto& c = m_clones[idx];
                if (c.state != CloneStatus::Dead) continue;
                if (c.shift > 0) deadLate = std::min(deadLate, c.shift);
                else if (c.shift < 0) deadEarly = std::min(deadEarly, -c.shift);
            }
            for (int idx : job.clones) {
                auto& c = m_clones[idx];
                if (c.state != CloneStatus::Running) continue;
                if ((c.shift > 0 && c.shift > deadLate + scan + 1e-9) || (c.shift < 0 && -c.shift > deadEarly + scan + 1e-9)) c.state = CloneStatus::Cancelled;
            }
        }
        for (int idx : job.clones) {
            auto& c = m_clones[idx];
            if (c.state != CloneStatus::Running) continue;
            if (!c.inputPending && c.frameDone >= job.horizonFrame - 1e-9) {
                // v0.11.0: alive at the look-ahead's end but never settled = not a pass, not a death
                if (m_cfg.settle.enabled && c.shift != 0.0 && !c.settle.settled) {
                    c.state = CloneStatus::Cancelled;
                    c.invalidReason = "unsettled";
                    ++m_counters.unsettled;
                }
                else c.state = CloneStatus::Alive;
                continue;
            }
            if (m_cfg.settle.enabled && c.shift != 0.0 && !c.inputPending && c.settle.settled) {
                // settled: on the ground after the input (or flying long enough) with every later
                // input replayed as performed - the "survived" proof of v0.11.0
                c.state = CloneStatus::Alive;
                ++m_counters.settled;
                continue;
            }
            if (control && control != &c && control->state == CloneStatus::Running && !c.inputPending
                && control->stepDone == c.stepDone) {
                if (samePhysics(c.obj, control->obj)) {
                    if (c.converged == 0) c.rejoinStartFrame = c.frameDone;   // v0.7.0: the streak's first frame (§2.3)
                    if (++c.converged >= CONVERGE_STEPS) {
                        c.state = CloneStatus::Alive;
                        c.resynced = true;
                    }
                }
                else c.converged = 0;
            }
        }
        // v0.14.0: a connected-control job's finished copies feed its compensation planner (a death
        // starts the job: in lockstep the followers that acted before it are logged by then)
        if (job.connected && job.pass == 0 && job.mismatch.empty()) compNoteClones(job);
        // v0.11.0: the control verifies the model while any shifted copy still runs; once every
        // other copy is done it has nothing left to verify and the pass resolves
        if (m_cfg.settle.enabled && control && control->state == CloneStatus::Running && !control->inputPending) {
            bool others = false;
            for (int idx : job.clones) {
                if (&m_clones[idx] != control && m_clones[idx].state == CloneStatus::Running) { others = true; break; }
            }
            if (!others) control->state = CloneStatus::Alive;
        }
        if (!control) continue;
        // control vs the real player: exact every step (D7), or died together with it (D6)
        if (control->state == CloneStatus::Running && control->stepDone == target && p1 && !m_realDead) {
            float d = ccpDistance(control->obj->getPosition(), p1->getPosition());
            if (d > job.maxDrift) job.maxDrift = d;
            if (compareControl && job.mismatch.empty() && !control->inputPending) {
                std::string why;
                mirrorNextStepSpeed(*control);   // a speed change queued for the step that just began is not a mismatch
                if (!statesMatch(control->obj, p1, why)) {
                    job.mismatch = fmt::format("step {}: {} | real {} | control {}", target, why, stateStr(p1), stateStr(control->obj));
                }
            }
        }
        if (control->state == CloneStatus::Dead && !job.controlDiedWithReal && job.mismatch.empty()) {
            // died together with the real player (or one of its would-be deaths): same object, same
            // frame +-1, and for two object -1 deaths the same place (v0.7.1, Fable D6:
            // core/solver/miss_attribution diedWithReal)
            bool withReal = false;
            for (auto const& mk : m_deathMarks) {
                if (solver::diedWithReal(control->deathFrame, control->deathObjId, control->deathX, mk.frame, mk.obj, mk.x)) {
                    withReal = true;
                    job.missDeathFrame = mk.frame;
                }
            }
            if (withReal) {
                job.controlDiedWithReal = true;
                job.extension = job.extension || m_realDead;
            }
            else {
                job.mismatch = fmt::format("step {}: the control died on #{} at x={:.1f} (frame {:.1f}; killer x {:.0f}) but the real player did not{}", control->deathStep,
                    control->deathObjId, control->deathX, control->deathFrame, control->deathObjX,
                    m_deathMarks.empty() ? std::string() : fmt::format(" (last real death: #{} x={:.1f} frame {:.1f})", m_deathMarks.back().obj, m_deathMarks.back().x,
                                                                       m_deathMarks.back().frame));
            }
        }
        if (m_realDead && control->state != CloneStatus::Dead && control->stepDone >= m_realDeathStep && job.mismatch.empty()) {
            job.mismatch = fmt::format("step {}: the real player died on #{} at frame {:.1f} but the control survived ({})", m_realDeathStep, m_realDeathObj,
                m_realDeathFrame, stateStr(control->obj));
        }
        if (!job.mismatch.empty()) {
            // stop the clones: the pass is unusable
            for (int idx : job.clones) {
                auto& c = m_clones[idx];
                if (c.state == CloneStatus::Running) c.state = CloneStatus::Cancelled;
            }
        }
    }
}

bool CloneEngine::jobResolved(Job const& job) const {
    if (!job.spawned) return false;
    if (!job.pendingShifts.empty()) return false;   // deferred shifts still waiting for a clone
    for (int idx : job.clones) {
        if (m_clones[idx].state == CloneStatus::Running) return false;
    }
    return true;
}

void CloneEngine::finalizePass(Job& job) {
    if (!job.dropReason.empty()) return;
    std::vector<solver::ShiftOutcome> outs;
    Clone* control = nullptr;
    bool controlOk = false;
    std::string detail;
    std::vector<Clone const*> sorted;
    for (int idx : job.clones) sorted.push_back(&m_clones[idx]);
    std::sort(sorted.begin(), sorted.end(), [](Clone const* a, Clone const* b) { return a->shift < b->shift; });
    int steps = 0;
    solver::ShiftOutcome controlDeath;
    bool controlDied = false;
    if (job.pass == 0 && m_cfg.traceMaxTicks > 0) job.traces.clear();
    for (auto const* cp : sorted) {
        auto const& c = *cp;
        steps = std::max(steps, c.stepDone - job.step + 1);
        if (job.pass == 0 && c.tracing) {
            // v0.7.0 debug trace (§2.12): keep this trajectory until the job's window is known
            solver::trace::TraceClone tc = c.trace;
            tc.shiftFrames = c.shift;
            tc.passed = c.state == CloneStatus::Alive;
            job.traces.push_back(std::move(tc));
        }
        if (c.shift == 0.0) {
            control = const_cast<Clone*>(cp);
            controlOk = c.state == CloneStatus::Alive;
            detail += fmt::format("+0{} ", statusChar(c.state));
            if (c.state == CloneStatus::Dead) {
                // a miss: the control's own death attributes the run edge it bounds (§2.3)
                controlDied = true;
                controlDeath.kind = solver::ShiftKind::Died;
                controlDeath.deathAfterFrames = c.deathFrame - job.t;
                controlDeath.objectId = c.deathObjId;
                controlDeath.laterFixed = c.laterFixed;
                controlDeath.deathX = c.deathX;
                controlDeath.extension = c.extension;
            }
            continue;
        }
        solver::ShiftOutcome o;
        o.nominalFrames = c.shift;
        o.appliedFrames = std::isnan(c.appliedFrame) ? c.shift : c.appliedFrame - job.t;
        o.extension = c.extension;
        switch (c.state) {
            case CloneStatus::Alive:
                o.kind = c.resynced ? solver::ShiftKind::Resynced : solver::ShiftKind::Survived;
                if (c.resynced && !std::isnan(c.rejoinStartFrame)) o.rejoinAfterFrames = c.rejoinStartFrame - job.t;
                break;
            case CloneStatus::Dead:
                o.kind = solver::ShiftKind::Died;
                o.deathAfterFrames = c.deathFrame - job.t;
                o.objectId = c.deathObjId;
                o.laterFixed = c.laterFixed;   // counted by simStep (§2.3), never re-derived
                o.deathX = c.deathX;
                break;
            case CloneStatus::Invalid:
                o.kind = solver::ShiftKind::Invalid;
                o.reason = c.invalidReason.empty() ? "invalid" : c.invalidReason;
                break;
            case CloneStatus::Cancelled:
                o.kind = solver::ShiftKind::NotTested;
                o.reason = c.invalidReason.empty() ? "cancelled" : c.invalidReason;   // v0.11.0: "unsettled"
                break;
            case CloneStatus::Limit:
                o.kind = solver::ShiftKind::NotTested;
                o.reason = "limit";
                break;
            default:
                o.kind = solver::ShiftKind::NotTested;
                o.reason = "unresolved";
                break;
        }
        detail += fmt::format("{:+g}{}", c.shift, statusChar(c.state));
        if (c.state == CloneStatus::Dead) {
            // `@N/kL` + s / d (v0.7.0): k later fixed inputs applied before the death, self / downstream
            detail += fmt::format("@{:.1f}/{}L{}", c.deathFrame - job.t, c.laterFixed, c.laterFixed >= 1 ? 'd' : 's');
            if (c.deathObjId >= 0) detail += fmt::format("#{}", c.deathObjId);
        }
        if (c.extension) detail += 'e';
        detail += ' ';
        outs.push_back(std::move(o));
    }
    for (auto const& u : job.untested) {
        solver::ShiftOutcome o;
        o.nominalFrames = o.appliedFrames = u.shift;
        o.kind = solver::ShiftKind::NotTested;
        o.reason = u.reason;
        outs.push_back(std::move(o));
    }
    if (!job.untested.empty()) detail += fmt::format("({} untested)", job.untested.size());
    bool hadControl = control != nullptr;
    bool controlInvalid = control && control->state == CloneStatus::Invalid;
    std::string controlInvalidWhy = controlInvalid ? (control->invalidReason.empty() ? std::string("invalid") : control->invalidReason) : std::string();
    for (int idx : job.clones) freeClone(m_clones[idx]);
    job.clones.clear();
    job.untested.clear();
    job.passShifts.clear();
    job.pendingShifts.clear();

    std::string controlText = !job.mismatch.empty() ? "MISMATCH " + job.mismatch
        : job.controlDiedWithReal            ? std::string("died with the real player")
        : !hadControl                        ? std::string("MISSING (pool)")
        : controlInvalid                     ? "INVALID (" + controlInvalidWhy + ")"
        : controlOk                          ? std::string("exact")
                                             : std::string("UNRESOLVED");
    bool passUsable = hadControl && job.mismatch.empty() && (controlOk || job.controlDiedWithReal);
    if (job.pass >= 1 && !passUsable) {
        // A refinement pass that cannot be trusted (pool, teleport / dual portal, a mismatch that
        // only the re-run saw) is not ingested: the coarse window of pass 0 - whose control matched
        // the real player in lockstep - is emitted as it is (tick resolution, gprl-clone/1).
        slog(1, fmt::format("GPRL solver: job {} pass {} unusable ({}), coarse window kept: {}", job.id, job.pass, controlText, detail));
        job.planner.finish();
        finalize(job);
        return;
    }
    if (controlDied && job.pass == 0) job.planner.setControlDeath(controlDeath);
    job.planner.ingest(outs, controlOk, job.controlDiedWithReal, job.spawnComplete);
    if (job.historyMissingEarly) job.planner.markEarlyBlocked();
    slog(2, fmt::format("GPRL solver: job {} pass {} resolved in {} steps: {}| control {}, drift {:.3f}", job.id, job.pass, steps, detail, controlText, job.maxDrift));

    if (!hadControl) { dropJob(job, "pool"); return; }
    if (!job.mismatch.empty()) { dropJob(job, "mismatch"); return; }
    if (controlInvalid) {
        // teleport / dual portal inside the horizon: the model cannot follow, nothing was wrong
        job.mismatch = "the control clone became invalid (" + controlInvalidWhy + ")";
        dropJob(job, "invalid");
        return;
    }
    if (job.planner.controlFailed()) {
        job.mismatch = "the control clone neither survived nor died with the real player";
        dropJob(job, "mismatch");
        return;
    }
    if (job.planner.done()) {
        finalize(job);
        return;
    }
    // next (refinement) pass
    ++job.pass;
    job.spawned = false;
    job.spawnComplete = true;
    job.historyMissingEarly = false;
}

// Every drop lands in exactly one summary bucket (core/solver/diagnostics.hpp); a mismatch is
// also counted by kind so the 5 s summary says WHAT diverged (rings, speed, position, ...).
void CloneEngine::countDrop(std::string const& reason, std::string const& mismatch) {
    using namespace solver::diagnostics;
    ++m_counters.dropped;
    ++m_attempt.dropped;
    switch (dropBucket(reason)) {
        case DropBucket::Mismatch:
            ++m_counters.dropMismatch;
            ++m_counters.mismatchKinds[mismatchKind(mismatch)];
            break;
        case DropBucket::Invalid: ++m_counters.dropInvalid; break;
        case DropBucket::NoPass: ++m_counters.dropNoPass; break;
        case DropBucket::Pool: ++m_counters.dropPool; break;
        case DropBucket::Budget: ++m_counters.dropBudget; break;
        case DropBucket::History: ++m_counters.dropHistory; break;
        case DropBucket::Blocked: ++m_counters.dropBlocked; break;
        case DropBucket::Unbound: ++m_counters.dropUnbound; break;
        case DropBucket::Reset: ++m_counters.dropReset; break;
        case DropBucket::LevelEnd: ++m_counters.dropLevelEnd; break;
        case DropBucket::Other: ++m_counters.dropOther; break;
    }
}

void CloneEngine::dropJob(Job& job, std::string reason) {
    if (!job.dropReason.empty()) return;
    job.dropReason = reason;
    for (int idx : job.clones) freeClone(m_clones[idx]);
    job.clones.clear();
    job.pendingShifts.clear();
    countDrop(reason, job.mismatch);
    if (m_sink) {
        JobResult r;
        r.jobId = job.id;
        r.inputSeq = job.inputSeq;
        r.attemptId = job.attemptId;
        r.eventT = job.eventT;
        r.tick = job.tick;
        r.down = job.down;
        r.t = job.t;
        r.step = job.step;
        r.levelTime = job.levelTime;
        r.ok = false;
        r.dropReason = reason;
        r.mismatch = job.mismatch;
        r.detail = job.planner.describe();
        r.passes = job.planner.passesIngested();
        r.maxDrift = job.maxDrift;
        r.attemptInputIndex = job.attemptInputIndex;
        r.subTickMs = job.subTickMs;
        r.engineSubTickMs = job.engineSubTickMs;
        r.invalidWhy = job.mismatch;
        m_sink(r);
    }
    resultDrop(job, reason);   // v0.7.0: the bound job's one timing_result (unresolved / state_replay_failed)
}

void CloneEngine::finalize(Job& job) {
    if (!job.dropReason.empty()) return;
    // v0.7.0 (RC-minor 6): the input's canonical time includes its sub-tick part (0 without CBF);
    // v0.7.1 (Fable D11): the ENGINE's sub-tick - every shift is measured from job.t
    double actualMs = solver::units::actualMs(job.eventT, job.engineSubTickMs);
    JobResult r;
    r.jobId = job.id;
    r.inputSeq = job.inputSeq;
    r.attemptId = job.attemptId;
    r.eventT = job.eventT;
    r.tick = job.tick;
    r.practice = job.practice;
    r.down = job.down;
    r.t = job.t;
    r.step = job.step;
    r.levelTime = job.levelTime;
    r.pre = job.pre;
    r.geometryHash = job.geometryHash;
    r.ceilingTouch = job.ceilingTouch;
    if (!std::isnan(job.prevGapFrames)) r.prevGapMs = solver::timeline::framesToMs(job.prevGapFrames);
    if (!std::isnan(job.nextGapFrames)) r.nextGapMs = solver::timeline::framesToMs(job.nextGapFrames);
    if (!job.down && !std::isnan(job.pressFrame)) {
        double hold = solver::timeline::framesToMs(job.t - job.pressFrame);
        r.holdMs = hold;
        r.pressMs = actualMs - hold;
    }
    else if (job.down && !std::isnan(job.releaseFrame)) r.holdMs = solver::timeline::framesToMs(job.releaseFrame - job.t);
    r.firstPortalId = job.firstPortalId;
    r.window = job.planner.result(actualMs);
    r.refined = job.planner.refined();
    r.miss = job.planner.miss();
    r.extension = job.extension;
    r.passes = job.planner.passesIngested();
    r.detail = job.planner.describe();
    r.mismatch = job.mismatch;
    r.maxDrift = job.maxDrift;
    for (double f : m_wouldBeDeathFrames) {
        if (f >= job.t && f <= job.t + kWouldBeDeathWindowFrames) r.wouldBeDeathAfter = true;
    }
    r.attemptInputIndex = job.attemptInputIndex;
    r.subTickMs = job.subTickMs;
    r.engineSubTickMs = job.engineSubTickMs;
    r.ok = r.window.valid && job.mismatch.empty();
    if (!r.ok) {
        if (r.window.invalidReason.find("passing shift") != std::string::npos) r.dropReason = "no_pass";
        else if (r.window.invalidReason == solver::kNoShiftTested) r.dropReason = "no_shift_tested";   // RC-minor 2: never a 0 ms window
        else r.dropReason = "invalid";
        r.invalidWhy = !r.window.invalidReason.empty() ? r.window.invalidReason : job.mismatch;
    }
    if (r.ok && r.miss) {
        // v0.7.1 (Fable D6): a MISS with a passing run waits until every job that could still die
        // with the same real / would-be death resolved; then only the LATEST such input is the
        // player's miss (settleMisses). A miss without a passing run is no candidate (no_pass).
        m_pendingMisses.push_back({job, r, job.missDeathFrame});
        job.dropReason = "done";
        return;
    }
    emitFinalized(job, r);
}

void CloneEngine::emitFinalized(Job& job, JobResult& r) {
    bool emitted = false;
    if (m_sink) emitted = m_sink(r);
    if (r.missDownstream) {
        // Fable D6: not the player's miss, not a hit measurement - no timing_window (the sink
        // logged it), the timing_result says sequence_dependent (miss_downstream)
        ++m_counters.missDownstream;
        ++m_attempt.missDownstream;
    }
    else if (r.ok && emitted) {
        ++m_counters.emitted;
        ++m_attempt.emitted;
        if (r.miss) {
            ++m_counters.misses;
            ++m_attempt.misses;
        }
        else noteLocalDone(job, r);   // a hit with an emitted window: the sequence solver can build on it
    }
    else countDrop(r.dropReason.empty() ? std::string("ring") : r.dropReason, r.mismatch);   // "ring": the telemetry ring refused the event
    job.dropReason = "done";
    resultLocal(job, r);   // v0.7.0: the local part of the job's timing_result (SA next, when needed)
}

void CloneEngine::settleMisses(bool force) {
    // Fable D6 (core/solver/miss_attribution): per real / would-be death, the pending MISS jobs
    // (control died with it, a passing run) wait until no unresolved job whose input precedes the
    // death could still join them; then the LATEST input is the player's miss and every earlier
    // one is miss_downstream. Bounded: every iteration settles and erases one death's group (the
    // pending list holds at most kMaxJobs entries).
    for (int guard = 0; guard < kMaxJobs + 1 && !m_pendingMisses.empty(); ++guard) {
        double death = m_pendingMisses.front().deathFrame;
        for (auto const& p : m_pendingMisses) {
            if (std::isnan(death) || p.deathFrame < death) death = p.deathFrame;
        }
        if (!force) {
            for (auto const& j : m_jobs) {
                if (j.dropReason.empty() && (std::isnan(death) || j.t < death + solver::kMissMatch.frameTolerance + 1e-9)) return;
            }
        }
        std::vector<PendingMiss> group, rest;
        for (auto& p : m_pendingMisses) {
            bool same = std::isnan(death) ? std::isnan(p.deathFrame) : std::fabs(p.deathFrame - death) <= solver::kMissMatch.frameTolerance + 1e-6;
            (same ? group : rest).push_back(std::move(p));
        }
        m_pendingMisses = std::move(rest);
        std::vector<solver::MissCandidate> candidates;
        for (auto const& g : group) candidates.push_back({g.job.id, g.job.t, g.r.ok});
        auto miss = solver::attributeMiss(candidates);
        if (group.size() > 1 || m_cfg.verbosity >= 2) {
            slog(1, fmt::format("GPRL solver: death at frame {:.1f}: {} MISS candidate(s), the miss is job {} (the latest input), {} miss_downstream{}", death,
                                group.size(), miss ? *miss : 0, miss ? group.size() - 1 : group.size(), force ? " (settled at the restart / level end)" : ""));
        }
        for (auto& g : group) {
            g.r.missDownstream = !miss || g.job.id != *miss;
            emitFinalized(g.job, g.r);
        }
    }
}

void CloneEngine::abortJobs(char const* why) {
    for (auto& job : m_jobs) {
        if (job.dropReason.empty()) dropJob(job, why);
    }
    m_jobs.clear();
}

void CloneEngine::extensionSteps() {
    // Death pause (D6): the world is frozen, the real player is dead; surviving clones of jobs whose
    // control died with the real player finish with synthetic whole ticks (at most a few per frame).
    if (!m_realDead || m_jobs.empty()) return;
    if (m_guard.breached()) return;   // v0.8.2 isolation breaker
    double isoT0 = 0.0;
    bool const iso = isoBegin(m_isoBefore, isoT0);
    for (auto& j : m_jobs) {
        if (!j.spawned || !j.pendingShifts.empty()) trySpawn(j);   // a refinement pass may start inside the death pause
    }
    for (auto& job : m_jobs) {
        if (!job.spawned || !job.dropReason.empty()) continue;
        for (int idx : job.clones) {
            auto& c = m_clones[idx];
            int ran = 0;
            // real steps still ahead of a lagging clone are consumed normally (history entries);
            // beyond the real timeline simStep runs synthetic whole ticks
            while (c.state == CloneStatus::Running && ran < kExtensionStepsPerFrame && m_cloneStepsThisFrame < m_stepBudget) {
                if (simStep(c, job, c.stepDone + 1) >= 0) ++ran;
            }
        }
    }
    advance(m_step, false);
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (!it->dropReason.empty()) { it = m_jobs.erase(it); continue; }
        if (jobResolved(*it)) {
            finalizePass(*it);
            if (!it->dropReason.empty() || it->planner.done()) it = m_jobs.erase(it);
            else ++it;
        }
        else ++it;
    }
    if (iso) isoEnd(m_isoBefore, isoT0, "extension", nullptr);
    isoFinishBreach();
    // v0.7.1 (Fable D6): once every job the death pause finishes has resolved, ONE miss per death
    if (!m_pendingMisses.empty()) settleMisses(false);
}

void CloneEngine::onLevelComplete() {
    if (!m_pl || m_offForVisit) return;
    // After the finish line the real player is moved by the end animation, not by physics: a
    // delayed replay could not be compared with it. Nothing is queued or run until the restart.
    abortSequences("level_end");
    m_seqClosed = true;
    if (m_jobs.empty()) {
        settleMisses(true);   // v0.7.1 (Fable D6)
        compAbortAll(solver::status::Reason::SaCutByRestart);
    saAbortAll(solver::status::Reason::SaCutByRestart);
        drainResults();
        return;
    }
    for (auto& j : m_jobs) {
        j.cutByLevelEnd = true;   // v0.7.0: a side left incomplete is `cut_by_level_end`
        if (!j.spawned || !j.pendingShifts.empty()) trySpawn(j);
        for (double s : j.pendingShifts) j.untested.push_back({s, "level end"});
        j.pendingShifts.clear();
    }
    if (!m_guard.breached()) {
        double isoT0 = 0.0;
        bool const iso = isoBegin(m_isoBefore, isoT0);
        advance(m_step, false);
        if (iso) isoEnd(m_isoBefore, isoT0, "level_end", nullptr);
        isoFinishBreach();
    }
    for (auto& j : m_jobs) {
        if (!j.dropReason.empty()) continue;
        bool applied = false;
        for (int idx : j.clones) {
            auto& c = m_clones[idx];
            if (c.state == CloneStatus::Running) {
                if (!c.inputPending) { c.state = CloneStatus::Alive; applied = true; }
                else c.state = CloneStatus::Cancelled;
            }
            else if (c.state != CloneStatus::Idle) applied = true;
        }
        if (!applied && !j.planner.ingested()) {
            dropJob(j, "level_end");
            continue;
        }
        j.planner.finish();
        finalizePass(j);
        if (j.dropReason.empty()) {
            j.planner.finish();
            finalize(j);
        }
    }
    m_jobs.clear();
    settleMisses(true);   // v0.7.1 (Fable D6)
    // v0.7.0: no delayed replay can follow the end animation - SA candidates end undecided
    compAbortAll(solver::status::Reason::SaCutByRestart);
    saAbortAll(solver::status::Reason::SaCutByRestart);
    drainResults();
}

// ---- deaths and invalid clones (destroyPlayer / teleport / dual hooks) ----

Clone* CloneEngine::cloneFor(PlayerObject* p) {
    if (!p) return nullptr;
    if (p == m_shadow.obj) return &m_shadow;
    if (p == m_shadow2.obj) return &m_shadow2;
    auto it = m_index.find(p);
    if (it == m_index.end()) return nullptr;
    return &m_clones[it->second];
}

// The game sometimes reports a copy's death with the main player's pointer, so any death that
// happens while a clone is being stepped belongs to that clone.
void CloneEngine::cloneDied(PlayerObject* p, GameObject* by) {
    if (m_sim) p = m_sim;
    auto c = cloneFor(p);
    if (!c || c->state != CloneStatus::Running) return;
    c->state = CloneStatus::Dead;
    c->deathObjId = by ? by->m_objectID : -1;
    c->deathObjX = by ? by->getPositionX() : 0.f;
    if (c->tracing && c->obj) {
        // v0.7.0 debug trace (§2.12): the killer, the clone's hitbox and its four collision ids
        auto& d = c->trace.death;
        d.died = true;
        d.killerId = c->deathObjId;
        d.killerType = by ? static_cast<int>(by->m_objectType) : -1;
        d.killerX = by ? by->getPositionX() : 0.f;
        d.killerY = by ? by->getPositionY() : 0.f;
        if (by) {
            auto r = by->getObjectRect();
            d.rectX = r.origin.x;
            d.rectY = r.origin.y;
            d.rectW = r.size.width;
            d.rectH = r.size.height;
        }
        auto box = c->obj->getObjectRect();
        d.x = c->obj->getPositionX();
        d.y = c->obj->getPositionY();
        d.boxW = box.size.width;
        d.boxH = box.size.height;
        d.lastCollision[0] = c->obj->m_lastCollisionTop;
        d.lastCollision[1] = c->obj->m_lastCollisionBottom;
        d.lastCollision[2] = c->obj->m_lastCollisionLeft;
        d.lastCollision[3] = c->obj->m_lastCollisionRight;
    }
}

void CloneEngine::noteAnticheatTouch() {
    if (m_anticheatTouches++ == 0) log::info("GPRL solver: a clone touched GD's anti-cheat spike; ignored like GD does (not a death)");
}

void CloneEngine::cloneInvalid(PlayerObject* p, char const* why) {
    if (m_sim) p = m_sim;
    auto c = cloneFor(p);
    if (!c) return;
    if (c->state == CloneStatus::Running) {
        c->state = CloneStatus::Invalid;
        c->invalidReason = why ? why : "invalid";
    }
}

// ---- shadow: the same simulation run one step behind the real player, compared every step ----

bool CloneEngine::statesMatch(PlayerObject* a, PlayerObject* b, std::string& why) const {
    auto pa = a->getPosition();
    auto pb = b->getPosition();
    if (std::fabs(pa.x - pb.x) > 1e-3f || std::fabs(pa.y - pb.y) > 1e-3f) {
        why = fmt::format("position off by ({:.4f},{:.4f})", pa.x - pb.x, pa.y - pb.y);
        return false;
    }
    if (std::fabs(a->m_yVelocity - b->m_yVelocity) > 1e-6) {
        why = fmt::format("y velocity {:.5f} vs {:.5f}", a->m_yVelocity, b->m_yVelocity);
        return false;
    }
    if (a->m_isOnGround != b->m_isOnGround) { why = "ground flag"; return false; }
    if (a->m_isUpsideDown != b->m_isUpsideDown) { why = "gravity"; return false; }
    if (modeChar(a) != modeChar(b)) { why = "game mode"; return false; }
    if (a->m_isDashing != b->m_isDashing) { why = "dash"; return false; }
    if (a->m_isOnSlope != b->m_isOnSlope) { why = "slope flag"; return false; }
    if (a->m_vehicleSize != b->m_vehicleSize) { why = "size"; return false; }
    if (a->m_playerSpeed != b->m_playerSpeed) { why = "speed"; return false; }
    if (a->m_holdingButtons != b->m_holdingButtons) { why = "held buttons"; return false; }
    int ra = a->m_touchingRings ? a->m_touchingRings->count() : 0;
    int rb = b->m_touchingRings ? b->m_touchingRings->count() : 0;
    if (ra != rb) { why = fmt::format("touching rings {} vs {}", ra, rb); return false; }
    if (std::fabs(a->m_lastPosition.x - b->m_lastPosition.x) > 1e-3f || std::fabs(a->m_lastPosition.y - b->m_lastPosition.y) > 1e-3f) {
        why = "last position";
        return false;
    }
    return true;
}

// GJBaseGameLayer::update hands the real players a queued speed change at the very start of a
// step, BEFORE processCommands (GD_PHYSICS_NOTES "Speed portals are NOT collisions"), so when a
// clone that finished step k is compared with the real player at the next stepBegin, the real
// player can already carry the speed of step k + 1 while the clone (correctly) still has the
// speed step k ran with: position and velocity equal, only m_playerSpeed differs. simStep would
// mirror that speed at the clone's next step anyway; doing it here, before the comparison, keeps
// a speed portal from reading as a control / shadow mismatch. Only when the clone's speed IS the
// one the real player ran step k with (the ring snapshot of step k): any other speed stays a
// mismatch.
void CloneEngine::mirrorNextStepSpeed(Clone& c) {
    if (!c.obj || c.stepDone < 1 || m_hist.empty()) return;
    auto const& ran = m_hist[ring(c.stepDone)];
    auto const& next = m_hist[ring(c.stepDone + 1)];
    if (ran.step != c.stepDone || next.step != c.stepDone + 1) return;
    float const now = c.obj->m_playerSpeed;
    if (now == next.state.f.m_playerSpeed || now != ran.state.f.m_playerSpeed) return;
    // same context as simStep's own mirror: the clone is "being stepped" for every side-effect guard
    PlayerObject* const stepping = m_sim;
    m_sim = c.obj;
    c.obj->updateTimeMod(next.state.f.m_playerSpeed, true);
    m_sim = stepping;
}

void CloneEngine::shadowResync(int k) {
    auto p1 = m_pl->m_player1;
    auto& c = m_shadow;
    if (!c.obj || !p1) return;
    freeClone(c);
    copyState(c.obj, p1);
    std::vector<std::pair<EnhancedGameObject*, uint8_t>> flags;
    captureFlags(p1->getPositionX(), flags);
    for (auto& [o, f] : flags) c.flags[o] = f;
    c.state = CloneStatus::Running;
    c.stepDone = k;
    c.frameDone = m_frame;
}

// ---- v0.8.3 dual pair shadow (docs/LIVE_ISOLATION_DESIGN.md §2.7, ISO-D6) ----
//
// In a dual section the real game steps TWO players; the single shadow could not follow and no
// clone could ever flip gravity without reaching "the other player" by unique id (D1). The pair
// shadow is a P1 clone + a P2 clone stepped in GD's own dual order, read from GJBaseGameLayer::update
// (2.2081, update.asm 0x238011-0x238686): processCommands (inputs reach P1 then P2: handleButton
// 0x2338e0) -> resetTouchedRings / collision-log clears P1, P2 -> P2.m_maybeReverseSpeed /
// m_maybeReverseAcceleration = P1's (0x2383d2-0x238403, unconditional) -> updateInternalActions P1,
// P2 (0x238415 / 0x23842e) -> P1 update + checkCollisions (0x238448 / 0x23846e) -> P2 update +
// checkCollisions (0x238536 / 0x238561) -> getRotation into +0xa08 P1, P2 (0x23862e / 0x23864d) ->
// updateRotation P1, P2 (0x23866d / 0x238686) -> previous positions. Both clones are compared with
// their real player every step; the P2 clone steps with activation bit kOther and the pair partner
// set, so the gravity link (IsolationHooks H5) and the portal gravity rule act on the PAIR, never on
// a real player. Shadow only: inputs in dual still get no job (pair jobs are the next step).

void CloneEngine::simBegin(Clone& c, bool halfTick, PlayerObject* partner) {
    m_sim = c.obj;
    m_simClone = &c;
    m_simHalfTick = halfTick;
    m_simSelfBit = c.player2 ? activation::kOther : activation::kSelf;
    m_simPartner = partner;
}

void CloneEngine::pairResync(int k) {
    auto p1 = m_pl->m_player1;
    auto p2 = m_pl->m_player2;
    if (!p1 || !p2 || !m_shadow.obj || !m_shadow2.obj) return;
    auto sync = [&](Clone& c, PlayerObject* real, bool second) {
        freeClone(c);
        copyState(c.obj, real);
        c.obj->m_isSecondPlayer = second;   // copyState clears it; the real P2 has it
        c.player2 = second;
        std::vector<std::pair<EnhancedGameObject*, uint8_t>> flags;
        captureFlags(real->getPositionX(), flags);
        for (auto& [o, f] : flags) c.flags[o] = f;
        c.state = CloneStatus::Running;
        c.stepDone = k;
        c.frameDone = m_frame;
    };
    sync(m_shadow, p1, false);
    sync(m_shadow2, p2, true);
}

int CloneEngine::simStepPair(Clone& c1, Clone& c2, int k) {
    auto& h = m_hist[ring(k)];
    if (h.step != k || !h.dual) {
        for (Clone* c : {&c1, &c2}) {
            c->state = CloneStatus::Invalid;
            c->invalidReason = "history lost";
            c->stepDone = k;
        }
        return -1;
    }
    float const dt = h.dtKnown ? h.dt : m_stepDt;
    double const f0 = h.frame;
    double const fr = h.frames;
    if (fr <= 0.0) {
        c1.stepDone = c2.stepDone = k;
        return -1;
    }
    // every logged input of the step, in order (all fixed: the shadow replays the real run)
    struct Ev { double frac; bool down; uint32_t order; };
    Ev evs[16];
    int n = 0;
    {
        auto it = std::lower_bound(m_log.begin(), m_log.end(), f0 - 1e-9, [](InputEvent const& e, double v) { return e.t < v; });
        for (; it != m_log.end() && it->t < f0 + fr - 1e-9 && n < 16; ++it) evs[n++] = {std::clamp((it->t - f0) / fr, 0.0, 1.0), it->down, it->id};
    }
    std::stable_sort(evs, evs + n, [](Ev const& a, Ev const& b) { return a.frac < b.frac || (a.frac == b.frac && a.order < b.order); });
    Clone* cl[2] = {&c1, &c2};
    PlayerObject* p[2] = {c1.obj, c2.obj};
    PlayerState const* st[2] = {&h.state, &h.state2};
    auto partnerOf = [&](int i) { return p[1 - i]; };
    auto applyTo = [&](int i, Ev const& ev) {
        simBegin(*cl[i], h.halfTick, partnerOf(i));
        if (ev.down) p[i]->pushButton(PlayerButton::Jump);
        else p[i]->releaseButton(PlayerButton::Jump);
    };
    for (int i = 0; i < 2; ++i) {
        simBegin(*cl[i], h.halfTick, partnerOf(i));
        p[i]->m_totalTime = h.totalTime;
        if (p[i]->m_playerSpeed != st[i]->f.m_playerSpeed) p[i]->updateTimeMod(st[i]->f.m_playerSpeed, true);
    }
    // 1. inputs queued for the step start: P1 then P2 (one control drives both, dual::inputTargets)
    int first = 0;
    for (; first < n && evs[first].frac <= 1e-6; ++first) {
        applyTo(0, evs[first]);
        applyTo(1, evs[first]);
    }
    // 2. per-step resets P1, P2; the P1 -> P2 copy; updateInternalActions P1, P2
    for (int i = 0; i < 2; ++i) {
        simBegin(*cl[i], h.halfTick, partnerOf(i));
        gameStepReset(p[i]);
    }
    p[1]->m_maybeReverseSpeed = p[0]->m_maybeReverseSpeed;
    p[1]->m_maybeReverseAcceleration = p[0]->m_maybeReverseAcceleration;
    for (int i = 0; i < 2; ++i) {
        simBegin(*cl[i], h.halfTick, partnerOf(i));
        if (p[i]->m_actionManager) p[i]->updateInternalActions(dt);
    }
    // 3./4. movement + collisions, P1 then P2 (mid-step inputs split per player like simStep)
    CCPoint startPos[2];
    bool splitP[2] = {false, false};
    float lastSub[2] = {dt, dt};
    for (int i = 0; i < 2; ++i) {
        auto* q = p[i];
        simBegin(*cl[i], h.halfTick, partnerOf(i));
        startPos[i] = q->getPosition();
        bool const startedOnGround = q->m_isOnGround;
        bool const canSplit = m_cfg.subtick && notBuffering(q);
        float elapsed = 0.f;
        for (int j = first; j < n; ++j) {
            auto const& ev = evs[j];
            if (canSplit && ev.frac > elapsed + 1e-6 && ev.frac < 1.0 - 1e-6) {
                float sub = dt * static_cast<float>(ev.frac - elapsed);
                q->update(sub);
                if (!splitP[i] && ((q->m_yVelocity < 0) != q->m_isUpsideDown)) q->m_isOnGround = startedOnGround;
                if (!q->m_isOnSlope || q->m_isDart) m_pl->checkCollisions(q, 0.f, true);
                else m_pl->checkCollisions(q, dt, true);
                q->updateRotation(sub);
                resetLog(q);
                elapsed = static_cast<float>(ev.frac);
                splitP[i] = true;
                if (cl[i]->state != CloneStatus::Running) break;
            }
            applyTo(i, ev);
        }
        if (cl[i]->state == CloneStatus::Running) {
            float rest = dt * (1.f - elapsed);
            lastSub[i] = splitP[i] ? rest : dt;
            if (rest > 0.f) q->update(rest);
        }
        if (cl[i]->state == CloneStatus::Running) m_pl->checkCollisions(q, dt, false);
    }
    // 5. rotation and the end-of-step bookkeeping, P1 then P2
    for (int i = 0; i < 2; ++i) {
        auto* q = p[i];
        simBegin(*cl[i], h.halfTick, partnerOf(i));
        if (cl[i]->state == CloneStatus::Running) {
            q->m_unkUnused3 = q->getRotation();
            q->updateRotation(lastSub[i]);
            if (splitP[i]) q->m_lastPosition = startPos[i];
            q->m_shipRotation = q->getPosition();
        }
    }
    m_sim = nullptr;
    m_simClone = nullptr;
    m_simPartner = nullptr;
    m_simSelfBit = activation::kSelf;
    for (int i = 0; i < 2; ++i) {
        cl[i]->stepDone = k;
        cl[i]->frameDone = f0 + fr;
        if (cl[i]->state == CloneStatus::Dead) {
            cl[i]->deathStep = k;
            cl[i]->deathFrame = f0 + fr;
            cl[i]->deathX = p[i]->getPositionX();
        }
    }
    m_cloneSteps += 2;
    m_cloneStepsThisFrame += 2;
    return n;
}

void CloneEngine::pairShadowStep(int k, bool compare) {
    auto p1 = m_pl->m_player1;
    auto p2 = m_pl->m_player2;
    if (!p1 || !p2 || k < 1 || !m_pl->m_started || m_platformer || m_pl->m_hasCompletedLevel) return;
    // two-player levels: player 2's own inputs are not logged, the pair cannot be replayed
    if (m_pl->m_levelSettings && m_pl->m_levelSettings->m_twoPlayerMode) return;
    if (!m_shadow2.obj) {
        m_shadow2 = {};
        m_shadow2.obj = makeClone();
        if (!m_shadow2.obj) return;
        m_shadow2.player2 = true;
    }
    auto& hk = m_hist[ring(k)];
    if (hk.step != k || !hk.dual) { pairResync(k); return; }
    if (hk.frames <= 0.0) {
        m_shadow.stepDone = m_shadow2.stepDone = k;
        return;
    }
    if (m_shadow.state != CloneStatus::Running || m_shadow2.state != CloneStatus::Running || m_shadow.stepDone != k - 1 || m_shadow2.stepDone != k - 1
        || !m_shadow2.player2 || !m_shadow2.obj->m_isSecondPlayer) {
        pairResync(k);
        return;
    }
    if (!m_pairLogged) {
        m_pairLogged = true;
        slog(0, fmt::format("GPRL dual: pair shadow on (dual section at x={:.1f}, real P2 uid {} -> activation bit kOther, gravity link and portal gravity act on the pair partner; shadow only, inputs in dual are not measured yet)",
                            p1->getPositionX(), p2->m_uniqueID));
    }
    int n = simStepPair(m_shadow, m_shadow2, k);
    ++m_counters.pairShadowSteps;
    ++m_attempt.pairShadowSteps;
    m_counters.shadowSteps++;
    m_attempt.shadowSteps++;
    if (!compare) { pairResync(k); return; }
    if (m_shadow.state == CloneStatus::Invalid || m_shadow2.state == CloneStatus::Invalid) {
        pairResync(k);   // dual / solo / teleport portal: cannot be followed, not a simulation fault
        return;
    }
    bool const realDied = m_realDeathStep == k;
    bool const anyDead = m_shadow.state == CloneStatus::Dead || m_shadow2.state == CloneStatus::Dead;
    std::string why;
    bool ok = true;
    if (realDied) {
        ok = anyDead;
        if (!ok) why = "the real pair died here but the pair shadow survived";
    }
    else if (anyDead) {
        ok = false;
        why = fmt::format("the pair shadow's {} died but the real pair survived", m_shadow.state == CloneStatus::Dead ? "P1" : "P2");
    }
    else {
        mirrorNextStepSpeed(m_shadow);
        mirrorNextStepSpeed(m_shadow2);
        std::string w1, w2;
        bool const ok1 = statesMatch(m_shadow.obj, p1, w1);
        bool const ok2 = statesMatch(m_shadow2.obj, p2, w2);
        ok = ok1 && ok2;
        if (!ok) why = fmt::format("P1 {} | P2 {}", ok1 ? "ok" : w1, ok2 ? "ok" : w2);
    }
    onReplayStep(!ok, why);
    if (ok) return;
    noteShadowMismatch(hk.frame, realDied);
    ++m_counters.pairShadowMismatches;
    ++m_attempt.pairShadowMismatches;
    m_counters.shadowMismatches++;
    m_attempt.shadowMismatches++;
    if (m_shadowLogged < SHADOW_LOG_MAX) {
        m_shadowLogged++;
        log::warn("GPRL shadow mismatch #{} (dual pair) at step {} (frame {:.1f}, dt {:.4f}, half {}, {} input(s) in step): {} | real P1 {} P2 {} | shadow P1 {} P2 {}",
                  m_attempt.shadowMismatches, k, hk.frame, hk.dt, hk.halfTick ? 1 : 0, n, why, stateStr(p1), stateStr(p2), stateStr(m_shadow.obj), stateStr(m_shadow2.obj));
    }
    pairResync(k);
}

void CloneEngine::shadowStep(int k, bool compare) {
    auto p1 = m_pl->m_player1;
    auto& c = m_shadow;
    if (!c.obj || !p1 || k < 1 || !m_pl->m_started || m_platformer) return;
    // v0.7.0 (RC-minor 3): after the finish line the end animation moves the player, not physics:
    // v0.6.2 logged 240 fake mismatches per completion and dropped the last inputs `mismatch`
    if (m_pl->m_hasCompletedLevel) return;
    auto& hk = m_hist[ring(k)];
    if (hk.step != k) { shadowResync(k); return; }
    if (hk.frames <= 0.0) {
        c.stepDone = k;
        return;
    }
    if (c.state != CloneStatus::Running || c.stepDone != k - 1) { shadowResync(k); return; }

    Job none;
    none.inputId = kNoInputId;
    none.down = false;
    none.subtick = m_cfg.subtick;
    int n = simStep(c, none, k);
    m_counters.shadowSteps++;
    m_attempt.shadowSteps++;
    if (!compare) { shadowResync(k); return; }

    bool realDied = m_realDeathStep == k;
    std::string why;
    bool ok;
    if (realDied) {
        ok = c.state == CloneStatus::Dead;
        if (!ok) why = "the real player died here but the shadow survived";
    }
    else if (c.state == CloneStatus::Dead) {
        ok = false;
        why = c.deathObjId >= 0 ? fmt::format("the shadow died on object #{} at x={:.0f} but the real player survived", c.deathObjId, c.deathObjX)
                                : "the shadow died on a level boundary but the real player survived";
    }
    else if (c.state == CloneStatus::Invalid) {
        shadowResync(k);   // teleport / dual portal: cannot be followed, not a simulation fault
        return;
    }
    else {
        mirrorNextStepSpeed(c);   // a speed change queued for the step that just began is not a mismatch
        ok = statesMatch(c.obj, p1, why);
    }
    onReplayStep(!ok, why);   // v0.7.0 replay breaker (V2-D13)
    if (ok) return;

    // v0.7.0: shadow mismatches near an input make its timing low_confidence; a real death the
    // shadow did not reproduce makes every job spanning it state_replay_failed (§2.10, §8 Q8)
    noteShadowMismatch(hk.frame, realDied);
    m_counters.shadowMismatches++;
    m_attempt.shadowMismatches++;
    if (m_shadowLogged < SHADOW_LOG_MAX) {
        m_shadowLogged++;
        log::warn("GPRL shadow mismatch #{} at step {} (frame {:.1f}, dt {:.4f}, half {}, {} input(s) in step): {} | real {} | shadow {}",
            m_attempt.shadowMismatches, k, hk.frame, hk.dt, hk.halfTick ? 1 : 0, n, why, stateStr(p1), stateStr(c.obj));
    }
    shadowResync(k);
}

int CloneEngine::runningClones() const {
    int running = 0;
    for (auto const& c : m_clones) if (c.state == CloneStatus::Running) running++;
    return running;
}

double CloneEngine::coveragePercent() const {
    solver::budget::CoverageInput c;
    c.inputs = m_counters.inputs;
    c.emitted = m_counters.emitted;
    c.skippedDead = m_counters.skippedDead;
    c.notWindowable = notWindowable();
    return solver::budget::coveragePercent(c);
}

// The 5 s summary (docs/SOLVER_DESIGN.md §8, §12): every skip and drop reason by name, the
// mismatch kinds, coverage, and the budget the frame ran with, so the Geode log alone answers
// "why did this input get no window" without verbose logging.
std::string CloneEngine::summaryLine() const {
    auto const& c = m_counters;
    std::string kinds;
    for (auto const& [k, n] : c.mismatchKinds) kinds += fmt::format("{}{} {}", kinds.empty() ? "" : ", ", k, n);
    // v0.7.0 (docs/TIMING_SOLVER_V2.md §6 item 5): the timing_result statuses, the local edges by
    // cause and the sequence-adjusted candidates / jobs
    using S = solver::status::TimingStatus;
    auto sc = [&](S s) { return c.statusCount[static_cast<int>(s)]; };
    auto const& a = m_saCounters;
    std::string v2 = fmt::format(
        " | results {} (sent {}, fallback {}): statuses ok {}, low {}, unresolved {}, replay_failed {}, seq_dependent {}, no_effect {}; "
        "local edges self {} / downstream {} / ext {}; sa candidates {} (+{} decided without a replay), measured {}, undecided sides {}, "
        "dropped {} (control {}, negative {}, expired {}, restart {}, invalid {}, death {}), not started {} (budget {}, wide {}, no negative {}, expired {}, death {}), "
        "waiting {}, {} trials / {} clone steps; skipped replay {}{}; "
        "miss downstream {}, speed changes {} (flagged {}), subtick clock: engine vs tracker max |d| {:.3f} ms; isolation tripwires {}, portals modelled {}, invariant {} (blocks {}, checks {} at {:.1f} us, breaches {}, inputs skipped after a breach {})",
        c.results, c.resultsSent, c.resultsFallback, sc(S::Ok), sc(S::LowConfidence), sc(S::Unresolved), sc(S::StateReplayFailed), sc(S::SequenceDependent),
        sc(S::NoEffect), c.edgeSelf, c.edgeDownstream, c.edgeExtension, a.candidates, a.immediate, a.measured, a.undecidedSides, a.dropped(), a.dropControl,
        a.dropNegative, a.dropExpired, a.dropReset, a.dropInvalid, a.dropDeath, a.notStarted(), a.notStartedBudget, a.notStartedWide, a.notStartedNoNegative,
        a.notStartedExpired, a.notStartedDeath, m_saCands.size(), a.trials, a.cloneSteps, c.skippedReplay, m_replayBroken ? " (replay broken this attempt)" : "",
        c.missDownstream, c.speedChanges, c.speedFlagged, m_subTickClockMaxMs, m_tripwiresLevel, m_portalModelLevel,
        solver::isolation::name(m_guard.mode()), m_guard.blocks(), m_guard.checks(), m_guard.costUs(), m_guard.breaches(), c.skippedIsolation)
        + fmt::format("; settled look-ahead {} ({:.0f} s max, ground {} ticks): settled {}, unsettled {}", m_cfg.settle.enabled ? "ON" : "off",
                      m_cfg.settle.maxSeconds, m_cfg.settle.groundTicks, c.settled, c.unsettled)
        + compSummary();
    return fmt::format(
        "GPRL solver: 5 s summary - inputs {}, windows {} (misses {}), coverage {:.0f}% of {} windowable, "
        "dropped {} (mismatch {} [{}], invalid {}, pool {}, budget {}, history {}, blocked {}, unbound {}, other {}), "
        "not windowable {} (death unrelated {}, restart {}, level end {}, miss downstream {}), "
        "skipped {} (dead {}, dual {}, paused {}, throttle {}, slow frame {}, jobs {}, other {}), "
        "deferred spawns {} (expired {}), shadow {} steps / {} mismatches, "
        "sim {:.1f} ms/frame (peak {:.1f}, {:.1f} us/step, budget {} steps/frame), {} clones running (pool {}, created {}), {} jobs, throttled={}",
        c.inputs, c.emitted, c.misses, coveragePercent(), std::max(0, c.inputs - c.skippedDead - notWindowable()),
        c.dropped, c.dropMismatch, kinds.empty() ? "-" : kinds, c.dropInvalid, c.dropPool, c.dropBudget, c.dropHistory, c.dropBlocked, c.dropUnbound, c.dropOther,
        notWindowable(), c.dropNoPass, c.dropReset, c.dropLevelEnd, c.missDownstream,
        c.skipped, c.skippedDead, c.skippedDual, c.skippedPaused, c.skippedThrottle, c.skippedFrame, c.skippedJobs, c.skippedOther,
        c.deferredSpawns, c.deferredExpired, c.shadowSteps, c.shadowMismatches,
        m_simEma, m_simPeakMs, m_stepCostUs, m_stepBudget, runningClones(), m_clones.size(), c.clonesCreated, m_jobs.size(), m_throttled ? 1 : 0) + v2;
}

void CloneEngine::frameEnd(float dt) {
    if (!m_pl || m_offForVisit) return;   // stopForVisit: no extension steps, no pool growth, no summaries
    double t0 = nowMs();
    m_frameSlow = dt > SLOW_FRAME_DT;
    extensionSteps();
    double spent = nowMs() - t0;
    m_simMs += spent;
    m_simFrameMs += spent;
    // the worst rendered frame since the last summary: a hitch the 1 s average would hide
    if (m_simFrameMs > m_simPeakMs) m_simPeakMs = m_simFrameMs;
    // adaptive budget (core/solver/budget.hpp): the measured cost of a clone step sets how many
    // clone steps the next rendered frame may run for kBudget.targetSimMsPerFrame
    m_stepCostUs = solver::budget::updateStepCostUs(m_stepCostUs, m_simFrameMs, m_cloneStepsThisFrame);
    m_stepBudget = solver::budget::stepBudgetForFrame(m_stepCostUs);
    m_simFrameMs = 0.0;
    m_localStepsLastFrame = std::max(0, m_cloneStepsThisFrame - m_seqStepsThisFrame);
    m_cloneStepsThisFrame = 0;
    m_createdThisFrame = 0;
    m_seqStepsThisFrame = 0;
    m_seqCreatedThisFrame = 0;
    m_simFrames++;
    double now = nowMs();
    if (now - m_lastSummaryLog > 5000.0) {
        m_lastSummaryLog = now;
        if (m_counters.inputs > 0 || m_counters.shadowSteps > 0) slog(0, summaryLine());
        if (m_seqCounters.groups > 0) slog(0, seqSummaryLine());
        if (m_counters.pairShadowSteps > 0) {
            slog(0, fmt::format("GPRL dual: 5 s - pair shadow {} steps / {} mismatches (this attempt {} / {}), dual inputs skipped {} (shadow only: not measured yet)",
                                m_counters.pairShadowSteps, m_counters.pairShadowMismatches, m_attempt.pairShadowSteps, m_attempt.pairShadowMismatches, m_counters.skippedDual));
        }
        m_simPeakMs = 0.0;
    }
    if (now - m_lastPerfLog > 1000.0 && m_simFrames > 0) {
        double avg = m_simMs / m_simFrames;
        m_simEma = m_simEma <= 0.0 ? avg : 0.6 * m_simEma + 0.4 * avg;
        if (!m_throttled && m_simEma > THROTTLE_ON_MS) {
            m_throttled = true;
            log::warn("GPRL solver: simulation load {:.1f} ms/frame, pausing new measurements until it drops", m_simEma);
        }
        else if (m_throttled && m_simEma < THROTTLE_OFF_MS) {
            m_throttled = false;
            log::info("GPRL solver: load back to {:.1f} ms/frame, measuring again", m_simEma);
        }
        slog(2, fmt::format("GPRL solver perf: {:.3f} ms/frame in clone sim (peak {:.1f}, {:.2f} us/step, budget {} steps/frame), {} clone steps, {} jobs, {} clones running (pool {}), step {} frame {:.1f} dt {:.5f} tick {:.5f}",
            avg, m_simPeakMs, m_stepCostUs, m_stepBudget, m_cloneSteps, m_jobs.size(), runningClones(), m_clones.size(), m_step, m_frame, m_stepDt, m_tickDt));
        m_simMs = 0;
        m_simFrames = 0;
        m_cloneSteps = 0;
        m_lastPerfLog = now;
    }
}

// ---- sequence jobs (v0.6.1, docs/SOLVER_DESIGN.md §13) ----
//
// The search is core/solver/sequence (pure, host-tested); this part only turns a lattice point
// into a delayed clone replay and back into pass / fail. A sequence clone uses the same simStep
// as every other clone: `Clone::moved` replaces the group's logged inputs by their shifted copies.

namespace {

constexpr auto const& kSeq = gprl::solver::kSequence;
constexpr size_t kSeqLocalsKept = 16;
constexpr int kSeqCloneJob = -2;   // Clone::job of a sequence clone (never an index into m_jobs)

// v0.7.0: the SA jobs run on the M4 machinery with budget/3's SA budget (the same arithmetic,
// re-tuned shares: core/solver/budget.hpp SABudget)
gprl::solver::SequenceConfig makeSASequenceConfig() {
    gprl::solver::SequenceConfig c;
    auto const& b = gprl::solver::budget::kSABudget;
    auto const& sa = gprl::solver::kSA;
    c.frameShare = b.frameShare;
    c.targetMsPerFrame = b.targetMsPerFrame;
    c.idleLoadShare = b.idleLoadShare;
    c.stepsPerClonePerStep = b.stepsPerClonePerStep;
    c.parallelClones = b.parallelClones;
    c.poolReserve = b.poolReserve;
    c.cloneCreatesPerFrame = b.cloneCreatesPerFrame;
    c.convergeSteps = sa.convergeSteps;
    c.ringReserveSteps = sa.ringReserveSteps;
    c.queueMax = sa.queueMax;
    c.requireNegativeControl = sa.requireNegativeControl;
    return c;
}
gprl::solver::SequenceConfig const kSASeq = makeSASequenceConfig();

// samePhysics against the real player's RECORDED state (the ring snapshot taken at the start of
// the next step = the state after this one): a delayed replay has no live player to compare with.
bool samePhysicsAsSnapshot(PlayerObject* a, PlayerState const& s) {
    if (ccpDistance(a->getPosition(), s.pos) > 0.02f) return false;
    if (std::fabs(a->m_yVelocity - s.f.m_yVelocity) > 1e-4) return false;
    if (a->m_isOnGround != s.f.m_isOnGround || a->m_isUpsideDown != s.f.m_isUpsideDown) return false;
    if (a->m_isShip != s.f.m_isShip || a->m_isBall != s.f.m_isBall || a->m_isBird != s.f.m_isBird || a->m_isDart != s.f.m_isDart
        || a->m_isRobot != s.f.m_isRobot || a->m_isSpider != s.f.m_isSpider || a->m_isSwing != s.f.m_isSwing) return false;
    if (a->m_isDashing != s.f.m_isDashing || a->m_isOnSlope != s.f.m_isOnSlope || a->m_jumpBuffered != s.f.m_jumpBuffered) return false;
    if (a->m_vehicleSize != s.f.m_vehicleSize || a->m_playerSpeed != s.f.m_playerSpeed) return false;
    if (a->m_holdingButtons != s.f.m_holdingButtons) return false;
    int ra = a->m_touchingRings ? static_cast<int>(a->m_touchingRings->count()) : 0;
    return ra == static_cast<int>(s.rings.size());
}

// statesMatch against the recorded state, same tolerances: the control's exactness proof.
bool matchesSnapshot(PlayerObject* a, PlayerState const& s, std::string& why) {
    auto pa = a->getPosition();
    if (std::fabs(pa.x - s.pos.x) > 1e-3f || std::fabs(pa.y - s.pos.y) > 1e-3f) {
        why = fmt::format("position off by ({:.4f},{:.4f})", pa.x - s.pos.x, pa.y - s.pos.y);
        return false;
    }
    if (std::fabs(a->m_yVelocity - s.f.m_yVelocity) > 1e-6) {
        why = fmt::format("y velocity {:.5f} vs {:.5f}", a->m_yVelocity, s.f.m_yVelocity);
        return false;
    }
    if (a->m_isOnGround != s.f.m_isOnGround) { why = "ground flag"; return false; }
    if (a->m_isUpsideDown != s.f.m_isUpsideDown) { why = "gravity"; return false; }
    if (a->m_isShip != s.f.m_isShip || a->m_isBall != s.f.m_isBall || a->m_isBird != s.f.m_isBird || a->m_isDart != s.f.m_isDart
        || a->m_isRobot != s.f.m_isRobot || a->m_isSpider != s.f.m_isSpider || a->m_isSwing != s.f.m_isSwing) { why = "game mode"; return false; }
    if (a->m_isDashing != s.f.m_isDashing) { why = "dash"; return false; }
    if (a->m_isOnSlope != s.f.m_isOnSlope) { why = "slope flag"; return false; }
    if (a->m_vehicleSize != s.f.m_vehicleSize) { why = "size"; return false; }
    if (a->m_playerSpeed != s.f.m_playerSpeed) { why = "speed"; return false; }
    if (a->m_holdingButtons != s.f.m_holdingButtons) { why = "held buttons"; return false; }
    int ra = a->m_touchingRings ? static_cast<int>(a->m_touchingRings->count()) : 0;
    if (ra != static_cast<int>(s.rings.size())) { why = fmt::format("touching rings {} vs {}", ra, s.rings.size()); return false; }
    if (std::fabs(a->m_lastPosition.x - s.lastPosition.x) > 1e-3f || std::fabs(a->m_lastPosition.y - s.lastPosition.y) > 1e-3f) {
        why = "last position";
        return false;
    }
    return true;
}

}  // namespace

void CloneEngine::setSequences(bool on) {
    if (on == m_seqOn) return;
    m_seqOn = on;
    if (!on && m_pl) {
        // only the M4 joint-share work: a running sequence-adjusted job is not this setting's
        if (m_seqActive && m_seq.kind == SeqKind::JointShare) dropSequence(m_seq, "off", "");
        if (!m_seqQueue.empty()) {
            m_seqCounters.skipReset += static_cast<int>(m_seqQueue.size());
            m_seqQueue.clear();
        }
        m_seqLocals.clear();
    }
    slog(0, fmt::format("GPRL sequence: joint-share jobs {} (measure-joint-share setting)", on ? "on" : "off"));
}

void CloneEngine::setSequenceAdjusted(bool on) {
    if (on == m_saOn) return;
    m_saOn = on;
    if (!on && m_pl) {
        if (m_seqActive && m_seq.kind == SeqKind::SequenceAdjusted) dropSequence(m_seq, "off", "");
        compAbortAll(solver::status::Reason::SaNotMeasuredBudget);
        saAbortAll(solver::status::Reason::SaNotMeasuredBudget);
        drainResults();
    }
    slog(0, fmt::format("GPRL sa: sequence-adjusted windows {} (measure-sequence-adjusted setting)", on ? "on" : "off"));
}

std::string CloneEngine::seqName(SeqJob const& job) const {
    if (job.kind == SeqKind::SequenceAdjusted) {
        std::string s;
        for (size_t i = 0; i < job.saMembers.size(); ++i) {
            auto const& m = job.saMembers[i];
            s += fmt::format("{}#{} {}", i ? " + " : "", m.index, m.down ? "press" : "release");
        }
        return fmt::format("inputs {} ({})", s, job.saMembers.size());
    }
    std::string inputs, seqs, gaps;
    for (size_t i = 0; i < job.members.size(); ++i) {
        auto const& m = job.members[i];
        inputs += fmt::format("{}#{} {}", i ? " + " : "", m.inputId, m.down ? "press" : "release");
        seqs += fmt::format("{}{}", i ? ", " : "", m.inputSeq);
        if (i > 0) gaps += fmt::format("{}{:.2f}", i > 1 ? ", " : "", m.t - job.members[i - 1].t);
    }
    return fmt::format("{} {} (seq {}; gap {} ticks)", job.members.size() == 2 ? "pair" : "triple", inputs, seqs, gaps);
}

// A local job finished with an emitted HIT window: remember what the sequence solver needs of it
// and look for neighbours whose windows are finished too.
void CloneEngine::noteLocalDone(Job const& job, JobResult const& r) {
    if (!m_seqOn || !m_pl) return;
    LocalDone d;
    d.inputId = job.inputId;
    d.inputSeq = job.inputSeq;
    d.attemptId = job.attemptId;
    d.eventT = job.eventT;
    d.tick = job.tick;
    d.t = job.t;
    d.step = job.step;
    d.down = job.down;
    d.subtick = job.subtick;
    d.x = job.pre.x;
    d.info = solver::localWindowInfo(r.window);
    if (!d.info.valid || !d.info.hit) return;
    m_seqLocals.push_back(std::move(d));
    while (m_seqLocals.size() > kSeqLocalsKept) m_seqLocals.pop_front();
    considerGroups(job.inputId);
}

// Every pair / triple of NEIGHBOURS in the attempt's input log that contains `inputId` and whose
// members all have a finished local window (core/solver/sequence groupEndingAt decides the gaps).
void CloneEngine::considerGroups(uint32_t inputId) {
    if (!m_seqOn || m_seqClosed || !m_pl || m_log.empty() || m_pl->m_hasCompletedLevel) return;
    int p = -1;
    for (int i = static_cast<int>(m_log.size()) - 1; i >= 0; --i) {
        if (m_log[static_cast<size_t>(i)].id == inputId) {
            p = i;
            break;
        }
    }
    if (p < 0) return;
    int const lo = std::max(0, p - 2);
    int const hi = std::min(static_cast<int>(m_log.size()) - 1, p + 2);
    std::vector<double> frames;
    for (int i = lo; i <= hi; ++i) frames.push_back(m_log[static_cast<size_t>(i)].t);
    auto doneOf = [&](uint32_t id) -> LocalDone const* {
        for (auto const& d : m_seqLocals) {
            if (d.inputId == id) return &d;
        }
        return nullptr;
    };
    for (int size = 2; size <= std::min(solver::kSequenceMaxInputs, kSeq.maxGroupSize); ++size) {
        for (int last = p; last <= std::min(hi, p + size - 1); ++last) {
            auto group = solver::sequence::groupEndingAt(frames, last - lo, size, kSeq);
            if (group.empty()) continue;
            std::vector<LocalDone> members;
            for (int gi : group) {
                auto const* d = doneOf(m_log[static_cast<size_t>(lo + gi)].id);
                if (!d) break;
                members.push_back(*d);
            }
            if (static_cast<int>(members.size()) == size) queueGroup(std::move(members));
        }
    }
}

// A candidate group: counted once per attempt, then every reason it cannot be measured is
// counted and logged by name; what passes waits in the queue for an idle solver.
void CloneEngine::queueGroup(std::vector<LocalDone> members) {
    int const n = static_cast<int>(members.size());
    uint64_t key = (static_cast<uint64_t>(members.front().inputId) << 4) | static_cast<uint64_t>(n);
    if (!m_seqSeen.insert(key).second) return;
    ++m_seqCounters.groups;
    ++(n == 2 ? m_seqCounters.pairs : m_seqCounters.triples);

    SeqJob job;
    job.members = std::move(members);
    job.subtick = job.members.front().subtick;
    std::string const name = seqName(job);
    auto skip = [&](int& counter, std::string const& why) {
        ++counter;
        slog(2, fmt::format("GPRL sequence: {} not measured: {}", name, why));
    };
    for (auto const& m : job.members) {
        if (m.subtick != job.subtick || m.attemptId != job.members.front().attemptId) return skip(m_seqCounters.skipInvalid, "the inputs were measured in different modes");
    }
    if (m_seqJobsThisAttempt >= kSeq.maxJobsPerAttempt) return skip(m_seqCounters.skipAttemptCap, fmt::format("{} sequences per attempt reached", kSeq.maxJobsPerAttempt));
    if (n >= 3 && m_seqTriplesThisAttempt >= kSeq.maxTriplesPerAttempt) return skip(m_seqCounters.skipAttemptCap, fmt::format("{} triples per attempt reached", kSeq.maxTriplesPerAttempt));
    job.positionKey = solver::sequence::positionKey(job.members.front().x, n, kSeq);
    if (auto it = m_seqPlaces.find(job.positionKey); it != m_seqPlaces.end() && it->second >= kSeq.maxPerPosition)
        return skip(m_seqCounters.skipPosition, fmt::format("this place (x {:.0f}) was already measured {} times on this level visit", job.members.front().x, it->second));
    // negative control: the member whose known death lies nearest to its window
    double nearest = 0.0;
    for (int i = 0; i < n; ++i) {
        auto const& info = job.members[static_cast<size_t>(i)].info;
        if (!info.hasFail) continue;
        if (job.negMember < 0 || std::fabs(info.failShiftMs) < nearest) {
            job.negMember = i;
            nearest = std::fabs(info.failShiftMs);
        }
    }
    if (kSeq.requireNegativeControl && job.negMember < 0)
        return skip(m_seqCounters.skipNoNegative, "no input of the group has a known death next to its window (nothing proves that a delayed replay still dies here)");
    job.horizonFrame = solver::timeline::horizonFrame(job.members.back().t, m_cfg.horizonSeconds, m_cfg.maxShiftTicks);
    double const spanStart = job.members.front().t - static_cast<double>(m_cfg.maxShiftTicks) - 1.0;
    for (auto const& mk : m_deathMarks) {
        double const frame = mk.frame;
        if (frame >= spanStart && frame <= job.horizonFrame + 1.0) return skip(m_seqCounters.skipDeath, fmt::format("a death at frame {:.1f} lies inside the look-ahead", frame));
    }
    std::vector<solver::SequenceAxis> axes;
    std::vector<double> times;
    std::string shape;
    job.baseFrame = job.members.front().t;
    for (int i = 0; i < n; ++i) {
        auto const& m = job.members[static_cast<size_t>(i)];
        auto axis = solver::makeAxis(m.info, job.subtick, n == 2 ? kSeq.maxAtomsPerAxisPair : kSeq.maxAtomsPerAxisTriple);
        if (!axis.valid) return skip(m_seqCounters.skipInvalid, fmt::format("input #{}: {}", m.inputId, axis.invalidReason));
        double earliest = axis.positions.front();
        if (i == job.negMember) earliest = std::min(earliest, m.info.failShiftMs / kTickMs);
        job.baseFrame = std::min(job.baseFrame, m.t + std::min(0.0, earliest));
        shape += fmt::format("{}{}", i ? "x" : "", axis.size());
        axes.push_back(std::move(axis));
        times.push_back(m.t);
    }
    job.planner = solver::SequencePlanner(kSeq, std::move(axes), std::move(times));
    if (!job.planner.valid()) return skip(m_seqCounters.skipInvalid, job.planner.invalidReason());
    if (job.planner.trivial()) {
        ++m_seqCounters.trivial;
        slog(2, fmt::format("GPRL sequence: {} is trivial ({} atoms: every combination is a local trial or swaps the input order): nothing to measure", name, shape));
        return;
    }
    job.baseStep = solver::timeline::stepForFrame(job.members.front().step, job.baseFrame, [this](int k, double& f) { return frameOf(k, f); });
    if (job.baseStep < 1) return skip(m_seqCounters.skipExpired, "the history ring does not reach back to the first shifted input");
    if (static_cast<int>(m_seqQueue.size()) >= kSeq.queueMax) return skip(m_seqCounters.skipQueue, fmt::format("{} sequences are already waiting for an idle solver", m_seqQueue.size()));
    job.id = m_seqNextId++;
    job.queuedStep = m_step;
    ++m_seqJobsThisAttempt;
    if (n >= 3) ++m_seqTriplesThisAttempt;
    slog(2, fmt::format("GPRL sequence: job {} queued: {}, {} atoms, {} combinations to simulate first, base step {} ({} steps old), {} waiting", job.id, name, shape,
        job.planner.requested(), job.baseStep, m_step - job.baseStep, m_seqQueue.size()));
    m_seqQueue.push_back(std::move(job));
}

// An idle clone beyond the local jobs' reserve, or (rationed) a new one. -1 = none right now.
int CloneEngine::seqAllocClone() {
    int idle = 0, first = -1;
    for (size_t i = 0; i < m_clones.size(); ++i) {
        if (m_clones[i].state != CloneStatus::Idle) continue;
        ++idle;
        if (first < 0) first = static_cast<int>(i);
    }
    auto grant = solver::sequence::cloneGrant(idle, static_cast<int>(m_clones.size()), kMaxClones, m_createdThisFrame, m_seqCreatedThisFrame,
                                              kBudget.cloneCreatesPerFrame, seqCfg(m_seqActive ? m_seq.kind : SeqKind::JointShare));
    if (grant == solver::sequence::CloneGrant::Idle) return first;
    if (grant != solver::sequence::CloneGrant::Create) return -1;
    auto obj = makeClone();
    if (!obj) return -1;
    ++m_createdThisFrame;
    ++m_seqCreatedThisFrame;
    ++m_counters.clonesCreated;
    Clone c;
    c.obj = obj;
    freeClone(c);
    m_index[obj] = static_cast<int>(m_clones.size());
    m_clones.push_back(std::move(c));
    return static_cast<int>(m_clones.size()) - 1;
}

solver::SequenceConfig const& CloneEngine::seqCfg(SeqKind kind) const { return kind == SeqKind::SequenceAdjusted ? kSASeq : kSeq; }

int CloneEngine::seqAllowance() const {
    return solver::sequence::stepAllowance(m_stepBudget, std::max(0, m_cloneStepsThisFrame - m_seqStepsThisFrame), m_localStepsLastFrame, m_seqStepsThisFrame,
                                           m_stepCostUs, seqCfg(m_seqActive ? m_seq.kind : SeqKind::JointShare));
}

bool CloneEngine::startSequence(SeqJob& job) {
    job.startedStep = m_step;
    job.startedMs = nowMs();
    job.phase = SeqPhase::ControlFirst;
    m_seqCtx = Job{};
    m_seqCtx.inputId = kNoInputId;   // no single replaced input: Clone::moved carries the group
    m_seqCtx.down = false;
    m_seqCtx.subtick = job.subtick;
    if (job.kind == SeqKind::SequenceAdjusted) {
        ++m_saCounters.jobs;
        return true;   // saStart logged the job
    }
    ++m_seqCounters.jobs;
    slog(2, fmt::format("GPRL sequence: job {} started after waiting {} steps: base step {} is {} steps old (ring {}), look-ahead to frame {:.1f}, placement {}, negative control = input #{} at {:+.2f} ticks",
        job.id, m_step - job.queuedStep, job.baseStep, m_step - job.baseStep, kHistorySteps, job.horizonFrame, job.subtick ? "sub-tick" : "whole ticks",
        job.negMember >= 0 ? job.members[static_cast<size_t>(job.negMember)].inputId : 0u,
        job.negMember >= 0 ? job.members[static_cast<size_t>(job.negMember)].info.failShiftMs / kTickMs : 0.0));
    return true;
}

// One delayed replay from the job's base snapshot. Control: nothing moved. Negative: one member
// at the shift the local solver saw die. Sample: every member at the lattice point's shift.
int CloneEngine::spawnSeqTrial(SeqJob& job, SeqRole role, solver::SequenceTrial const* point, int idx, solver::SATrial const* saTrial) {
    if (job.baseStep < 1) return -1;
    auto& h = m_hist[ring(job.baseStep)];
    if (h.step != job.baseStep) return -1;
    if (idx < 0) idx = seqAllocClone();
    if (idx < 0) return 0;
    auto& c = m_clones[static_cast<size_t>(idx)];
    applyState(c.obj, h.state);
    c.flags.clear();
    for (auto& [o, f] : h.flags) c.flags[o] = f;
    c.state = CloneStatus::Running;
    c.job = kSeqCloneJob;
    c.pass = 0;
    c.shift = 0.0;
    c.inputFrame = 0.0;
    c.inputPending = false;
    c.appliedFrame = solver::kNaN;
    c.stepDone = job.baseStep - 1;
    c.frameDone = h.frame;
    c.deathStep = -1;
    c.deathObjId = -1;
    c.converged = 0;
    c.resynced = false;
    c.extension = false;
    c.moved.clear();
    c.attributeAfterFrame = 1e300;
    c.laterFixed = 0;
    SeqTrial t;
    if (job.kind == SeqKind::SequenceAdjusted) {
        // v0.7.0 SA trial (docs/TIMING_SOLVER_V2.md §3.3): the member and its followers at their
        // shifted frames; laterFixed counts the non-moved later inputs (§2.3)
        if (role == SeqRole::Negative && job.saNegative.valid) {
            c.moved.push_back({job.saNegative.inputId, job.saNegative.down, job.saNegative.frame, true, solver::kNaN});
        }
        else if (role == SeqRole::Sample && saTrial) {
            for (auto const& mv : saTrial->moved) c.moved.push_back({mv.id, mv.down, mv.frame, true, solver::kNaN});
            c.attributeAfterFrame = saTrial->attributeAfterFrame;
            t.saTrialId = saTrial->id;
            t.lookAheadFrame = saTrial->lookAheadFrame;
            t.saMember = saTrial->member;
            t.saAdaptation = static_cast<int>(saTrial->adaptation);
            t.saShift = saTrial->shiftFrames;
            // debug trace (§2.12 second block): the SA trials of an input whose local window is traced
            if (saTrial->member >= 0 && static_cast<size_t>(saTrial->member) < job.saMembers.size()) {
                if (ResultEntry const* e = m_ledger.data(job.saMembers[static_cast<size_t>(saTrial->member)].jobId); e && e->traced) {
                    c.tracing = true;
                    c.trace.shiftFrames = saTrial->shiftFrames;
                }
            }
        }
        ++m_saCounters.trials;
    }
    else if (role == SeqRole::Negative && job.negMember >= 0) {
        auto const& m = job.members[static_cast<size_t>(job.negMember)];
        c.moved.push_back({m.inputId, m.down, m.t + m.info.failShiftMs / kTickMs, true, solver::kNaN});
    }
    else if (role == SeqRole::Sample && point) {
        for (size_t i = 0; i < job.members.size(); ++i) {
            auto const& m = job.members[i];
            c.moved.push_back({m.inputId, m.down, m.t + point->shiftFrames[i], true, solver::kNaN});
        }
    }
    t.clone = idx;
    t.role = role;
    t.pointId = point ? point->id : -1;
    job.running.push_back(t);
    ++job.trials;
    if (job.kind == SeqKind::JointShare) ++m_seqCounters.trials;
    return 1;
}

// After a step of a sequence clone: died / became invalid / re-joined the recorded real run /
// reached the look-ahead. Returns true when the trial is over (its clone is free again).
bool CloneEngine::seqAfterStep(SeqJob& job, SeqTrial& trial, bool physicsRan, std::string& fail, std::string& failKind) {
    auto& c = m_clones[static_cast<size_t>(trial.clone)];
    bool const firstControl = job.phase == SeqPhase::ControlFirst;
    auto over = [&] {
        freeClone(c);
        trial.clone = -1;
        return true;
    };
    bool const sa = job.kind == SeqKind::SequenceAdjusted;
    if (sa && trial.role == SeqRole::Sample) {
        // v0.7.0 SA trial (docs/TIMING_SOLVER_V2.md §3.3): died (attributed) / invalid / re-joined
        // the recorded run for convergeSteps after every moved input applied / alive at its own
        // look-ahead with every moved input applied
        solver::SAOutcome o;
        o.id = trial.saTrialId;
        auto overSA = [&] {
            if (c.tracing && job.saTraced.size() < 96) {
                // keep the traced trial until the job's SA edges are known (saMembersDone)
                solver::trace::SATraced rec;
                rec.adaptation = trial.saAdaptation;
                rec.shiftFrames = trial.saShift;
                rec.clone = c.trace;
                rec.clone.passed = o.kind == solver::SAOutcome::Kind::Pass;
                job.saTraced.emplace_back(trial.saMember, std::move(rec));
            }
            freeClone(c);
            trial.clone = -1;
            return true;
        };
        if (c.state == CloneStatus::Dead) {
            o.kind = solver::SAOutcome::Kind::Died;
            o.deathFrame = c.deathFrame;
            o.laterFixed = c.laterFixed;
            o.objectId = c.deathObjId;
            o.deathX = c.deathX;
            o.extension = c.extension;
            job.sa.ingest(o);
            return overSA();
        }
        if (c.state != CloneStatus::Running) {
            o.kind = solver::SAOutcome::Kind::Invalid;
            o.reason = c.invalidReason.empty() ? "invalid" : c.invalidReason;
            job.sa.ingest(o);
            return overSA();
        }
        bool applied = true;
        for (auto const& m : c.moved) applied = applied && !m.pending;
        auto const& next = m_hist[ring(c.stepDone + 1)];
        bool const have = next.step == c.stepDone + 1;
        bool rejoined = false;
        if (physicsRan && have) mirrorNextStepSpeed(c);
        if (physicsRan && have && applied) {
            if (samePhysicsAsSnapshot(c.obj, next.state)) rejoined = ++trial.converged >= solver::kSA.convergeSteps;
            else trial.converged = 0;
        }
        if (!applied && c.frameDone >= trial.lookAheadFrame - 1e-9) {
            o.kind = solver::SAOutcome::Kind::Invalid;
            o.reason = "a moved input was never applied";
            job.sa.ingest(o);
            return overSA();
        }
        if (!rejoined && !(applied && c.frameDone >= trial.lookAheadFrame - 1e-9)) return false;
        o.kind = solver::SAOutcome::Kind::Pass;
        o.rejoined = rejoined;
        job.sa.ingest(o);
        return overSA();
    }
    if (c.state == CloneStatus::Dead) {
        switch (trial.role) {
            case SeqRole::Control: {
                // v0.7.1: a real / would-be death that came AFTER the job started (saStart only sees
                // the deaths before it) lies inside its span: the control dying WITH it is the
                // delayed replay reproducing the real run, not failing to - nothing can be proven
                // beyond that death (sa_death_in_span), and the replay proof did not fail. Seen in
                // the owner's 19.47.37 log ("the first control died on #216 .. frame 867.0" next to
                // "death at frame 867.0"), where such jobs were dropped as control mismatches.
                bool withReal = false;
                if (sa) {
                    for (auto const& mk : m_deathMarks) {
                        if (solver::diedWithReal(c.deathFrame, c.deathObjId, c.deathX, mk.frame, mk.obj, mk.x)) withReal = true;
                    }
                }
                if (withReal) {
                    failKind = "death";
                    fail = fmt::format("the {} control died on #{} at frame {:.1f} together with the real player: a death inside the job's span, nothing to prove beyond it",
                        firstControl ? "first" : "last", c.deathObjId, c.deathFrame);
                    break;
                }
                failKind = "control";
                fail = fmt::format("the {} control died on #{} at x={:.0f} (step {}, frame {:.1f}) but the real player did not: the delayed replay does not reproduce the real run here",
                    firstControl ? "first" : "last", c.deathObjId, c.deathObjX, c.deathStep, c.deathFrame);
                break;
            }
            case SeqRole::Negative: {
                if (sa) {
                    // v0.7.0: the SA proof is strict - the SAME death (object, frame +-1) as in
                    // lockstep, not just any death (every negative control in the owner's v0.6.x
                    // logs died "as known", so this costs nothing where the replay is sound)
                    auto const& n = job.saNegative;
                    if (c.deathObjId != n.objectId || std::fabs(c.deathFrame - n.deathFrame) > 1.0 + 1e-6) {
                        failKind = "negative";
                        fail = fmt::format("input id {} at {:+.2f} ticks died on #{} at frame {:.1f} in lockstep but on #{} at frame {:.1f} in the delayed replay: "
                                           "hazards are not reproduced back there", n.inputId, n.shift, n.objectId, n.deathFrame, c.deathObjId, c.deathFrame);
                        break;
                    }
                    job.negText = fmt::format("negative control died on #{} as known (input id {} at {:+.2f} ticks, frame {:.1f})", c.deathObjId, n.inputId, n.shift,
                                              c.deathFrame);
                    job.phase = SeqPhase::Sampling;
                    break;
                }
                auto const& m = job.members[static_cast<size_t>(job.negMember)];
                job.negText = fmt::format("negative control died on #{}{} (input #{} at {:+.2f} ticks)", c.deathObjId,
                    c.deathObjId == m.info.failObjectId ? " as known" : fmt::format(", the lockstep trial on #{}", m.info.failObjectId), m.inputId, m.info.failShiftMs / kTickMs);
                job.phase = SeqPhase::Sampling;
                break;
            }
            case SeqRole::Sample:
                job.planner.ingest({trial.pointId, false, false});
                break;
        }
        return over();
    }
    if (c.state == CloneStatus::Invalid) {
        std::string why = c.invalidReason.empty() ? std::string("invalid") : c.invalidReason;
        if (trial.role == SeqRole::Sample) job.planner.ingest({trial.pointId, false, true});
        else {
            failKind = why == "history lost" ? "expired" : "invalid";
            fail = fmt::format("the {} became invalid ({})", trial.role == SeqRole::Control ? "control" : "negative control", why);
        }
        return over();
    }
    if (c.state != CloneStatus::Running) return over();

    bool applied = true;
    for (auto const& m : c.moved) applied = applied && !m.pending;
    // the real player's recorded state after this step is the ring snapshot of the next one
    auto const& next = m_hist[ring(c.stepDone + 1)];
    bool const have = next.step == c.stepDone + 1;
    bool rejoined = false;
    if (physicsRan && have) mirrorNextStepSpeed(c);   // the recorded state already carries the next step's speed
    if (trial.role == SeqRole::Control) {
        if (physicsRan && have) {
            std::string why;
            if (!matchesSnapshot(c.obj, next.state, why)) {
                failKind = "control";
                fail = fmt::format("the {} control differs from the recorded run at step {} (frame {:.1f}, {} steps in): {} | control {}", firstControl ? "first" : "last",
                    c.stepDone, c.frameDone, trial.compared, why, stateStr(c.obj));
                return over();
            }
            ++trial.compared;
        }
    }
    else if (physicsRan && have && applied) {
        if (samePhysicsAsSnapshot(c.obj, next.state)) rejoined = ++trial.converged >= kSeq.convergeSteps;
        else trial.converged = 0;
    }
    if (!applied && c.frameDone >= job.horizonFrame - 1e-9) {
        // a shifted input never found room in its step (more than 16 inputs in one step): not a result
        if (trial.role == SeqRole::Sample) job.planner.ingest({trial.pointId, false, true});
        else {
            failKind = "invalid";
            fail = "the negative control's input was never applied before the look-ahead ended";
        }
        return over();
    }
    bool const horizon = applied && c.frameDone >= job.horizonFrame - 1e-9;
    // v0.11.0: a sample passes when it re-joined or SETTLED; alive at the end unsettled = an
    // undecided trial (ingested invalid: the member stays sequence_dependent, never widened)
    bool const settledExit = m_cfg.settle.enabled && applied && trial.role == SeqRole::Sample && c.settle.settled;
    if (!rejoined && !horizon && !settledExit) return false;
    if (trial.role == SeqRole::Sample && !rejoined && !settledExit && horizon && m_cfg.settle.enabled) {
        ++m_counters.unsettled;
        job.planner.ingest({trial.pointId, false, true});
        return over();
    }
    if (settledExit && !rejoined) ++m_counters.settled;
    switch (trial.role) {
        case SeqRole::Control:
            if (trial.compared < 1) {
                failKind = "control";
                fail = "the control was never compared with the recorded run";
                break;
            }
            job.controlSteps[firstControl ? 0 : 1] = trial.compared;
            if (firstControl) job.phase = (sa ? job.saNegative.valid : job.negMember >= 0) ? SeqPhase::Negative : SeqPhase::Sampling;
            else job.controlLastDone = true;
            break;
        case SeqRole::Negative: {
            failKind = "negative";
            if (sa) {
                auto const& n = job.saNegative;
                fail = fmt::format("input id {} at {:+.2f} ticks died on #{} in lockstep but {} in the delayed replay: hazards are not reproduced back there", n.inputId,
                    n.shift, n.objectId, rejoined ? "re-joined the real run" : "survived the look-ahead");
                break;
            }
            auto const& m = job.members[static_cast<size_t>(job.negMember)];
            fail = fmt::format("input #{} at {:+.2f} ticks died on #{} in lockstep but {} in the delayed replay: hazards are not reproduced back there", m.inputId,
                m.info.failShiftMs / kTickMs, m.info.failObjectId, rejoined ? "re-joined the real run" : "survived the look-ahead");
            break;
        }
        case SeqRole::Sample:
            job.planner.ingest({trial.pointId, true, false});
            break;
    }
    return over();
}

// Runs after the local jobs of the step, with what they left of the frame's budget.
void CloneEngine::runSequence(int target) {
    if (!m_pl || !m_pl->m_player1) return;
    // a candidate that waited until its history is about to leave the ring is dropped
    while (!m_seqQueue.empty()) {
        auto const& q = m_seqQueue.front();
        bool gone = m_hist[ring(q.baseStep)].step != q.baseStep || m_step - q.baseStep >= kHistorySteps - kSeq.ringReserveSteps;
        if (!gone) break;
        ++m_seqCounters.skipExpired;
        slog(2, fmt::format("GPRL sequence: job {} never started: the solver was not idle for {} steps and its history left the ring", q.id, m_step - q.queuedStep));
        m_seqQueue.pop_front();
    }
    // v0.7.0: SA candidates whose history is about to leave the ring end undecided (sa_expired).
    // Bounded: every iteration either keeps one candidate (++i) or erases it.
    if (!m_saCands.empty()) {
        for (size_t i = 0; i < m_saCands.size();) {
            auto const& c = m_saCands[i];
            int base = solver::timeline::stepForFrame(c.step, c.t - static_cast<double>(m_cfg.maxShiftTicks) - 1.0, [this](int k, double& f) { return frameOf(k, f); });
            if (base >= 1 && m_step - base < kHistorySteps - kSASeq.ringReserveSteps) {
                ++i;
                continue;
            }
            SACandidate gone = c;
            m_saCands.erase(m_saCands.begin() + static_cast<std::ptrdiff_t>(i));
            saGiveUp(gone, solver::status::Reason::SaExpired, m_saCounters.notStartedExpired);
            slog(2, fmt::format("GPRL sa: input #{} never started: the solver was not idle and its history left the ring", gone.index));
        }
        drainResults();
    }
    // the guards that pause NEW local measurements pause sequence work too, and so does a measured
    // load within the sequence job's own share of the throttle: the throttle counts ALL clone sim
    // time (m_simEma), so sequence work must never be what pushes it over and skips local inputs
    bool const baseGate = m_measuring && !m_throttled && !m_frameSlow;
    bool const saGate = baseGate && m_saOn && solver::sequence::loadAllows(m_simEma, THROTTLE_ON_MS, kSASeq);
    bool const jointGate = baseGate && m_seqOn && solver::sequence::loadAllows(m_simEma, THROTTLE_ON_MS, kSeq);
    if (!m_seqActive) {
        // idle = no local job is waiting for a clone, and the frame's step budget has room
        for (auto const& j : m_jobs) {
            if (!j.pendingShifts.empty()) return;
        }
        int const used = std::max(0, m_cloneStepsThisFrame - m_seqStepsThisFrame);
        if (saGate && !m_saCands.empty()
            && solver::sequence::stepAllowance(m_stepBudget, used, m_localStepsLastFrame, m_seqStepsThisFrame, m_stepCostUs, kSASeq) > 0) {
            m_seqActive = saStart();   // sequence-adjusted jobs first (V2-D5)
            if (!m_seqActive) return;
        }
        else {
            // M4 joint-share jobs only while no SA candidate waits (measure-joint-share, default off)
            if (!jointGate || m_seqQueue.empty() || !m_saCands.empty()) return;
            if (seqAllowance() <= 0) return;
            m_seq = std::move(m_seqQueue.front());
            m_seqQueue.pop_front();
            m_seqActive = startSequence(m_seq);
            if (!m_seqActive) return;
        }
    }
    SeqJob& job = m_seq;
    bool const isSA = job.kind == SeqKind::SequenceAdjusted;
    auto const& cfg = seqCfg(job.kind);
    bool const gate = isSA ? saGate : jointGate;
    // the ring moved past the job's first snapshot: nothing can be spawned any more
    bool const baseGone = m_hist[ring(job.baseStep)].step != job.baseStep;
    bool const baseLate = m_step - job.baseStep >= kHistorySteps - cfg.ringReserveSteps;
    if (!gate) {
        if (baseGone) dropSequence(job, "expired", "the solver was paused until the job's history left the ring");
        return;   // paused: the clones keep their state
    }

    // 1. what the phase needs
    auto spawnOne = [&](SeqRole role) {
        int r = spawnSeqTrial(job, role, nullptr);
        if (r < 0) dropSequence(job, "expired", "the job's first snapshot left the history ring before every trial could start");
        return r;
    };
    switch (job.phase) {
        case SeqPhase::ControlFirst:
        case SeqPhase::Negative:
            if (job.running.empty() && spawnOne(job.phase == SeqPhase::ControlFirst ? SeqRole::Control : SeqRole::Negative) < 0) return;
            break;
        case SeqPhase::Sampling: {
            if (isSA) {
                // v0.7.0 SA sampling: the planner's trials, `parallelClones` at once (§3.3)
                if (baseLate && !job.sa.done()) {
                    job.sa.finish(solver::status::Reason::SaExpired);   // out of history: undecided sides stay undecided
                    slog(2, fmt::format("GPRL sa: job {} stops sampling: its base snapshot is {} steps old", job.id, m_step - job.baseStep));
                }
                // bounded (T-PROG-1, host-tested in sequence_adjusted_tests): each iteration spawns
                // one trial (the planner pops one entry) or breaks; no clone right now = the planner
                // is not asked; a trial that cannot be started is ingested Invalid, never lost
                if (!baseGone) {
                    solver::spawnSATrials(job.sa, static_cast<int>(job.running.size()), cfg.parallelClones, [this] { return seqAllocClone(); },
                                          [&](int idx, solver::SATrial const& t) { return spawnSeqTrial(job, SeqRole::Sample, nullptr, idx, &t) > 0; });
                }
                if (job.sa.done() && !job.controlLastSpawned) {
                    int r = spawnOne(SeqRole::Control);
                    if (r < 0) return;
                    if (r > 0) {
                        job.controlLastSpawned = true;
                        job.phase = SeqPhase::ControlLast;
                    }
                }
                break;
            }
            if (baseLate && !job.planner.done()) {
                job.planner.finish();   // out of history: keep what is measured, no new samples
                slog(2, fmt::format("GPRL sequence: job {} stops sampling: its first snapshot is {} steps old", job.id, m_step - job.baseStep));
            }
            while (!baseGone && static_cast<int>(job.running.size()) < kSeq.parallelClones && job.planner.hasPending()) {
                int idx = seqAllocClone();
                if (idx < 0) break;                    // no clone right now: ask the planner only when one is free
                auto batch = job.planner.nextBatch(1);
                if (batch.empty()) break;
                int r = spawnSeqTrial(job, SeqRole::Sample, &batch.front(), idx);
                if (r <= 0) {
                    job.planner.ingest({batch.front().id, false, true});   // could not be run: an invalid sample
                    break;
                }
            }
            if (job.planner.done() && !job.controlLastSpawned) {
                int r = spawnOne(SeqRole::Control);
                if (r < 0) return;
                if (r > 0) {
                    job.controlLastSpawned = true;
                    job.phase = SeqPhase::ControlLast;   // samples still running are ingested as they finish
                }
            }
            break;
        }
        case SeqPhase::ControlLast:
            break;
    }

    // 2. advance, round robin, within the sequence share of the frame
    int allowance = seqAllowance();
    std::string fail, failKind;
    size_t const count = job.running.size();
    for (size_t k = 0; k < count && allowance > 0 && fail.empty(); ++k) {
        auto& trial = job.running[(m_seqRoundRobin + k) % count];
        if (trial.clone < 0) continue;
        int quota = std::min(cfg.stepsPerClonePerStep, allowance);
        while (quota > 0 && trial.clone >= 0 && fail.empty()) {
            auto& c = m_clones[static_cast<size_t>(trial.clone)];
            if (c.state == CloneStatus::Running && c.stepDone >= target) break;   // caught up with the real timeline
            bool ran = false;
            if (c.state == CloneStatus::Running) {
                ran = simStep(c, m_seqCtx, c.stepDone + 1) >= 0;
                if (ran) {
                    --quota;
                    --allowance;
                    ++m_seqStepsThisFrame;
                    ++job.cloneSteps;
                    if (isSA) ++m_saCounters.cloneSteps;
                    else ++m_seqCounters.cloneSteps;
                    ++trial.steps;
                }
            }
            seqAfterStep(job, trial, ran, fail, failKind);
        }
    }
    ++m_seqRoundRobin;
    std::erase_if(job.running, [](SeqTrial const& t) { return t.clone < 0; });
    if (!fail.empty()) {
        dropSequence(job, failKind.c_str(), fail);
        return;
    }
    // 3. done: every sample ingested and the last control exact
    if (job.phase == SeqPhase::ControlLast && job.controlLastDone && job.running.empty()) finishSequence(job);
}

void CloneEngine::finishSequence(SeqJob& job) {
    if (job.kind == SeqKind::SequenceAdjusted) {
        // v0.7.0: both controls exact and the negative control reproduced: every member's SA
        // result is final (undecided sides stay undecided, never guessed)
        std::string const work = fmt::format("{} trials / {} clone steps over {} real steps ({:.0f} ms)", job.trials, job.cloneSteps, m_step - job.startedStep,
                                             nowMs() - job.startedMs);
        slog(1, fmt::format("GPRL sa: job {} done: controls exact ({} + {} steps), {}, {} | {}", job.id, job.controlSteps[0], job.controlSteps[1],
                            job.negText.empty() ? std::string("no negative control") : job.negText, work, seqName(job)));
        if (m_cfg.verbosity >= 2) {
            for (int i = 0; i < job.sa.members(); ++i) slog(2, fmt::format("GPRL sa: job {} #{}: {}", job.id, job.saMembers[static_cast<size_t>(i)].index, job.sa.describe(i)));
        }
        saMembersDone(job, true, solver::status::Reason::SaUndecided, false);
        m_seqActive = false;
        m_seq = {};
        return;
    }
    SeqResult r;
    r.jobId = job.id;
    for (auto const& m : job.members) r.inputSeqs.push_back(m.inputSeq);
    r.attemptId = job.members.front().attemptId;
    r.eventT = job.members.front().eventT;
    r.tick = job.members.front().tick;
    r.result = job.planner.result();
    auto const& s = r.result;
    std::string const work = fmt::format("{} trials / {} clone steps over {} real steps ({:.0f} ms)", job.trials, job.cloneSteps, m_step - job.startedStep, nowMs() - job.startedMs);
    if (!s.valid) {
        ++m_seqCounters.dropInvalid;
        slog(1, fmt::format("GPRL sequence: job {} {}: DROPPED invalid ({}) | {} | {}", job.id, seqName(job), s.invalidReason, job.planner.describe(), work));
    }
    else {
        ++m_seqCounters.measured;
        ++m_seqPlaces[job.positionKey];
        SeqEmit emit = m_seqSink ? m_seqSink(r) : SeqEmit{0, "NOT SENT (no telemetry sink)"};
        if (emit.status > 0) ++m_seqCounters.emitted;
        else if (emit.status == 0) ++m_seqCounters.unsent;
        else ++m_seqCounters.refused;
        std::string widths;
        for (size_t i = 0; i < s.localWidthsMs.size(); ++i) widths += fmt::format("{}{:.2f}", i ? " x " : "", s.localWidthsMs[i]);
        slog(1, fmt::format("GPRL sequence: job {} {}: share {:.3f} of {} ms ({:.1f} of {:.1f} ms^{}), {} samples (pass {}, fail {}{}; local {}, crossed {} of {} points), res {:.2f} ms{}{}, "
                            "controls exact ({} + {} steps against the ring), {}, {} -> {} ({})",
            job.id, seqName(job), s.jointFeasibleShare, widths, s.jointMeasure, s.productMeasure, s.inputs, s.samples, s.passed, s.failed,
            s.invalidTrials ? fmt::format(", invalid {}", s.invalidTrials) : std::string(), s.knownLocal, s.crossed, s.latticePoints, s.resolutionMs,
            s.budgetExhausted ? " (sample cap)" : "", s.complete ? "" : " (partial)", job.controlSteps[0], job.controlSteps[1],
            job.negText.empty() ? std::string("no negative control") : job.negText, work, emit.text, s.solverVersion));
        if (m_cfg.verbosity >= 2) {
            auto rows = job.planner.map();
            auto const& axes = job.planner.axes();
            for (size_t i = 0; i < rows.size(); ++i) {
                double shift = axes.size() > 1 ? axes[1].positions[rows.size() - 1 - i] : 0.0;
                slog(2, fmt::format("GPRL sequence: job {} map  input 2 at {:+6.2f} |{}|  (input 1 from {:+.2f} to {:+.2f} ticks; # pass, . fail, x order, o local, ! invalid)", job.id, shift,
                    rows[i], axes[0].positions.front(), axes[0].positions.back()));
            }
        }
    }
    m_seqActive = false;
    m_seq = {};
}

void CloneEngine::dropSequence(SeqJob& job, char const* kind, std::string const& why) {
    for (auto const& t : job.running) {
        if (t.clone >= 0 && t.clone < static_cast<int>(m_clones.size())) freeClone(m_clones[static_cast<size_t>(t.clone)]);
    }
    std::string_view k = kind ? kind : "reset";
    if (job.kind == SeqKind::SequenceAdjusted) {
        // v0.7.0: a failed proof (control / negative) makes the SA part sequence_dependent and the
        // replay proof invalid for these inputs; expiry / restart just leave them undecided
        using R = solver::status::Reason;
        R why2 = R::SaCutByRestart;
        bool proof = false;
        if (k == "control") { ++m_saCounters.dropControl; why2 = R::SaControlMismatch; proof = true; }
        else if (k == "death") { ++m_saCounters.dropDeath; why2 = R::SaDeathInSpan; }   // v0.7.1: the replay reproduced a real death
        else if (k == "negative") { ++m_saCounters.dropNegative; why2 = R::SaNegativeNotReproduced; proof = true; }
        else if (k == "invalid") { ++m_saCounters.dropInvalid; why2 = R::SaInvalidTrials; }
        else if (k == "expired") { ++m_saCounters.dropExpired; why2 = R::SaExpired; }
        else if (k == "off") { ++m_saCounters.dropReset; why2 = R::SaNotMeasuredBudget; }
        else ++m_saCounters.dropReset;   // reset / teardown / level end
        slog(1, fmt::format("GPRL sa: job {} DROPPED {}{} | {} | {} trials / {} clone steps over {} real steps", job.id, k,
                            why.empty() ? std::string() : ": " + why, seqName(job), job.trials, job.cloneSteps, m_step - job.startedStep));
        saMembersDone(job, false, why2, proof);
        m_seqActive = false;
        m_seq = {};
        return;
    }
    if (k == "control") ++m_seqCounters.dropControl;
    else if (k == "negative") ++m_seqCounters.dropNegative;
    else if (k == "invalid") ++m_seqCounters.dropInvalid;
    else if (k == "expired") ++m_seqCounters.dropExpired;
    else if (k == "level_end") ++m_seqCounters.dropLevelEnd;
    else ++m_seqCounters.dropReset;   // reset / teardown / off
    slog(1, fmt::format("GPRL sequence: job {} {}: DROPPED {}{} | {} | {} trials / {} clone steps over {} real steps", job.id, seqName(job), k,
        why.empty() ? std::string() : " (" + why + ")", job.planner.describe(), job.trials, job.cloneSteps, m_step - job.startedStep));
    m_seqActive = false;
    m_seq = {};
}

void CloneEngine::abortSequences(char const* kind) {
    if (m_seqActive) dropSequence(m_seq, kind, "");
    if (!m_seqQueue.empty()) {
        m_seqCounters.skipReset += static_cast<int>(m_seqQueue.size());
        slog(2, fmt::format("GPRL sequence: {} waiting job(s) cut ({})", m_seqQueue.size(), kind ? kind : "reset"));
        m_seqQueue.clear();
    }
}

// The 5 s summary of the sequence solver: every reason a group was not measured, by name.
std::string CloneEngine::seqSummaryLine() const {
    auto const& c = m_seqCounters;
    return fmt::format(
        "GPRL sequence: 5 s summary - groups {} (pairs {}, triples {}), measured {} (emitted {}, not sent {}, refused {}), trivial {}, "
        "dropped {} (control {}, negative {}, invalid {}, history {}, restart {}, level end {}), "
        "not started {} (attempt cap {}, place already measured {}, no known death {}, death in look-ahead {}, queue full {}, waited too long {}, cut by restart {}, other {}), "
        "waiting {}, running {}, {} trials / {} clone steps, at most {} of the step budget and {} ms per frame when idle, v={}",
        c.groups, c.pairs, c.triples, c.measured, c.emitted, c.unsent, c.refused, c.trivial,
        c.dropped(), c.dropControl, c.dropNegative, c.dropInvalid, c.dropExpired, c.dropReset, c.dropLevelEnd,
        c.notStarted(), c.skipAttemptCap, c.skipPosition, c.skipNoNegative, c.skipDeath, c.skipQueue, c.skipExpired, c.skipReset, c.skipInvalid,
        m_seqQueue.size(), m_seqActive ? 1 : 0, c.trials, c.cloneSteps, fmt::format("{:.0f}%", kSeq.frameShare * 100.0), kSeq.targetMsPerFrame, kSeq.version);
}

// ============================================================================================
// v0.7.0 (docs/TIMING_SOLVER_V2.md): one timing_result per bound job, sequence-adjusted jobs on
// the M4 machinery, cluster links, shadow / portal marks, the replay breaker and debug traces.
// The decisions are the pure core's (core/solver/{sequence_adjusted, timing_status, cluster,
// result_ledger, timing_result_event, trace}); this part only feeds it what the game produced.
// ============================================================================================

namespace {

using solver::status::Reason;
namespace cl = gprl::solver::cluster;

std::string edgeText(telemetry::TimingEdgeV2Payload const& e) {
    if (e.stop != "fail" || !e.failMs) return e.stop;
    std::string s = fmt::format("fail@{:+.2f}", *e.failMs / kTickMs);
    if (e.cause) s += " " + *e.cause;
    if (e.laterInputs && *e.laterInputs > 0) s += fmt::format(" {}", *e.laterInputs);
    return s;
}

std::string windowText(telemetry::TimingWindowV2Payload const& w, double actualMs) {
    double width = w.latestMs - w.earliestMs;
    return fmt::format("{:.2f} ms [{:+.2f},{:+.2f}] ({})", width, w.earliestMs - actualMs, w.latestMs - actualMs, solver::units::framesText(width));
}

char const* triText(std::optional<bool> const& v) { return !v ? "?" : *v ? "y" : "n"; }

double sideMid(solver::BoundaryResult const& b) {
    return b.bounded && std::isfinite(b.failShiftMs) ? 0.5 * (b.passShiftMs + b.failShiftMs) : b.passShiftMs;
}

/// GD's speed portal label (0.5x, 1x, 2x, 3x, 4x) for the trace header.
double speedLabel(Speed s) {
    switch (s) {
        case Speed::Slow: return 0.5;
        case Speed::Normal: return 1.0;
        case Speed::Fast: return 2.0;
        case Speed::Faster: return 3.0;
        case Speed::Fastest: return 4.0;
    }
    return 1.0;
}

gprl::solver::SAConfig saConfigFor(gprl::clone::EngineConfig const& e) {
    gprl::solver::SAConfig c = gprl::solver::kSA;
    c.maxShiftTicks = e.maxShiftTicks;
    c.horizonSeconds = e.horizonSeconds;
    return c;
}

}  // namespace

InputEvent* CloneEngine::logEntry(uint32_t inputId) {
    for (auto it = m_log.rbegin(); it != m_log.rend(); ++it) {
        if (it->id == inputId) return &*it;
    }
    return nullptr;
}

int CloneEngine::logIndex(uint32_t inputId) const {
    for (size_t i = m_log.size(); i-- > 0;) {
        if (m_log[i].id == inputId) return static_cast<int>(i + 1);
    }
    return 0;
}

double CloneEngine::ringMegabytes() const {
    return static_cast<double>(sizeof(Snapshot)) * static_cast<double>(std::max<size_t>(m_hist.size(), static_cast<size_t>(kHistorySteps))) / (1024.0 * 1024.0);
}

void CloneEngine::noteLastInputSeq(int64_t seq) {
    // an input that got no job (skipped) still has its telemetry seq: a release's hold names its press by it
    if (m_lastInputLogged && !m_log.empty() && m_log.back().seq < 0) m_log.back().seq = seq;
}

bool CloneEngine::forcedBreakBetween(double a, double b) const {
    if (std::isnan(a) || std::isnan(b)) return false;
    for (auto const& [f, id] : m_portalMarks) {
        if (f > a + 1e-9 && f <= b + 1e-9 && cl::breaksCluster(cl::portalKind(id))) return true;
    }
    for (auto const& mk : m_deathMarks) {
        if (mk.frame > a + 1e-9 && mk.frame <= b + 1e-9) return true;
    }
    return false;
}

void CloneEngine::setClusterLink(int index, cl::ConnectInput const& in) {
    auto c = cl::connectedNext(in);
    m_clusters.setConnectedNext(index, c.value);
}

// ---- the ledger ----

void CloneEngine::resultOpen(Job const& job) {
    ResultEntry e;
    e.inputSeq = job.inputSeq;
    e.attemptId = job.attemptId;
    e.eventT = job.eventT;
    e.tick = job.tick;
    e.down = job.down;
    e.inputId = job.inputId;
    e.t = job.t;
    e.attemptInputIndex = std::max(1, job.attemptInputIndex);
    e.x = job.pre.x;
    e.percent = job.percent;
    e.subTickMs = job.subTickMs;
    e.engineSubTickMs = job.engineSubTickMs;
    e.gamemode = job.pre.gamemode;
    e.speed = job.pre.speed;
    e.flipped = job.pre.gravityFlipped;
    e.mini = job.pre.mini;
    e.subtick = job.subtick;
    e.halfTick = job.halfTick;
    e.horizonFrame = job.horizonFrame;
    e.replayBroken = m_replayBroken;
    if (!job.down && !std::isnan(job.pressFrame)) {
        // the same press time the timing_window's hold fields use (finalize: actualMs - hold)
        e.pressMs = solver::units::actualMs(job.eventT, job.engineSubTickMs) - solver::timeline::framesToMs(job.t - job.pressFrame);
        int idx = logIndex(job.inputId);
        for (int i = idx - 1; i >= 1; --i) {
            auto const& p = m_log[static_cast<size_t>(i - 1)];
            if (p.down) {
                e.pressSeq = p.seq;
                break;
            }
        }
    }
    if (!m_ledger.open(job.id, std::move(e))) slog(1, fmt::format("GPRL solver: job {} bound twice (ignored)", job.id));
}

void CloneEngine::resultDrop(Job const& job, std::string const& reason) {
    if (auto* le = logEntry(job.inputId)) le->localDone = true;
    ResultEntry* e = m_ledger.data(job.id);
    if (!e) return;   // never bound: no input event to attach a result to
    Reason r = Reason::PayloadInvalid;
    std::string const& mm = job.mismatch;
    if (reason == "mismatch") r = Reason::ControlMismatch;
    else if (reason == "invalid") {
        if (mm.find("dual") != std::string::npos) r = Reason::ControlInvalidDualPortal;
        else if (mm.find("teleport") != std::string::npos) r = Reason::ControlInvalidTeleport;
        else r = Reason::ControlMismatch;
    }
    else if (reason == "pool") r = Reason::PoolExhausted;
    else if (reason == "blocked") r = Reason::BlockedBothSides;
    else if (reason == "history lost") r = Reason::HistoryLost;
    else if (reason == "reset" || reason == "teardown") r = Reason::CutByRestart;
    else if (reason == "level_end") r = Reason::CutByLevelEnd;
    else if (reason == "unbound") r = Reason::UnboundInput;
    else if (reason == "live_mutation") r = Reason::LiveMutationDetected;   // v0.8.2 (docs/LIVE_ISOLATION_DESIGN.md §3.3)
    e->ended.push_back(r);
    if (solver::status::group(r) == solver::status::TimingStatus::StateReplayFailed || r == Reason::LiveMutationDetected) e->stateReplayValid = false;
    e->controlSims = job.controlSims;
    e->horizonFrame = job.horizonFrame;
    e->outcomes = job.planner.outcomes();
    e->localTrials = 0;
    for (auto const& o : e->outcomes) {
        if (o.kind != solver::ShiftKind::NotTested && std::fabs(o.appliedFrames) > 1e-9) ++e->localTrials;
    }
    m_clusters.setConnectedNext(e->attemptInputIndex, cl::Tri::Unknown);
    m_ledger.close(job.id);
    drainResults();
}

void CloneEngine::resultLocal(Job& job, JobResult const& r) {
    auto* le = logEntry(job.inputId);
    if (le) le->localDone = true;
    ResultEntry* e = m_ledger.data(job.id);
    if (!e) return;
    e->horizonFrame = job.horizonFrame;
    e->controlSims = job.controlSims;
    e->outcomes = job.planner.outcomes();
    e->localTrials = 0;
    for (auto const& o : e->outcomes) {
        if (o.kind != solver::ShiftKind::NotTested && std::fabs(o.appliedFrames) > 1e-9) ++e->localTrials;
    }
    // the cluster link to the next input (§2.7): measured from this input's own outcomes
    int const idx = e->attemptInputIndex;
    InputEvent const* next = idx >= 1 && static_cast<size_t>(idx) < m_log.size() ? &m_log[static_cast<size_t>(idx)] : nullptr;
    cl::ConnectInput ci;
    ci.haveOutcomes = r.ok && !r.miss;
    ci.frame = job.t;
    ci.horizonFrame = job.horizonFrame;
    ci.nextFrame = next ? next->t : solver::kNaN;
    ci.forcedBreak = next && forcedBreakBetween(job.t, next->t);
    ci.outcomes = &e->outcomes;
    setClusterLink(idx, ci);

    if (!r.ok) {
        compDiscard(job.id);   // v0.14.0: no local window, nothing to compensate
        Reason why = Reason::PayloadInvalid;
        std::string const& w = r.invalidWhy;
        if (r.dropReason == "no_pass") why = Reason::NoPassDeathUnrelated;
        else if (r.dropReason == "no_shift_tested") why = Reason::NoShiftTested;
        else if (w.find("dual") != std::string::npos) why = Reason::ControlInvalidDualPortal;
        else if (w.find("teleport") != std::string::npos) why = Reason::ControlInvalidTeleport;
        else if (w.find("history lost") != std::string::npos) why = Reason::HistoryLost;
        else if (w.find("control") != std::string::npos || !job.mismatch.empty()) why = Reason::ControlMismatch;
        e->ended.push_back(why);
        if (solver::status::group(why) == solver::status::TimingStatus::StateReplayFailed) e->stateReplayValid = false;
        m_ledger.close(job.id);
        drainResults();
        return;
    }
    e->haveLocal = true;
    e->window = r.window;
    // v0.7.1 (Fable D6): only the attributed miss is the player's miss; an earlier job of the
    // died run is miss_downstream (`miss` false on the wire, no sequence window)
    e->miss = r.miss && !r.missDownstream;
    e->missDownstream = r.missDownstream;
    e->extension = r.extension;
    e->refined = r.refined;
    {
        auto const& w = r.window;
        double width = sideMid(w.late) - sideMid(w.early);
        double res = 0.0;
        if (w.early.bounded) res = std::max(res, w.early.bracketMs);
        if (w.late.bounded) res = std::max(res, w.late.bracketMs);
        if (res <= 0.0) res = kTickMs;
        // v0.7.1: float noise is no gap (units::widthBelowResolution; the owner's 19.47.37 log
        // labelled one-tick windows width_below_resolution by an ulp)
        e->widthBelowResolution = w.early.bounded && w.late.bounded && solver::units::widthBelowResolution(width, res);
        for (auto const* side : {&w.early, &w.late}) {
            if (!side->bounded) continue;
            if (side->edge.cause == solver::EdgeCause::Downstream) ++m_counters.edgeDownstream;
            else if (side->edge.cause == solver::EdgeCause::Extension) ++m_counters.edgeExtension;
            else ++m_counters.edgeSelf;
        }
        if (job.cutByLevelEnd && (w.early.edge.stop == solver::EdgeStop::Untested || w.late.edge.stop == solver::EdgeStop::Untested)) {
            e->ended.push_back(Reason::CutByLevelEnd);
        }
    }
    bool const noEffect = !r.miss && !r.window.boundedEarly && !r.window.boundedLate
                       && solver::allShiftsRejoined(e->outcomes, job.t, next ? next->t : solver::kNaN, job.horizonFrame);
    if (le) le->noEffect = noEffect;
    // debug traces (§2.12): windows at most `solver-trace-max-ticks` wide, <= 8 per attempt
    if (m_cfg.traceMaxTicks > 0 && !job.traces.empty()) {
        double widthFrames = (sideMid(r.window.late) - sideMid(r.window.early)) / kTickMs;
        if (solver::trace::wanted(widthFrames, r.window.boundedEarly && r.window.boundedLate, m_cfg.traceMaxTicks, m_tracedThisAttempt)) {
            e->traced = true;
            e->traces = std::move(job.traces);
            ++m_tracedThisAttempt;
        }
    }
    // negative-control donors: recent lockstep deaths (§3.3 "borrow the nearest lockstep death")
    for (auto const& o : e->outcomes) {
        if (o.kind == solver::ShiftKind::Died && !o.extension) {
            SACandidate d;
            d.jobId = job.id;
            d.inputId = job.inputId;
            d.index = idx;
            d.t = job.t;
            d.step = job.step;
            d.down = job.down;
            d.outcomes = e->outcomes;
            m_saRecent.push_back(std::move(d));
            while (m_saRecent.size() > 24) m_saRecent.pop_front();
            break;
        }
    }
    if (r.miss || noEffect || r.extension || !e->ended.empty() || e->replayBroken) {
        // misses / no-effect inputs / frozen-world windows get no sequence window (§2.5); a replay
        // the breaker declared broken proves nothing a delayed replay could add
        compDiscard(job.id);   // v0.14.0
        m_ledger.localDone(job.id, false);
        drainResults();
        return;
    }
    if (job.connected) {
        // v0.14.0 (docs/SHIP_SOLVER.md §4.3, RC-S3): connected-control inputs never enter the
        // delayed-replay queue; their sequence window is the lockstep compensation planner's
        compConsider(job, *e);
        drainResults();
        return;
    }
    saConsider(job, *e);
    drainResults();
}

void CloneEngine::drainResults() {
    m_ledger.drain([this](int id, ResultEntry& e) { emitResult(id, e); });
}

void CloneEngine::flushResults(Reason local, Reason sa) {
    m_ledger.flush(
        [&](int, ResultEntry& e, solver::LedgerState before) {
            if (before == solver::LedgerState::AwaitingLocal) e.ended.push_back(local);
            else if (before == solver::LedgerState::AwaitingSA) {
                e.saRan = false;
                e.saWhy = sa;
            }
        },
        [this](int id, ResultEntry& e) { emitResult(id, e); });
}

void CloneEngine::emitResult(int jobId, ResultEntry& e) {
    int const idx = e.attemptInputIndex;
    InputEvent const* next = idx >= 1 && static_cast<size_t>(idx) < m_log.size() ? &m_log[static_cast<size_t>(idx)] : nullptr;
    solver::LocalEvidence ev;
    ev.window = e.haveLocal ? &e.window : nullptr;
    ev.outcomes = &e.outcomes;
    ev.miss = e.miss;
    ev.missDownstream = e.missDownstream;
    ev.extension = e.extension;
    ev.widthBelowResolution = e.widthBelowResolution;
    ev.frame = e.t;
    ev.nextFrame = next ? next->t : solver::kNaN;
    ev.nextFollows = next && !forcedBreakBetween(e.t, next->t) && m_clusters.connectedNext(idx) != cl::Tri::No;
    ev.horizonFrame = e.horizonFrame;
    solver::status::JobFacts jf;
    jf.ended = e.ended;
    auto lf = solver::localFacts(ev);
    auto sf = e.saRan ? solver::saFacts(&e.sa) : solver::saFacts(nullptr, e.saWhy);
    solver::status::ContextFacts cx;
    double const maxShift = static_cast<double>(m_cfg.maxShiftTicks);
    for (auto const& [f, realDeath] : m_shadowMarks) {
        if (f < e.t - maxShift - 1e-9 || f > e.horizonFrame + 1e-9) continue;
        if (realDeath) cx.realDeathNotReproduced = true;
        else cx.shadowMismatchNearby = true;
    }
    cx.replayBreaker = e.replayBroken;
    cx.halfTickInput = e.halfTick;
    {
        // Fable D7: a real speed change inside [t - maxShift, look-ahead] (the SA job's when it ran)
        std::vector<double> changes;
        for (auto const& [f, spd] : m_speedMarks) {
            (void)spd;
            changes.push_back(f);
        }
        double lookAhead = e.horizonFrame;
        if (std::isfinite(e.saLookAheadFrame)) lookAhead = std::max(lookAhead, e.saLookAheadFrame);
        cx.speedChangeInSpan = solver::status::speedChangeInSpan(changes, e.t, maxShift, lookAhead);
        if (cx.speedChangeInSpan) {
            ++m_counters.speedFlagged;
            ++m_attempt.speedFlagged;
        }
    }
    std::optional<Gamemode> after;
    for (auto const& [f, id] : m_portalMarks) {
        if (f < e.t - 1e-9 || f > e.t + static_cast<double>(solver::status::kStatusConfig.transitionTicks) + 1e-9) continue;
        if (!cl::transitionPortal(cl::portalKind(id))) continue;
        cx.transitionInput = true;
        Gamemode g;
        if (!after && cl::portalGamemode(id, g)) after = g;
    }
    cx.isolated = e.saRan && e.sa.isolated;
    auto st = solver::status::statusOf(jf, lf, sf, cx);
    // a status of the replay itself makes the result's stateReplayValid false (validate.ts:
    // state_replay_failed => !stateReplayValid)
    bool const replayValid = e.stateReplayValid && st.status != solver::status::TimingStatus::StateReplayFailed;
    solver::TimingResultContext ctx;
    ctx.inputSeq = e.inputSeq;
    ctx.kind = e.down ? InputKind::Press : InputKind::Release;
    ctx.attemptInputIndex = idx;
    ctx.x = e.x;
    ctx.percentAtInput = e.percent;
    ctx.subTickMs = e.subTickMs;
    ctx.engineSubTickMs = e.engineSubTickMs;   // v0.7.1 (Fable D11): always reported, the source of actualMs
    ctx.gamemode = e.gamemode;
    ctx.speed = e.speed;
    ctx.gamemodeAfter = after;
    ctx.eventT = e.eventT;
    ctx.subtick = e.subtick;
    ctx.refined = e.refined;
    ctx.pressMs = e.pressMs;
    ctx.pressSeq = e.pressSeq;
    ctx.cluster = m_clusters.ref(e.attemptId, idx);
    ctx.controlSimulations = e.controlSims + e.saControls;
    ctx.boundarySimulations = e.localTrials + e.saTrials;
    TimingResultOut out;
    out.jobId = jobId;
    out.attemptId = e.attemptId;
    out.eventT = e.eventT;
    out.tick = e.tick;
    out.down = e.down;
    out.attemptInputIndex = idx;
    out.build = solver::buildTimingResultEvent(ctx, ev, e.saRan ? &e.sa : nullptr, st, replayValid);
    auto const& p = out.build.payload;
    if (p.local) out.localWidthMs = p.local->latestMs - p.local->earliestMs;
    if (p.sequence) out.seqWidthMs = p.sequence->window.latestMs - p.sequence->window.earliestMs;
    out.line = resultLine(e, out.build, jobId);
    ++m_counters.results;
    ++m_attempt.results;
    ++m_counters.statusCount[static_cast<int>(out.build.status.status)];
    ++m_attempt.statusCount[static_cast<int>(out.build.status.status)];
    if (!out.build.ok) {
        ++m_counters.resultsFallback;
        ++m_attempt.resultsFallback;
    }
    if (e.saRan) {
        for (int s = 0; s < 2; ++s) {
            if (!e.sa.sideDecided[s]) ++m_saCounters.undecidedSides;
        }
    }
    if (e.traced) traceEmit(e, out.build);
    if (m_resultSink && m_resultSink(out)) {
        ++m_counters.resultsSent;
        ++m_attempt.resultsSent;
    }
}

std::string CloneEngine::resultLine(ResultEntry const& e, solver::TimingResultBuild const& b, int jobId) const {
    auto const& p = b.payload;
    std::string local = "local -";
    if (p.local) local = "local " + windowText(*p.local, p.actualMs) + " early " + edgeText(p.local->early) + " late " + edgeText(p.local->late);
    std::string seq = "sequence -";
    if (p.sequence) {
        std::string used;
        for (auto const& a : p.sequence->adaptationUsed) used += (used.empty() ? "" : "+") + a;
        if (used.empty()) used = e.saRan && e.sa.isolated ? "isolated" : "local";
        seq = "sequence " + windowText(p.sequence->window, p.actualMs) + " " + used + " " + (p.sequence->decided ? "decided" : "undecided");
        if (!p.sequence->decided) seq += " (early " + p.sequence->window.early.stop + ", late " + p.sequence->window.late.stop + ")";
        // Fable D3b: what proved each side's widening, l(ocal) / r(ejoined) / s(urvived)
        auto const& we = p.sequence->window.early;
        auto const& wl = p.sequence->window.late;
        if (we.proof && wl.proof && !we.proof->empty() && !wl.proof->empty()) seq += fmt::format(" proof {}/{}", we.proof->front(), wl.proof->front());
    }
    std::string pair = p.pair ? "pair " + windowText(*p.pair, p.actualMs) : std::string("pair -");
    std::string hold = p.hold ? fmt::format("hold [{:.2f},{:.2f}] ms {}", p.hold->minMs, p.hold->maxMs, p.hold->basis) : std::string("hold -");
    std::string reasons;
    for (auto const& r : p.statusReasons) reasons += (reasons.empty() ? "" : ", ") + r;
    return fmt::format("input #{} {} (seq {}, job {}) t={:.4f} tick={} x={:.1f} pct={:.3f}{}: {} | {} | {} | {} | status {} ({}) | sims {} (+{} controls) | cluster {} #{} prev {} next {}{}",
                       e.attemptInputIndex, e.down ? "press" : "release", e.inputSeq, jobId, e.eventT, e.tick, e.x, e.percent,
                       // Fable D11: both clocks when they differ (CBF / a half tick)
                       std::fabs(e.engineSubTickMs - e.subTickMs) > 1e-9 ? fmt::format(" sub-tick engine {:.3f} / tracker {:.3f} ms", e.engineSubTickMs, e.subTickMs) : std::string(),
                       local, seq, pair, hold, p.status,
                       reasons.empty() ? "-" : reasons, p.boundarySimulations, p.controlSimulations, p.cluster.id, p.cluster.index, triText(p.cluster.connectedPrev),
                       triText(p.cluster.connectedNext), b.ok ? "" : " [fallback: " + b.error + "]");
}

// ---- sequence-adjusted candidates and jobs (§3.3) ----

solver::SAContext CloneEngine::saContext(int firstIndex, int lastIndex, double maxFrame) const {
    solver::SAContext ctx;
    int lo = std::max(1, firstIndex);
    int hi = std::min(static_cast<int>(m_log.size()), lastIndex);
    for (int i = lo; i <= hi; ++i) {
        auto const& le = m_log[static_cast<size_t>(i - 1)];
        // an input beyond every member's look-ahead cannot act on a trial's decision (it would
        // only be moved by a chain whose deaths all happen before it): not part of the job
        if (i > lo && le.t > maxFrame + 1e-9) break;
        solver::SAContextInput in;
        in.id = le.id;
        in.frame = le.t;
        in.down = le.down;
        if (i > lo) {
            auto const& prev = m_log[static_cast<size_t>(i - 2)];
            in.breakBefore = forcedBreakBetween(prev.t, le.t) || m_clusters.connectedNext(i - 1) == cl::Tri::No;
        }
        ctx.inputs.push_back(in);
    }
    return ctx;
}

solver::SAInput CloneEngine::saInput(SACandidate const& c) const {
    solver::SAInput in;
    in.id = c.inputId;
    in.frame = c.t;
    in.down = c.down;
    in.subtick = c.subtick;
    in.earlyLimitFrames = c.earlyLimitFrames;
    in.earlyLimitKind = c.earlyLimitKind;
    in.lateLimitFrames = c.lateLimitFrames;   // v0.7.1 (MOD verifier, D1): the local window's own late limit
    in.local = c.outcomes;
    in.pairWalk = false;
    return in;
}

bool CloneEngine::saReady(SACandidate const& c) const {
    if (static_cast<size_t>(c.index) >= m_log.size()) return true;   // no later input logged
    auto const& next = m_log[static_cast<size_t>(c.index)];
    if (next.jobId == 0 || next.localDone) return true;             // skipped, or its local window is known
    return m_frame > c.t + solver::timeline::horizonFrame(0.0, m_cfg.horizonSeconds, m_cfg.maxShiftTicks) + 40.0;   // never wait forever
}

void CloneEngine::saConsider(Job const& job, ResultEntry& e) {
    if (!m_saOn) {
        e.saRan = false;
        e.saWhy = Reason::SaNotMeasuredBudget;
        m_ledger.localDone(job.id, false);
        return;
    }
    SACandidate c;
    c.jobId = job.id;
    c.inputId = job.inputId;
    c.index = e.attemptInputIndex;
    c.t = job.t;
    c.step = job.step;
    c.down = job.down;
    c.subtick = job.subtick;
    c.earlyLimitFrames = job.earlyLimitFrames;
    c.earlyLimitKind = job.earlyLimitKind;
    c.lateLimitFrames = job.lateLimitFrames;   // what the local planner's late side used (applyLateNeighbour)
    c.outcomes = e.outcomes;
    c.widthFrames = (sideMid(e.window.late) - sideMid(e.window.early)) / kTickMs;
    c.order = ++m_saOrder;
    c.x = e.x;
    if (static_cast<size_t>(c.index) < m_log.size()) {
        auto const& next = m_log[static_cast<size_t>(c.index)];
        c.wantsPairWalk = job.down && !next.down && next.t <= job.horizonFrame + 1e-9 && !forcedBreakBetween(job.t, next.t);
    }
    auto cfg = saConfigFor(m_cfg);
    solver::SAPlanner p(cfg, {saInput(c)}, saContext(c.index - 1, c.index + 4, job.horizonFrame));
    bool const needs = p.needsTrials();
    // the PAIR walk is the only SA work a press may still want once nothing else needs a trial:
    // off by default (Fable D9, pairWalkForPresses) and never for an open-range window (D3a)
    bool const pairWanted = c.wantsPairWalk && cfg.pairWalkForPresses && !p.memberOpenRange(0);
    if (!needs && !pairWanted) {
        // decided without a replay: isolated, self edges, no follower that could act (§2.5), or
        // both local sides open to the search limit (open_range, Fable D3a)
        e.sa = p.result(0);
        e.saRan = true;
        ++m_saCounters.immediate;
        m_ledger.localDone(job.id, false);
        return;
    }
    if (c.widthFrames > cfg.priorityMaxLocalWidthTicks + 1e-9) {
        if (!needs) {
            e.sa = p.result(0);
            e.saRan = true;
            ++m_saCounters.immediate;
        }
        else {
            e.saRan = false;
            e.saWhy = Reason::SaNotMeasuredBudget;
            ++m_saCounters.notStartedWide;
        }
        m_ledger.localDone(job.id, false);
        return;
    }
    ++m_saCounters.candidates;
    m_ledger.localDone(job.id, true);
    m_saCands.push_back(std::move(c));
    // queue cap: the widest / oldest waiting candidates give up first (sa_not_measured_budget).
    // Bounded: each iteration erases one candidate.
    while (static_cast<int>(m_saCands.size()) > cfg.queueMax) {
        size_t worst = 0;
        for (size_t i = 1; i < m_saCands.size(); ++i) {
            auto const& a = m_saCands[i];
            auto const& w = m_saCands[worst];
            if (a.widthFrames > w.widthFrames + 1e-9 || (std::fabs(a.widthFrames - w.widthFrames) <= 1e-9 && a.order < w.order)) worst = i;
        }
        SACandidate dropped = m_saCands[worst];
        m_saCands.erase(m_saCands.begin() + static_cast<std::ptrdiff_t>(worst));
        saGiveUp(dropped, Reason::SaNotMeasuredBudget, m_saCounters.notStartedBudget);
    }
}

void CloneEngine::saGiveUp(SACandidate const& c, Reason why, int& counter) {
    ++counter;
    ResultEntry* e = m_ledger.data(c.jobId);
    if (!e) return;
    e->saRan = false;
    e->saWhy = why;
    m_ledger.saDone(c.jobId);
}

void CloneEngine::saAbortAll(Reason why) {
    // bounded: one give-up per waiting candidate
    for (auto const& c : m_saCands) saGiveUp(c, why, m_saCounters.dropReset);
    m_saCands.clear();
    drainResults();
}

SANegative CloneEngine::saNegativeFor(std::vector<SACandidate> const& members, double baseFrame, double horizonFrame) const {
    SANegative best;
    double bestMag = 1e9;
    auto consider = [&](SACandidate const& c) {
        for (auto const& o : c.outcomes) {
            if (o.kind != solver::ShiftKind::Died || o.extension) continue;
            double frame = c.t + o.appliedFrames;
            double death = c.t + o.deathAfterFrames;
            if (frame < baseFrame + 1.0 || death > horizonFrame) continue;
            double mag = std::fabs(o.appliedFrames);
            if (mag >= bestMag) continue;
            bestMag = mag;
            best.valid = true;
            best.inputId = c.inputId;
            best.down = c.down;
            best.frame = frame;
            best.shift = o.appliedFrames;
            best.objectId = o.objectId;
            best.deathFrame = death;
        }
    };
    for (auto const& m : members) consider(m);
    if (!best.valid) {
        for (auto const& d : m_saRecent) consider(d);   // borrow the nearest lockstep death inside the span
    }
    return best;
}

bool CloneEngine::saStart() {
    auto cfg = saConfigFor(m_cfg);
    // ready candidates (the next input's local window is known, or none comes)
    std::vector<cl::ChunkCandidate> ready;
    std::vector<size_t> where;
    for (size_t i = 0; i < m_saCands.size(); ++i) {
        auto const& c = m_saCands[i];
        if (!saReady(c)) continue;
        cl::ChunkCandidate cc;
        cc.index = c.index;
        cc.frame = c.t;
        cc.widthFrames = c.widthFrames;
        cc.connectedNext = m_clusters.connectedNext(c.index);
        cc.order = c.order;
        // Fable D10: a place measured saMaxPerPosition times this level visit waits behind the rest
        if (auto it = m_saPlaces.find(solver::sequence::positionKey(c.x, 1)); it != m_saPlaces.end()) cc.positionMeasured = it->second >= cfg.saMaxPerPosition;
        ready.push_back(cc);
        where.push_back(i);
    }
    if (ready.empty()) return false;
    // sort by input index (chunkFor walks consecutive indices)
    std::vector<size_t> order(ready.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ready[a].index < ready[b].index; });
    std::vector<cl::ChunkCandidate> sorted;
    std::vector<size_t> sortedWhere;
    for (size_t k : order) {
        sorted.push_back(ready[k]);
        sortedWhere.push_back(where[k]);
    }
    int seed = cl::pickSeed(sorted);
    if (seed < 0) return false;
    auto chunk = cl::chunkFor(sorted, static_cast<size_t>(seed), cfg.maxInputsPerJob, cfg.maxJobSpanTicks);
    std::vector<size_t> take;
    for (size_t k : chunk) take.push_back(sortedWhere[k]);
    std::sort(take.begin(), take.end());
    std::vector<SACandidate> members;
    for (size_t i : take) members.push_back(m_saCands[i]);
    for (size_t k = take.size(); k-- > 0;) m_saCands.erase(m_saCands.begin() + static_cast<std::ptrdiff_t>(take[k]));
    std::sort(members.begin(), members.end(), [](SACandidate const& a, SACandidate const& b) { return a.index < b.index; });

    // the planner over the chunk, with the pair walk for presses whose release is effective
    std::vector<solver::SAInput> inputs;
    for (auto const& m : members) {
        auto in = saInput(m);
        if (m.wantsPairWalk && static_cast<size_t>(m.index) < m_log.size()) {
            auto const& rel = m_log[static_cast<size_t>(m.index)];
            in.pairWalk = rel.localDone && !rel.noEffect && rel.jobId != 0;
        }
        inputs.push_back(std::move(in));
    }
    double const maxShiftF = static_cast<double>(m_cfg.maxShiftTicks);
    double const horizonF = m_cfg.horizonSeconds * kTicksPerSecond + maxShiftF;
    auto ctx = saContext(members.front().index - 1, members.back().index + 4, members.back().t + horizonF);
    solver::SAPlanner planner(cfg, std::move(inputs), ctx);
    SeqJob job;
    job.kind = SeqKind::SequenceAdjusted;
    job.id = m_seqNextId++;
    job.subtick = members.front().subtick;
    job.saMembers = members;
    job.sa = std::move(planner);
    if (!job.sa.needsTrials()) {
        // everything decided without a replay (e.g. the releases turned out not to be effective)
        m_saCounters.immediate += static_cast<int>(members.size());
        saMembersDone(job, true, Reason::SaUndecided, false);
        return false;
    }
    // span: the earliest shifted input to the latest look-ahead any trial may use. Fable D2: a
    // chain's look-ahead runs one horizon past its LAST moved follower (SAPlanner::need), so the
    // job's look-ahead - how far both controls prove the delayed replay - is the planner's bound
    // over every trial it can hand out (the ring holds it: tuning_tests saWorstSpanFrames)
    double baseFrame = 1e300;
    for (auto const& m : members) baseFrame = std::min(baseFrame, m.t - maxShiftF - 1.0);
    double horizonFrame = std::max(job.sa.lookAheadBound(), members.back().t + maxShiftF + horizonF);
    for (auto const& mk : m_deathMarks) {
        double const f = mk.frame;
        if (f >= baseFrame && f <= horizonFrame + 1.0) {
            for (auto const& m : members) saGiveUp(m, Reason::SaDeathInSpan, m_saCounters.notStartedDeath);
            slog(2, fmt::format("GPRL sa: {} not measured: a death at frame {:.1f} lies inside the span", seqName(job), f));
            drainResults();
            return false;
        }
    }
    job.saNegative = saNegativeFor(members, baseFrame, horizonFrame);
    if (cfg.requireNegativeControl && !job.saNegative.valid) {
        for (auto const& m : members) saGiveUp(m, Reason::SaNoNegativeControl, m_saCounters.notStartedNoNegative);
        slog(2, fmt::format("GPRL sa: {} not measured: no lockstep death inside the span proves that a delayed replay still dies here", seqName(job)));
        drainResults();
        return false;
    }
    if (job.saNegative.valid) baseFrame = std::min(baseFrame, job.saNegative.frame - 1.0);
    job.baseFrame = baseFrame;
    job.horizonFrame = horizonFrame;
    job.baseStep = solver::timeline::stepForFrame(members.front().step, baseFrame, [this](int k, double& f) { return frameOf(k, f); });
    if (job.baseStep < 1 || m_step - job.baseStep >= kHistorySteps - kSASeq.ringReserveSteps) {
        for (auto const& m : members) saGiveUp(m, Reason::SaExpired, m_saCounters.notStartedExpired);
        drainResults();
        return false;
    }
    job.queuedStep = m_step;
    double narrowest = 1e9;
    for (auto const& m : members) narrowest = std::min(narrowest, m.widthFrames);
    slog(1, fmt::format("GPRL sa: job {} queued: inputs #{}..#{} ({}), narrowest local {:.2f} ms, base step {} ({} steps old), look-ahead to frame {:.1f}, "
                        "{} trials planned first, negative control input id {} at {:+.2f} ticks, {} waiting",
                        job.id, members.front().index, members.back().index, members.size(), narrowest * kTickMs, job.baseStep, m_step - job.baseStep,
                        job.horizonFrame, job.sa.requested(), job.saNegative.inputId, job.saNegative.shift, m_saCands.size()));
    m_seq = std::move(job);
    return startSequence(m_seq);
}

void CloneEngine::saMembersDone(SeqJob& job, bool ok, Reason why, bool proofFailed) {
    int const n = std::max<int>(1, static_cast<int>(job.saMembers.size()));
    int const controls = (job.controlSteps[0] > 0 ? 1 : 0) + (job.controlSteps[1] > 0 ? 1 : 0) + (job.saNegative.valid && !job.negText.empty() ? 1 : 0);
    for (size_t i = 0; i < job.saMembers.size(); ++i) {
        auto const& m = job.saMembers[i];
        ResultEntry* e = m_ledger.data(m.jobId);
        if (!e) continue;
        if (ok) {
            e->sa = job.sa.result(static_cast<int>(i));
            e->saRan = true;
            e->saTrials = e->sa.trials;
            e->saControls = (controls + n - 1) / n;
            if (job.horizonFrame > 0.0) e->saLookAheadFrame = job.horizonFrame;   // Fable D7: the trials' span
            if (job.controlSteps[1] > 0) {
                ++m_saCounters.measured;   // a job ran (both controls exact), not an immediate decision
                ++m_saPlaces[solver::sequence::positionKey(m.x, 1)];   // Fable D10: this place is measured once more
            }
            if (e->traced) {
                for (auto& [member, rec] : job.saTraced) {
                    if (member == static_cast<int>(i)) e->saTraces.push_back(std::move(rec));
                }
            }
        }
        else {
            e->saRan = false;
            e->saWhy = why;
            e->saTrials = 0;
            if (proofFailed) e->stateReplayValid = false;
        }
        m_ledger.saDone(m.jobId);
    }
    drainResults();
}

// ---- marks, the replay breaker, traces ----

void CloneEngine::noteShadowMismatch(double frame, bool realDeath) {
    // pruned by age, never by count (kMarkKeepFrames): a mismatch inside a pending result's span
    // must still be there when that result is emitted (shadow_mismatch_nearby / real_death_not_reproduced)
    pushMark(m_shadowMarks, {frame, realDeath}, frame);
}

void CloneEngine::onReplayStep(bool mismatch, std::string const& why) {
    if (mismatch && m_firstShadowWhy.empty()) m_firstShadowWhy = why;
    if (!m_breaker.step(mismatch)) return;
    m_replayBroken = true;
    log::warn("GPRL solver: replay broken this attempt ({} shadow mismatches in {} steps, first: {}) - measuring paused until the restart",
              m_breaker.mismatches(), m_breaker.steps(), m_firstShadowWhy.empty() ? std::string("-") : m_firstShadowWhy);
    // Inputs still being measured when the attempt's replay is declared broken never get a
    // precise number (AUDIT §16 "trajectory diverges unexpectedly"): their results carry
    // state_replay_failed (replay_breaker); no delayed replay is started for them. Bounded by the
    // open ledger entries.
    for (auto const& en : m_ledger.entries()) {
        if (en.state == solver::LedgerState::Done) continue;
        if (ResultEntry* e = m_ledger.data(en.jobId)) e->replayBroken = true;
    }
    if (m_seqActive && m_seq.kind == SeqKind::SequenceAdjusted) dropSequence(m_seq, "control", "the replay breaker tripped (the shadow keeps mismatching)");
    if (!m_saCands.empty()) {
        for (auto const& c : m_saCands) saGiveUp(c, Reason::SaControlMismatch, m_saCounters.dropControl);
        m_saCands.clear();
        drainResults();
    }
}

void CloneEngine::traceRecord(Clone& c) {
    if (!c.obj) return;
    solver::trace::TraceStep s;
    s.frame = c.frameDone;
    s.x = c.obj->getPositionX();
    s.y = c.obj->getPositionY();
    s.yVel = static_cast<float>(c.obj->m_yVelocity);
    s.rotation = c.obj->getRotation();
    s.mode = modeChar(c.obj);
    s.onGround = c.obj->m_isOnGround;
    auto r = c.obj->getObjectRect();
    s.w = r.size.width;
    s.h = r.size.height;
    solver::trace::record(c.trace, s);
    if (c.state == CloneStatus::Dead) {
        c.trace.death.died = true;
        c.trace.death.frame = c.deathFrame;
        c.trace.death.laterInputs = c.laterFixed;
        c.trace.death.extension = c.extension;
    }
}

void CloneEngine::traceEmit(ResultEntry const& e, solver::TimingResultBuild const& b) {
    auto const& p = b.payload;
    solver::trace::Header h;
    h.inputIndex = e.attemptInputIndex;
    h.down = e.down;
    h.seq = e.inputSeq;
    h.t = e.eventT;
    h.tick = e.tick;
    h.x = e.x;
    h.percent = e.percent;
    h.mode = std::string(name(e.gamemode));
    h.speed = speedLabel(e.speed);
    h.flipped = e.flipped;
    h.mini = e.mini;
    if (p.local) h.local = windowText(*p.local, p.actualMs);
    if (p.sequence) {
        std::string used;
        for (auto const& a : p.sequence->adaptationUsed) used += (used.empty() ? "" : "+") + a;
        h.sequence = windowText(p.sequence->window, p.actualMs) + " " + (used.empty() ? std::string("local") : used) + " " + (p.sequence->decided ? "decided" : "undecided");
    }
    if (p.hold) h.hold = fmt::format("[{:.2f}, {:.2f}] ms {}", p.hold->minMs, p.hold->maxMs, p.hold->basis);
    log::info("{}", solver::trace::header(h));
    solver::trace::View view;
    view.serial = m_lastTrace.serial + 1;
    view.header = h;
    auto const& w = e.window;
    auto sel = solver::trace::select(e.traces, w.early.passShiftMs / kTickMs, w.early.bounded ? w.early.failShiftMs / kTickMs : solver::kNaN,
                                     w.late.passShiftMs / kTickMs, w.late.bounded ? w.late.failShiftMs / kTickMs : solver::kNaN);
    for (auto const& [role, idx] : solver::trace::ordered(sel)) {
        if (idx < 0) continue;
        auto const& tc = e.traces[static_cast<size_t>(idx)];
        log::info("{}", solver::trace::trajectory(e.attemptInputIndex, role, tc));
        view.trajectories.emplace_back(role, tc);
    }
    // second block (§2.12): the sequence-adjusted edges, from the SA job's trials of this input
    if (e.saRan && !e.saTraces.empty()) {
        auto const& sq = e.sa.sequence;
        auto sas = solver::trace::selectSA(e.saTraces, sq.early.passFrames, sq.early.bounded() ? sq.early.failFrames : solver::kNaN, sq.late.passFrames,
                                           sq.late.bounded() ? sq.late.failFrames : solver::kNaN, w.early.passShiftMs / kTickMs, w.late.passShiftMs / kTickMs);
        std::pair<char const*, int> const roles[] = {{"sa-earliest-valid", sas.earliestValid}, {"sa-early-invalid", sas.earlyInvalid},
                                                     {"sa-latest-valid", sas.latestValid}, {"sa-late-invalid", sas.lateInvalid}};
        for (auto const& [role, idx] : roles) {
            if (idx < 0) continue;
            auto const& rec = e.saTraces[static_cast<size_t>(idx)];
            std::string r = fmt::format("{} ({})", role, solver::name(static_cast<solver::SAAdaptation>(std::clamp(rec.adaptation, 0, 6))));
            log::info("{}", solver::trace::trajectory(e.attemptInputIndex, r.c_str(), rec.clone));
            view.trajectories.emplace_back(r, rec.clone);
        }
    }
    m_lastTrace = std::move(view);
}


// ---- v0.14.0 lockstep compensation (docs/SHIP_SOLVER.md §4.3; core/solver/compensation.hpp) ----
//
// For a CONNECTED-CONTROL input (ship by default) the sequence question is answered here, in
// lockstep, instead of by the delayed-replay SA job (RC-S3). A CompJob is created the moment a
// shifted copy of the local job dies; its planner receives the local outcomes as they finish, the
// later inputs as they are logged, and the final local view when the pass resolves. Its trials
// are clones spawned from the history ring (the snapshot before the earliest moved input) that
// catch up and run in lockstep like the local copies; ONE delayed control per job - the unshifted
// replay from the oldest base a trial uses - must match the live player step for step once it has
// caught up, or every open side ends `sa_control_mismatch` (the proof invalid). The deviation of a
// trial from the recorded run (y, y velocity against the ring snapshot of the same step) feeds the
// planner's response model. The result lands in the job's ledger entry as its `sa` (an SAResult),
// so the status rules and the event builder are the ones every sequence window uses.

namespace {
constexpr int kCompCloneJob = -3;        // Clone::job of a compensation clone (never an index into m_jobs)
constexpr size_t kMaxDevSamples = 512;   // deviation samples kept per trial (2 s of half-tick steps at most)
}

bool CloneEngine::connectedModeOf(PlayerObject* p) const {
    auto const& c = m_cfg.compensation;
    if (!p || !c.enabled) return false;
    if (p->m_isShip) return c.ship;
    if (p->m_isDart) return c.wave;
    if (p->m_isBird) return c.ufo;
    if (p->m_isSwing) return c.swing;
    return false;
}

solver::comp::CompConfig CloneEngine::compConfigFor() const {
    solver::comp::CompConfig c = solver::comp::kComp;
    c.maxShiftTicks = m_cfg.maxShiftTicks;
    // the follower horizon stays 0.5 s whatever the local look-ahead is (the settle maximum is 8 s)
    c.horizonFrames = 0.5 * kTicksPerSecond;
    double look = 0.5 * kTicksPerSecond + static_cast<double>(m_cfg.maxShiftTicks);
    if (m_cfg.settle.enabled) look = std::max(look, m_cfg.settle.flySeconds * kTicksPerSecond + 8.0);
    c.lookAheadFrames = look;
    return c;
}

CompJob* CloneEngine::findComp(int jobId) {
    for (auto& j : m_compJobs) {
        if (j.jobId == jobId) return &j;
    }
    return nullptr;
}

CompJob* CloneEngine::compCreate(Job& job) {
    if (job.compCreated) return findComp(job.id);
    if (!job.bound || !m_saOn) return nullptr;   // not yet bound: tried again at the next death / the local result
    job.compCreated = true;
    // the cap counts the jobs that run (or may still run) trials; a job decided from the local facts
    // alone costs nothing (F5)
    int active = 0;
    for (auto const& cj : m_compJobs) {
        if (cj.planner.requested() > 0 || cj.planner.hasPending() || !cj.running.empty()) ++active;
    }
    if (active >= m_cfg.compensation.maxJobs) {
        ++m_compCounters.notStartedBudget;
        return nullptr;
    }
    CompJob cj;
    cj.jobId = job.id;
    cj.inputId = job.inputId;
    cj.index = job.attemptInputIndex;
    cj.t = job.t;
    cj.step = job.step;
    cj.down = job.down;
    cj.startedMs = nowMs();
    solver::comp::CompMember m;
    m.id = job.inputId;
    m.frame = job.t;
    m.down = job.down;
    m.subtick = job.subtick;
    m.earlyLimitFrames = job.earlyLimitFrames;
    m.earlyLimitKind = job.earlyLimitKind;
    // the inputs logged after the member so far (time order); a portal / death between two of them
    // is a cluster break (cluster.hpp): nothing after it follows
    std::vector<solver::comp::CompFollower> followers;
    int const idx = logIndex(job.inputId);   // 1-based
    for (size_t i = static_cast<size_t>(std::max(1, idx)); i < m_log.size(); ++i) {
        auto const& le = m_log[i];
        auto const& prev = m_log[i - 1];
        followers.push_back({le.id, le.t, le.down, forcedBreakBetween(prev.t, le.t)});
    }
    cj.planner = solver::comp::CompPlanner(compConfigFor(), m, followers);
    cj.planner.setNow(m_frame);
    m_compJobs.push_back(std::move(cj));
    ++m_compCounters.jobs;
    return &m_compJobs.back();
}

void CloneEngine::compNoteInput(InputEvent const& le) {
    if (m_compJobs.empty() || m_log.size() < 2) return;
    auto const& prev = m_log[m_log.size() - 2];
    bool const brk = forcedBreakBetween(prev.t, le.t);
    for (auto& cj : m_compJobs) cj.planner.addFollower({le.id, le.t, le.down, brk});
}

void CloneEngine::compNoteClones(Job& job) {
    // the lockstep local copies that finished: a death that compensation could change starts the
    // job (a later fixed input acted before it, or an early shift whose follower could still act,
    // docs §4.2 step 1); passes and deaths feed it
    bool anyDeath = false;
    for (int idx : job.clones) {
        auto const& c = m_clones[idx];
        if (c.shift != 0.0 && c.state == CloneStatus::Dead && (c.laterFixed >= 1 || c.shift < 0.0)) anyDeath = true;
    }
    CompJob* cj = findComp(job.id);
    if (!cj) {
        if (!anyDeath) return;
        cj = compCreate(job);
        if (!cj) return;
    }
    for (int idx : job.clones) {
        auto& c = m_clones[idx];
        if (c.shift == 0.0 || c.compNoted) continue;
        if (c.state != CloneStatus::Dead && c.state != CloneStatus::Alive) continue;
        solver::ShiftOutcome o;
        o.nominalFrames = c.shift;
        o.appliedFrames = std::isnan(c.appliedFrame) ? c.shift : c.appliedFrame - job.t;
        o.extension = c.extension;
        if (c.state == CloneStatus::Alive) {
            o.kind = c.resynced ? solver::ShiftKind::Resynced : solver::ShiftKind::Survived;
            if (c.resynced && !std::isnan(c.rejoinStartFrame)) o.rejoinAfterFrames = c.rejoinStartFrame - job.t;
        }
        else {
            o.kind = solver::ShiftKind::Died;
            o.deathAfterFrames = c.deathFrame - job.t;
            o.objectId = c.deathObjId;
            o.laterFixed = c.laterFixed;
            o.deathX = c.deathX;
        }
        o.pass = job.pass;
        c.compNoted = true;
        cj->planner.noteLocal(o);
    }
}

void CloneEngine::compDiscard(int jobId) {
    for (size_t i = 0; i < m_compJobs.size(); ++i) {
        if (m_compJobs[i].jobId != jobId) continue;
        compFree(m_compJobs[i]);
        ++m_compCounters.discarded;
        m_compJobs.erase(m_compJobs.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void CloneEngine::compFree(CompJob& job) {
    if (job.control >= 0 && job.control < static_cast<int>(m_clones.size())) freeClone(m_clones[static_cast<size_t>(job.control)]);
    job.control = -1;
    for (auto& r : job.running) {
        if (r.clone >= 0 && r.clone < static_cast<int>(m_clones.size())) freeClone(m_clones[static_cast<size_t>(r.clone)]);
        r.clone = -1;
    }
    job.running.clear();
}

void CloneEngine::compConsider(Job& job, ResultEntry& e) {
    // the local part of a connected job is final: hand the planner its final local view and let
    // the ledger wait for the compensation result (or take it now)
    if (e.compDone) {
        m_ledger.localDone(job.id, false);
        return;
    }
    CompJob* cj = findComp(job.id);
    if (!cj && !job.compCreated) cj = compCreate(job);
    if (!cj) {
        e.saRan = false;
        e.saWhy = Reason::SaNotMeasuredBudget;
        m_ledger.localDone(job.id, false);
        return;
    }
    cj->planner.finalizeLocal(job.planner.outcomes(), job.lateLimitFrames);
    cj->localDone = true;
    cj->planner.setNow(m_frame);
    double const widthTicks = (sideMid(e.window.late) - sideMid(e.window.early)) / kTickMs;
    if (widthTicks > compConfigFor().priorityMaxLocalWidthTicks + 1e-9 && !cj->planner.done()) {
        // a wide local window barely matters for precision (SA D10): the search budget goes to the
        // narrow ones; the sides it would have decided stay undecided with the budget reason
        cj->planner.finish(Reason::SaNotMeasuredBudget);
        ++m_compCounters.notStartedWide;
    }
    m_ledger.localDone(job.id, true);
    if (cj->planner.done()) {
        compFinish(*cj, true, Reason::SaUndecided, false);
        compDiscard(job.id);
    }
}

int CloneEngine::spawnCompControl(CompJob& job) {
    // the unshifted replay from the oldest base any trial of this job can use: it must match the
    // live player step for step once it has caught up (the local control rule, D7)
    double const baseFrame = job.t - static_cast<double>(m_cfg.maxShiftTicks) - 1.0;
    int k0 = solver::timeline::stepForFrame(job.step, baseFrame, [this](int k, double& f) { return frameOf(k, f); });
    double f1 = 0.0;
    if (k0 < 1 && frameOf(1, f1)) k0 = 1;   // F7: the attempt start is a valid base (the ring holds step 1)
    if (k0 < 1) return -1;
    auto& h = m_hist[ring(k0)];
    if (h.step != k0) return -1;
    int idx = allocClone(true);
    if (idx < 0) return 0;
    auto& c = m_clones[static_cast<size_t>(idx)];
    applyState(c.obj, h.state);
    c.flags.clear();
    for (auto& [o, f] : h.flags) c.flags[o] = f;
    c.state = CloneStatus::Running;
    c.job = kCompCloneJob;
    c.pass = 0;
    c.shift = 0.0;
    c.inputFrame = 0.0;
    c.inputPending = false;
    c.appliedFrame = solver::kNaN;
    c.stepDone = k0 - 1;
    c.frameDone = h.frame;
    c.deathStep = -1;
    c.deathObjId = -1;
    c.converged = 0;
    c.resynced = false;
    c.extension = false;
    c.moved.clear();
    c.attributeAfterFrame = 1e300;
    c.laterFixed = 0;
    c.settle = {};
    job.control = idx;
    job.controlBaseStep = k0;
    ++m_compCounters.controls;
    return 1;
}

int CloneEngine::spawnCompTrial(CompJob& job, int idx, solver::comp::CompTrial const& t) {
    double earliest = t.moved.front().frame;
    for (auto const& mv : t.moved) earliest = std::min(earliest, mv.frame);
    int k0 = solver::timeline::stepForFrame(job.step, earliest, [this](int k, double& f) { return frameOf(k, f); });
    double f1 = 0.0;
    if (k0 < 1 && frameOf(1, f1)) k0 = 1;   // F7
    if (k0 < 1) return -1;
    auto& h = m_hist[ring(k0)];
    if (h.step != k0) return -1;
    if (idx < 0 || idx >= static_cast<int>(m_clones.size())) return -1;
    auto& c = m_clones[static_cast<size_t>(idx)];
    applyState(c.obj, h.state);
    c.flags.clear();
    for (auto& [o, f] : h.flags) c.flags[o] = f;
    c.state = CloneStatus::Running;
    c.job = kCompCloneJob;
    c.pass = 0;
    c.shift = t.shiftFrames;
    c.inputFrame = t.moved.front().frame;   // the member's shifted frame: the settle rule's first moved input
    c.inputPending = false;
    c.appliedFrame = solver::kNaN;
    c.stepDone = k0 - 1;
    c.frameDone = h.frame;
    c.deathStep = -1;
    c.deathObjId = -1;
    c.converged = 0;
    c.resynced = false;
    c.extension = false;
    c.moved.clear();
    for (auto const& mv : t.moved) c.moved.push_back({mv.id, mv.down, mv.frame, true, solver::kNaN});
    c.attributeAfterFrame = t.attributeAfterFrame;
    c.laterFixed = 0;
    c.settle = {};
    c.tracing = m_cfg.traceMaxTicks > 0;
    c.trace.shiftFrames = t.shiftFrames;
    CompTrialRun r;
    r.clone = idx;
    r.trialId = t.id;
    r.assessFrame = t.assessFrame;
    r.devFromFrame = t.devFromFrame;
    r.lookAheadFrame = t.lookAheadFrame;
    r.adaptation = static_cast<int>(t.adaptation);
    r.shift = t.shiftFrames;
    r.offsets = t.offsetsFrames;
    job.running.push_back(std::move(r));
    job.lookAheadMax = std::max(job.lookAheadMax, t.lookAheadFrame);
    ++job.trials;
    ++m_compCounters.trials;
    return 1;
}

void CloneEngine::compTrialDone(CompJob& job, CompTrialRun& run, solver::comp::CompOutcome o) {
    o.id = run.trialId;
    o.dev = std::move(run.dev);
    if (run.clone >= 0 && run.clone < static_cast<int>(m_clones.size())) {
        auto& c = m_clones[static_cast<size_t>(run.clone)];
        if (c.tracing && job.saTraces.size() < 96) {
            solver::trace::SATraced rec;
            rec.adaptation = run.adaptation;
            rec.shiftFrames = run.shift;
            rec.clone = c.trace;
            rec.clone.passed = o.kind == solver::comp::CompOutcome::Kind::Pass;
            job.saTraces.push_back(std::move(rec));
        }
        freeClone(c);
    }
    run.clone = -1;
    switch (o.kind) {
        case solver::comp::CompOutcome::Kind::Pass:
            ++m_compCounters.passes;
            if (run.adaptation >= static_cast<int>(solver::SAAdaptation::Comp1)) ++m_compCounters.compensated;
            break;
        case solver::comp::CompOutcome::Kind::Died: ++m_compCounters.fails; break;
        case solver::comp::CompOutcome::Kind::Invalid: ++m_compCounters.invalid; break;
    }
    job.planner.ingest(o);
}

/// F2: a moved input of a lockstep trial may be crossed by an input the player made AFTER the trial
/// was requested (it is replayed as fixed, so the simulated order differs from any schedule a
/// player could perform). True when some moved copy passed a fixed logged input in either direction.
bool CloneEngine::compTrialCrossed(Clone const& c) const {
    for (auto const& m : c.moved) {
        InputEvent const* rec = nullptr;
        for (auto const& le : m_log) if (le.id == m.id) { rec = &le; break; }
        if (!rec) continue;
        double const lo = std::min(rec->t, m.frame), hi = std::max(rec->t, m.frame);
        for (auto const& le : m_log) {
            bool moved = false;
            for (auto const& mm : c.moved) moved = moved || mm.id == le.id;
            if (moved) continue;
            if (le.t > lo - 1e-9 && le.t < hi + 1e-9 && std::fabs(le.t - rec->t) > 1e-9) return true;
        }
    }
    return false;
}

void CloneEngine::compAfterStep(CompJob& job, CompTrialRun& run) {
    auto& c = m_clones[static_cast<size_t>(run.clone)];
    solver::comp::CompOutcome o;
    // F2: checked whenever the attempt's input log grew since this trial last looked (cheap)
    if (c.state == CloneStatus::Running && run.logChecked != m_log.size()) {
        run.logChecked = m_log.size();
        if (compTrialCrossed(c)) {
            o.kind = solver::comp::CompOutcome::Kind::Invalid;
            o.reason = "a later input crossed a moved input";
            o.notTested = true;
            ++m_compCounters.crossed;
            compTrialDone(job, run, std::move(o));
            return;
        }
    }
    if (c.state == CloneStatus::Dead) {
        o.kind = solver::comp::CompOutcome::Kind::Died;
        o.deathFrame = c.deathFrame;
        o.laterFixed = c.laterFixed;
        o.objectId = c.deathObjId;
        o.deathX = c.deathX;
        o.extension = c.extension;
        compTrialDone(job, run, std::move(o));
        return;
    }
    if (c.state != CloneStatus::Running) {
        o.kind = solver::comp::CompOutcome::Kind::Invalid;
        o.reason = c.invalidReason.empty() ? std::string(c.state == CloneStatus::Invalid ? "invalid" : "cancelled") : c.invalidReason;
        o.notTested = c.state != CloneStatus::Invalid;   // cancelled / limit: never a failed proof
        compTrialDone(job, run, std::move(o));
        return;
    }
    bool applied = true;
    for (auto const& m : c.moved) applied = applied && !m.pending;
    // the real player's state after this step is the ring snapshot of the next one
    auto const& next = m_hist[ring(c.stepDone + 1)];
    bool const have = next.step == c.stepDone + 1;
    if (have && c.frameDone >= run.devFromFrame - 1e-9 && run.dev.size() < kMaxDevSamples) {
        run.dev.push_back({c.frameDone, static_cast<double>(c.obj->getPositionY() - next.state.pos.y), c.obj->m_yVelocity - next.state.f.m_yVelocity});
    }
    if (have) mirrorNextStepSpeed(c);
    bool rejoined = false;
    if (have && applied) {
        if (samePhysicsAsSnapshot(c.obj, next.state)) rejoined = ++run.converged >= CONVERGE_STEPS;
        else run.converged = 0;
    }
    if (!applied && c.frameDone >= run.lookAheadFrame - 1e-9) {
        o.kind = solver::comp::CompOutcome::Kind::Invalid;
        o.reason = "a moved input was never applied";
        compTrialDone(job, run, std::move(o));
        return;
    }
    bool const settled = m_cfg.settle.enabled && applied && c.settle.settled;
    bool const horizon = applied && c.frameDone >= run.lookAheadFrame - 1e-9;
    if (!rejoined && !settled && !horizon) return;
    if (!rejoined && !settled && horizon && m_cfg.settle.enabled) {
        // v0.11.0: alive at the look-ahead's end but never settled = not tested (never a pass)
        ++m_counters.unsettled;
        o.kind = solver::comp::CompOutcome::Kind::Invalid;
        o.reason = "unsettled";
        o.notTested = true;
        compTrialDone(job, run, std::move(o));
        return;
    }
    if (settled && !rejoined) ++m_counters.settled;
    o.kind = solver::comp::CompOutcome::Kind::Pass;
    o.rejoined = rejoined;
    compTrialDone(job, run, std::move(o));
}

void CloneEngine::compFinish(CompJob& job, bool ok, Reason why, bool proofFailed) {
    compFree(job);
    ResultEntry* e = m_ledger.data(job.jobId);
    std::string const work = fmt::format("{} trials / {} clone steps, control {} steps compared, {:.0f} ms", job.trials, job.cloneSteps, job.controlCompared, nowMs() - job.startedMs);
    if (e) {
        if (ok) {
            e->sa = job.planner.result();
            e->saRan = true;
            e->saTrials = job.planner.trials();
            e->saControls = job.controlCompared > 0 ? 1 : 0;
            e->saLookAheadFrame = job.lookAheadMax > 0.0 ? job.lookAheadMax : solver::kNaN;
            if (!job.saTraces.empty()) e->saTraces = std::move(job.saTraces);
            if (e->sa.decided) ++m_compCounters.decided;
            else ++m_compCounters.undecided;
            slog(1, fmt::format("GPRL comp: input #{} (job {}) {}: {} | {}", job.index, job.jobId, e->sa.decided ? "decided" : "undecided", job.planner.describe(), work));
        }
        else {
            e->saRan = false;
            e->saWhy = why;
            if (proofFailed) e->stateReplayValid = false;
            slog(1, fmt::format("GPRL comp: input #{} (job {}) DROPPED {}: {} | {}", job.index, job.jobId, solver::status::name(why), job.fail, work));
        }
        e->compDone = true;
        if (m_ledger.state(job.jobId) == solver::LedgerState::AwaitingSA) {
            m_ledger.saDone(job.jobId);
            drainResults();
        }
    }
}

void CloneEngine::compAbortAll(Reason why) {
    // bounded: one finish per job; the result keeps what was decided, open sides end with `why`
    for (auto& job : m_compJobs) {
        job.planner.finish(why);
        ++m_compCounters.cutByRestart;
        compFinish(job, true, why, false);
    }
    m_compJobs.clear();
}

void CloneEngine::runCompensation(int target) {
    if (!m_pl || !m_pl->m_player1) return;
    auto p1 = m_pl->m_player1;
    bool const gate = m_measuring && !m_throttled && !m_frameSlow;
    Job ctx;
    ctx.inputId = kNoInputId;   // every logged input replays unless a trial moved it (Clone::moved)
    ctx.down = false;
    ctx.subtick = m_cfg.subtick;
    for (size_t ji = 0; ji < m_compJobs.size();) {
        CompJob& job = m_compJobs[ji];
        // the ledger entry is gone (the job was dropped / its result emitted): nothing to attach to
        if (!m_ledger.has(job.jobId)) {
            compFree(job);
            ++m_compCounters.discarded;
            m_compJobs.erase(m_compJobs.begin() + static_cast<std::ptrdiff_t>(ji));
            continue;
        }
        job.planner.setNow(m_frame);
        // 1. spawn: the delayed control before the first trial, then the planner's trials
        if (gate && job.planner.hasPending() && job.control < 0 && !job.controlDone) {
            int r = spawnCompControl(job);
            if (r < 0) {
                job.fail = "the job's base snapshot left the history ring before its first trial";
                compFinish(job, false, Reason::SaExpired, false);
                m_compJobs.erase(m_compJobs.begin() + static_cast<std::ptrdiff_t>(ji));
                continue;
            }
        }
        if (gate && job.control >= 0) {
            solver::comp::spawnCompTrials(job.planner, static_cast<int>(job.running.size()), m_cfg.compensation.parallelClones, [this] { return allocClone(false); },
                                          [&](int idx, solver::comp::CompTrial const& t) {
                                              int r = spawnCompTrial(job, idx, t);
                                              if (r <= 0 && idx >= 0 && idx < static_cast<int>(m_clones.size())) freeClone(m_clones[static_cast<size_t>(idx)]);
                                              return r > 0;
                                          });
        }
        // 2. step the control and the trials (the local jobs' catch-up budget and frame budget)
        std::string controlFail;
        auto stepClone = [&](Clone& c, CompTrialRun* run) {
            int budget = solver::budget::catchUpBudget(target - c.stepDone);
            while (c.state == CloneStatus::Running && c.stepDone < target && budget > 0 && m_cloneStepsThisFrame < m_stepBudget) {
                bool const ran = simStep(c, ctx, c.stepDone + 1) >= 0;
                if (ran) {
                    --budget;
                    ++job.cloneSteps;
                    ++m_compCounters.cloneSteps;
                }
                if (run) {
                    compAfterStep(job, *run);
                    if (run->clone < 0) break;
                }
                else {
                    if (c.state != CloneStatus::Running) break;
                    // F1: the delayed control is checked against the RECORDED run on every catch-up
                    // step (the ring snapshot of the next step), exactly like the SA job's control
                    // (matchesSnapshot): the trials decide during this stretch
                    auto const& next = m_hist[ring(c.stepDone + 1)];
                    if (ran && next.step == c.stepDone + 1 && c.stepDone < target) {
                        std::string why;
                        mirrorNextStepSpeed(c);
                        if (!matchesSnapshot(c.obj, next.state, why)) {
                            controlFail = fmt::format("the delayed control differs from the recorded run at step {} (frame {:.1f}, base step {}): {} | control {}", c.stepDone,
                                                      c.frameDone, job.controlBaseStep, why, stateStr(c.obj));
                            break;
                        }
                        ++job.controlCompared;
                    }
                }
            }
        };
        if (job.control >= 0) stepClone(m_clones[static_cast<size_t>(job.control)], nullptr);
        if (!controlFail.empty()) {
            job.fail = controlFail;
            ++m_compCounters.controlMismatch;
            compFinish(job, false, Reason::SaControlMismatch, true);
            m_compJobs.erase(m_compJobs.begin() + static_cast<std::ptrdiff_t>(ji));
            continue;
        }
        for (auto& run : job.running) {
            if (run.clone < 0) continue;
            stepClone(m_clones[static_cast<size_t>(run.clone)], &run);
        }
        std::erase_if(job.running, [](CompTrialRun const& r) { return r.clone < 0; });
        // 3. the delayed control against the live player (the proof of the catch-up replay)
        if (job.control >= 0) {
            auto& c = m_clones[static_cast<size_t>(job.control)];
            bool dropped = false;
            if (c.state == CloneStatus::Running && c.stepDone == target && !m_realDead) {
                std::string why;
                mirrorNextStepSpeed(c);
                if (!statesMatch(c.obj, p1, why)) {
                    job.fail = fmt::format("the delayed control differs from the live player at step {} (frame {:.1f}, base step {}): {} | control {}", target, c.frameDone,
                                           job.controlBaseStep, why, stateStr(c.obj));
                    ++m_compCounters.controlMismatch;
                    compFinish(job, false, Reason::SaControlMismatch, true);
                    dropped = true;
                }
                else ++job.controlCompared;
            }
            else if (c.state == CloneStatus::Dead) {
                bool withReal = false;
                for (auto const& mk : m_deathMarks) {
                    if (solver::diedWithReal(c.deathFrame, c.deathObjId, c.deathX, mk.frame, mk.obj, mk.x)) withReal = true;
                }
                if (withReal) {
                    // the replay reproduced a real death inside the span: nothing to prove beyond it
                    job.planner.finish(Reason::SaDeathInSpan);
                    freeClone(c);
                    job.control = -1;
                    job.controlDone = true;
                }
                else {
                    job.fail = fmt::format("the delayed control died on #{} at x={:.0f} (frame {:.1f}) but the live player did not", c.deathObjId, c.deathObjX, c.deathFrame);
                    ++m_compCounters.controlMismatch;
                    compFinish(job, false, Reason::SaControlMismatch, true);
                    dropped = true;
                }
            }
            else if (c.state != CloneStatus::Running) {
                job.fail = "the delayed control became invalid (" + (c.invalidReason.empty() ? std::string("invalid") : c.invalidReason) + ")";
                compFinish(job, false, Reason::SaInvalidTrials, false);
                dropped = true;
            }
            if (dropped) {
                m_compJobs.erase(m_compJobs.begin() + static_cast<std::ptrdiff_t>(ji));
                continue;
            }
        }
        // 4. done: every reachable slot decided (trials beyond a fail are cancelled). A result that
        // ran trials needs its control to have matched at least once (F1): the control catches up
        // within a few steps, so this never waits long
        bool const proven = job.trials == 0 || job.controlCompared > 0 || job.controlDone || job.control < 0;
        if (job.planner.done() && job.localDone && proven) {
            compFinish(job, true, Reason::SaUndecided, false);
            m_compJobs.erase(m_compJobs.begin() + static_cast<std::ptrdiff_t>(ji));
            continue;
        }
        ++ji;
    }
}

std::string CloneEngine::compSummary() const {
    auto const& k = m_compCounters;
    return fmt::format("; compensation (ship): jobs {} ({} open), trials {} ({} passes, {} compensated, {} died, {} invalid, {} crossed), clone steps {}, controls {} (mismatch {}), "
                       "decided {}, undecided {}, aborted {}, discarded {}, skipped wide {}, not started (budget) {}",
                       k.jobs, m_compJobs.size(), k.trials, k.passes, k.compensated, k.fails, k.invalid, k.crossed, k.cloneSteps, k.controls, k.controlMismatch, k.decided,
                       k.undecided, k.cutByRestart, k.discarded, k.notStartedWide, k.notStartedBudget);
}

}  // namespace gprl::clone
