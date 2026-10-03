#include "sequence_adjusted.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "timeline.hpp"

namespace gprl::solver {

namespace {

constexpr double kEps = 1e-6;

/// Local contiguous pass run of one side and the far end of its bracket, in |frames| (NotTested
/// gaps neither break nor extend the run, like PassPlanner::walkSide; the latest outcome per shift
/// counts): the SA window starts from the local window (W_local ⊆ W_SA). `fail` = the first Died /
/// Invalid beyond the run, NaN when the side is open.
struct LocalRun {
    double run = 0.0;
    double fail = kNaN;
};
LocalRun localRun(std::vector<ShiftOutcome> const& local, bool late, double limit) {
    std::map<int64_t, ShiftOutcome const*> byMag;
    for (auto const& o : local) {
        double s = o.appliedFrames;
        if (late ? s <= kEps : s >= -kEps) continue;
        double mag = std::fabs(s);
        if (mag > limit + kEps) continue;
        int64_t key = std::llround(mag * 1e6);
        auto it = byMag.find(key);
        if (it == byMag.end() || it->second->pass <= o.pass) byMag[key] = &o;
    }
    LocalRun r;
    for (auto const& [k, o] : byMag) {
        (void)k;
        if (passed(o->kind)) r.run = std::fabs(o->appliedFrames);
        else if (o->kind == ShiftKind::Died || o->kind == ShiftKind::Invalid) {
            r.fail = std::fabs(o->appliedFrames);
            break;
        }
    }
    return r;
}

EdgeCause causeOf(int laterFixed, bool extension) {
    if (extension) return EdgeCause::Extension;
    return laterFixed >= 1 ? EdgeCause::Downstream : EdgeCause::Self;
}

std::string fmtShift(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+g", std::round(v * 1000.0) / 1000.0);
    return buf;
}

}  // namespace

double localEdgeFrames(BoundaryResult const& side) {
    double e = side.bounded && !std::isnan(side.failShiftMs) ? 0.5 * (side.passShiftMs + side.failShiftMs) : side.passShiftMs;
    return e / kTickMs;
}

SAPlanner::SAPlanner(SAConfig const& cfg, std::vector<SAInput> members, SAContext ctx) : m_cfg(cfg), m_ctx(std::move(ctx)) {
    double const maxShift = static_cast<double>(std::max(1, m_cfg.maxShiftTicks));
    double const horizon = m_cfg.horizonSeconds * kTicksPerSecond + maxShift;
    for (auto& in : members) {
        Member m;
        m.in = std::move(in);
        for (size_t i = 0; i < m_ctx.inputs.size(); ++i) {
            if (m_ctx.inputs[i].id == m.in.id) {
                m.ctxIndex = static_cast<int>(i);
                break;
            }
        }
        m.nextFrame.fill(kNaN);
        m.followerOk.fill(false);
        m.nextId.fill(0);
        m.nextDown.fill(true);
        m.followerOk[0] = true;
        if (m.ctxIndex >= 0) {
            for (int f = 1; f <= 4; ++f) {
                size_t idx = static_cast<size_t>(m.ctxIndex + f);
                if (idx >= m_ctx.inputs.size()) break;
                auto const& c = m_ctx.inputs[idx];
                m.nextFrame[static_cast<size_t>(f)] = c.frame;
                m.nextId[static_cast<size_t>(f)] = c.id;
                m.nextDown[static_cast<size_t>(f)] = c.down;
                m.followerOk[static_cast<size_t>(f)] = m.followerOk[static_cast<size_t>(f - 1)] && !c.breakBefore;
            }
        }
        m.isolated = std::isnan(m.nextFrame[1]) || m.nextFrame[1] > m.in.frame + horizon + kEps;
        m.limit[0] = maxShift;
        if (!std::isnan(m.in.earlyLimitFrames) && m.in.earlyLimitFrames < maxShift) m.limit[0] = std::max(0.0, m.in.earlyLimitFrames);
        m.limit[1] = maxShift;
        m.pairWalk = m.in.pairWalk && m_cfg.pairWalkForPresses && m.followerOk[1] && !std::isnan(m.nextFrame[1]);
        // slots: the tick lattice (aligned like the local planner's grid) plus every tested local shift
        auto off = timeline::gridOffsets(m.in.frame);
        for (int side = 0; side < 2; ++side) {
            bool late = side == 1;
            double offset = late ? off.late : off.early;
            double limit = m.limit[side];
            std::vector<double> lattice;
            for (int k = 0; k < m_cfg.maxShiftTicks; ++k) {
                double mag = offset > 1e-9 ? offset + static_cast<double>(k) : static_cast<double>(k + 1);
                if (mag > limit + kEps) break;
                if (mag > kEps) lattice.push_back(mag);
            }
            m.lattice[side] = lattice;
            std::vector<double> mags = lattice;
            for (auto const& o : m.in.local) {
                if (o.kind == ShiftKind::NotTested) continue;
                double s = o.appliedFrames;
                if (late ? s <= kEps : s >= -kEps) continue;
                double mag = std::fabs(s);
                if (mag <= limit + kEps) mags.push_back(mag);
            }
            std::sort(mags.begin(), mags.end());
            std::vector<double> uniq;
            for (double v : mags) {
                if (uniq.empty() || v - uniq.back() > kEps) uniq.push_back(v);
            }
            // the local window's own view of its run / bracket: within the LOCAL planner's limits
            // (the late neighbour limit too - an outcome beyond it is no part of the local window)
            double localLimit = limit;
            if (late && !std::isnan(m.in.lateLimitFrames)) localLimit = std::min(localLimit, std::max(0.0, m.in.lateLimitFrames));
            auto lr = localRun(m.in.local, late, localLimit);
            double const run = lr.run;
            m.localRun[side] = lr.run;
            m.localFail[side] = lr.fail;
            for (double v : uniq) {
                // strictly inside the local bracket (an untested gap between the local run and its
                // first fail: pool / cancellation): the local planner's bracket, not SA territory -
                // testing it could only put an SA edge INSIDE the local edge (W_local ⊆ W_SA, §2.9:
                // "SA walks outward from the local edges")
                if (v > run + kEps && !std::isnan(lr.fail) && v < lr.fail - kEps) continue;
                Slot s;
                s.mag = v;
                // the local window's contiguous run is SA-valid by definition (W_local ⊆ W_SA)
                if (v <= run + kEps) {
                    s.st = St::Pass;
                    s.used = SAAdaptation::Local;
                }
                m.sa[side].slots.push_back(s);
            }
            if (m.pairWalk) {
                for (double v : lattice) {
                    Slot s;
                    s.mag = v;
                    m.pair[side].slots.push_back(s);
                }
            }
        }
        // Fable review D3a (W4): both local sides open (no fail) to the SA search limit - every SA
        // slot lies inside the local run, so the walks end at their limits at once and the sequence
        // window IS the local one (decided, adaptationUsed []). An SA search cannot widen a side that
        // already reached the search range; no SA job (no pair walk) is ever queued for it. A late
        // side stopped by the next input (the local neighbour limit) is NOT open: the next input can
        // follow, so there are SA slots beyond it.
        m.openRange = true;
        for (int side = 0; side < 2; ++side) {
            if (!std::isnan(m.localFail[side])) m.openRange = false;
            for (auto const& s : m.sa[side].slots) {
                if (s.st != St::Pass) m.openRange = false;
            }
        }
        if (m.openRange) {
            m.pairWalk = false;
            m.pair[0].slots.clear();
            m.pair[1].slots.clear();
        }
        m_members.push_back(std::move(m));
    }
    advanceAll();
}

double SAPlanner::lookAheadBound() const {
    double const maxShift = static_cast<double>(m_cfg.maxShiftTicks);
    double const horizon = m_cfg.horizonSeconds * kTicksPerSecond + maxShift;
    double const converge = static_cast<double>(m_cfg.convergeSteps) + 1.0;
    int const maxChain = std::clamp(m_cfg.maxChain, 0, 3);
    double bound = 0.0;
    for (auto const& m : m_members) {
        // the local member (k = 0): t + horizon; the shifted member itself: t + s + converge
        bound = std::max(bound, m.in.frame + horizon);
        bound = std::max(bound, m.in.frame + maxShift + (m_cfg.horizonFromLastMoved ? horizon : std::max(horizon, converge)));
        for (int k = 1; k <= maxChain; ++k) {
            double f = m.nextFrame[static_cast<size_t>(k)];
            if (!m.followerOk[static_cast<size_t>(k)] || std::isnan(f)) break;
            bound = std::max(bound, f + maxShift + (m_cfg.horizonFromLastMoved ? horizon : converge));
        }
    }
    return bound;
}

bool SAPlanner::memberNeedsTrials(int i) const {
    if (i < 0 || i >= members()) return false;
    auto const& m = m_members[static_cast<size_t>(i)];
    for (int s = 0; s < 2; ++s) {
        if (!m.sa[s].ended) return true;
        if (m.pairWalk && !m.pair[s].ended) return true;
    }
    return false;
}

bool SAPlanner::crosses(Member const& m, int k, double s) const {
    if (s <= 0.0) return false;
    double last = k == 0 ? m.in.frame : m.nextFrame[static_cast<size_t>(k)];
    double next = k + 1 <= 4 ? m.nextFrame[static_cast<size_t>(k + 1)] : kNaN;
    if (std::isnan(last) || std::isnan(next)) return false;
    return last + s >= next - m_cfg.neighbourMarginFrames - 1e-9;
}

bool SAPlanner::anyLegalMember(Member const& m, double s) const {
    if (!crosses(m, 0, s)) return true;
    int const maxChain = std::clamp(m_cfg.maxChain, 0, 3);
    for (int k = 1; k <= maxChain; ++k) {
        if (!m.followerOk[static_cast<size_t>(k)] || std::isnan(m.nextFrame[static_cast<size_t>(k)])) return false;
        if (!crosses(m, k, s)) return true;
    }
    return false;
}

ShiftOutcome const* SAPlanner::localAt(Member const& m, double s) const {
    ShiftOutcome const* best = nullptr;
    for (auto const& o : m.in.local) {
        if (o.kind == ShiftKind::NotTested) continue;
        if (std::fabs(o.appliedFrames - s) > kEps) continue;
        if (!best || best->pass <= o.pass) best = &o;
    }
    return best;
}

SAPlanner::Need SAPlanner::need(int mi, int k, double s, SAOutcome const** out) {
    Key key{mi, k, shiftKey(s)};
    auto& c = m_cache[key];
    if (c.known) {
        *out = &c.out;
        return Need::Known;
    }
    if (c.requested) return Need::Waiting;
    if (m_finished) return Need::Impossible;
    auto& m = m_members[static_cast<size_t>(mi)];
    if (m.trials >= m_cfg.maxTrialsPerInput) {
        m.budgetHit = true;
        return Need::Budget;
    }
    SATrial t;
    t.id = m_nextId++;
    t.member = mi;
    t.shiftFrames = s;
    t.adaptation = static_cast<SAAdaptation>(k);
    t.moved.push_back({m.in.id, m.in.down, m.in.frame + s});
    for (int f = 1; f <= k; ++f) {
        t.moved.push_back({m.nextId[static_cast<size_t>(f)], m.nextDown[static_cast<size_t>(f)], m.nextFrame[static_cast<size_t>(f)] + s});
    }
    double const horizon = m_cfg.horizonSeconds * kTicksPerSecond + static_cast<double>(m_cfg.maxShiftTicks);
    // the local member keeps the LOCAL success rule exactly (look-ahead t + horizon + max shift,
    // fixed, SD §3.3). A family member (Fable review D2): one horizon after its LAST moved input,
    // so a chain's last follower gets the same evidence the local window gives one input (Opus
    // §3.3's t + s + horizon left a follower 96 ticks later ~34 frames); never before the last
    // moved input was applied and could re-join
    double lastMoved = t.moved.front().frame;
    for (auto const& mv : t.moved) lastMoved = std::max(lastMoved, mv.frame);
    double const base = k == 0 ? m.in.frame + horizon : m_cfg.horizonFromLastMoved ? lastMoved + horizon : m.in.frame + s + horizon;
    t.lookAheadFrame = std::max(base, lastMoved + static_cast<double>(m_cfg.convergeSteps) + 1.0);
    t.attributeAfterFrame = m.in.frame;
    m_trialKey[t.id] = key;
    m_queue.push_back(std::move(t));
    c.requested = true;
    ++m.trials;
    ++m_requested;
    return Need::Waiting;
}

SAPlanner::St SAPlanner::decide(int mi, int side, Slot& slot, double s) {
    (void)side;
    auto& m = m_members[static_cast<size_t>(mi)];
    int const maxChain = std::clamp(m_cfg.maxChain, 0, 3);
    ShiftOutcome const* lo = localAt(m, s);
    bool const localLegal = !crosses(m, 0, s);
    bool canFail = false;
    bool haveDeath = false;
    int k = 0;
    double D = kNaN;
    int L = 0;
    bool E = false;
    auto noteDeath = [&](double deathFrame, int laterFixed, int obj, double x, bool ext) {
        slot.haveDeath = true;
        slot.deathFrame = deathFrame;
        slot.laterFixed = laterFixed;
        slot.objectId = obj;
        slot.deathX = x;
        slot.extension = ext;
    };
    if (lo && passed(lo->kind)) {
        slot.used = SAAdaptation::Local;
        return St::Pass;
    }
    if (lo && lo->kind == ShiftKind::Invalid) {
        slot.why = status::Reason::SaInvalidTrials;
        return St::Undecided;
    }
    if (lo && lo->kind == ShiftKind::Died) {
        canFail = true;
        haveDeath = true;
        D = m.in.frame + lo->deathAfterFrames;   // deathAfterFrames: from the UNSHIFTED input frame
        L = lo->laterFixed;
        E = lo->extension;
        noteDeath(D, L, lo->objectId, lo->deathX, E);
    }
    else if (localLegal) {
        // local unknown but legal (a CBF refinement point, a shift the pool left untested): the
        // delayed replay runs the LOCAL member itself first - fail needs local(s) to have died
        SAOutcome const* o = nullptr;
        switch (need(mi, 0, s, &o)) {
            case Need::Waiting: return St::Open;
            case Need::Budget:
                slot.why = status::Reason::SaNotMeasuredBudget;
                return St::Undecided;
            case Need::Impossible:
                slot.why = m_finishReason;
                return St::Undecided;
            case Need::Known: break;
        }
        if (o->kind == SAOutcome::Kind::Pass) {
            slot.used = SAAdaptation::Local;
            // D3b (MOD verifier, v0.7.1): this pass comes from an SA trial (the delayed replay ran
            // the member alone at a shift the lockstep job left untested: a pool cut, a CBF
            // refinement point) and can lie BEYOND the local window. Fable §3.1 gives SA trials the
            // proofs `rejoined` / `survived` only; `local` is the lockstep local job's, inside the
            // local window. Labelled `local`, a side widened this way failed the evidence gate's
            // "a `local` side never widened" (proof_inconsistent) - sequence_adjusted_tests
            // testLocalTrialBeyondLocalRunProof
            slot.proof = o->rejoined ? SAProof::Rejoined : SAProof::Survived;
            return St::Pass;
        }
        if (o->kind == SAOutcome::Kind::Invalid) {
            slot.why = status::Reason::SaInvalidTrials;
            return St::Undecided;
        }
        canFail = true;
        haveDeath = true;
        D = o->deathFrame;
        L = o->laterFixed;
        E = o->extension;
        noteDeath(D, L, o->objectId, o->deathX, E);
    }
    else {
        // the local schedule is illegal (the input would cross the next one): the first legal
        // member plays its role
        canFail = true;
    }
    // escalation none -> pair -> chain2 -> chain3: at most maxChain steps (T-PROG-3)
    for (int guard = 0; guard <= maxChain + 1; ++guard) {
        if (haveDeath && canFail && !E && L == 0) {
            double nf = k + 1 <= 4 ? m.nextFrame[static_cast<size_t>(k + 1)] : kNaN;
            // the death happened before the next input a longer member would move could act
            if (std::isnan(nf) || nf + s >= D - 1e-6) {
                slot.used = static_cast<SAAdaptation>(k);
                return St::Fail;
            }
        }
        int k2 = k + 1;
        while (k2 <= maxChain && m.followerOk[static_cast<size_t>(k2)] && !std::isnan(m.nextFrame[static_cast<size_t>(k2)]) && crosses(m, k2, s)) ++k2;
        if (k2 > maxChain || !m.followerOk[static_cast<size_t>(k2)] || std::isnan(m.nextFrame[static_cast<size_t>(k2)])) {
            // F is exhausted (no longer legal member: maxChain, a cluster break, no later input).
            // V2-D2: invalid when the member reached last died INSIDE its adapted span (no
            // non-moved later input was applied before the death); undecided when a fixed later
            // input acted first (a larger family could still change it: sequence_dependent)
            if (haveDeath && canFail && !E && L == 0) {
                slot.used = static_cast<SAAdaptation>(k);
                return St::Fail;
            }
            slot.why = status::Reason::SaUndecided;
            return St::Undecided;
        }
        SAOutcome const* o = nullptr;
        switch (need(mi, k2, s, &o)) {
            case Need::Waiting: return St::Open;
            case Need::Budget:
                slot.why = status::Reason::SaNotMeasuredBudget;
                return St::Undecided;
            case Need::Impossible:
                slot.why = m_finishReason;
                return St::Undecided;
            case Need::Known: break;
        }
        if (o->kind == SAOutcome::Kind::Pass) {
            slot.used = static_cast<SAAdaptation>(k2);
            slot.proof = o->rejoined ? SAProof::Rejoined : SAProof::Survived;   // D3b
            return St::Pass;
        }
        if (o->kind == SAOutcome::Kind::Invalid) {
            slot.why = status::Reason::SaInvalidTrials;
            return St::Undecided;
        }
        // died: attribute it and try the next member (or decide fail above)
        if (!haveDeath && !localLegal) canFail = true;
        k = k2;
        haveDeath = true;
        D = o->deathFrame;
        L = o->laterFixed;
        E = o->extension;
        noteDeath(D, L, o->objectId, o->deathX, E);
    }
    slot.why = status::Reason::SaUndecided;
    return St::Undecided;
}

void SAPlanner::endWalk(Walk& w, EdgeStop stop, size_t at) {
    w.ended = true;
    w.stop = stop;
    w.endIndex = at;
}

void SAPlanner::advanceWalk(int mi, int side) {
    auto& m = m_members[static_cast<size_t>(mi)];
    Walk& w = m.sa[side];
    if (w.ended) return;
    double const sign = side == 1 ? 1.0 : -1.0;
    // bounded: every iteration either advances the frontier or ends / waits (T-PROG-5); a
    // refinement round inserts at most refinePointsSubtick slots, once
    size_t const guardMax = w.slots.size() + static_cast<size_t>(std::max(0, m_cfg.refinePointsSubtick)) + 4;
    for (size_t guard = 0; guard < guardMax && w.frontier < w.slots.size(); ++guard) {
        Slot& slot = w.slots[w.frontier];
        if (slot.st == St::Open && side == 1 && !anyLegalMember(m, sign * slot.mag)) {
            // the next input is in the way and cannot follow (a cluster break, or every chain in F
            // would cross the next fixed input): the local neighbour limit applies to SA too
            endWalk(w, EdgeStop::Neighbour, w.frontier);
            return;
        }
        if (slot.st == St::Open) slot.st = decide(mi, side, slot, sign * slot.mag);
        switch (slot.st) {
            case St::Open: return;   // waiting for a trial
            case St::Pass: ++w.frontier; continue;
            case St::Fail: {
                if (slot.refine) {
                    endWalk(w, EdgeStop::Fail, w.frontier);
                    return;
                }
                double passMag = w.frontier > 0 ? w.slots[w.frontier - 1].mag : 0.0;
                int n = m_cfg.refinePointsSubtick;
                // only a bracket OUTSIDE the local window: [local run, local fail] is the local
                // planner's (refined by its own CBF passes); refining it here could only move the
                // SA edge inside the local edge
                bool const beyondLocal = passMag > m.localRun[side] + kEps || std::isnan(m.localFail[side]);
                if (m.in.subtick && n > 0 && !w.refinePlanned && beyondLocal && slot.mag - passMag > 0.25 + 1e-9) {
                    // CBF: one refinement round inside the bracket [last pass, first fail]
                    std::vector<Slot> ins;
                    for (int q = 1; q <= n; ++q) {
                        Slot r;
                        r.mag = passMag + (slot.mag - passMag) * static_cast<double>(q) / static_cast<double>(n + 1);
                        r.refine = true;
                        ins.push_back(r);
                    }
                    w.refinePlanned = true;
                    w.refineFailIndex = w.frontier + ins.size();
                    w.slots.insert(w.slots.begin() + static_cast<std::ptrdiff_t>(w.frontier), ins.begin(), ins.end());
                    continue;
                }
                endWalk(w, EdgeStop::Fail, w.frontier);
                return;
            }
            case St::Undecided:
                if (slot.refine) {
                    // a refinement point that cannot be decided: the bracket stays [last pass, whole-tick fail]
                    endWalk(w, EdgeStop::Fail, w.refineFailIndex);
                    return;
                }
                endWalk(w, EdgeStop::Undecided, w.frontier);
                return;
        }
    }
    if (w.frontier >= w.slots.size()) {
        double const maxShift = static_cast<double>(m_cfg.maxShiftTicks);
        EdgeStop stop = EdgeStop::Range;
        if (side == 0 && m.limit[0] < maxShift - kEps) stop = stopOf(m.in.earlyLimitKind);
        endWalk(w, stop, w.slots.size());
    }
}

void SAPlanner::advancePair(int mi, int side) {
    auto& m = m_members[static_cast<size_t>(mi)];
    if (!m.pairWalk) return;
    Walk& w = m.pair[side];
    if (w.ended) return;
    double const sign = side == 1 ? 1.0 : -1.0;
    for (size_t guard = 0; guard <= w.slots.size() && w.frontier < w.slots.size(); ++guard) {
        Slot& slot = w.slots[w.frontier];
        double s = sign * slot.mag;
        if (slot.st == St::Open) {
            if (side == 1 && crosses(m, 1, s)) {
                endWalk(w, EdgeStop::Neighbour, w.frontier);
                return;
            }
            SAOutcome const* o = nullptr;
            switch (need(mi, 1, s, &o)) {
                case Need::Waiting: return;
                case Need::Budget:
                    slot.st = St::Undecided;
                    slot.why = status::Reason::SaNotMeasuredBudget;
                    break;
                case Need::Impossible:
                    slot.st = St::Undecided;
                    slot.why = m_finishReason;
                    break;
                case Need::Known:
                    slot.used = SAAdaptation::Pair;
                    if (o->kind == SAOutcome::Kind::Pass) slot.st = St::Pass;
                    else if (o->kind == SAOutcome::Kind::Invalid) {
                        slot.st = St::Undecided;
                        slot.why = status::Reason::SaInvalidTrials;
                    }
                    else {
                        slot.st = St::Fail;
                        slot.haveDeath = true;
                        slot.deathFrame = o->deathFrame;
                        slot.laterFixed = o->laterFixed;
                        slot.objectId = o->objectId;
                        slot.deathX = o->deathX;
                        slot.extension = o->extension;
                    }
                    break;
            }
        }
        if (slot.st == St::Pass) {
            ++w.frontier;
            continue;
        }
        endWalk(w, slot.st == St::Fail ? EdgeStop::Fail : EdgeStop::Undecided, w.frontier);
        return;
    }
    if (w.frontier >= w.slots.size()) {
        double const maxShift = static_cast<double>(m_cfg.maxShiftTicks);
        EdgeStop stop = EdgeStop::Range;
        if (side == 0 && m.limit[0] < maxShift - kEps) stop = stopOf(m.in.earlyLimitKind);
        endWalk(w, stop, w.slots.size());
    }
}

void SAPlanner::advanceAll() {
    for (int mi = 0; mi < members(); ++mi) {
        for (int side = 0; side < 2; ++side) {
            advanceWalk(mi, side);
            advancePair(mi, side);
        }
    }
}

std::vector<SATrial> SAPlanner::nextBatch(int max) {
    std::vector<SATrial> out;
    // pops one queue entry per returned trial (T-PROG-2); entries a finished walk no longer
    // needs are still run (their result is cached and may serve the other walk)
    while (!m_finished && max > 0 && !m_queue.empty()) {
        out.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
        ++m_outstanding;
        --max;
    }
    return out;
}

void SAPlanner::ingest(SAOutcome const& outcome) {
    auto it = m_trialKey.find(outcome.id);
    if (it == m_trialKey.end()) return;
    auto& c = m_cache[it->second];
    if (!c.known) {
        c.known = true;
        c.out = outcome;
        if (m_outstanding > 0) --m_outstanding;
    }
    if (!m_finished) advanceAll();
}

void SAPlanner::finish(status::Reason why) {
    if (m_finished) return;
    m_finished = true;
    m_finishReason = status::group(why) == status::TimingStatus::SequenceDependent ? why : status::Reason::SaUndecided;
    m_queue.clear();
    m_outstanding = 0;
    for (auto& m : m_members) {
        for (int side = 0; side < 2; ++side) {
            EdgeStop limitStop = side == 0 && m.limit[0] < static_cast<double>(m_cfg.maxShiftTicks) - kEps ? stopOf(m.in.earlyLimitKind) : EdgeStop::Range;
            for (int which = 0; which < 2; ++which) {
                if (which == 1 && !m.pairWalk) continue;   // a pair walk that was never wanted stays absent
                Walk& w = which == 0 ? m.sa[side] : m.pair[side];
                if (w.ended) continue;
                if (w.frontier >= w.slots.size()) {
                    endWalk(w, limitStop, w.slots.size());
                    continue;
                }
                auto& slot = w.slots[w.frontier];
                if (slot.st == St::Open) {
                    slot.st = St::Undecided;
                    slot.why = m_finishReason;
                }
                if (slot.refine) endWalk(w, EdgeStop::Fail, w.refineFailIndex);
                else endWalk(w, EdgeStop::Undecided, w.frontier);
            }
        }
    }
}

bool SAPlanner::done() const {
    if (m_finished) return true;
    if (m_outstanding > 0) return false;
    for (auto const& m : m_members) {
        for (int s = 0; s < 2; ++s) {
            if (!m.sa[s].ended) return false;
            if (m.pairWalk && !m.pair[s].ended) return false;
        }
    }
    return true;
}

SAPlanner::Walk SAPlanner::pairFromCache(int mi, int side, bool& complete, int& trials) const {
    Walk w;
    auto const& m = m_members[static_cast<size_t>(mi)];
    double const sign = side == 1 ? 1.0 : -1.0;
    // bounded by the side's lattice (<= maxShiftTicks points)
    for (double mag : m.lattice[side]) {
        double s = sign * mag;
        if (side == 1 && crosses(m, 1, s)) {
            endWalk(w, EdgeStop::Neighbour, w.slots.size());
            return w;
        }
        auto it = m_cache.find(Key{mi, 1, shiftKey(s)});
        if (it == m_cache.end() || !it->second.known || it->second.out.kind == SAOutcome::Kind::Invalid) {
            complete = false;
            return w;
        }
        auto const& o = it->second.out;
        Slot sl;
        sl.mag = mag;
        sl.used = SAAdaptation::Pair;
        ++trials;
        if (o.kind == SAOutcome::Kind::Pass) {
            sl.st = St::Pass;
            w.slots.push_back(sl);
            continue;
        }
        sl.st = St::Fail;
        sl.haveDeath = true;
        sl.deathFrame = o.deathFrame;
        sl.laterFixed = o.laterFixed;
        sl.objectId = o.objectId;
        sl.deathX = o.deathX;
        sl.extension = o.extension;
        w.slots.push_back(sl);
        endWalk(w, EdgeStop::Fail, w.slots.size() - 1);
        return w;
    }
    double const maxShift = static_cast<double>(m_cfg.maxShiftTicks);
    EdgeStop stop = EdgeStop::Range;
    if (side == 0 && m.limit[0] < maxShift - kEps) stop = stopOf(m.in.earlyLimitKind);
    endWalk(w, stop, w.slots.size());
    return w;
}

SAEdge SAPlanner::edgeOf(Member const& m, Walk const& w, int side, bool pairWalk) const {
    SAEdge e;
    double const sign = side == 1 ? 1.0 : -1.0;
    size_t end = w.ended ? std::min(w.endIndex, w.slots.size()) : w.frontier;
    double lastPass = 0.0;
    // Fable D3b: the weakest proof among the passes beyond the local edge, and the last pass
    // contiguous from the local edge that a re-join (or the local rule) proved
    SAProof weakest = SAProof::Local;
    double proven = pairWalk ? 0.0 : m.localRun[side];
    bool provenOpen = true;
    for (size_t i = 0; i < end && i < w.slots.size(); ++i) {
        auto const& sl = w.slots[i];
        if (sl.st != St::Pass) break;
        lastPass = sl.mag;
        if (pairWalk || sl.mag <= m.localRun[side] + kEps) continue;
        if (sl.proof > weakest) weakest = sl.proof;
        if (sl.proof == SAProof::Survived) provenOpen = false;
        else if (provenOpen) proven = sl.mag;
    }
    // a side that did not widen reports the local pass edge's exact double (the slot may hold the
    // lattice value of the same shift, equal within kEps)
    if (!pairWalk && std::fabs(lastPass - m.localRun[side]) <= kEps) lastPass = m.localRun[side];
    e.passFrames = sign * lastPass;
    if (!pairWalk) {
        e.proof = weakest;
        e.provenPassFrames = sign * std::min(proven, lastPass);
    }
    if (!w.ended) {
        e.stop = EdgeStop::Undecided;
        return e;
    }
    e.stop = w.stop;
    if (w.stop == EdgeStop::Fail && w.endIndex < w.slots.size()) {
        auto const& f = w.slots[w.endIndex];
        double failMag = f.mag;
        if (!pairWalk && !std::isnan(m.localFail[side]) && std::fabs(failMag - m.localFail[side]) <= kEps) failMag = m.localFail[side];
        e.failFrames = sign * failMag;
        e.refined = w.refinePlanned;
        if (f.haveDeath) {
            e.laterInputs = pairWalk ? f.laterFixed : 0;
            e.cause = pairWalk ? causeOf(f.laterFixed, f.extension) : EdgeCause::Self;
            e.failAfterFrames = f.deathFrame - m.in.frame;
            e.failObjectId = f.objectId;
            e.failDeathX = f.deathX;
        }
        else {
            e.cause = EdgeCause::Self;
            e.laterInputs = 0;
        }
    }
    return e;
}

SAResult SAPlanner::result(int mi) const {
    SAResult r;
    if (mi < 0 || mi >= members()) return r;
    auto const& m = m_members[static_cast<size_t>(mi)];
    r.valid = true;
    r.isolated = m.isolated;
    r.openRange = m.openRange;
    r.sequence.present = true;
    r.sequence.early = edgeOf(m, m.sa[0], 0, false);
    r.sequence.late = edgeOf(m, m.sa[1], 1, false);
    // Fable review D1 (W1): a side that ended without an SA fail at the local window's last pass
    // (undecided right beyond the local edge, or a limit there) reports the LOCAL edge unchanged -
    // its exact pass / fail values (the doubles PassPlanner reports: |appliedFrames| of the same
    // outcomes) and so its midpoint - while `stop` keeps saying why the SA side ended. Without it
    // the side's reported edge (its last pass) lay half a bracket INSIDE the local midpoint.
    for (int s = 0; s < 2; ++s) {
        SAEdge& e = s == 0 ? r.sequence.early : r.sequence.late;
        double const sign = s == 1 ? 1.0 : -1.0;
        if (e.bounded() || std::isnan(m.localFail[s])) continue;
        if (std::fabs(std::fabs(e.passFrames) - m.localRun[s]) > kEps) continue;
        e.passFrames = sign * m.localRun[s];
        e.failFrames = sign * m.localFail[s];
        e.inherited = true;
        e.cause = EdgeCause::None;
        e.laterInputs = -1;
        e.proof = SAProof::Local;   // never widened (D3b)
        e.provenPassFrames = e.passFrames;
    }
    r.survivedOnly = r.sequence.early.proof == SAProof::Survived || r.sequence.late.proof == SAProof::Survived;
    double res = 0.0;
    for (auto const* e : {&r.sequence.early, &r.sequence.late}) {
        if (e->hasBracket()) res = std::max(res, std::fabs(e->failFrames - e->passFrames));
    }
    r.sequence.resolutionFrames = res > 0.0 ? res : 1.0;
    r.sequence.trials = m.trials;
    for (int s = 0; s < 2; ++s) {
        EdgeStop st = s == 0 ? r.sequence.early.stop : r.sequence.late.stop;
        r.sideDecided[s] = m.sa[s].ended && st != EdgeStop::Undecided && st != EdgeStop::Untested;
    }
    r.decided = r.sideDecided[0] && r.sideDecided[1];
    bool used[4] = {false, false, false, false};
    bool budget = false, invalid = false, finishReason = false;
    for (int s = 0; s < 2; ++s) {
        for (auto const& slot : m.sa[s].slots) {
            if (slot.st == St::Pass && slot.used != SAAdaptation::Local) used[static_cast<int>(slot.used)] = true;
            if (slot.st == St::Undecided && !slot.refine) {
                ++r.undecidedShifts;
                if (slot.why == status::Reason::SaNotMeasuredBudget) budget = true;
                else if (slot.why == status::Reason::SaInvalidTrials) invalid = true;
                else if (slot.why != status::Reason::SaUndecided) finishReason = true;
            }
        }
    }
    for (int k = 1; k <= 3; ++k) {
        if (used[k]) r.adaptationUsed.push_back(static_cast<SAAdaptation>(k));
    }
    if (finishReason) r.failure = m_finishReason;
    else if (budget) r.failure = status::Reason::SaNotMeasuredBudget;
    else if (invalid) r.failure = status::Reason::SaInvalidTrials;
    else r.failure = status::Reason::SaUndecided;
    if (m.pairWalk && m.pair[0].ended && m.pair[1].ended && m.pair[0].stop != EdgeStop::Undecided && m.pair[1].stop != EdgeStop::Undecided) {
        r.pair.present = true;
        r.pair.early = edgeOf(m, m.pair[0], 0, true);
        r.pair.late = edgeOf(m, m.pair[1], 1, true);
        double pr = 0.0;
        for (auto const* e : {&r.pair.early, &r.pair.late}) {
            if (e->bounded()) pr = std::max(pr, std::fabs(e->failFrames - e->passFrames));
        }
        r.pair.resolutionFrames = pr > 0.0 ? pr : 1.0;
        int pt = 0;
        for (int s = 0; s < 2; ++s) {
            for (auto const& slot : m.pair[s].slots) {
                if (slot.st == St::Pass || slot.st == St::Fail) ++pt;
            }
        }
        r.pair.trials = pt;
    }
    else if (!m.pairWalk && !m.openRange) {
        // Fable review D9: without the full pair walk, `pair` is emitted only when the SA walk's
        // own pair trials cover a contiguous run from 0 outward to a fail or a limit on BOTH sides
        // (inside the local window the walk runs no pair trial, so this is rare by design)
        bool complete = true;
        int pt = 0;
        Walk pw[2] = {pairFromCache(mi, 0, complete, pt), pairFromCache(mi, 1, complete, pt)};
        if (complete) {
            r.pair.present = true;
            r.pair.early = edgeOf(m, pw[0], 0, true);
            r.pair.late = edgeOf(m, pw[1], 1, true);
            double pr = 0.0;
            for (auto const* e : {&r.pair.early, &r.pair.late}) {
                if (e->bounded()) pr = std::max(pr, std::fabs(e->failFrames - e->passFrames));
            }
            r.pair.resolutionFrames = pr > 0.0 ? pr : 1.0;
            r.pair.trials = pt;
        }
    }
    r.trials = m.trials;
    r.debug.push_back(describe(mi));
    return r;
}

std::string SAPlanner::describe(int mi) const {
    if (mi < 0 || mi >= members()) return {};
    auto const& m = m_members[static_cast<size_t>(mi)];
    std::string out;
    for (int side = 1; side >= 0; --side) {
        auto const& w = m.sa[side];
        double sign = side == 1 ? 1.0 : -1.0;
        out += side == 1 ? "late" : " | early";
        int localPasses = 0;
        for (auto const& slot : w.slots) {
            if (slot.st == St::Pass && slot.used == SAAdaptation::Local) {
                ++localPasses;
                continue;
            }
            if (slot.st == St::Open) continue;
            out += " " + fmtShift(sign * slot.mag);
            switch (slot.st) {
                case St::Pass: out += std::string(" ") + name(slot.used); break;
                case St::Fail: {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " fail@%.1f/%dL", slot.deathFrame - m.in.frame, slot.laterFixed);
                    out += buf;
                    break;
                }
                case St::Undecided: out += std::string(" undecided(") + status::name(slot.why) + ")"; break;
                default: break;
            }
            if (slot.refine) out += "r";
        }
        if (localPasses) out += " [" + std::to_string(localPasses) + " local]";
        out += w.ended ? std::string(" -> ") + name(w.stop) : std::string(" -> ...");
    }
    if (m.pairWalk) {
        out += " | pair";
        for (int side = 0; side < 2; ++side) {
            auto const& w = m.pair[side];
            out += side == 0 ? " early " : " late ";
            out += w.ended ? name(w.stop) : "...";
            size_t end = w.ended ? std::min(w.endIndex, w.slots.size()) : w.frontier;
            double last = 0.0;
            for (size_t i = 0; i < end; ++i) {
                if (w.slots[i].st == St::Pass) last = w.slots[i].mag;
            }
            out += "@" + fmtShift((side == 1 ? 1.0 : -1.0) * last);
        }
    }
    out += " | trials " + std::to_string(m.trials);
    if (m.budgetHit) out += " (budget)";
    return out;
}

int runSAAgainstOracle(IPhysicsOracle& oracle, SnapshotId base, InputSchedule const& schedule, SARunInput const& in, SAPlanner& out) {
    SAContext ctx;
    for (size_t i = 0; i < schedule.inputs.size(); ++i) {
        SAContextInput c;
        c.id = static_cast<uint32_t>(i + 1);
        c.frame = schedule.inputs[i].tMs / kTickMs;
        c.down = schedule.inputs[i].down;
        c.breakBefore = std::find(in.breaks.begin(), in.breaks.end(), i) != in.breaks.end();
        ctx.inputs.push_back(c);
    }
    std::vector<SAInput> members;
    for (size_t mi = 0; mi < in.members.size(); ++mi) {
        size_t idx = in.members[mi];
        if (idx >= schedule.inputs.size()) continue;
        SAInput s;
        s.id = static_cast<uint32_t>(idx + 1);
        s.frame = schedule.inputs[idx].tMs / kTickMs;
        s.down = schedule.inputs[idx].down;
        s.subtick = in.subtick;
        if (mi < in.locals.size() && in.locals[mi]) {
            auto const& p = *in.locals[mi];
            s.local = p.outcomes();
            s.earlyLimitFrames = p.early().limit;
            if (p.late().limitKnown) s.lateLimitFrames = p.late().limit;   // the local window's late limit (MOD verifier, D1)
        }
        // the oracle's history reaches back to the schedule's start: a limited early side is the
        // previous input, or the attempt start for the first one
        s.earlyLimitKind = idx > 0 ? LimitKind::Neighbour : LimitKind::AttemptStart;
        if (mi < in.pairWalk.size()) s.pairWalk = in.pairWalk[mi];
        else s.pairWalk = s.down && idx + 1 < schedule.inputs.size();
        members.push_back(std::move(s));
    }
    out = SAPlanner(in.config, std::move(members), std::move(ctx));
    int trials = 0;
    // bounded: each round runs every queued trial; a member asks for at most maxTrialsPerInput
    int const maxRounds = std::max(1, static_cast<int>(in.members.size())) * (in.config.maxTrialsPerInput + 2) + 8;
    for (int round = 0; round < maxRounds && !out.done(); ++round) {
        auto batch = out.nextBatch(1 << 20);
        if (batch.empty()) break;
        for (auto const& t : batch) {
            InputSchedule sched = schedule;
            std::vector<size_t> moved;
            for (auto const& mv : t.moved) {
                size_t idx = static_cast<size_t>(mv.id) - 1;
                if (idx < sched.inputs.size()) {
                    sched.inputs[idx].tMs = mv.frame * kTickMs;
                    moved.push_back(idx);
                }
            }
            double first = t.moved.front().frame;
            for (auto const& mv : t.moved) first = std::min(first, mv.frame);
            double memberRecorded = t.attributeAfterFrame;
            double earliestDiff = std::min(first, memberRecorded);
            double horizonSeconds = (t.lookAheadFrame - earliestDiff) / kTicksPerSecond;
            Outcome o = oracle.trial(base, sched, horizonSeconds);
            ++trials;
            SAOutcome so;
            so.id = t.id;
            switch (o.kind) {
                case OutcomeKind::Survived: so.kind = SAOutcome::Kind::Pass; break;
                case OutcomeKind::Resynced:
                    so.kind = SAOutcome::Kind::Pass;
                    so.rejoined = true;
                    break;
                case OutcomeKind::Died: {
                    so.kind = SAOutcome::Kind::Died;
                    so.deathFrame = o.tMs / kTickMs;
                    so.objectId = o.objectId;
                    size_t memberIdx = moved.empty() ? 0 : moved.front();
                    std::vector<size_t> followers(moved.begin() + (moved.empty() ? 0 : 1), moved.end());
                    so.laterFixed = laterFixedBefore(schedule, memberIdx, followers, o.tMs);
                    break;
                }
                case OutcomeKind::Invalid:
                    so.kind = SAOutcome::Kind::Invalid;
                    so.reason = o.reason;
                    break;
            }
            out.ingest(so);
        }
    }
    return trials;
}

}  // namespace gprl::solver
