#include "death_detector.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace gprl::death {

char const* name(Origin o) {
    switch (o) {
        case Origin::LivePlayLayer: return "live_play_layer";
        case Origin::TimingSolver: return "timing_solver";
        case Origin::Analyzer: return "level_analyzer";
        case Origin::Replay: return "replay_clone";
        case Origin::PracticeSim: return "practice_simulation";
    }
    return "unknown";
}

char const* name(Via v) {
    switch (v) {
        case Via::DestroyHook: return "destroy_hook";
        case Via::CollisionReturn: return "collision_return";
    }
    return "unknown";
}

char const* name(Source s) {
    switch (s) {
        case Source::LiveGdDeath: return "live_gd_death";
        case Source::ExternalKill: return "external_kill";
        case Source::SimulatedCollision: return "simulated_collision";
        case Source::RejectedClone: return "rejected_clone";
        case Source::RejectedStaleAttempt: return "rejected_stale_attempt";
        case Source::RejectedUnknownPlayer: return "rejected_unknown_player";
        case Source::RejectedNotLethal: return "rejected_not_lethal";
    }
    return "unknown";
}

char const* name(Decision d) {
    switch (d) {
        case Decision::Death: return "death";
        case Decision::WouldBeDeath: return "would_be_death";
        case Decision::ContinuesContact: return "same_contact";
        case Decision::Rejected: return "rejected";
    }
    return "unknown";
}

char const* name(Reason r) {
    switch (r) {
        case Reason::RealDeath: return "real_death";
        case Reason::ExternalKillDeath: return "external_kill";
        case Reason::NewLethalContact: return "new_lethal_contact";
        case Reason::SameContact: return "same_contact";
        case Reason::NotLiveOrigin: return "not_live_origin";
        case Reason::SolverClone: return "solver_clone";
        case Reason::SolverStep: return "solver_step";
        case Reason::UnknownPlayer: return "unknown_player";
        case Reason::StaleSession: return "stale_session";
        case Reason::StaleAttempt: return "stale_attempt";
        case Reason::NoOpenAttempt: return "no_open_attempt";
        case Reason::DuringReset: return "during_reset";
        case Reason::AnticheatSpike: return "anticheat_spike";
        case Reason::AlreadyDead: return "already_dead";
        case Reason::LockedPlayer: return "gd_ignores_locked_player";
        case Reason::PlayerDiedFlag: return "gd_ignores_after_death";
        case Reason::IgnoreDamage: return "ignore_damage";
        case Reason::OutsideLiveStep: return "outside_live_step";
    }
    return "unknown";
}

// ---- detector ----

void Detector::beginSession(uint32_t sessionGen) {
    m_sessionGen = sessionGen;
    m_sessionOpen = true;
    m_open = false;
    m_attemptGen = 0;
    m_contact = {};
    m_nextContactId = 0;
    m_firstWouldBeTick = -1;
    m_counters = Counters{};
}

void Detector::beginAttempt(uint32_t attemptGen) {
    m_attemptGen = attemptGen;
    m_open = true;
    m_contact = {};
    m_nextContactId = 0;
    m_firstWouldBeTick = -1;
    m_counters = Counters{};
}

void Detector::endAttempt() { m_open = false; }

void Detector::endSession() {
    m_open = false;
    m_sessionOpen = false;
}

Verdict Detector::reject(Reason r, Source s, int player) {
    switch (s) {
        case Source::RejectedClone: ++m_counters.rejectedClone; break;
        case Source::RejectedStaleAttempt: ++m_counters.rejectedStale; break;
        case Source::RejectedUnknownPlayer: ++m_counters.rejectedUnknownPlayer; break;
        case Source::RejectedNotLethal: ++m_counters.rejectedNotLethal; break;
        case Source::SimulatedCollision: ++m_counters.simulated; break;
        default: break;
    }
    Verdict v;
    v.decision = Decision::Rejected;
    v.reason = r;
    v.source = s;
    v.player = player;
    return v;
}

Verdict Detector::onCandidate(Candidate const& c) {
    // 1. where it came from: only the real PlayLayer's death path can ever be a live death
    if (c.origin == Origin::TimingSolver) return reject(Reason::SolverClone, Source::RejectedClone);
    if (c.origin != Origin::LivePlayLayer) return reject(Reason::NotLiveOrigin, Source::SimulatedCollision);
    // 2. the solver's hidden clones can reach the live death hook: rejected before anything changes
    if (c.solverClone) return reject(Reason::SolverClone, Source::RejectedClone);
    if (c.solverStepping) return reject(Reason::SolverStep, Source::RejectedClone);
    // 3. strict identity: pointer-equal to the active PlayLayer's player 1 / player 2
    if (c.playerSlot != 1 && c.playerSlot != 2) return reject(Reason::UnknownPlayer, Source::RejectedUnknownPlayer);
    int const slot = c.playerSlot;
    // 4. the kill must belong to the CURRENT session and attempt generation
    if (!m_sessionOpen || c.sessionGen != m_sessionGen) return reject(Reason::StaleSession, Source::RejectedStaleAttempt, slot);
    if (c.attemptGen != m_attemptGen) return reject(Reason::StaleAttempt, Source::RejectedStaleAttempt, slot);
    if (!m_open) return reject(Reason::NoOpenAttempt, Source::RejectedStaleAttempt, slot);
    if (c.duringReset) return reject(Reason::DuringReset, Source::RejectedStaleAttempt, slot);
    // 5. calls GD itself never treats as a kill
    if (c.anticheatSpike) return reject(Reason::AnticheatSpike, Source::RejectedNotLethal, slot);
    if (c.wasDeadBefore) return reject(Reason::AlreadyDead, Source::RejectedNotLethal, slot);

    bool const live = !c.liveStepKnown || c.inLiveStep;
    // 6. the player is dead now: a real death, never a would-be death as well
    if (c.deadAfter) {
        ++m_counters.deaths;
        m_open = false;
        Verdict v;
        v.decision = Decision::Death;
        v.reason = live ? Reason::RealDeath : Reason::ExternalKillDeath;
        v.source = live ? Source::LiveGdDeath : Source::ExternalKill;
        v.player = slot;
        return v;
    }
    // 7. alive: kills GD's own rules would not have executed (no noclip involved)
    if (c.playerLocked) return reject(Reason::LockedPlayer, Source::RejectedNotLethal, slot);
    if (c.layerPlayerDied) return reject(Reason::PlayerDiedFlag, Source::RejectedNotLethal, slot);
    // a collision check that ran with ignoreDamage found a contact but raised no kill: Click Between
    // Frames probes the real player like this between its sub-steps (the game's own check follows),
    // and GD's "Ignore Damage" option does the same - the return value alone is never a death
    if (c.via == Via::CollisionReturn && c.ignoreDamage) return reject(Reason::IgnoreDamage, Source::RejectedNotLethal, slot);
    // 8. a swallowed kill for the real player that GD's own physics step did not raise
    if (!live) return reject(Reason::OutsideLiveStep, Source::SimulatedCollision, slot);

    // 9. GD tried to kill the real current player in its own physics step and the player is alive:
    //    one would-be death per continuous lethal contact (per player)
    Contact& contact = m_contact[static_cast<size_t>(slot)];
    bool const objectless = !c.hasObject;
    bool continues = false;
    if (contact.any) {
        // the same tick, the next one, or an older one (clamped clocks) continue the contact; so do two
        // kills without an object two ticks apart (GD's out-of-bounds kill re-arms every second tick)
        if (c.tick <= contact.lastTick + 1) continues = true;
        else if (objectless && contact.lastObjectless && c.tick <= contact.lastTick + 2) continues = true;
    }
    if (c.tick >= contact.lastTick || !contact.any) {
        contact.lastTick = contact.any ? std::max(contact.lastTick, c.tick) : c.tick;
        contact.lastObjectless = objectless;
    }
    Verdict v;
    v.source = Source::LiveGdDeath;
    v.player = slot;
    if (continues) {
        ++m_counters.continued;
        v.decision = Decision::ContinuesContact;
        v.reason = Reason::SameContact;
        v.contactId = contact.id;
        return v;
    }
    contact.any = true;
    contact.id = ++m_nextContactId;
    ++m_counters.wouldBeDeaths[static_cast<size_t>(slot)];
    if (m_firstWouldBeTick < 0) m_firstWouldBeTick = c.tick;
    v.decision = Decision::WouldBeDeath;
    v.reason = Reason::NewLethalContact;
    v.contactId = contact.id;
    return v;
}

// ---- debug records ----

namespace {

std::string fmt(char const* f, ...) {
    char buf[512];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof buf, f, args);
    va_end(args);
    return buf;
}

std::string boxStr(Box const& b) {
    if (!b.valid) return "none";
    return fmt("(%.1f,%.1f %.1fx%.1f)", b.x, b.y, b.w, b.h);
}

char const* yn(bool v) { return v ? "yes" : "no"; }

}  // namespace

bool overlaps(Box const& a, Box const& b) {
    if (!a.valid || !b.valid) return false;
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool overlapCentre(Box const& a, Box const& b, float& x, float& y) {
    if (!overlaps(a, b)) return false;
    float x0 = std::max(a.x, b.x), x1 = std::min(a.x + a.w, b.x + b.w);
    float y0 = std::max(a.y, b.y), y1 = std::min(a.y + a.h, b.y + b.h);
    x = (x0 + x1) / 2.f;
    y = (y0 + y1) / 2.f;
    return true;
}

std::string format(DebugRecord const& r) {
    auto const& c = r.candidate;
    auto const& v = r.verdict;
    std::string out = "GPRL death: ";
    out += v.accepted() ? "ACCEPT " : "REJECT ";
    out += name(v.decision);
    if (v.contactId) out += fmt(" contact=%u", v.contactId);
    out += fmt(" reason=%s source=%s via=%s origin=%s", name(v.reason), name(v.source), name(c.via), name(c.origin));
    out += fmt(" | session=%s attempt=%s gen=%u/%u tick=%lld pct=%.3f", r.sessionId.empty() ? "-" : r.sessionId.c_str(),
               r.attemptId.empty() ? "-" : r.attemptId.c_str(), c.sessionGen, c.attemptGen, static_cast<long long>(r.tick), r.percent);
    out += fmt(" | player=%s ptr=0x%llx%s pos=(%.1f,%.1f) box=%s", v.player == 1 ? "P1" : v.player == 2 ? "P2" : c.playerSlot == 1 ? "P1" : c.playerSlot == 2 ? "P2" : "none",
               static_cast<unsigned long long>(r.playerPtr), r.nullPlayerArg ? "(null arg = player 1)" : "", r.playerX, r.playerY, boxStr(r.playerBox).c_str());
    if (r.hasObject) {
        out += fmt(" | object id=%d uid=%d type=%d pos=(%.1f,%.1f) rot=%.1f scale=(%.2f,%.2f)", r.objectId, r.objectUid, r.objectType, r.objectX, r.objectY,
                   r.objectRotation, r.objectScaleX, r.objectScaleY);
        if (r.objectRadius > 0.f) out += fmt(" radius=%.1f", r.objectRadius);
        out += " box=" + boxStr(r.objectBox);
        if (r.playerBox.valid && r.objectBox.valid) out += fmt(" boxes_overlap=%s", yn(overlaps(r.playerBox, r.objectBox)));
    }
    else out += " | object none (block crush / slope / squeeze / out of bounds)";
    out += fmt(" | gd_death_fired=%s dead_after=%s dead_after_gd=%s was_dead=%s locked=%s player_died_flag=%s anticheat_spike=%s", yn(c.gdDeathFired), yn(c.deadAfter),
               yn(r.deadAfterGd), yn(c.wasDeadBefore), yn(c.playerLocked), yn(c.layerPlayerDied), yn(c.anticheatSpike));
    out += fmt(" | gprl_inferred=no in_live_step=%s%s depth=%d ignore_damage=%s solver_clone=%s solver_step=%s during_reset=%s", yn(c.inLiveStep),
               c.liveStepKnown ? "" : "(scope unknown)", r.collisionDepth, yn(c.ignoreDamage), yn(c.solverClone), yn(c.solverStepping), yn(c.duringReset));
    out += fmt(" | megahack=%s noclip_observed=%s menu_noclip=%s", yn(r.megahackLoaded), yn(r.noclipObserved), yn(r.menuNoclip));
    if (r.repeats > 0) out += fmt(" | +%d more through tick %lld", r.repeats, static_cast<long long>(r.lastTick));
    return out;
}

std::string overlayLabel(DebugRecord const& r) {
    auto const& v = r.verdict;
    std::string out;
    switch (v.decision) {
        case Decision::Death: out = "DEATH"; break;
        case Decision::WouldBeDeath: out = fmt("WOULD-BE #%u", v.contactId); break;
        case Decision::ContinuesContact: out = fmt("contact #%u", v.contactId); break;
        case Decision::Rejected: out = "REJECTED"; break;
    }
    out += fmt(" %s", v.accepted() ? "LIVE_GD_DEATH" : name(v.reason));
    out += fmt(" %s t%lld", v.player == 1 ? "P1" : v.player == 2 ? "P2" : "-", static_cast<long long>(r.tick));
    if (r.hasObject) out += fmt(" obj %d", r.objectId);
    else out += " no object";
    if (r.repeats > 0) out += fmt(" x%d", r.repeats + 1);
    return out;
}

bool DebugHistory::sameKind(DebugRecord const& a, DebugRecord const& b) {
    if (a.verdict.decision != b.verdict.decision || a.verdict.reason != b.verdict.reason) return false;
    if (a.candidate.sessionGen != b.candidate.sessionGen || a.candidate.attemptGen != b.candidate.attemptGen) return false;
    if (a.candidate.origin != b.candidate.origin) return false;
    // a clone pool dying is one line per attempt however far apart the trials are
    if (a.verdict.source == Source::RejectedClone) return true;
    if (a.candidate.playerSlot != b.candidate.playerSlot || a.verdict.contactId != b.verdict.contactId) return false;
    if (a.hasObject != b.hasObject || a.objectUid != b.objectUid) return false;
    return b.tick <= a.lastTick + 2;
}

bool DebugHistory::add(DebugRecord const& r) {
    ++m_revision;
    // a death or a new would-be death is always its own entry
    bool foldable = r.verdict.decision == Decision::Rejected || r.verdict.decision == Decision::ContinuesContact;
    if (foldable && !m_entries.empty() && sameKind(m_entries.back(), r)) {
        auto& last = m_entries.back();
        ++last.repeats;
        last.lastTick = std::max(last.lastTick, r.tick);
        return false;
    }
    if (m_entries.size() >= m_capacity && !m_entries.empty()) {
        m_entries.erase(m_entries.begin());
        ++m_dropped;
    }
    m_entries.push_back(r);
    m_entries.back().lastTick = r.tick;
    m_entries.back().repeats = 0;
    return true;
}

void DebugHistory::clear() {
    m_entries.clear();
    m_dropped = 0;
    ++m_revision;
}

}  // namespace gprl::death
