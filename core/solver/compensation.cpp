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

bool allEqual(std::vector<double> const& off, double s) {
    for (double v : off) {
        if (std::fabs(v - s) > kEps) return false;
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

/// The reason an Invalid outcome gives a slot: `sa_undecided` for a trial that could not be decided
/// (cancelled, crossed), `sa_invalid_trials` for a failed proof (F6).
status::Reason invalidWhy(CompOutcome const* o) {
    return o && o->notTested ? status::Reason::SaUndecided : status::Reason::SaInvalidTrials;
}

}  // namespace

// ---- construction and facts ----

CompPlanner::CompPlanner(CompConfig const& cfg, CompMember member, std::vector<CompFollower> followers) : m_cfg(cfg), m_m(std::move(member)) {
    m_cfg.maxFollowers = std::clamp(m_cfg.maxFollowers, 0, kMaxFollowers);
    int const range = std::max(1, m_cfg.rangeTicks());
    double const maxShift = static_cast<double>(range);
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
        for (int k = 0; k < range; ++k) {
            double mag = offset > 1e-9 ? offset + static_cast<double>(k) : static_cast<double>(k + 1);
            if (mag > sd.limit + kEps) break;
            if (mag <= kEps) continue;
            Slot s;
            s.mag = mag;
            sd.slots.push_back(s);
        }
        sd.walkLimit = sd.limit;
        sd.walkKind = sd.limitKind;
    }
    for (auto const& f : followers) addFollower(f);
}

void CompPlanner::addFollower(CompFollower f) {
    if (m_breakSeen) return;
    double const from = lastRigidFrame();
    if (f.frame <= from + kEps) return;   // not after the member (and its rigid inputs)
    if (f.breakBefore) {
        // a cluster break: nothing from here on can follow; the input itself is the next FIXED one
        m_breakSeen = true;
        m_followers.push_back(f);
        advanceAll();
        return;
    }
    // beyond the horizon: a fixed input; a later one cannot be a follower either (time order)
    if (f.frame > from + m_cfg.horizonFrames + kEps) {
        f.breakBefore = true;
        m_breakSeen = true;
        m_followers.push_back(f);
    }
    else {
        // the cap keeps one more input: the next FIXED one after the last follower that may move
        if (static_cast<int>(m_followers.size()) >= m_cfg.maxFollowers + 1) return;
        if (static_cast<int>(m_followers.size()) == m_cfg.maxFollowers) m_capSeen = true;   // it could have followed: the cap binds
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

bool CompPlanner::moreFollowersBeyondCap() const { return m_capSeen; }

void CompPlanner::setNow(double frame) {
    if (frame <= m_now) return;
    m_now = frame;
    if (!m_isolated && m_now >= lastRigidFrame() + m_cfg.horizonFrames - kEps && followersUsable() == 0) m_isolated = true;
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
        // CT-D6: a local side that is OPEN TO THE RANGE (it never failed inside the range the
        // frozen search uses) keeps the local range: nothing to compensate, the extra range is not
        // searched. A side open only up to the next input may still be widened by followers moving
        // along (the pair shift), so its walk goes on. A rigid member (a phase search) has no local
        // facts: its walk uses the whole range
        double const localRange = static_cast<double>(std::max(1, m_cfg.maxShiftTicks));
        bool const neighbourLimited = late ? (!std::isnan(lateLimitFrames) && lateLimitFrames < localRange - 1.0 + kEps)
                                           : (m_side[side].limit < localRange - 1.0 + kEps);
        m_side[side].localOpenStop = !neighbourLimited ? EdgeStop::Range : late ? EdgeStop::Neighbour : stopOf(m_side[side].limitKind);
        if (std::isnan(lr.fail) && !neighbourLimited && m_m.rigid.empty() && !all.empty() && m_side[side].limit > localRange + kEps) {
            m_side[side].walkLimit = localRange;
            m_side[side].walkKind = LimitKind::Range;
        }
    }
    m_localFinal = true;
    advanceAll();
}

// ---- the search ----

/// true = a follower (from index `fromFollower` on) shifted along by s acts before the death
/// (a follower that could still be logged before D + |s| is the caller's "wait" case).
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

/// Clamps `offsets` into the legal lattice of shift s (the range between "as recorded" and "moved
/// along with the member" widened by offsetRangeTicks, the input order, the next fixed input) and
/// gives the last moved frame. false = no legal schedule with that many followers.
bool CompPlanner::legalOffsets(double s, std::vector<double>& offsets, double& lastMoved) const {
    double prev = lastRigidFrame() + s;   // the member's (its last rigid input's) shifted frame
    int const k = static_cast<int>(offsets.size());
    int const usable = followersUsable();
    if (k > usable) return false;
    // the member may not cross a FIXED next input (k = 0 with a next input that cannot follow)
    if (k == 0 && !m_followers.empty()) {
        auto const& f0 = m_followers.front();
        if (prev >= f0.frame - m_cfg.neighbourMarginFrames - kEps) return false;
    }
    double const range = static_cast<double>(m_cfg.offsetRangeTicks);
    for (int j = 0; j < k; ++j) {
        auto const& f = m_followers[static_cast<size_t>(j)];
        double lo = std::max(std::min(s, 0.0) - range, prev + m_cfg.neighbourMarginFrames - f.frame);
        double hi = std::max(s, 0.0) + range;
        // the next FIXED input (the follower after the last moved one, or the first unusable input)
        if (j == k - 1 && static_cast<size_t>(j + 1) < m_followers.size()) {
            double next = m_followers[static_cast<size_t>(j + 1)].frame;
            hi = std::min(hi, next - m_cfg.neighbourMarginFrames - f.frame);
        }
        // whole ticks unless sub-tick placement is available (a follower moved along with a
        // half-tick member keeps the member's fraction: the clamp does not round)
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
    t.adaptation = t.followers == 0 ? SAAdaptation::Local : allEqual(offsets, s) ? uniformAdaptation(t.followers) : compensatedAdaptation(t.followers);
    t.moved.push_back({m_m.id, m_m.down, m_m.frame + s});
    for (auto const& r : m_m.rigid) t.moved.push_back({r.id, r.down, r.frame + s});
    for (int j = 0; j < t.followers; ++j) {
        auto const& f = m_followers[static_cast<size_t>(j)];
        t.moved.push_back({f.id, f.down, f.frame + offsets[static_cast<size_t>(j)]});
    }
    t.offsetsFrames = offsets;
    double last = m_m.frame + s;
    for (auto const& mv : t.moved) last = std::max(last, mv.frame);
    t.lastMovedFrame = last;
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
    constexpr size_t N = static_cast<size_t>(kMaxFollowers);
    size_t const k = std::min<size_t>(columns.size(), N);
    std::vector<double> d(k, 0.0);
    if (k == 0 || !(probe > 0.0)) return d;
    // J (2 x k): column j = (dev(P_j) - dev(U)) / probe; unknown columns are dropped (offset 0)
    std::array<std::array<double, 2>, N> J{};
    std::array<bool, N> use{};
    for (size_t j = 0; j < k; ++j) {
        auto const& c = columns[j];
        bool ok = std::isfinite(c.dy) && std::isfinite(c.dvy);
        use[j] = ok;
        J[j] = ok ? std::array<double, 2>{(c.dy - base.dy) / probe, (c.dvy - base.dvy) / probe} : std::array<double, 2>{0.0, 0.0};
    }
    // normal equations (J^T J + ridge I) d = -J^T base, k x k (k <= 6), Gauss-Jordan with pivoting
    double A[N][N] = {};
    double b[N] = {};
    for (size_t i = 0; i < k; ++i) {
        for (size_t j = 0; j < k; ++j) A[i][j] = J[i][0] * J[j][0] + J[i][1] * J[j][1] + (i == j ? ridge : 0.0);
        b[i] = -(J[i][0] * base.dy + J[i][1] * base.dvy);
    }
    for (size_t i = 0; i < k; ++i) {
        if (use[i]) continue;
        // an unknown column takes no part: its row and column are the identity's, its offset 0
        for (size_t j = 0; j < k; ++j) {
            A[i][j] = i == j ? 1.0 : 0.0;
            A[j][i] = i == j ? 1.0 : 0.0;
        }
        b[i] = 0.0;
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

bool CompPlanner::isPass(CompOutcome const& o) const {
    if (o.kind != CompOutcome::Kind::Pass) return false;
    return !m_cfg.requireRejoin || rejoin::rejoined(o.rejoin);
}

void CompPlanner::passSlot(Slot& slot, double s, CompOutcome const& o, std::vector<double> offsets) {
    // followers left as recorded at the end of the schedule did not take part: the pass moved
    // only the ones before them
    while (!offsets.empty() && std::fabs(offsets.back()) <= kEps && std::fabs(s) > kEps) offsets.pop_back();
    int const k = static_cast<int>(offsets.size());
    slot.st = St::Pass;
    slot.stage = Stage::Done;
    slot.used = k == 0 ? SAAdaptation::Local : allEqual(offsets, s) ? uniformAdaptation(k) : compensatedAdaptation(k);
    // docs/SHIP_SOLVER.md §4.1, §11.2: an exact re-join (or the level end: nothing follows) is
    // `rejoined`; a re-join inside the tolerance is `compensated`; without requireRejoin a settled
    // pass with adapted followers is `compensated` and the member alone that only settled `survived`
    if (o.rejoin == rejoin::Kind::Exact || o.rejoin == rejoin::Kind::LevelEnd) slot.proof = SAProof::Rejoined;
    else if (o.rejoin == rejoin::Kind::Approx || o.rejoin == rejoin::Kind::Parallel) slot.proof = SAProof::Compensated;
    else slot.proof = k == 0 ? SAProof::Survived : SAProof::Compensated;
    slot.offsets = std::move(offsets);
    slot.rejoin = o.rejoin;
    slot.rejoinFrame = o.rejoinFrame;
    slot.rejoinErrY = o.rejoinErrY;
    slot.rejoinErrVy = o.rejoinErrVy;
}

void CompPlanner::resetModel(Slot& slot) {
    slot.base.clear();
    slot.baseId = -1;
    slot.baseWarm = false;
    slot.probeIds.clear();
    slot.probeSteps.clear();
    slot.wantOwnProbes = false;
    slot.ownAll = false;
    slot.modelBuilt = false;
    slot.pendingCands.clear();
    slot.verifyIds.clear();
    slot.allVerifyIds.clear();
    slot.bestId = -1;
    slot.bestScore = -1e300;
    slot.rebases = 0;
}

/// How far a trial that did not pass got: alive at its look-ahead beats every death, the smaller
/// its deviation the better; among deaths the later one.
double CompPlanner::scoreOf(CompOutcome const& o) const {
    if (o.kind == CompOutcome::Kind::Pass) {
        double norm = 0.0;
        if (!o.dev.empty()) {
            auto const& d = o.dev.back();
            norm = std::fabs(d.dy) / std::max(1e-9, m_cfg.rejoin.tolY) + std::fabs(d.dvy) / std::max(1e-9, m_cfg.rejoin.tolVy);
        }
        return 1e6 - std::min(norm, 1e5);
    }
    if (o.kind == CompOutcome::Kind::Died && !std::isnan(o.deathFrame)) return o.deathFrame - m_m.frame;
    return -1e300;
}

/// A finished trial of the slot that did not pass: the flags the escalation rules read, and the
/// trial that got furthest (the base the search continues from).
void CompPlanner::noteOutcome(Slot& slot, int trialId, CompOutcome const& o) {
    if (o.kind == CompOutcome::Kind::Died && o.laterFixed >= 1) slot.fixedActed = true;
    if (o.kind == CompOutcome::Kind::Pass && !rejoin::rejoined(o.rejoin)) slot.survived = true;   // alive without a re-join
    if (o.kind == CompOutcome::Kind::Invalid) return;
    double const score = scoreOf(o);
    if (score > slot.bestScore + 1e-9) {
        slot.bestScore = score;
        slot.bestId = trialId;
    }
}

/// The family's search continues from `trialId` (already simulated): it is the base now, the
/// probes are asked again where no known response reaches as far as it got.
void CompPlanner::rebase(Slot& slot, int trialId, std::vector<double> offsets) {
    slot.base = std::move(offsets);
    slot.baseId = trialId;
    slot.baseWarm = false;
    slot.probeIds.assign(slot.base.size(), -1);
    slot.probeSteps.assign(slot.base.size(), 0.0);
    slot.wantOwnProbes = false;
    slot.modelBuilt = false;
    slot.pendingCands.clear();
    slot.verifyIds.clear();
    slot.allVerifyIds.clear();
    slot.stage = Stage::Probes;
}

/// The WARM base (CT-D5): the schedule of the nearest passing slot of this side closer to 0 that
/// moved followers, continued to this shift - along the trend of the last TWO such schedules when
/// they moved the same followers (compensations of neighbouring shifts change linearly: each
/// follower by its own step per tick), else moved along by the difference of the shifts.
/// false = none (the uniform base).
bool CompPlanner::warmBase(int side, Slot const& slot, double s, std::vector<double>& base) const {
    Side const& sd = m_side[side];
    Slot const* from = nullptr;
    Slot const* before = nullptr;
    for (auto const& x : sd.slots) {
        if (x.mag >= slot.mag - kEps) break;
        if (x.st != St::Pass) continue;
        if (!x.offsets.empty()) {
            before = from;
            from = &x;
        }
        else {   // a nearer LOCAL pass: no schedule to continue
            from = nullptr;
            before = nullptr;
        }
    }
    if (!from) return false;
    double const sign = side == 1 ? 1.0 : -1.0;
    double const delta = s - sign * from->mag;
    int const usable = followersUsable();
    int const k = std::min(std::max(static_cast<int>(from->offsets.size()), slot.k), usable);
    if (k < 1) return false;
    // followers beyond the continued schedule: moved along when the local death says they acted
    // (slot.k), else left as recorded
    base.assign(static_cast<size_t>(k), 0.0);
    for (size_t j = from->offsets.size(); j < base.size(); ++j) base[j] = s;
    // the trend only between NEIGHBOURING shifts one step apart, and only when it is a plausible
    // continuation (no follower moving more than two ticks per tick of shift): two schedules of a
    // different shape extrapolate to nonsense
    bool trend = before && before->offsets.size() == from->offsets.size() && from->mag - before->mag > kEps
              && std::fabs(std::fabs(delta) - (from->mag - before->mag)) <= kEps;
    if (trend) {
        for (size_t j = 0; j < from->offsets.size(); ++j) {
            double const per = (from->offsets[j] - before->offsets[j]) / (sign * (from->mag - before->mag));
            if (std::fabs(per) > 2.0 + kEps) trend = false;
        }
    }
    for (size_t j = 0; j < from->offsets.size() && j < base.size(); ++j) {
        double step = delta;   // along with the member
        if (trend) step = (from->offsets[j] - before->offsets[j]) / (sign * (from->mag - before->mag)) * delta;
        base[j] = from->offsets[j] + (m_m.subtick ? step : std::round(step));
    }
    return true;
}

/// One probe of this slot: its base with `follower` moved one tick more (or, when that is not a
/// new legal schedule - the range / the next fixed input clamps it back onto the base - one less).
void CompPlanner::requestProbe(int side, Slot& slot, double s, int follower) {
    size_t const j = static_cast<size_t>(follower);
    if (slot.probeIds.size() < slot.base.size()) {
        slot.probeIds.resize(slot.base.size(), -1);
        slot.probeSteps.resize(slot.base.size(), 0.0);
    }
    if (j >= slot.base.size() || slot.probeIds[j] != -1) return;
    slot.probeIds[j] = -2;   // asked for: not asked again (even when nothing could be requested)
    double const step = static_cast<double>(m_cfg.probeOffsetTicks);
    for (double dir : {1.0, -1.0}) {
        std::vector<double> po = slot.base;
        po[j] += dir * step;
        int id = request(side, slot, s, Role::Probe, follower, po);
        if (id >= 0) {
            auto got = offsetsOf(id);
            slot.probeIds[j] = id;
            slot.probeSteps[j] = got[j] - slot.base[j];
            return;
        }
        if (id == -1) slot.budgetCut = true;
        if (id == -1 || id == -2) return;   // budget / finished: no more requests
    }
}

/// The response of one follower measured by this slot's own probe: the trace of
/// (dev(probe) - dev(base)) per tick of offset. false = no usable probe of that follower.
bool CompPlanner::ownColumn(Slot const& slot, int follower, std::vector<DevSample>& col) const {
    col.clear();
    size_t const j = static_cast<size_t>(follower);
    if (j >= slot.probeIds.size() || slot.probeIds[j] < 0) return false;
    double const step = slot.probeSteps[j];
    if (std::fabs(step) < 1e-9) return false;
    CompOutcome const* u = known(slot.baseId);
    CompOutcome const* p = known(slot.probeIds[j]);
    if (!u || !p || u->dev.empty() || p->dev.empty()) return false;
    size_t bi = 0;
    for (auto const& d : p->dev) {
        // frames are step ends: both traces carry the same frames while both are alive
        while (bi < u->dev.size() && u->dev[bi].frame < d.frame - kEps) ++bi;
        if (bi >= u->dev.size()) break;
        if (std::fabs(u->dev[bi].frame - d.frame) > kEps) continue;
        col.push_back({d.frame, (d.dy - u->dev[bi].dy) / step, (d.dvy - u->dev[bi].dvy) / step});
    }
    return !col.empty();
}

/// The response model the slot verifies with (CT-D5): per follower its OWN probe when it has
/// one, else the cached response of k followers, else the one cached for a smaller family (the
/// response of a follower barely depends on how many later inputs move). An empty column = not
/// known: that follower's offset is not modelled (and gets a probe when the slot asks for one).
CompPlanner::Response CompPlanner::modelOf(Slot const& slot) const {
    Response m;
    int const k = std::max(0, slot.k);
    m.columns.assign(static_cast<size_t>(k), {});
    if (m_cfg.reuseResponse) {
        for (int kk = k; kk >= 1; --kk) {
            auto it = m_response.find(kk);
            if (it == m_response.end()) continue;
            for (size_t j = 0; j < it->second.columns.size() && j < m.columns.size(); ++j) {
                if (m.columns[j].empty()) m.columns[j] = it->second.columns[j];
            }
        }
    }
    for (int j = 0; j < k; ++j) {
        std::vector<DevSample> col;
        if (ownColumn(slot, j, col)) m.columns[static_cast<size_t>(j)] = std::move(col);
    }
    return m;
}

/// Some other slot is measuring a response of k followers right now (its probes are in flight):
/// the same response is not measured twice at once (both sides usually need it in the same round).
bool CompPlanner::probesInFlight(int k, Slot const* except) const {
    for (int side = 0; side < 2; ++side) {
        for (auto const& x : m_side[side].slots) {
            if (&x == except || x.k != k || x.st != St::Open) continue;
            for (int id : x.probeIds) {
                if (id >= 0 && !known(id)) return true;
            }
        }
    }
    return false;
}

std::vector<std::vector<double>> CompPlanner::candidates(Slot const& slot, double s, Response const* model) const {
    // the response model at the latest frame the base trial and every known column reach
    std::vector<std::vector<double>> out;
    int const k = slot.k;
    if (k <= 0 || static_cast<int>(slot.base.size()) != k) return out;
    CompOutcome const* u = known(slot.baseId);
    DevSample base;
    std::vector<double> d(static_cast<size_t>(k), 0.0);
    bool modelled = false;
    if (u && !u->dev.empty() && model) {
        double common = u->dev.back().frame;
        bool any = false;
        for (auto const& col : model->columns) {
            if (col.empty()) continue;
            common = std::min(common, col.back().frame);
            any = true;
        }
        if (any && devAt(u->dev, common, base)) {
            std::vector<DevSample> cols;
            for (int j = 0; j < k; ++j) {
                DevSample c{common, kNaN, kNaN};
                DevSample x;
                if (static_cast<size_t>(j) < model->columns.size() && devAt(model->columns[static_cast<size_t>(j)], common, x)) {
                    // solveOffsets takes the probes' own deviations: base + response x one tick
                    c.dy = base.dy + x.dy;
                    c.dvy = base.dvy + x.dvy;
                }
                cols.push_back(c);
            }
            d = solveOffsets(base, cols, 1.0, m_cfg.ridge);
            modelled = true;
        }
    }
    double const range = static_cast<double>(m_cfg.offsetRangeTicks);
    double const lo = std::min(s, 0.0) - range, hi = std::max(s, 0.0) + range;
    auto push = [&](std::vector<double> off) {
        for (auto& v : off) v = std::clamp(v, lo, hi);
        for (auto const& t : out) if (sameOffsets(t, off)) return;
        out.push_back(std::move(off));
    };
    if (modelled) {
        std::vector<double> v1(static_cast<size_t>(k)), v2(static_cast<size_t>(k));
        size_t flip = 0;
        double flipFrac = -1.0;
        for (size_t j = 0; j < static_cast<size_t>(k); ++j) {
            double r = std::round(d[j]);
            v1[j] = slot.base[j] + r;
            double frac = std::fabs(d[j] - r);
            if (frac > flipFrac + 1e-12) { flipFrac = frac; flip = j; }
        }
        // V2: the other rounding of the least decided component, or one more tick along the
        // largest offset when every component was already whole
        v2 = v1;
        if (flipFrac > 1e-9) v2[flip] = slot.base[flip] + (d[flip] > std::round(d[flip]) ? std::ceil(d[flip]) : std::floor(d[flip]));
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
    // without a model (nothing sampled): the last follower one tick either way around the base
    std::vector<double> a = slot.base, b = slot.base;
    a.back() += 1.0;
    b.back() -= 1.0;
    push(a);
    push(b);
    return out;
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

void CompPlanner::undecide(int side, Slot& slot, status::Reason why) {
    (void)side;
    slot.st = St::Undecided;
    slot.why = why;
    slot.stage = Stage::Done;
}

/// CT-D6: an undecided slot that is no doubt about the window. The frozen search already says how
/// far the side is open (to its range, or to the next input); what the compensation walk explores
/// BEYOND that - the extra range, or the far side of the next input - is a bonus: when it cannot
/// be decided, the side ends OPEN at its last pass with the stop the local side has. Only a side
/// the frozen search bounded with a fail INSIDE its range can be left undecided.
bool CompPlanner::openCut(int side, Slot const& slot) const {
    if (slot.st != St::Undecided || slot.neighbourLimit || !m_localFinal || !m_m.rigid.empty()) return false;
    Side const& sd = m_side[side];
    double const localRange = static_cast<double>(std::max(1, m_cfg.maxShiftTicks));
    return std::isnan(sd.localFail) || slot.mag > localRange + kEps;
}

/// One more follower joins the slot's family (CT-D4). The family's best trial stays the base
/// and the new follower starts as recorded (offset 0: the same schedule, nothing to simulate
/// again) - only its response is measured. When no trial of the family may serve (none, or the
/// new follower cannot stay where it is behind it), the larger family starts over from its
/// uniform / warm base.
void CompPlanner::grow(int side, Slot& slot, double s) {
    (void)side;
    if (slot.bestId >= 0 && known(slot.bestId)) {
        std::vector<double> ext = offsetsOf(slot.bestId);
        if (static_cast<int>(ext.size()) <= slot.k) {
            ext.resize(static_cast<size_t>(slot.k) + 1, 0.0);
            std::vector<double> legal = ext;
            double last = 0.0;
            // 0 must be a legal offset of the new follower behind the best schedule
            if (legalOffsets(s, legal, last) && sameOffsets(legal, ext)) {
                int const best = slot.bestId;
                double const score = slot.bestScore;
                ++slot.k;
                rebase(slot, best, ext);
                slot.ownAll = false;
                slot.rebases = 0;
                slot.bestId = best;
                slot.bestScore = score;
                slot.triedOffsets.push_back(ext);   // the same schedule as the best trial: never simulated again
                return;
            }
        }
    }
    ++slot.k;
    resetModel(slot);
    slot.stage = Stage::Base;
}

/// A verification round ended without a pass. Either the next candidate of the model is requested,
/// or one more follower joins (causal evidence that it matters), or the slot's own probes are
/// measured (the last resort of a family that came close), or the slot is decided. Leaves the
/// slot Open with a new stage (Verify / Probes / Base / Wait), or decided.
void CompPlanner::afterVerifications(int side, Slot& slot, double s) {
    // 0. the model's next candidate (one verification at a time: a pass ends the slot)
    while (!slot.pendingCands.empty()) {
        std::vector<double> c = std::move(slot.pendingCands.front());
        slot.pendingCands.erase(slot.pendingCands.begin());
        int id = request(side, slot, s, Role::Verify, -1, c);
        if (id == -1) { slot.budgetCut = true; slot.pendingCands.clear(); break; }
        if (id == -2) { undecide(side, slot, m_finishReason); return; }
        if (id < 0) continue;   // illegal / duplicate candidate
        slot.verifyIds.assign(1, id);
        slot.stage = Stage::Verify;
        return;
    }
    if (slot.budgetCut) {
        // the trial budget refused a probe / a candidate of this shift: the search was not finished,
        // so nothing may be concluded from it (prompt §16: unresolved, never a guessed narrow window)
        undecide(side, slot, status::Reason::SaNotMeasuredBudget);
        return;
    }
    // 1. every candidate of this base failed. When some trial of the family got further than its
    // base (alive closer to the recorded run, or dead later), the search continues from that trial
    // (CT-D5): its own responses are measured where the known ones do not reach
    if (slot.bestId >= 0 && slot.bestId != slot.baseId && slot.rebases < m_cfg.maxRebases && known(slot.bestId)
        && static_cast<int>(offsetsOf(slot.bestId).size()) == slot.k) {
        CompOutcome const* base = known(slot.baseId);
        if (!base || slot.bestScore > scoreOf(*base) + 1e-9) {
            ++slot.rebases;
            rebase(slot, slot.bestId, offsetsOf(slot.bestId));
            return;
        }
    }
    // 2. one more follower joins when a trial showed that it matters (F4, docs §4.2 step 4): a
    // fixed later input acted before some death, the NEXT follower shifted along could still act
    // before the last death, or a schedule survived without re-joining (one more follower can
    // bring it back)
    CompOutcome const* last = nullptr;
    for (int id : slot.allVerifyIds) {
        if (auto const* o = known(id); o && o->kind == CompOutcome::Kind::Died) last = o;
    }
    if (!last) {
        if (auto const* o = known(slot.baseId); o && o->kind == CompOutcome::Kind::Died) last = o;
    }
    int const usable = followersUsable();
    bool const wantGrow = slot.fixedActed || slot.survived || (last && canFollowerAct(s, last->deathFrame, slot.k));
    if (wantGrow && slot.k < usable) {
        grow(side, slot, s);
        return;
    }
    // 3. no larger family. Optionally every follower's response is measured HERE once more (the
    // known responses were another schedule's) before the slot ends undecided
    if (m_cfg.ownProbesLastResort && slot.survived && !slot.ownAll && slot.k >= 1) {
        slot.ownAll = true;
        rebase(slot, slot.baseId, slot.base);
        slot.wantOwnProbes = true;
        return;
    }
    if (slot.fixedActed || slot.survived) {
        bool const moreMayCome = !m_breakSeen && !m_capSeen && static_cast<int>(m_followers.size()) <= slot.k && slot.k < m_cfg.maxFollowers
                              && m_now < lastRigidFrame() + m_cfg.horizonFrames - kEps;
        if (moreMayCome) {
            slot.stage = Stage::Wait;   // the next follower (or the horizon) re-opens the decision
            return;
        }
        // the follower CAP (compute), not causality, stopped the search: unresolved by budget, never
        // a fail (CT-D4); a schedule survived but none re-joined: SURVIVES_NO_REJOIN (CT-D2); else a
        // fixed later input acted and nothing may move it: a larger family could still change the
        // outcome (V2-D2) - undecided, never guessed
        if (slot.k >= m_cfg.maxFollowers && m_capSeen) undecide(side, slot, status::Reason::SaNotMeasuredBudget);
        else if (slot.survived) undecide(side, slot, status::Reason::SaSurvivesNoRejoin);
        else undecide(side, slot, status::Reason::SaUndecided);
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
        // bracket is run by simulation (the member alone, or the nearest passing schedule moved
        // along; proof by its outcome, D3b), inside it never
        if (!m_localFinal) return false;
        double const run = m_side[side].localRun, fail = m_side[side].localFail;
        if (slot.mag <= run + kEps) { slot.st = St::Pass; slot.used = SAAdaptation::Local; slot.proof = SAProof::Local; slot.stage = Stage::Done; return true; }
        if (!std::isnan(fail) && slot.mag < fail - kEps) return false;   // the local planner's gap: skipped by the walk
        slot.k = 0;
        slot.stage = Stage::Base;
        return true;
    }
    auto const& lo = slot.local;
    if (passed(lo.kind)) { slot.st = St::Pass; slot.used = SAAdaptation::Local; slot.proof = SAProof::Local; slot.stage = Stage::Done; return true; }
    if (lo.kind == ShiftKind::Invalid) { undecide(side, slot, status::Reason::SaInvalidTrials); return true; }
    if (lo.kind == ShiftKind::NotTested) {
        if (!m_localFinal) return false;
        slot.k = 0;
        slot.stage = Stage::Base;
        return true;
    }
    // died in lockstep
    double const D = m_m.frame + lo.deathAfterFrames;
    if (lo.extension) { undecide(side, slot, status::Reason::SaDeathInSpan); return true; }
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
        if (m_breakSeen || m_now >= lastRigidFrame() + m_cfg.horizonFrames - kEps) {
            undecide(side, slot, status::Reason::SaUndecided);
            return true;
        }
        return false;   // else wait for the follower to be logged
    }
    // the first schedule moves the followers that acted before the local death, at most
    // startFollowersMax of them (CT-D4): the rest join only when a trial shows they matter
    slot.k = std::min({k0, usable, std::max(1, m_cfg.startFollowersMax)});
    slot.stage = Stage::Base;
    return true;
}

/// `frontier`: the slot is the nearest undecided one of its side. Only the frontier runs the
/// expensive stages (probes, verifications, larger families); a slot beyond it runs its base trial
/// (one simulation, in the same round) and then waits: if the frontier fails the side ends there.
void CompPlanner::advanceSlot(int side, Slot& slot, bool frontier) {
    if (slot.st != St::Open || m_finished) return;
    double const sign = side == 1 ? 1.0 : -1.0;
    double const s = sign * slot.mag;
    if (slot.stage == Stage::None && !decideLocal(side, slot)) return;
    // ---- the compensation stages (each step either requests trials, waits, or decides) ----
    int const guardMax = 8 * (kMaxFollowers + 2);
    for (int guard = 0; guard < guardMax && slot.st == St::Open; ++guard) {
        if (slot.stage == Stage::Wait) {
            // waiting for one more follower to be logged (or for the horizon): the decision is taken again
            if (!frontier) return;
            afterVerifications(side, slot, s);
            if (slot.stage == Stage::Wait) return;
            continue;
        }
        if (slot.stage == Stage::Base) {
            if (slot.baseId < 0) {
                int id = -5;
                // WARM: the nearest passing schedule of this side, continued to this shift
                if (m_cfg.warmStart) {
                    std::vector<double> w;
                    if (warmBase(side, slot, s, w)) {
                        bool const uniform = allEqual(w, s);
                        id = request(side, slot, s, uniform ? Role::Uniform : Role::Warm, -1, w);
                        if (id >= 0) {
                            slot.base = offsetsOf(id);
                            slot.k = static_cast<int>(slot.base.size());
                            slot.baseWarm = !uniform;
                        }
                        else if (id == -3 || id == -4) id = -5;   // not a new legal schedule here: the uniform base
                    }
                }
                if (id == -5) {
                    std::vector<double> u(static_cast<size_t>(slot.k), s);
                    id = request(side, slot, s, Role::Uniform, -1, u);
                    if (id >= 0) {
                        slot.base = offsetsOf(id);
                        slot.baseWarm = false;
                    }
                }
                if (id == -1) { undecide(side, slot, status::Reason::SaNotMeasuredBudget); return; }
                if (id == -2) { undecide(side, slot, m_finishReason); return; }
                if (id == -3) {
                    // no legal uniform schedule: the member (or a follower) would cross a fixed
                    // input. More followers moving may make room; when none can, the shift is out
                    // of reach the way a neighbour limit is (SAPlanner anyLegalMember): never a fail
                    if (slot.k < followersUsable()) { ++slot.k; continue; }
                    slot.st = St::Undecided;
                    slot.neighbourLimit = true;
                    slot.why = status::Reason::SaUndecided;
                    slot.stage = Stage::Done;
                    return;
                }
                if (id == -4) { undecide(side, slot, status::Reason::SaInvalidTrials); return; }
                slot.baseId = id;
                slot.probeIds.assign(slot.base.size(), -1);
                slot.probeSteps.assign(slot.base.size(), 0.0);
                return;   // waiting
            }
            CompOutcome const* u = known(slot.baseId);
            if (!u) return;
            if (isPass(*u)) { passSlot(slot, s, *u, slot.base); return; }
            if (u->kind == CompOutcome::Kind::Invalid) { undecide(side, slot, invalidWhy(u)); return; }
            if (!frontier) return;   // the rest of this slot's search waits for the nearer shifts
            noteOutcome(slot, slot.baseId, *u);
            if (slot.k == 0) {
                // the member alone: the same rule as a lockstep death
                bool const died = u->kind == CompOutcome::Kind::Died;
                if (died && u->extension) { undecide(side, slot, status::Reason::SaDeathInSpan); return; }
                if (died && u->laterFixed <= 0 && !canFollowerAct(s, u->deathFrame)) { failSlot(slot, u); return; }
                int const usable = followersUsable();
                if (usable < 1) {
                    bool const moreMayCome = !m_breakSeen && m_now < lastRigidFrame() + m_cfg.horizonFrames - kEps;
                    if (moreMayCome) return;   // a follower may still be logged
                    undecide(side, slot, slot.survived ? status::Reason::SaSurvivesNoRejoin : status::Reason::SaUndecided);
                    return;
                }
                if (!died) {
                    // alive without a re-join: the first follower joins as recorded, its response is measured
                    grow(side, slot, s);
                    continue;
                }
                slot.k = std::min({std::max(1, u->laterFixed), usable, std::max(1, m_cfg.startFollowersMax)});
                resetModel(slot);
                continue;   // the base trial of k followers
            }
            slot.stage = Stage::Probes;
            continue;
        }
        if (slot.stage == Stage::Probes) {
            if (!frontier) return;
            // LAZY (CT-D5): a follower gets its own probe only when no response of it is known
            // (cached or measured here) - or when the slot measures every follower itself
            // (wantOwnProbes: the cached responses' candidates failed and no larger family exists)
            if (slot.probeIds.size() < static_cast<size_t>(slot.k)) {
                slot.probeIds.resize(static_cast<size_t>(slot.k), -1);
                slot.probeSteps.resize(static_cast<size_t>(slot.k), 0.0);
            }
            Response const model = modelOf(slot);
            // a known response is usable when it reaches (nearly) as far as the base trial got:
            // the offsets are solved at the base's last frame
            double baseEnd = -1e300;
            if (CompOutcome const* u = known(slot.baseId); u && !u->dev.empty()) baseEnd = u->dev.back().frame;
            std::vector<int> need;
            for (int j = 0; j < slot.k; ++j) {
                if (slot.probeIds[static_cast<size_t>(j)] != -1) continue;   // asked already
                auto const& col = model.columns[static_cast<size_t>(j)];
                bool const reaches = !col.empty() && col.back().frame >= baseEnd - static_cast<double>(m_cfg.rejoin.steps) - kEps;
                if (slot.wantOwnProbes || !reaches) need.push_back(j);
            }
            if (!need.empty()) {
                // the same response is being measured by another slot right now: its result is awaited
                if (!slot.wantOwnProbes && m_cfg.reuseResponse && probesInFlight(slot.k, &slot)) return;
                for (int j : need) requestProbe(side, slot, s, j);
            }
            bool all = true;
            for (size_t j = 0; j < slot.probeIds.size(); ++j) {
                int id = slot.probeIds[j];
                if (id < 0) continue;
                CompOutcome const* p = known(id);
                if (!p) { all = false; continue; }
                // a probe that passed is a compensated pass
                if (isPass(*p)) { passSlot(slot, s, *p, offsetsOf(id)); return; }
                noteOutcome(slot, id, *p);
            }
            if (!all) return;   // waiting for the probes
            slot.wantOwnProbes = false;
            // what was measured here is the cached response of this family from now on
            if (m_cfg.reuseResponse) {
                Response merged = modelOf(slot);
                bool any = false;
                for (auto const& c : merged.columns) any = any || !c.empty();
                if (any) m_response[slot.k] = std::move(merged);
            }
            slot.stage = Stage::Verify;
            continue;
        }
        if (slot.stage == Stage::Verify) {
            if (!frontier) return;
            if (!slot.modelBuilt) {
                slot.modelBuilt = true;
                Response const model = modelOf(slot);
                auto cands = candidates(slot, s, &model);
                if (static_cast<int>(cands.size()) > m_cfg.verifyRoundings) cands.resize(static_cast<size_t>(std::max(0, m_cfg.verifyRoundings)));
                slot.pendingCands = std::move(cands);
                slot.verifyIds.clear();
                afterVerifications(side, slot, s);   // requests the first candidate (or decides)
                if (slot.st != St::Open || slot.stage == Stage::Wait) return;
                if (slot.stage == Stage::Verify && !slot.verifyIds.empty()) return;   // waiting
                continue;
            }
            bool all = true;
            for (int id : slot.verifyIds) {
                CompOutcome const* v = known(id);
                if (!v) { all = false; continue; }
                if (isPass(*v)) { passSlot(slot, s, *v, offsetsOf(id)); return; }
                if (v->kind == CompOutcome::Kind::Invalid) { undecide(side, slot, invalidWhy(v)); return; }
                noteOutcome(slot, id, *v);
            }
            if (!all) return;
            for (int id : slot.verifyIds) slot.allVerifyIds.push_back(id);
            slot.verifyIds.clear();
            afterVerifications(side, slot, s);
            if (slot.st != St::Open || slot.stage == Stage::Wait) return;
            if (slot.stage == Stage::Verify && !slot.verifyIds.empty()) return;   // waiting for the next candidate
            continue;
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
            if (slot.mag > sd.walkLimit + kEps) break;
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
            advanceSlot(side, slot, inFlight == 0);
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
            if (s.mag > sd.walkLimit + kEps) break;
            if (s.mag <= sd.localRun + kEps) continue;
            if (!std::isnan(sd.localFail) && s.mag < sd.localFail - kEps) continue;
            if (s.st == St::Open) return false;
            if (s.st == St::Fail || s.st == St::Undecided) break;
        }
    }
    return true;
}

int CompPlanner::followersUsed() const {
    int n = 0;
    for (int side = 0; side < 2; ++side) {
        for (auto const& s : m_side[side].slots) {
            if (s.st == St::Pass) n = std::max(n, static_cast<int>(s.offsets.size()));
        }
    }
    return n;
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
    Slot const* lastPassSlot = nullptr;
    bool ended = false;
    Slot const* failSlotP = nullptr;
    // the late side: the member may not cross a FIXED next input (one that cannot follow)
    double fixedLimit = kNaN;
    if (side == 1 && !m_followers.empty() && m_followers.front().breakBefore) fixedLimit = m_followers.front().frame - m_cfg.neighbourMarginFrames - lastRigidFrame();
    EdgeStop limitStop = sd.walkKind == LimitKind::Range ? EdgeStop::Range : stopOf(sd.walkKind);
    bool neighbourCut = false, rangeCut = false;
    for (auto const& s : sd.slots) {
        if (s.mag > sd.walkLimit + kEps) break;
        if (s.mag <= sd.localRun + kEps) continue;
        if (!std::isnan(sd.localFail) && s.mag < sd.localFail - kEps) continue;
        if (!std::isnan(fixedLimit) && s.mag > fixedLimit + kEps) { neighbourCut = true; break; }
        if (s.st == St::Pass) {
            lastPass = s.mag;
            if (s.proof > weakest) weakest = s.proof;
            if (s.proof == SAProof::Survived) provenOpen = false;
            else if (provenOpen) proven = s.mag;
            lastPassSlot = &s;
            continue;
        }
        if (s.st == St::Undecided && s.neighbourLimit) { neighbourCut = true; break; }
        if (openCut(side, s)) { rangeCut = true; break; }   // CT-D6: open to its last pass
        ended = true;
        if (s.st == St::Fail) failSlotP = &s;
        break;
    }
    e.passFrames = sign * lastPass;
    e.proof = weakest;
    e.provenPassFrames = sign * std::min(proven, lastPass);
    if (lastPassSlot) {
        e.followerOffsetsFrames = lastPassSlot->offsets;
        if (lastPassSlot->used != SAAdaptation::Local || rejoin::rejoined(lastPassSlot->rejoin)) {
            e.rejoin = lastPassSlot->rejoin;
            if (!std::isnan(lastPassSlot->rejoinFrame)) e.rejoinAfterFrames = lastPassSlot->rejoinFrame - m_m.frame;
            e.rejoinErrY = lastPassSlot->rejoinErrY;
            e.rejoinErrVy = lastPassSlot->rejoinErrVy;
        }
    }
    if (!ended) {
        // CT-D6: a side the walk could not decide beyond what the frozen search calls open ends there
        e.stop = neighbourCut ? EdgeStop::Neighbour : rangeCut ? (std::isnan(sd.localFail) ? sd.localOpenStop : EdgeStop::Range) : limitStop;
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
    r.isolated = m_isolated || (m_localFinal && followersUsable() == 0 && (m_breakSeen || m_now >= lastRigidFrame() + m_cfg.horizonFrames - kEps));
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
        e.rejoin = rejoin::Kind::None;
        e.rejoinAfterFrames = kNaN;
        e.rejoinErrY = kNaN;
        e.rejoinErrVy = kNaN;
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
    bool used[kSAAdaptationCount] = {};
    bool budget = false, invalid = false, finishReason = false, noRejoin = false;
    for (int s = 0; s < 2; ++s) {
        Side const& sd = m_side[s];
        // the walk's own view (F3): the slots inside the local run and the local planner's gap are
        // not the planner's, and nothing beyond the slot that ended the side ever mattered
        for (auto const& slot : sd.slots) {
            if (slot.mag > sd.walkLimit + kEps) break;
            if (slot.mag <= sd.localRun + kEps) continue;
            if (!std::isnan(sd.localFail) && slot.mag < sd.localFail - kEps) continue;
            if (slot.st == St::Pass) {
                if (slot.used != SAAdaptation::Local) used[static_cast<int>(slot.used)] = true;
                continue;
            }
            if (slot.st == St::Undecided && !slot.neighbourLimit && !openCut(s, slot)) {
                ++r.undecidedShifts;
                if (slot.why == status::Reason::SaNotMeasuredBudget) budget = true;
                else if (slot.why == status::Reason::SaInvalidTrials) invalid = true;
                else if (slot.why == status::Reason::SaSurvivesNoRejoin) {
                    noRejoin = true;
                    r.survivedNoRejoin[s] = true;
                }
                else if (slot.why != status::Reason::SaUndecided) finishReason = true;
            }
            break;   // Fail / Undecided / neighbour: the side ends here
        }
    }
    for (int k = 1; k < kSAAdaptationCount; ++k) if (used[k]) r.adaptationUsed.push_back(static_cast<SAAdaptation>(k));
    r.followersUsed = followersUsed();
    if (finishReason) r.failure = m_finishReason;
    else if (budget) r.failure = status::Reason::SaNotMeasuredBudget;
    else if (invalid) r.failure = status::Reason::SaInvalidTrials;
    else if (noRejoin) r.failure = status::Reason::SaSurvivesNoRejoin;
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
            if (s.st == St::Pass && s.used == SAAdaptation::Local && !rejoin::rejoined(s.rejoin)) { ++localPasses; continue; }
            if (s.st == St::Open) continue;
            out += " " + fmtNum(sign * s.mag);
            switch (s.st) {
                case St::Pass:
                    out += std::string(" ") + name(s.used) + fmtOffsets(s.offsets) + " " + name(s.proof);
                    if (s.rejoin == rejoin::Kind::Approx || s.rejoin == rejoin::Kind::Parallel) {
                        char buf[64];
                        std::snprintf(buf, sizeof buf, "%s(dy %.2f dvy %.3f)", s.rejoin == rejoin::Kind::Approx ? "~" : "=", s.rejoinErrY, s.rejoinErrVy);
                        out += buf;
                    }
                    else if (s.rejoin == rejoin::Kind::LevelEnd) out += "(level end)";
                    break;
                case St::Fail: {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " fail@%.1f", s.deathFrame - m_m.frame);
                    out += buf;
                    break;
                }
                case St::Undecided:
                    out += s.neighbourLimit ? std::string(" neighbour") : std::string(openCut(side, s) ? " open(" : " undecided(") + status::name(s.why) + ")";
                    break;
                default: break;
            }
        }
        if (localPasses) out += " [" + std::to_string(localPasses) + " local]";
        SAEdge e = edgeOf(side);
        out += std::string(" -> ") + name(e.stop);
    }
    out += " | followers " + std::to_string(followersUsable()) + (m_capSeen ? "+" : "") + (m_isolated ? " (isolated)" : "") + " | trials " + std::to_string(m_requested);
    return out;
}

}  // namespace gprl::solver::comp
