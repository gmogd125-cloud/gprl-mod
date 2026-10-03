#include "compensation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "timeline.hpp"

namespace gprl::solver::comp {

namespace {

constexpr double kEps = 1e-6;

std::string fmtNum(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+g", std::round(v * 1000.0) / 1000.0);
    return buf;
}

std::string fmtOffsets(std::vector<double> const& off) {
    std::string s = "[";
    for (size_t i = 0; i < off.size(); ++i) s += (i ? "," : "") + fmtNum(off[i]);
    return s + "]";
}

bool sameOffsets(std::vector<double> const& a, std::vector<double> const& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i] - b[i]) > kEps) return false;
    }
    return true;
}

/// Local contiguous pass run of one side and the far end of its bracket (SAPlanner's localRun).
struct LocalRun {
    double run = 0.0;
    double fail = kNaN;
};
LocalRun localRunOf(std::vector<ShiftOutcome> const& local, bool late, double limit) {
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

}  // namespace

// ---- construction and facts ----

CompPlanner::CompPlanner(CompConfig const& cfg, CompMember member, std::vector<CompFollower> followers) : m_cfg(cfg), m_m(member) {
    double const maxShift = static_cast<double>(std::max(1, m_cfg.maxShiftTicks));
    auto off = timeline::gridOffsets(m_m.frame);
    for (int side = 0; side < 2; ++side) {
        Side& sd = m_side[side];
        bool const late = side == 1;
        sd.limit = maxShift;
        sd.limitKind = LimitKind::Range;
        if (!late && !std::isnan(m_m.earlyLimitFrames) && m_m.earlyLimitFrames < maxShift) {
            sd.limit = std::max(0.0, m_m.earlyLimitFrames);
            sd.limitKind = m_m.earlyLimitKind;
        }
        double const offset = late ? off.late : off.early;
        for (int k = 0; k < m_cfg.maxShiftTicks; ++k) {
            double mag = offset > 1e-9 ? offset + static_cast<double>(k) : static_cast<double>(k + 1);
            if (mag > sd.limit + kEps) break;
            if (mag <= kEps) continue;
            Slot s;
            s.mag = mag;
            sd.slots.push_back(s);
        }
    }
    for (auto const& f : followers) addFollower(f);
}

void CompPlanner::addFollower(CompFollower f) {
    if (m_breakSeen) return;
    if (f.frame <= m_m.frame + kEps) return;   // not a later input
    if (f.breakBefore) {
        // a cluster break: nothing from here on can follow; the input itself is the next FIXED one
        m_breakSeen = true;
        m_followers.push_back(f);
        advanceAll();
        return;
    }
    // beyond the horizon: a fixed input; a later one cannot be a follower either (time order)
    if (f.frame > m_m.frame + m_cfg.horizonFrames + kEps) {
        f.breakBefore = true;
        m_breakSeen = true;
        m_followers.push_back(f);
    }
    else {
        if (static_cast<int>(m_followers.size()) >= m_cfg.maxFollowers + 1) return;
        m_followers.push_back(f);
    }
    advanceAll();
}

int CompPlanner::followersUsable() const {
    int n = 0;
    for (auto const& f : m_followers) {
        if (f.breakBefore) break;
        if (n >= m_cfg.maxFollowers) break;
        ++n;
    }
    return n;
}

void CompPlanner::setNow(double frame) {
    if (frame <= m_now) return;
    m_now = frame;
    if (!m_isolated && m_now >= m_m.frame + m_cfg.horizonFrames - kEps && followersUsable() == 0) m_isolated = true;
    advanceAll();
}

CompPlanner::Slot* CompPlanner::slotAt(int side, double mag) {
    Side& sd = m_side[side];
    for (auto& s : sd.slots) {
        if (std::fabs(s.mag - mag) <= kEps) return &s;
    }
    if (mag > sd.limit + kEps || mag <= kEps) return nullptr;
    // a tested shift off the lattice (a refinement point): one more slot, kept sorted
    Slot s;
    s.mag = mag;
    auto it = std::upper_bound(sd.slots.begin(), sd.slots.end(), mag, [](double v, Slot const& x) { return v < x.mag; });
    return &*sd.slots.insert(it, s);
}

void CompPlanner::noteLocal(ShiftOutcome const& o) {
    double const s = o.appliedFrames;
    if (std::fabs(s) <= kEps) return;
    int const side = s > 0 ? 1 : 0;
    Slot* slot = slotAt(side, std::fabs(s));
    if (!slot) return;
    if (slot->localKnown && slot->local.pass > o.pass) return;   // a later pass's view wins
    slot->localKnown = true;
    slot->local = o;
    advanceAll();
}

void CompPlanner::finalizeLocal(std::vector<ShiftOutcome> const& all, double lateLimitFrames) {
    for (auto const& o : all) {
        if (o.kind == ShiftKind::NotTested) continue;
        double const s = o.appliedFrames;
        if (std::fabs(s) <= kEps) continue;
        Slot* slot = slotAt(s > 0 ? 1 : 0, std::fabs(s));
        if (!slot) continue;
        if (slot->localKnown && slot->local.pass > o.pass) continue;
        slot->localKnown = true;
        slot->local = o;
    }
    for (int side = 0; side < 2; ++side) {
        bool const late = side == 1;
        double localLimit = m_side[side].limit;
        if (late && !std::isnan(lateLimitFrames)) localLimit = std::min(localLimit, std::max(0.0, lateLimitFrames));
        auto lr = localRunOf(all, late, localLimit);
        m_side[side].localRun = lr.run;
        m_side[side].localFail = lr.fail;
    }
    m_localFinal = true;
    advanceAll();
}

// ---- the search ----

/// 1 = a follower shifted along by s acts before the death, 0 = none can (a follower that could
/// still be logged before D + |s| is the caller's "wait" case).
bool CompPlanner::canFollowerAct(double s, double deathFrame, int fromFollower) const {
    if (s > 0.0) return false;   // a follower moved later acts later: had it acted, laterFixed >= 1
    int i = 0;
    for (auto const& f : m_followers) {
        if (f.breakBefore) break;
        if (i >= m_cfg.maxFollowers) break;
        if (i++ < fromFollower) continue;
        if (f.frame + s < deathFrame - kEps) return true;
    }
    return false;
}

bool CompPlanner::legalOffsets(double s, std::vector<double>& offsets, double& lastMoved) const {
    double prev = m_m.frame + s;   // the member's shifted frame
    int const k = static_cast<int>(offsets.size());
    int const usable = followersUsable();
    if (k > usable) return false;
    // the member may not cross a FIXED next input (k = 0 with a next input that cannot follow)
    if (k == 0 && !m_followers.empty()) {
        auto const& f0 = m_followers.front();
        if (prev >= f0.frame - m_cfg.neighbourMarginFrames - kEps) return false;
    }
    for (int j = 0; j < k; ++j) {
        auto const& f = m_followers[static_cast<size_t>(j)];
        double lo = std::max(s - static_cast<double>(m_cfg.offsetRangeTicks), prev + m_cfg.neighbourMarginFrames - f.frame);
        double hi = s + static_cast<double>(m_cfg.offsetRangeTicks);
        // the next FIXED input (the follower after the last moved one, or the first unusable input)
        if (j == k - 1 && static_cast<size_t>(j + 1) < m_followers.size()) {
            double next = m_followers[static_cast<size_t>(j + 1)].frame;
            hi = std::min(hi, next - m_cfg.neighbourMarginFrames - f.frame);
        }
        // whole ticks unless sub-tick placement is available
        if (!m_m.subtick) {
            lo = std::ceil(lo - kEps);
            hi = std::floor(hi + kEps);
        }
        if (lo > hi + kEps) return false;
        offsets[static_cast<size_t>(j)] = std::clamp(offsets[static_cast<size_t>(j)], lo, hi);
        prev = f.frame + offsets[static_cast<size_t>(j)];
    }
    lastMoved = prev;
    return true;
}

/// >= 0 the trial id; -1 budget; -2 finished; -3 no legal schedule; -4 already simulated.
int CompPlanner::request(int side, Slot& slot, double s, Role role, int probeIndex, std::vector<double> offsets) {
    if (m_finished) return -2;
    if (m_requested >= m_cfg.maxTrialsPerInput) return -1;
    double lastMoved = 0.0;
    if (!legalOffsets(s, offsets, lastMoved)) return -3;
    for (auto const& tried : slot.triedOffsets) {
        if (sameOffsets(tried, offsets)) return -4;
    }
    CompTrial t;
    t.id = m_nextId++;
    t.shiftFrames = s;
    t.followers = static_cast<int>(offsets.size());
    t.role = role;
    t.probeIndex = probeIndex;
    t.adaptation = t.followers == 0 ? SAAdaptation::Local : role == Role::Uniform ? uniformAdaptation(t.followers) : compensatedAdaptation(t.followers);
    t.moved.push_back({m_m.id, m_m.down, m_m.frame + s});
    for (int j = 0; j < t.followers; ++j) {
        auto const& f = m_followers[static_cast<size_t>(j)];
        t.moved.push_back({f.id, f.down, f.frame + offsets[static_cast<size_t>(j)]});
    }
    t.offsetsFrames = offsets;
    double last = m_m.frame + s;
    for (auto const& mv : t.moved) last = std::max(last, mv.frame);
    t.assessFrame = static_cast<size_t>(t.followers) < m_followers.size() ? m_followers[static_cast<size_t>(t.followers)].frame : last + m_cfg.horizonFrames;
    t.lookAheadFrame = std::max(last + m_cfg.lookAheadFrames, last + static_cast<double>(m_cfg.convergeSteps) + 1.0);
    t.attributeAfterFrame = m_m.frame;
    t.devFromFrame = std::min(m_m.frame + s, m_m.frame);
    slot.triedOffsets.push_back(offsets);
    m_trialSlot[t.id] = {side, slot.mag};
    m_trialOffsets[t.id] = offsets;
    m_queue.push_back(std::move(t));
    ++m_requested;
    return m_queue.back().id;
}

CompOutcome const* CompPlanner::known(int id) const {
    auto it = m_known.find(id);
    return it == m_known.end() ? nullptr : &it->second;
}

bool CompPlanner::devAt(std::vector<DevSample> const& dev, double frame, DevSample& out) {
    bool found = false;
    for (auto const& d : dev) {
        if (d.frame > frame + kEps) break;
        out = d;
        found = true;
    }
    return found;
}

std::vector<double> CompPlanner::solveOffsets(DevSample const& base, std::vector<DevSample> const& columns, double probe, double ridge) {
    size_t const k = std::min<size_t>(columns.size(), 3);
    std::vector<double> d(k, 0.0);
    if (k == 0 || !(probe > 0.0)) return d;
    // J (2 x k): column j = (dev(P_j) - dev(U)) / probe; unknown columns are dropped (offset 0)
    std::array<std::array<double, 2>, 3> J{};
    std::array<bool, 3> use{};
    for (size_t j = 0; j < k; ++j) {
        auto const& c = columns[j];
        bool ok = std::isfinite(c.dy) && std::isfinite(c.dvy);
        use[j] = ok;
        J[j] = ok ? std::array<double, 2>{(c.dy - base.dy) / probe, (c.dvy - base.dvy) / probe} : std::array<double, 2>{0.0, 0.0};
    }
    // normal equations (J^T J + ridge I) d = -J^T base, 3 x 3 at most, Gaussian elimination
    double A[3][3] = {};
    double b[3] = {};
    for (size_t i = 0; i < k; ++i) {
        for (size_t j = 0; j < k; ++j) A[i][j] = J[i][0] * J[j][0] + J[i][1] * J[j][1] + (i == j ? ridge : 0.0);
        b[i] = -(J[i][0] * base.dy + J[i][1] * base.dvy);
        if (!use[i]) {
            for (size_t j = 0; j < k; ++j) A[i][j] = i == j ? 1.0 : 0.0;
            b[i] = 0.0;
        }
    }
    for (size_t col = 0; col < k; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < k; ++r) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
        if (std::fabs(A[piv][col]) < 1e-18) continue;
        if (piv != col) {
            for (size_t j = 0; j < k; ++j) std::swap(A[piv][j], A[col][j]);
            std::swap(b[piv], b[col]);
        }
        for (size_t r = 0; r < k; ++r) {
            if (r == col) continue;
            double f = A[r][col] / A[col][col];
            for (size_t j = 0; j < k; ++j) A[r][j] -= f * A[col][j];
            b[r] -= f * b[col];
        }
    }
    for (size_t i = 0; i < k; ++i) d[i] = std::fabs(A[i][i]) < 1e-18 ? 0.0 : b[i] / A[i][i];
    for (size_t i = 0; i < k; ++i) if (!std::isfinite(d[i])) d[i] = 0.0;
    return d;
}

std::vector<std::vector<double>> CompPlanner::candidates(Slot const& slot, double s) const {
    // the response model from the uniform trial and the probes, at the latest common frame
    std::vector<std::vector<double>> out;
    int const k = slot.k;
    if (k <= 0) return out;
    CompOutcome const* u = known(slot.uniformId);
    std::vector<CompOutcome const*> probes;
    for (int id : slot.probeIds) probes.push_back(id >= 0 ? known(id) : nullptr);
    double common = 1e300;
    auto endOf = [](CompOutcome const* o) {
        if (!o || o->dev.empty()) return -1e300;
        return o->dev.back().frame;
    };
    if (u) common = std::min(common, endOf(u));
    for (auto const* p : probes) if (p) common = std::min(common, endOf(p));
    DevSample base;
    std::vector<double> d(static_cast<size_t>(k), 0.0);
    bool modelled = false;
    if (u && common > -1e299 && devAt(u->dev, common, base)) {
        std::vector<DevSample> cols;
        for (int j = 0; j < k; ++j) {
            DevSample c{common, kNaN, kNaN};
            if (static_cast<size_t>(j) < probes.size() && probes[static_cast<size_t>(j)]) devAt(probes[static_cast<size_t>(j)]->dev, common, c);
            cols.push_back(c);
        }
        d = solveOffsets(base, cols, static_cast<double>(m_cfg.probeOffsetTicks), m_cfg.ridge);
        modelled = true;
    }
    auto push = [&](std::vector<double> off) {
        for (auto& v : off) v = std::clamp(v, s - static_cast<double>(m_cfg.offsetRangeTicks), s + static_cast<double>(m_cfg.offsetRangeTicks));
        for (auto const& t : out) if (sameOffsets(t, off)) return;
        out.push_back(std::move(off));
    };
    if (modelled) {
        std::vector<double> v1(static_cast<size_t>(k)), v2(static_cast<size_t>(k));
        size_t flip = 0;
        double flipFrac = -1.0;
        for (size_t j = 0; j < static_cast<size_t>(k); ++j) {
            double r = std::round(d[j]);
            v1[j] = s + r;
            double frac = std::fabs(d[j] - r);
            if (frac > flipFrac + 1e-12) { flipFrac = frac; flip = j; }
        }
        // V2: the other rounding of the least decided component, or one more tick along the
        // largest offset when every component was already whole
        v2 = v1;
        if (flipFrac > 1e-9) v2[flip] = s + (d[flip] > std::round(d[flip]) ? std::ceil(d[flip]) : std::floor(d[flip]));
        else {
            size_t big = 0;
            for (size_t j = 1; j < static_cast<size_t>(k); ++j) if (std::fabs(d[j]) > std::fabs(d[big])) big = j;
            double dir = d[big] >= 0.0 ? 1.0 : -1.0;
            if (std::fabs(d[big]) < 1e-9) dir = base.dy >= 0.0 ? -1.0 : 1.0;   // no response modelled: a nudge against the deviation
            v2[big] += dir;
        }
        push(v1);
        push(v2);
    }
    // without a model (nothing sampled): the last follower one tick either way
    std::vector<double> a(static_cast<size_t>(k), s), b(static_cast<size_t>(k), s);
    a.back() = s + 1.0;
    b.back() = s - 1.0;
    push(a);
    push(b);
    return out;
}

/// The reason an Invalid outcome gives a slot: `sa_undecided` for a trial that could not be decided
/// (unsettled at the look-ahead, cancelled), `sa_invalid_trials` for a failed proof (F6).
static status::Reason invalidWhy(CompOutcome const* o) {
    return o && o->notTested ? status::Reason::SaUndecided : status::Reason::SaInvalidTrials;
}

void CompPlanner::failSlot(Slot& slot, CompOutcome const* death) {
    slot.st = St::Fail;
    slot.stage = Stage::Done;
    if (death && death->kind == CompOutcome::Kind::Died) {
        slot.haveDeath = true;
        slot.deathFrame = death->deathFrame;
        slot.laterFixed = 0;
        slot.objectId = death->objectId;
        slot.deathX = death->deathX;
        slot.extension = death->extension;
    }
    else if (slot.localKnown && slot.local.kind == ShiftKind::Died) {
        slot.haveDeath = true;
        slot.deathFrame = m_m.frame + slot.local.deathAfterFrames;
        slot.laterFixed = 0;
        slot.objectId = slot.local.objectId;
        slot.deathX = slot.local.deathX;
        slot.extension = slot.local.extension;
    }
}

void CompPlanner::decideFromVerifications(int side, Slot& slot, double s) {
    (void)side;
    (void)s;
    // every verification of this (shift, k) died: escalate or fail
    CompOutcome const* last = nullptr;
    for (int id : slot.verifyIds) {
        if (auto const* o = known(id); o && o->kind == CompOutcome::Kind::Died) last = o;
    }
    if (!last) last = known(slot.uniformId);
    int const usable = followersUsable();
    // F4 (docs §4.2 step 4): every trial died inside the adapted span, but the NEXT follower,
    // shifted along, could still act before that death: escalate rather than fail
    if (!slot.fixedActed && last && slot.k < std::min(usable, m_cfg.maxFollowers) && canFollowerAct(s, last->deathFrame, slot.k)) {
        ++slot.k;
        slot.stage = Stage::Uniform;
        slot.uniformId = -1;
        slot.probeIds.clear();
        slot.verifyIds.clear();
        slot.verifications = 0;
        return;
    }
    if (slot.fixedActed) {
        if (slot.k < std::min(usable, m_cfg.maxFollowers)) {
            // one more follower may move: the (k+1)-th acted before a death
            ++slot.k;
            slot.stage = Stage::Uniform;
            slot.uniformId = -1;
            slot.probeIds.clear();
            slot.verifyIds.clear();
            slot.verifications = 0;
            return;
        }
        bool const moreMayCome = !m_breakSeen && static_cast<int>(m_followers.size()) <= slot.k && slot.k < m_cfg.maxFollowers
                              && m_now < m_m.frame + m_cfg.horizonFrames - kEps;
        if (moreMayCome) return;   // wait for the next follower (or the horizon)
        // a fixed later input acted and nothing can move it: a larger family could still change
        // the outcome (V2-D2) - undecided, never guessed
        slot.st = St::Undecided;
        slot.why = status::Reason::SaUndecided;
        slot.stage = Stage::Done;
        return;
    }
    failSlot(slot, last);
}

/// Decides what the slot's local outcome says (Stage::None -> a stage, Pass, Fail or Undecided).
/// Returns false when the slot must wait for a fact (the local copy, a follower, the horizon).
bool CompPlanner::decideLocal(int side, Slot& slot) {
    double const sign = side == 1 ? 1.0 : -1.0;
    double const s = sign * slot.mag;
    if (!slot.localKnown) {
        // not tested locally yet. Once the local pass is final an untested shift beyond the local
        // bracket is run as the member alone (k = 0, proof by its outcome; D3b), inside it never
        if (!m_localFinal) return false;
        double const run = m_side[side].localRun, fail = m_side[side].localFail;
        if (slot.mag <= run + kEps) { slot.st = St::Pass; slot.used = SAAdaptation::Local; slot.proof = SAProof::Local; slot.stage = Stage::Done; return true; }
        if (!std::isnan(fail) && slot.mag < fail - kEps) return false;   // the local planner's gap: skipped by the walk
        slot.k = 0;
        slot.stage = Stage::Uniform;
        return true;
    }
    auto const& lo = slot.local;
    if (passed(lo.kind)) { slot.st = St::Pass; slot.used = SAAdaptation::Local; slot.proof = SAProof::Local; slot.stage = Stage::Done; return true; }
    if (lo.kind == ShiftKind::Invalid) { slot.st = St::Undecided; slot.why = status::Reason::SaInvalidTrials; slot.stage = Stage::Done; return true; }
    if (lo.kind == ShiftKind::NotTested) {
        if (!m_localFinal) return false;
        slot.k = 0;
        slot.stage = Stage::Uniform;
        return true;
    }
    // died in lockstep
    double const D = m_m.frame + lo.deathAfterFrames;
    if (lo.extension) { slot.st = St::Undecided; slot.why = status::Reason::SaDeathInSpan; slot.stage = Stage::Done; return true; }
    int k0 = lo.laterFixed;
    if (k0 <= 0) {
        if (!canFollowerAct(s, D)) {
            // a follower logged later could still act before D when the shift is early
            bool const mayCome = s < 0.0 && !m_breakSeen && m_now < D - s - kEps && followersUsable() < m_cfg.maxFollowers;
            if (mayCome) return false;
            failSlot(slot, nullptr);
            return true;
        }
        k0 = 1;
    }
    int const usable = followersUsable();
    if (usable < 1) {
        // the input that acted lies across a break / beyond the horizon: nothing can move it
        if (m_breakSeen || m_now >= m_m.frame + m_cfg.horizonFrames - kEps) {
            slot.st = St::Undecided;
            slot.why = status::Reason::SaUndecided;
            slot.stage = Stage::Done;
            return true;
        }
        return false;   // else wait for the follower to be logged
    }
    slot.k = std::min({k0, usable, m_cfg.maxFollowers});
    slot.stage = Stage::Uniform;
    return true;
}

void CompPlanner::advanceSlot(int side, Slot& slot) {
    if (slot.st != St::Open || m_finished) return;
    double const sign = side == 1 ? 1.0 : -1.0;
    double const s = sign * slot.mag;
    if (slot.stage == Stage::None && !decideLocal(side, slot)) return;
    // ---- the compensation stages (each step either requests trials, waits, or decides) ----
    for (int guard = 0; guard < 8 && slot.st == St::Open; ++guard) {
        if (slot.stage == Stage::Uniform) {
            if (slot.uniformId < 0) {
                std::vector<double> off(static_cast<size_t>(slot.k), s);
                int id = request(side, slot, s, Role::Uniform, -1, off);
                if (id == -1) { slot.st = St::Undecided; slot.why = status::Reason::SaNotMeasuredBudget; slot.stage = Stage::Done; return; }
                if (id == -2) { slot.st = St::Undecided; slot.why = m_finishReason; slot.stage = Stage::Done; return; }
                if (id == -3) {
                    // no legal uniform schedule: the member (or a follower) would cross a fixed
                    // input. More followers moving may make room; when none can, the shift is out
                    // of reach the way a neighbour limit is (SAPlanner anyLegalMember): never a fail
                    int const usable = followersUsable();
                    if (slot.k < std::min(usable, m_cfg.maxFollowers)) { ++slot.k; continue; }
                    slot.st = St::Undecided;
                    slot.neighbourLimit = true;
                    slot.why = status::Reason::SaUndecided;
                    slot.stage = Stage::Done;
                    return;
                }
                if (id == -4) { slot.st = St::Undecided; slot.why = status::Reason::SaInvalidTrials; slot.stage = Stage::Done; return; }
                slot.uniformId = id;
                // the probes run in parallel with the uniform trial (one round instead of two)
                slot.probeIds.clear();
                for (int j = 0; j < slot.k; ++j) {
                    std::vector<double> po(static_cast<size_t>(slot.k), s);
                    po[static_cast<size_t>(j)] = s + static_cast<double>(m_cfg.probeOffsetTicks);
                    int pid = request(side, slot, s, Role::Probe, j, po);
                    slot.probeIds.push_back(pid >= 0 ? pid : -1);   // budget / illegal / duplicate: no column
                }
                return;   // waiting
            }
            CompOutcome const* u = known(slot.uniformId);
            if (!u) return;
            if (u->kind == CompOutcome::Kind::Pass) {
                slot.st = St::Pass;
                slot.used = slot.k == 0 ? SAAdaptation::Local : uniformAdaptation(slot.k);
                // docs/SHIP_SOLVER.md §4.1: a settled pass with adapted followers is `compensated`
                // (the proof a flying mode can give when an exact re-join is impossible); the member
                // alone that only settled is `survived`
                slot.proof = u->rejoined ? SAProof::Rejoined : slot.k == 0 ? SAProof::Survived : SAProof::Compensated;
                slot.offsets = std::vector<double>(static_cast<size_t>(slot.k), s);
                slot.stage = Stage::Done;
                return;
            }
            if (u->kind == CompOutcome::Kind::Invalid) { slot.st = St::Undecided; slot.why = invalidWhy(u); slot.stage = Stage::Done; return; }
            if (u->laterFixed >= 1) slot.fixedActed = true;
            if (slot.k == 0) {
                // the member alone died: the same rule as a lockstep death
                double const D = u->deathFrame;
                if (u->extension) { slot.st = St::Undecided; slot.why = status::Reason::SaDeathInSpan; slot.stage = Stage::Done; return; }
                if (u->laterFixed <= 0 && !canFollowerAct(s, D)) { failSlot(slot, u); return; }
                int const usable = followersUsable();
                if (usable < 1) { slot.st = St::Undecided; slot.why = status::Reason::SaUndecided; slot.stage = Stage::Done; return; }
                slot.k = std::min({std::max(1, u->laterFixed), usable, m_cfg.maxFollowers});
                slot.uniformId = -1;
                continue;   // the uniform trial of k followers
            }
            slot.stage = Stage::Probes;
            continue;
        }
        if (slot.stage == Stage::Probes) {
            bool all = true;
            for (size_t j = 0; j < slot.probeIds.size(); ++j) {
                int id = slot.probeIds[j];
                if (id < 0) continue;
                CompOutcome const* p = known(id);
                if (!p) { all = false; continue; }
                if (p->kind == CompOutcome::Kind::Pass) {
                    // a probe that passed is a compensated pass
                    slot.st = St::Pass;
                    slot.used = compensatedAdaptation(slot.k);
                    slot.proof = p->rejoined ? SAProof::Rejoined : SAProof::Compensated;
                    slot.offsets.assign(static_cast<size_t>(slot.k), s);
                    slot.offsets[j] = s + static_cast<double>(m_cfg.probeOffsetTicks);
                    slot.stage = Stage::Done;
                    return;
                }
                if (p->kind == CompOutcome::Kind::Died && p->laterFixed >= 1) slot.fixedActed = true;
            }
            if (!all) return;
            slot.stage = Stage::Verify;
            continue;
        }
        if (slot.stage == Stage::Verify) {
            if (slot.verifyIds.empty()) {
                // every verification candidate at once (one round)
                auto cands = candidates(slot, s);
                int budgetHit = 0;
                for (auto& c : cands) {
                    if (slot.verifications >= m_cfg.verifyRoundings) break;
                    int id = request(side, slot, s, Role::Verify, -1, c);
                    if (id == -1) { budgetHit = -1; break; }
                    if (id == -2) { budgetHit = -2; break; }
                    if (id < 0) continue;   // illegal / duplicate candidate
                    slot.verifyIds.push_back(id);
                    ++slot.verifications;
                }
                if (slot.verifyIds.empty()) {
                    if (budgetHit == -1) { slot.st = St::Undecided; slot.why = status::Reason::SaNotMeasuredBudget; slot.stage = Stage::Done; return; }
                    if (budgetHit == -2) { slot.st = St::Undecided; slot.why = m_finishReason; slot.stage = Stage::Done; return; }
                    // no new legal candidate: decide with what was simulated
                    slot.verifications = m_cfg.verifyRoundings;
                    decideFromVerifications(side, slot, s);
                    if (slot.stage == Stage::Uniform) continue;
                    return;
                }
                return;   // waiting
            }
            bool all = true;
            for (int id : slot.verifyIds) {
                CompOutcome const* v = known(id);
                if (!v) { all = false; continue; }
                if (v->kind == CompOutcome::Kind::Pass) {
                    slot.st = St::Pass;
                    slot.used = compensatedAdaptation(slot.k);
                    slot.proof = v->rejoined ? SAProof::Rejoined : SAProof::Compensated;
                    slot.offsets = offsetsOf(id);
                    slot.stage = Stage::Done;
                    return;
                }
                if (v->kind == CompOutcome::Kind::Invalid) { slot.st = St::Undecided; slot.why = invalidWhy(v); slot.stage = Stage::Done; return; }
                if (v->laterFixed >= 1) slot.fixedActed = true;
            }
            if (!all) return;
            decideFromVerifications(side, slot, s);
            if (slot.stage == Stage::Uniform) continue;   // escalated
            return;
        }
        return;
    }
}

std::vector<double> CompPlanner::offsetsOf(int trialId) const {
    auto it = m_trialOffsets.find(trialId);
    return it == m_trialOffsets.end() ? std::vector<double>{} : it->second;
}

void CompPlanner::advanceSide(int side) {
    Side& sd = m_side[side];
    // the frontier walk: only the first lookaheadSlots + 1 undecided slots beyond the local run
    // (the local planner's gap skipped) get trials, nearest first; a slot whose local copy is still
    // running is skipped over (optimistic: its compensation is needed whether that copy passes or dies)
    for (int round = 0; round < static_cast<int>(sd.slots.size()) + 2; ++round) {
        bool changed = false;
        int inFlight = 0;
        for (auto& slot : sd.slots) {
            if (m_localFinal) {
                if (slot.mag <= sd.localRun + kEps) continue;
                if (!std::isnan(sd.localFail) && slot.mag < sd.localFail - kEps) continue;
            }
            if (slot.st == St::Pass) continue;
            if (slot.st == St::Fail || slot.st == St::Undecided) break;
            if (!slot.localKnown && !m_localFinal) continue;   // the lockstep copy still runs
            if (inFlight > m_cfg.lookaheadSlots) break;
            St const before = slot.st;
            Stage const stageBefore = slot.stage;
            int const requestedBefore = m_requested;
            advanceSlot(side, slot);
            if (slot.st != before || slot.stage != stageBefore || m_requested != requestedBefore) changed = true;
            if (slot.st == St::Pass) continue;
            if (slot.st == St::Fail || slot.st == St::Undecided) break;
            ++inFlight;
        }
        if (!changed) break;
    }
}

void CompPlanner::advanceAll() {
    if (m_finished) return;
    for (int side = 0; side < 2; ++side) advanceSide(side);
}

// ---- trials in and out ----

std::vector<CompTrial> CompPlanner::nextBatch(int max) {
    std::vector<CompTrial> out;
    if (m_finished) return out;
    // nearest shifts first: the queue is kept in request order; the handed-out batch is sorted by |shift|
    std::vector<CompTrial> all(m_queue.begin(), m_queue.end());
    m_queue.clear();
    std::stable_sort(all.begin(), all.end(), [](CompTrial const& a, CompTrial const& b) { return std::fabs(a.shiftFrames) < std::fabs(b.shiftFrames); });
    for (auto& t : all) {
        if (max > 0) {
            out.push_back(std::move(t));
            ++m_outstanding;
            --max;
        }
        else m_queue.push_back(std::move(t));
    }
    return out;
}

void CompPlanner::ingest(CompOutcome const& outcome) {
    auto it = m_trialSlot.find(outcome.id);
    if (it == m_trialSlot.end()) return;
    if (m_known.count(outcome.id)) return;
    m_known[outcome.id] = outcome;
    if (m_outstanding > 0) --m_outstanding;
    if (m_finished) return;
    advanceAll();
}

void CompPlanner::finish(status::Reason why) {
    if (m_finished) return;
    m_finished = true;
    m_finishReason = status::group(why) == status::TimingStatus::SequenceDependent ? why : status::Reason::SaUndecided;
    m_queue.clear();
    m_outstanding = 0;
    for (int side = 0; side < 2; ++side) {
        for (auto& s : m_side[side].slots) {
            if (s.st != St::Open) continue;
            s.st = St::Undecided;
            s.why = m_finishReason;
            s.stage = Stage::Done;
        }
    }
}

bool CompPlanner::done() const {
    if (m_finished) return true;
    if (m_outstanding > 0 || !m_queue.empty()) return false;
    if (!m_localFinal) return false;
    // every slot the walk can reach is decided (an Open slot beyond a Fail does not matter)
    for (int side = 0; side < 2; ++side) {
        Side const& sd = m_side[side];
        for (auto const& s : sd.slots) {
            if (s.mag <= sd.localRun + kEps) continue;
            if (!std::isnan(sd.localFail) && s.mag < sd.localFail - kEps) continue;
            if (s.st == St::Open) return false;
            if (s.st == St::Fail || s.st == St::Undecided) break;
        }
    }
    return true;
}

// ---- the result ----

SAEdge CompPlanner::edgeOf(int side) const {
    SAEdge e;
    Side const& sd = m_side[side];
    double const sign = side == 1 ? 1.0 : -1.0;
    double lastPass = sd.localRun;
    SAProof weakest = SAProof::Local;
    double proven = sd.localRun;
    bool provenOpen = true;
    std::vector<double> lastOffsets;
    bool ended = false;
    Slot const* failSlotP = nullptr;
    // the late side: the member may not cross a FIXED next input (one that cannot follow)
    double fixedLimit = kNaN;
    if (side == 1 && !m_followers.empty() && m_followers.front().breakBefore) fixedLimit = m_followers.front().frame - m_cfg.neighbourMarginFrames - m_m.frame;
    EdgeStop limitStop = sd.limitKind == LimitKind::Range ? EdgeStop::Range : stopOf(sd.limitKind);
    bool neighbourCut = false;
    for (auto const& s : sd.slots) {
        if (s.mag <= sd.localRun + kEps) continue;
        if (!std::isnan(sd.localFail) && s.mag < sd.localFail - kEps) continue;
        if (!std::isnan(fixedLimit) && s.mag > fixedLimit + kEps) { neighbourCut = true; break; }
        if (s.st == St::Pass) {
            lastPass = s.mag;
            if (s.proof > weakest) weakest = s.proof;
            if (s.proof == SAProof::Survived) provenOpen = false;
            else if (provenOpen) proven = s.mag;
            lastOffsets = s.offsets;
            continue;
        }
        if (s.st == St::Undecided && s.neighbourLimit) { neighbourCut = true; break; }
        ended = true;
        if (s.st == St::Fail) failSlotP = &s;
        break;
    }
    e.passFrames = sign * lastPass;
    e.proof = weakest;
    e.provenPassFrames = sign * std::min(proven, lastPass);
    e.followerOffsetsFrames = lastOffsets;
    if (!ended) {
        e.stop = neighbourCut ? EdgeStop::Neighbour : limitStop;
        if (!m_localFinal) e.stop = EdgeStop::Undecided;
        return e;
    }
    if (failSlotP) {
        e.stop = EdgeStop::Fail;
        double failMag = failSlotP->mag;
        if (!std::isnan(sd.localFail) && std::fabs(failMag - sd.localFail) <= kEps) failMag = sd.localFail;
        e.failFrames = sign * failMag;
        if (failSlotP->haveDeath) {
            e.cause = failSlotP->extension ? EdgeCause::Extension : EdgeCause::Self;
            e.laterInputs = 0;
            e.failAfterFrames = failSlotP->deathFrame - m_m.frame;
            e.failObjectId = failSlotP->objectId;
            e.failDeathX = failSlotP->deathX;
        }
        else {
            e.cause = EdgeCause::Self;
            e.laterInputs = 0;
        }
        return e;
    }
    e.stop = EdgeStop::Undecided;
    return e;
}

SAResult CompPlanner::result() const {
    SAResult r;
    r.valid = true;
    r.isolated = m_isolated || (m_localFinal && followersUsable() == 0 && (m_breakSeen || m_now >= m_m.frame + m_cfg.horizonFrames - kEps));
    r.sequence.present = true;
    r.sequence.early = edgeOf(0);
    r.sequence.late = edgeOf(1);
    // Fable D1: a side that ended without a fail of its own at the local pass edge reports the
    // LOCAL bracket (inherited), so W_local ⊆ W_SA holds on the emitted numbers
    for (int s = 0; s < 2; ++s) {
        SAEdge& e = s == 0 ? r.sequence.early : r.sequence.late;
        double const sign = s == 1 ? 1.0 : -1.0;
        if (e.bounded() || std::isnan(m_side[s].localFail)) continue;
        if (std::fabs(std::fabs(e.passFrames) - m_side[s].localRun) > kEps) continue;
        e.passFrames = sign * m_side[s].localRun;
        e.failFrames = sign * m_side[s].localFail;
        e.inherited = true;
        e.cause = EdgeCause::None;
        e.laterInputs = -1;
        e.proof = SAProof::Local;
        e.provenPassFrames = e.passFrames;
        e.followerOffsetsFrames.clear();
    }
    r.survivedOnly = r.sequence.early.proof == SAProof::Survived || r.sequence.late.proof == SAProof::Survived;
    double res = 0.0;
    for (auto const* e : {&r.sequence.early, &r.sequence.late}) {
        if (e->hasBracket()) res = std::max(res, std::fabs(e->failFrames - e->passFrames));
    }
    r.sequence.resolutionFrames = res > 0.0 ? res : 1.0;
    r.sequence.trials = m_requested;
    r.trials = m_requested;
    for (int s = 0; s < 2; ++s) {
        EdgeStop st = s == 0 ? r.sequence.early.stop : r.sequence.late.stop;
        r.sideDecided[s] = st != EdgeStop::Undecided && st != EdgeStop::Untested;
    }
    r.decided = r.sideDecided[0] && r.sideDecided[1];
    // D3a: both local sides open to the range and no comp work -> the sequence window IS the local one
    r.openRange = m_localFinal && std::isnan(m_side[0].localFail) && std::isnan(m_side[1].localFail) && m_requested == 0 && r.decided;
    bool used[7] = {false, false, false, false, false, false, false};
    bool budget = false, invalid = false, finishReason = false;
    for (int s = 0; s < 2; ++s) {
        Side const& sd = m_side[s];
        // the walk's own view (F3): the slots inside the local run and the local planner's gap are
        // not the planner's, and nothing beyond the slot that ended the side ever mattered
        for (auto const& slot : sd.slots) {
            if (slot.mag <= sd.localRun + kEps) continue;
            if (!std::isnan(sd.localFail) && slot.mag < sd.localFail - kEps) continue;
            if (slot.st == St::Pass) {
                if (slot.used != SAAdaptation::Local) used[static_cast<int>(slot.used)] = true;
                continue;
            }
            if (slot.st == St::Undecided && !slot.neighbourLimit) {
                ++r.undecidedShifts;
                if (slot.why == status::Reason::SaNotMeasuredBudget) budget = true;
                else if (slot.why == status::Reason::SaInvalidTrials) invalid = true;
                else if (slot.why != status::Reason::SaUndecided) finishReason = true;
            }
            break;   // Fail / Undecided / neighbour: the side ends here
        }
    }
    for (int k = 1; k <= 6; ++k) if (used[k]) r.adaptationUsed.push_back(static_cast<SAAdaptation>(k));
    if (finishReason) r.failure = m_finishReason;
    else if (budget) r.failure = status::Reason::SaNotMeasuredBudget;
    else if (invalid) r.failure = status::Reason::SaInvalidTrials;
    else r.failure = status::Reason::SaUndecided;
    r.debug.push_back(describe());
    return r;
}

std::string CompPlanner::describe() const {
    std::string out;
    for (int side = 1; side >= 0; --side) {
        Side const& sd = m_side[side];
        double const sign = side == 1 ? 1.0 : -1.0;
        out += side == 1 ? "late" : " | early";
        int localPasses = 0;
        for (auto const& s : sd.slots) {
            if (s.st == St::Pass && s.used == SAAdaptation::Local) { ++localPasses; continue; }
            if (s.st == St::Open) continue;
            out += " " + fmtNum(sign * s.mag);
            switch (s.st) {
                case St::Pass: out += std::string(" ") + name(s.used) + fmtOffsets(s.offsets) + " " + name(s.proof); break;
                case St::Fail: {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " fail@%.1f", s.deathFrame - m_m.frame);
                    out += buf;
                    break;
                }
                case St::Undecided: out += s.neighbourLimit ? std::string(" neighbour") : std::string(" undecided(") + status::name(s.why) + ")"; break;
                default: break;
            }
        }
        if (localPasses) out += " [" + std::to_string(localPasses) + " local]";
        SAEdge e = edgeOf(side);
        out += std::string(" -> ") + name(e.stop);
    }
    out += " | followers " + std::to_string(followersUsable()) + (m_isolated ? " (isolated)" : "") + " | trials " + std::to_string(m_requested);
    return out;
}

}  // namespace gprl::solver::comp
