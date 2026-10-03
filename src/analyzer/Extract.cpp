// READ-ONLY RULE: nothing in src/analyzer ever writes a GD field or calls a GD method with a side
// effect. This file reads binding members and pure getters only (list in Extract.hpp); it never
// calls GameObject::getObjectRect() / getObjectRect2() / getOrientedBox() / getBoxOffset() (they
// write GD's caches, see Extract.hpp): the rect is GD's formula, replicated in
// core/analyzer_extract.hpp gdObjectRect, on the level's static fields.
#include "Extract.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <new>
#include <vector>

#include "../../core/analyzer_identity.hpp"
#include "../../core/display.hpp"
#include "../../core/sim/physics.hpp"
#include "../../core/solver/isolation_guard.hpp"
#include "../../core/solver/live_state.hpp"
#include "../Tracker.hpp"
#include "../solver/GdOracle.hpp"
#include "../solver/LiveCapture.hpp"
#include "Modes.hpp"

using namespace geode::prelude;
namespace live = gprl::solver::live;
namespace family = gprl::analyzer::family;

// GD 2.2081 (win) offsets of every GameObject field the rect rule reads, as the disassembly uses
// them (core/analyzer_extract.hpp, the comment block above gdObjectRect: getObjectRect 0x1976c0,
// getObjectRect2 0x197850, getRealPosition 0x197b20, getBoxOffset 0x1a17d0, updateIsOriented
// 0x1a1730, updateOrientedBox 0x1a1570, getObjectRotation 0x1a14f0, setRotation 0x197e00,
// updateStartValues 0x190920, objectFromVector 0x19d1e0, determineSlopeDirection 0x19c2c0). Every
// field has a binding name; these asserts pin the names to the offsets the asm reads, so a binding
// update that moves one fails the build instead of silently reading another field.
static_assert(offsetof(GameObject, m_rotationXOffset) == 0x2a8);
static_assert(offsetof(GameObject, m_rotationYOffset) == 0x2b0);
static_assert(offsetof(GameObject, m_isFlipX) == 0x2c9);
static_assert(offsetof(GameObject, m_isFlipY) == 0x2ca);
static_assert(offsetof(GameObject, m_customBoxOffset) == 0x2cc);
static_assert(offsetof(GameObject, m_boxOffsetCalculated) == 0x2d4);   // GD's DIRTY flag of m_boxOffset (cleared by getBoxOffset)
static_assert(offsetof(GameObject, m_boxOffset) == 0x2d8);
static_assert(offsetof(GameObject, m_shouldUseOuterOb) == 0x2e8);
static_assert(offsetof(GameObject, m_width) == 0x2fc);
static_assert(offsetof(GameObject, m_height) == 0x300);
static_assert(offsetof(GameObject, m_objectRect) == 0x358);
static_assert(offsetof(GameObject, m_isObjectRectDirty) == 0x368);
static_assert(offsetof(GameObject, m_isMirroredByScale) == 0x36d);
static_assert(offsetof(GameObject, m_objectRadius) == 0x38c);
static_assert(offsetof(GameObject, m_isRotationAligned) == 0x390);
static_assert(offsetof(GameObject, m_spriteWidthScale) == 0x394);
static_assert(offsetof(GameObject, m_spriteHeightScale) == 0x398);
static_assert(offsetof(GameObject, m_objectType) == 0x3a0);
static_assert(offsetof(GameObject, m_positionX) == 0x3b8);
static_assert(offsetof(GameObject, m_positionY) == 0x3c0);
static_assert(offsetof(GameObject, m_startPosition) == 0x3c8);
static_assert(offsetof(GameObject, m_startRotationX) == 0x3d4);
static_assert(offsetof(GameObject, m_startRotationY) == 0x3d8);
static_assert(offsetof(GameObject, m_startScaleX) == 0x3dc);
static_assert(offsetof(GameObject, m_startScaleY) == 0x3e0);
static_assert(offsetof(GameObject, m_startFlipX) == 0x3ec);
static_assert(offsetof(GameObject, m_startFlipY) == 0x3ed);
static_assert(offsetof(GameObject, m_slopeUphill) == 0x440);
static_assert(offsetof(GameObject, m_scaleX) == 0x488);
static_assert(offsetof(GameObject, m_scaleY) == 0x48c);
static_assert(offsetof(GameObject, m_isNoTouch) == 0x4c6);

namespace gprl::analyzer::extraction {

namespace {

constexpr unsigned kChunk = 64;
constexpr int kMaxRestarts = 1;

using Clock = std::chrono::steady_clock;

struct State {
    PlayLayer* pl = nullptr;
    unsigned index = 0;
    unsigned total = 0;
    int restarts = 0;
    extract::WorldBuilder builder;
    extract::BuildParams params;
    Progress progress;
    // level families: the objects the physics ignores, read in the same walk (core/analyzer_identity.hpp)
    gprl::identity::DecoSet deco;
    int decoCapped = 0;           // decoration objects over kConstants.maxDecoObjects (counted, not kept)
    int anticheatSkipped = 0;     // GD's anti-cheat spike (never a level object)
    int copiedFromGdId = 0;
    std::string nameHint;
};

State s_state;
std::optional<Handover> s_ready;
// the analyzer's own snapshots (Snapshot is ~4 KB: static, not on the stack)
live::Snapshot s_before;
live::Snapshot s_after;
std::vector<EnhancedGameObject*> const kNoAct;   // the activation part of the snapshot is the solver's;
std::vector<float> const kNoActX;                // the extraction checks its own objects per chunk

double usSince(Clock::time_point t0) { return std::chrono::duration<double, std::micro>(Clock::now() - t0).count(); }

uint64_t mixBits(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

uint64_t fbits(float f) { return std::bit_cast<uint32_t>(f); }

/// FNV-1a over what a write by the extraction could change in the objects [a, b): the rect cache
/// and its dirty flag (getObjectRect's write class), the box-offset / oriented-box dirty flags,
/// activation / disabled bytes, position, rotation, visibility. Read twice per chunk (before /
/// after) - the same reads, nothing else.
uint64_t fingerprint(CCArray* arr, unsigned a, unsigned b) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = a; i < b; ++i) {
        auto* g = typeinfo_cast<GameObject*>(arr->objectAtIndex(i));
        h = mixBits(h, reinterpret_cast<uint64_t>(g));
        if (!g) continue;
        auto const& r = g->m_objectRect;
        h = mixBits(h, (fbits(r.origin.x) << 32) | fbits(r.origin.y));
        h = mixBits(h, (fbits(r.size.width) << 32) | fbits(r.size.height));
        auto pos = g->getPosition();
        h = mixBits(h, (fbits(pos.x) << 32) | fbits(pos.y));
        h = mixBits(h, fbits(g->getRotation()));
        h = mixBits(h, static_cast<uint64_t>(g->m_isObjectRectDirty) | (static_cast<uint64_t>(g->m_isActivated) << 8) | (static_cast<uint64_t>(g->m_isDisabled) << 16)
                           | (static_cast<uint64_t>(g->m_isGroupDisabled) << 24) | (static_cast<uint64_t>(g->isVisible()) << 32)
                           | (static_cast<uint64_t>(g->m_isOrientedBoxDirty) << 40) | (static_cast<uint64_t>(g->m_boxOffsetCalculated) << 48));
    }
    return h;
}

sim::StartState fromSettings(LevelSettingsObject* ls, LevelSettingsObject* level) {
    sim::StartState s;
    if (ls) {
        s.mode = extract::gamemodeFromStartMode(ls->m_startMode);
        s.mini = ls->m_startMini;
        s.speed = extract::speedFromGeodeSpeed(static_cast<int>(ls->m_startSpeed));
        s.dual = ls->m_startDual;
        s.upsideDown = ls->m_isFlipped;
        s.mirror = ls->m_mirrorMode;
        s.reversed = ls->m_reverseGameplay;
    }
    if (level) {
        s.platformer = level->m_platformerMode;
        s.twoPlayer = level->m_twoPlayerMode;
    }
    return s;
}

/// GD's anti-cheat spike (GJBaseGameLayer::m_anticheatSpike, a type-2 hazard PlayLayer moves onto
/// player 1 every step): never a level object. The rest of the mod ignores it the same way
/// (GdOracle claimDestroy / onRealDestroy, Tracker onDestroyPlayer).
bool isAnticheatSpike(GameObject* g) {
    auto* pl = s_state.pl;
    if (!pl || !pl->m_anticheatSpike) return false;
    return extract::isAnticheatSpike(g, g->m_uniqueID, pl->m_anticheatSpike, pl->m_anticheatSpike->m_uniqueID);
}

/// The STATIC geometry (the level's own values: what objectFromVector / updateStartValues wrote at
/// load) and GD's LIVE values for the formula check. Plain field reads, nothing else.
template <class Reading>
void readStaticGeometry(GameObject* g, Reading& r) {
    r.rotation = g->m_startRotationX;
    r.rotationY = g->m_startRotationY;
    r.haveRotationY = true;
    r.scaleX = g->m_startScaleX;
    r.scaleY = g->m_startScaleY;
    r.flipX = g->m_startFlipX;
    r.flipY = g->m_startFlipY;
    r.width = g->m_width;
    r.height = g->m_height;
    r.spriteWidthScale = g->m_spriteWidthScale;
    r.spriteHeightScale = g->m_spriteHeightScale;
    r.mirroredByScale = g->m_isMirroredByScale;
    r.boxOffsetX = g->m_customBoxOffset.x;
    r.boxOffsetY = g->m_customBoxOffset.y;
    r.objectRadius = g->m_objectRadius;
}

void readLive(GameObject* g, extract::ObjectReading& r) {
    r.haveLive = true;
    auto& l = r.live;
    l.scaleX = g->m_scaleX;
    l.scaleY = g->m_scaleY;
    l.rotationX = g->m_startRotationX + g->m_rotationXOffset;   // getObjectRotation (0x1a14f0): movss +0x3d4; addss +0x2a8
    l.rotationY = g->m_startRotationY + g->m_rotationYOffset;
    l.flipX = g->m_isFlipX;
    l.flipY = g->m_isFlipY;
    l.aligned = g->m_isRotationAligned;
    l.oriented = g->m_shouldUseOuterOb;
    l.boxOffsetCached = !g->m_boxOffsetCalculated;   // the binding's name; GD uses the byte as "dirty"
    l.boxOffset = {g->m_boxOffset.x, g->m_boxOffset.y};
    r.rectExact = !g->m_isObjectRectDirty;   // getObjectRect() is never called (it writes the cache)
    if (r.rectExact) {
        auto const& rc = g->m_objectRect;
        r.rect = {rc.origin.x, rc.origin.y, rc.size.width, rc.size.height};
    }
}

/// Level families: an object the physics ignores becomes a DecoObject (decoration, collectibles)
/// or a TriggerObject (harmless triggers) of the identity. Reads only (the list in Extract.hpp);
/// what the object IS is decided by core/analyzer_identity.hpp (roleOf / makeDeco / makeTrigger).
void readPassive(GameObject* g, extract::ObjectReading const& r, extract::Classified const& c) {
    auto role = family::roleOf(c);
    if (role == family::Role::Trigger) {
        family::TriggerReading t;
        t.objectId = r.objectId;
        t.x = r.haveStart ? r.startX : r.x;
        if (auto* eff = typeinfo_cast<EffectGameObject*>(g)) t.duration = eff->m_duration;
        if (static_cast<int>(s_state.deco.triggers.size()) >= extract::kConstants.maxDecoObjects) {
            ++s_state.decoCapped;
            return;
        }
        s_state.deco.triggers.push_back(family::makeTrigger(t));
        return;
    }
    if (role != family::Role::Deco) return;
    if (static_cast<int>(s_state.deco.deco.size()) >= extract::kConstants.maxDecoObjects) {
        ++s_state.decoCapped;   // AN-D12: the identity's decoration set has the same cap, applied while collecting
        return;
    }
    family::DecoReading d;
    d.objectId = r.objectId;
    d.gdType = r.gdType;
    d.x = r.x;
    d.y = r.y;
    d.haveStart = r.haveStart;
    d.startX = r.startX;
    d.startY = r.startY;
    readStaticGeometry(g, d);
    d.zLayer = static_cast<int>(g->m_zLayer);
    d.zOrder = g->m_zOrder;
    d.noGlow = g->m_hasNoGlow;
    d.blending = g->m_baseOrDetailBlending;
    d.opacity = -1;   // CCNodeRGBA::getOpacity() is the LIVE (fading / alpha-triggered) value: not a static field
    d.noTouch = g->m_isNoTouch;
    d.passable = g->m_isPassable;
    s_state.deco.deco.push_back(family::makeDeco(d));
}

void readOne(CCObject* obj) {
    auto* g = typeinfo_cast<GameObject*>(obj);
    if (!g) return;
    if (isAnticheatSpike(g)) {
        ++s_state.anticheatSkipped;
        return;
    }
    extract::ObjectReading r;
    r.objectId = g->m_objectID;
    r.gdType = static_cast<int>(g->m_objectType);
    r.isTrigger = g->m_isTrigger;
    // getRealPosition() (0x197b20) without the call: the logical position as GD's collision uses it
    r.x = static_cast<float>(g->m_positionX);
    r.y = static_cast<float>(g->m_positionY);
    r.haveStart = true;
    r.startX = g->m_startPosition.x;
    r.startY = g->m_startPosition.y;
    auto c = extract::classify(r.gdType, r.objectId, r.isTrigger);
    if (c.use == extract::Use::Decoration || c.use == extract::Use::Skip) {
        // decoration is most of a level: the builder counts its column; the identity keeps its rect / z
        s_state.builder.add(r);
        readPassive(g, r, c);
        return;
    }
    if (c.use == extract::Use::StartPos) {
        s_state.builder.add(r);
        if (auto* sp = typeinfo_cast<StartPosObject*>(g); sp && sp->m_startSettings) {
            auto st = fromSettings(sp->m_startSettings, s_state.pl ? s_state.pl->m_levelSettings : nullptr);
            st.x = r.startX;
            st.y = r.startY;
            s_state.builder.addStartPos(st);
        }
        return;
    }
    r.uniqueId = g->m_uniqueID;
    readStaticGeometry(g, r);
    readLive(g, r);
    r.noTouch = g->m_isNoTouch;
    r.passable = g->m_isPassable;
    r.hiddenRuntime = !g->isVisible() || g->m_isDisabled;   // diagnostic count only (core WorldBuilder::add)
    r.slopeHazard = g->m_slopeIsHazard;
    if (c.kind == sim::ObjKind::Slope) {
        // the World's orientation comes from GD's own rules replicated on the STATIC rotation /
        // flips (core gdSlopeUphill = determineSlopeDirection 0x19c2c0, gdFacingDown =
        // isFacingDown 0x1a1910, which reads getObjectRotation() and isFlipY() - both change with
        // rotate triggers); m_slopeUphill as GD holds it now is only compared (slopesLiveOff)
        r.live.haveSlopeUphill = true;
        r.live.slopeUphill = g->m_slopeUphill;
    }
    if (auto* e = typeinfo_cast<EnhancedGameObject*>(g)) r.multiActivate = e->m_isMultiActivate;
    if (c.trigger) {
        if (auto* eff = typeinfo_cast<EffectGameObject*>(g)) r.targetGroupId = eff->m_targetGroupID;
    }
    if (g->m_groups && g->m_groupCount > 0) {
        int n = std::min<int>(g->m_groupCount, 10);
        for (int i = 0; i < n; ++i) r.groups[static_cast<size_t>(i)] = (*g->m_groups)[static_cast<size_t>(i)];
        r.groupCount = n;
    }
    s_state.builder.add(r);
}

/// The walk stops for this visit without touching anything (out of memory, an unexpected error).
void failWalk(char const* why) {
    s_state.progress.failed = true;
    s_state.progress.failReason = why;
    s_state.progress.active = false;
    // release what was collected (a failed allocation must not leave a half-built world behind)
    try {
        s_state.builder.reset();
        s_state.deco = gprl::identity::DecoSet{};
    } catch (...) {
    }
}

void stopForIsolation(std::vector<live::Diff> const& diffs, int total, unsigned a, unsigned b) {
    // 1. the owner's exact block for every differing field
    for (auto const& d : diffs) log::error("{}", live::ownerBlock(d));
    if (total > static_cast<int>(diffs.size())) log::error("LIVE_STATE_MUTATION_DETECTED\n... {} more field(s) differ", total - static_cast<int>(diffs.size()));
    // 2. the summary line, then stop the analyzer for this level visit
    auto const& d0 = diffs.front();
    log::error("GPRL analyzer: LIVE_STATE_MUTATION_DETECTED field: {} before: {} after: {}{} | extraction slice {} (objects {}..{} of {}) | action: extraction abandoned "
               "for this level visit, analyzer stopped (isolation), live solver breaker tripped",
               d0.field, d0.before, d0.after, total > 1 ? fmt::format(" (+{} more)", total - 1) : std::string(), s_state.progress.slices + 1, a, b, s_state.total);
    s_state.progress.isolationStopped = true;
    failWalk("isolation");
    // 3. the clone solver behaves as on its own breach (no-op without an engine)
    solver::oracle::tripIsolationBreaker("analyzer_extraction");
}

void restartWalk(unsigned count) {
    s_state.builder.reset();
    s_state.deco = gprl::identity::DecoSet{};
    s_state.decoCapped = 0;
    s_state.anticheatSkipped = 0;
    // decoration is typically 70-90 % of a level: one reservation instead of regrowth inside slices
    // (capped like the set itself)
    size_t want = std::min<size_t>(static_cast<size_t>(count) * 3u / 4u, static_cast<size_t>(extract::kConstants.maxDecoObjects));
    s_state.deco.deco.reserve(want);
    s_state.index = 0;
    s_state.total = count;
    s_state.progress.index = 0;
    s_state.progress.total = static_cast<int>(count);
}

}  // namespace

void begin(PlayLayer* pl) {
    s_ready.reset();
    s_state = State{};
    s_state.pl = pl;
    if (!pl || !pl->m_objects) {
        s_state.progress.failed = true;
        s_state.progress.failReason = "no object array";
        return;
    }
    auto& p = s_state.params;
    p.gdLevelId = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
    p.levelHash = tracker::levelHash();
    // level families: the copied-from hint (GJGameLevel::m_originalLevel, GD key 30) and the name hint
    s_state.copiedFromGdId = pl->m_level ? std::max(0, static_cast<int>(pl->m_level->m_originalLevel.value())) : 0;
    s_state.nameHint = pl->m_level ? display::levelNameHint(std::string(pl->m_level->m_levelName)) : std::string();
    p.groundY = extract::kConstants.groundY;
    p.start = fromSettings(pl->m_levelSettings, pl->m_levelSettings);
    // ONE start-state rule for the World and its gameplay hash, whatever this visit entered with (a
    // StartPos or not, wherever player 1 stands when the extraction starts): the level settings'
    // mode / mini / speed / ... at x = defaultStartX on the floor line (the simulator's
    // floorCentreY). Player 1's live spawn is only logged when it differs (the in-game check).
    float const startY = static_cast<float>(sim::floorCentreY(p.start.mode, p.start.mini, p.groundY));
    p.start.x = extract::kConstants.defaultStartX;
    p.start.y = startY;
    p.start.percent = 0.f;
    if (!pl->m_startPosObject && pl->m_player1) {
        auto pos = pl->m_player1->getPosition();
        if (std::fabs(pos.x - p.start.x) > 0.05f || std::fabs(pos.y - p.start.y) > 0.05f)
            log::info("GPRL analyzer: player 1 spawned at ({:.2f}, {:.2f}); the World's level start is ({:.2f}, {:.2f}) (the one start rule; check the constant)",
                      pos.x, pos.y, p.start.x, p.start.y);
    }
    try {
        restartWalk(pl->m_objects->count());
    } catch (std::exception const& e) {
        log::error("GPRL analyzer: extraction not started ({}); the level analysis stops for this visit", e.what());
        failWalk("out of memory");
        return;
    }
    s_state.progress.active = true;
    if (modes::analysisDebug()) {
        log::info("GPRL analyzer: extraction started - level {} ({} objects in m_objects), start {} at ({:.1f}, {:.1f}){}", p.gdLevelId, s_state.total,
                  sim::gamemodeName(p.start.mode), p.start.x, p.start.y, pl->m_startPosObject ? " (entered with a StartPos: the level start is still the World's start)" : "");
    }
}

bool slice(PlayLayer* pl, int budgetUs) {
    auto& st = s_state;
    if (!st.progress.active || pl != st.pl || budgetUs <= 0) return false;
    auto* arr = pl->m_objects;
    if (!arr) {
        st.progress.active = false;
        st.progress.failed = true;
        st.progress.failReason = "object array gone";
        return false;
    }
    try {
        unsigned count = arr->count();
        if (count != st.total) {
            if (st.restarts >= kMaxRestarts) {
                st.progress.active = false;
                st.progress.failed = true;
                st.progress.failReason = "the level's object list keeps changing";
                log::warn("GPRL analyzer: extraction stopped - m_objects changed from {} to {} objects twice during the walk", st.total, count);
                return false;
            }
            ++st.restarts;
            log::info("GPRL analyzer: m_objects changed from {} to {} objects during the walk - restarting it once", st.total, count);
            restartWalk(count);
        }
        auto t0 = Clock::now();
        // AN-D11: the slice is an analysis block inside the invariant check
        clone::captureLive(pl, kNoAct, kNoActX, st.progress.slices, s_before);
        ++st.progress.checks;
        while (st.index < count) {
            unsigned a = st.index;
            unsigned b = std::min(count, a + kChunk);
            uint64_t before = fingerprint(arr, a, b);
            for (unsigned i = a; i < b; ++i) readOne(arr->objectAtIndex(i));
            uint64_t after = fingerprint(arr, a, b);
            ++st.progress.checks;
            if (after != before) {
                live::Diff d{"objects.extraction_chunk", fmt::format("objects {}..{} fingerprint 0x{:016x}", a, b, before),
                             fmt::format("objects {}..{} fingerprint 0x{:016x}", a, b, after)};
                stopForIsolation({d}, 1, a, b);
                return false;
            }
            st.index = b;
            if (usSince(t0) >= budgetUs) break;
        }
        bool const complete = st.index >= count;
        // the level-wide facts are read INSIDE the block (before the closing capture), on the last slice
        // (m_levelLength is set during setup)
        if (complete) {
            st.params.lengthX = pl->m_levelLength;
            st.params.endX = pl->m_endPortal ? pl->m_endPortal->getPositionX() : pl->m_levelLength;
        }
        clone::captureLive(pl, kNoAct, kNoActX, st.progress.slices, s_after);
        int total = 0;
        auto diffs = live::compare(s_before, s_after, solver::isolation::kIsolationConfig.maxLoggedFields, &total);
        if (!diffs.empty()) {
            stopForIsolation(diffs, total, 0, st.index);
            return false;
        }
        double us = usSince(t0);
        st.progress.lastSliceUs = us;
        st.progress.maxSliceUs = std::max(st.progress.maxSliceUs, us);
        st.progress.totalMs += us / 1000.0;
        ++st.progress.slices;
        st.progress.index = static_cast<int>(st.index);
        if (!complete) return false;

        auto& p = st.params;
        if (!(p.endX > 0.f)) p.endX = p.lengthX;
        Handover h;
        h.counters = st.builder.counters();
        h.builder = std::move(st.builder);
        h.params = p;
        h.arrayObjects = static_cast<int>(count);
        h.slices = st.progress.slices;
        h.mainThreadMs = st.progress.totalMs;
        h.maxSliceUs = st.progress.maxSliceUs;
        h.deco = std::move(st.deco);
        h.copiedFromGdId = st.copiedFromGdId;
        h.nameHint = st.nameHint;
        h.anticheatSkipped = st.anticheatSkipped;
        h.decoCapped = st.decoCapped;
        st.builder = extract::WorldBuilder{};
        st.deco = gprl::identity::DecoSet{};
        st.progress.active = false;
        st.progress.done = true;
        auto const& c = h.counters;
        log::info("GPRL analyzer: extraction done ({}) - {} objects read in {} slices ({:.1f} ms on the game thread, max slice {:.0f} us, budget now {} us): gameplay {}"
                  "{}, decoration {}, skipped {} (harmless triggers {}), gameplay triggers {}, StartPos {}, anti-cheat spike skipped {}, rects: GD's formula on static "
                  "fields, estimate check {} of {} clean rects off by > {:.2f} ({} not computed by GD yet), {} oriented, {} negative, box offsets {} of {} off, oriented "
                  "flag {} off, slopes {} by GD's rules ({} corner derivation / {} live m_slopeUphill differ), no-touch {}, runtime-hidden {} (not used), away from "
                  "m_startPosition {} (kept at the start), length {:.0f}, end x {:.0f}, isolation checks {} (0 differences); identity set: {} decoration rects, {} "
                  "triggers{}, copied from {}",
                  extract::kVersion, count, h.slices, h.mainThreadMs, h.maxSliceUs, budgetUs, c.gameplay,
                  c.capDropped > 0 ? fmt::format(" ({} over the {} cap: TOO LARGE)", c.capDropped, extract::kConstants.maxObjects) : std::string(), c.decoration,
                  c.skipped, c.harmlessTriggers, c.gameplayTriggers, c.startPositions, h.anticheatSkipped, c.rectsEstimateOff, c.rectsChecked,
                  extract::kConstants.rectCheckTolerance, c.rectsUnchecked, c.rectsOriented, c.rectsNegative, c.boxOffsetsOff, c.boxOffsetsChecked, c.orientedFlagOff,
                  c.slopesFromGd, c.slopesDerivationOff, c.slopesLiveOff, c.hidden, c.hiddenRuntime, c.movedNow, p.lengthX, p.endX, st.progress.checks,
                  h.deco.deco.size(), h.deco.triggers.size(), h.decoCapped > 0 ? fmt::format(" ({} over the cap, not kept)", h.decoCapped) : std::string(),
                  h.copiedFromGdId);
        if (modes::analysisDebug() || c.rectsEstimateOff > 0) {
            for (auto const& m : c.rectMismatches) log::info("GPRL analyzer: GD's rect formula differs from GD's cache: {}", m);
        }
        if (modes::analysisDebug() && !c.unknownTriggerIds.empty()) {
            std::string ids;
            for (int id : c.unknownTriggerIds) ids += (ids.empty() ? "" : ", ") + std::to_string(id);
            log::info("GPRL analyzer: trigger ids treated as harmless (not in the gameplay table): {}", ids);
        }
        s_ready = std::move(h);
        return true;
    } catch (std::exception const& e) {
        // a push_back inside the walk ran out of memory (or another error): never out of postUpdate
        log::error("GPRL analyzer: extraction stopped at {} of {} objects ({}); the level analysis stops for this visit", st.index, st.total, e.what());
        failWalk(dynamic_cast<std::bad_alloc const*>(&e) ? "out of memory" : "extraction error");
        return false;
    }
}

std::optional<Handover> take() {
    std::optional<Handover> out = std::move(s_ready);
    s_ready.reset();
    return out;
}

void abandon(char const* why) {
    if (s_state.progress.active && modes::analysisDebug())
        log::info("GPRL analyzer: extraction abandoned at {} of {} objects ({})", s_state.index, s_state.total, why ? why : "?");
    s_state = State{};
    s_ready.reset();
}

Progress progress() { return s_state.progress; }

}  // namespace gprl::analyzer::extraction
