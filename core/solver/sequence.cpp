#include "sequence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gprl::solver {

namespace {

constexpr double kEps = 1e-9;

// The emitted edge of one side: the midpoint of a bounded side's bracket, else the conservative
// pass edge. Same rule as core/solver/window_event.cpp (D8); tests/sequence_tests.cpp checks the
// width against buildWindowEvent bit for bit.
double midpointEdge(BoundaryResult const& side) {
    if (side.bounded && !std::isnan(side.failShiftMs)) return 0.5 * (side.passShiftMs + side.failShiftMs);
    return side.passShiftMs;
}

std::string fmt(char const* f, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}

// The death next to the window on one side: the first fail beyond the pass edge, when the trial
// at exactly that shift died (an invalid or untested point is no negative control).
bool failOf(BoundaryResult const& side, double& shiftMs, int& objectId) {
    if (!side.bounded || std::isnan(side.failShiftMs)) return false;
    for (auto const& t : side.trials) {
        if (std::fabs(t.shiftMs - side.failShiftMs) > 1e-6) continue;
        if (t.pass || t.outcome.kind != OutcomeKind::Died) continue;
        shiftMs = t.shiftMs;
        objectId = t.outcome.objectId;
        return true;
    }
    return false;
}

int sideAtoms(double length, double target) {
    if (length <= kEps || target <= 0.0) return 0;
    return std::max(0, static_cast<int>(std::floor(length / target + 1e-9)));
}

}  // namespace

// ---- local window -> axis ----

LocalWindowInfo localWindowInfo(WindowResult const& w) {
    LocalWindowInfo i;
    if (!w.valid) {
        i.invalidReason = w.invalidReason.empty() ? "local window invalid" : w.invalidReason;
        return i;
    }
    i.earlyMs = midpointEdge(w.early);
    i.lateMs = midpointEdge(w.late);
    if (!std::isfinite(i.earlyMs) || !std::isfinite(i.lateMs) || !std::isfinite(w.actualMs)) {
        i.invalidReason = "local window edge not finite";
        return i;
    }
    // exactly the doubles the timing_window event subtracts (window_event.cpp: latest - earliest)
    double earliest = w.actualMs + i.earlyMs;
    double latest = w.actualMs + i.lateMs;
    i.widthMs = latest - earliest;
    if (!(i.widthMs >= 0.0)) {
        i.invalidReason = "local window inverted";
        return i;
    }
    i.hit = i.earlyMs <= 1e-9 && i.lateMs >= -1e-9;
    double res = 0.0;
    if (w.early.bounded) res = std::max(res, w.early.bracketMs);
    if (w.late.bounded) res = std::max(res, w.late.bracketMs);
    i.resolutionMs = res > 0.0 ? res : kTickMs;
    double lo = w.early.passShiftMs - 1e-6;
    double hi = w.late.passShiftMs + 1e-6;
    for (auto const* side : {&w.early, &w.late}) {
        for (auto const& t : side->trials) {
            if (!t.pass || std::fabs(t.shiftMs) <= 1e-9) continue;
            if (t.shiftMs < lo || t.shiftMs > hi) continue;   // an island beyond the first fail is not part of the window
            i.passShiftsMs.push_back(t.shiftMs);
        }
    }
    std::sort(i.passShiftsMs.begin(), i.passShiftsMs.end());
    i.passShiftsMs.erase(std::unique(i.passShiftsMs.begin(), i.passShiftsMs.end(), [](double a, double b) { return std::fabs(a - b) < 1e-6; }),
                         i.passShiftsMs.end());
    // negative control: the nearest known death (ties: the late side)
    double lateShift = kNaN, earlyShift = kNaN;
    int lateObj = -1, earlyObj = -1;
    bool haveLate = failOf(w.late, lateShift, lateObj);
    bool haveEarly = failOf(w.early, earlyShift, earlyObj);
    if (haveLate && (!haveEarly || std::fabs(lateShift) <= std::fabs(earlyShift))) {
        i.hasFail = true;
        i.failShiftMs = lateShift;
        i.failObjectId = lateObj;
    }
    else if (haveEarly) {
        i.hasFail = true;
        i.failShiftMs = earlyShift;
        i.failObjectId = earlyObj;
    }
    i.valid = true;
    return i;
}

SequenceAxis makeAxis(LocalWindowInfo const& info, bool continuous, int maxAtoms) {
    SequenceAxis a;
    a.continuous = continuous;
    if (!info.valid) {
        a.invalidReason = info.invalidReason.empty() ? "no local window" : info.invalidReason;
        return a;
    }
    if (!info.hit) {
        a.invalidReason = "the input missed its local window";
        return a;
    }
    a.earlyFrames = std::min(0.0, info.earlyMs / kTickMs);
    a.lateFrames = std::max(0.0, info.lateMs / kTickMs);
    a.localWidthMs = info.widthMs;
    double const width = a.lateFrames - a.earlyFrames;
    if (!(width > 1e-9) || !(info.widthMs > 0.0)) {
        a.invalidReason = "local window has no width";
        return a;
    }
    if (!continuous) {
        a.positions.push_back(0.0);
        for (double ms : info.passShiftsMs) {
            double s = ms / kTickMs;
            if (s < a.earlyFrames - 1e-6 || s > a.lateFrames + 1e-6) continue;
            a.positions.push_back(s);
        }
        std::sort(a.positions.begin(), a.positions.end());
        a.positions.erase(std::unique(a.positions.begin(), a.positions.end(), [](double x, double y) { return std::fabs(x - y) < 1e-6; }), a.positions.end());
    }
    else {
        int const cap = std::max(1, maxAtoms);
        double target = std::max(std::max(info.resolutionMs / kTickMs, 1e-6), width / static_cast<double>(cap));
        int ne = 0, nl = 0;
        for (int guard = 0; guard < 64; ++guard) {
            ne = sideAtoms(-a.earlyFrames, target);
            nl = sideAtoms(a.lateFrames, target);
            if (ne + nl + 1 <= cap) break;
            target *= 1.1;
        }
        // |edge| = (n + 1/2) * step on each side, so the atoms' cells tile the window exactly
        double const de = ne > 0 ? -a.earlyFrames / (static_cast<double>(ne) + 0.5) : 0.0;
        double const dl = nl > 0 ? a.lateFrames / (static_cast<double>(nl) + 0.5) : 0.0;
        for (int k = ne; k >= 1; --k) a.positions.push_back(-static_cast<double>(k) * de);
        a.positions.push_back(0.0);
        for (int k = 1; k <= nl; ++k) a.positions.push_back(static_cast<double>(k) * dl);
    }
    int const n = a.size();
    a.weights.resize(static_cast<size_t>(n));
    a.origin = 0;
    for (int j = 0; j < n; ++j) {
        double left = j == 0 ? a.earlyFrames : 0.5 * (a.positions[j - 1] + a.positions[j]);
        double right = j == n - 1 ? a.lateFrames : 0.5 * (a.positions[j] + a.positions[j + 1]);
        a.weights[j] = std::max(0.0, right - left);
        if (std::fabs(a.positions[j]) < 1e-9) a.origin = j;
    }
    a.valid = true;
    return a;
}

std::vector<int> sequenceNodes(int atoms, int origin, int nodes) {
    std::vector<int> out;
    if (atoms <= 0) return out;
    origin = std::clamp(origin, 0, atoms - 1);
    out = {0, origin, atoms - 1};
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    int const want = std::min(std::max(nodes, 2), atoms);
    while (static_cast<int>(out.size()) < want) {
        int best = -1, bestGap = 1;
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            int gap = out[i + 1] - out[i];
            if (gap > bestGap) {
                bestGap = gap;
                best = static_cast<int>(i);
            }
        }
        if (best < 0) break;   // every gap is one atom: nothing left to insert
        out.insert(out.begin() + best + 1, out[static_cast<size_t>(best)] + bestGap / 2);
    }
    return out;
}

// ---- planner ----

SequencePlanner::SequencePlanner(SequenceConfig const& cfg, std::vector<SequenceAxis> axes, std::vector<double> timesFrames)
    : m_cfg(cfg), m_axes(std::move(axes)), m_times(std::move(timesFrames)) {
    m_n = static_cast<int>(m_axes.size());
    if (m_n < kSequenceMinInputs || m_n > kSequenceMaxInputs || m_n > m_cfg.maxGroupSize) {
        m_invalidReason = "a sequence is " + std::to_string(kSequenceMinInputs) + " to " + std::to_string(std::min(kSequenceMaxInputs, m_cfg.maxGroupSize)) + " inputs";
        return;
    }
    if (static_cast<int>(m_times.size()) != m_n) {
        m_invalidReason = "one input time per axis is required";
        return;
    }
    for (int i = 0; i < m_n; ++i) {
        if (!m_axes[i].valid || m_axes[i].size() < 1) {
            m_invalidReason = "input " + std::to_string(i + 1) + ": " + (m_axes[i].invalidReason.empty() ? std::string("no axis") : m_axes[i].invalidReason);
            return;
        }
        if (!std::isfinite(m_times[i])) {
            m_invalidReason = "input time not finite";
            return;
        }
        if (i > 0 && m_times[i] - m_times[i - 1] < m_cfg.neighbourMarginFrames) {
            m_invalidReason = "inputs " + std::to_string(i) + " and " + std::to_string(i + 1) + " are at the same instant";
            return;
        }
        m_dim[i] = m_axes[i].size();
    }
    m_total = m_dim[0] * m_dim[1] * m_dim[2];
    m_kind.assign(static_cast<size_t>(m_total), Kind::Unknown);
    m_asked.assign(static_cast<size_t>(m_total), 0);
    for (int id = 0; id < m_total; ++id) {
        SequencePoint p = unflat(id);
        if (crossedPoint(p)) m_kind[id] = Kind::Crossed;
        else if (onAxis(p)) m_kind[id] = Kind::KnownPass;
    }
    // coarse boxes between neighbouring nodes
    std::array<std::vector<int>, kSequenceMaxInputs> nodes;
    int const perAxis = m_n == 2 ? m_cfg.nodesPerAxisPair : m_cfg.nodesPerAxisTriple;
    for (int i = 0; i < kSequenceMaxInputs; ++i) nodes[i] = i < m_n ? sequenceNodes(m_dim[i], m_axes[i].origin, perAxis) : std::vector<int>{0};
    auto intervals = [&](int i) {
        std::vector<std::pair<int, int>> v;
        if (nodes[i].size() < 2) v.emplace_back(nodes[i].front(), nodes[i].front());
        else for (size_t k = 0; k + 1 < nodes[i].size(); ++k) v.emplace_back(nodes[i][k], nodes[i][k + 1]);
        return v;
    };
    auto i0 = intervals(0), i1 = intervals(1), i2 = intervals(2);
    for (auto const& c : i2) {
        for (auto const& b : i1) {
            for (auto const& a : i0) {
                Box box;
                box.lo = {a.first, b.first, c.first};
                box.hi = {a.second, b.second, c.second};
                m_leaves.push_back(box);
            }
        }
    }
    // the coarse grid's unknown corners, the ones furthest from the performed timing first (a job
    // cut short still has the extremes)
    std::vector<int> need, cs;
    for (auto const& box : m_leaves) {
        corners(box, cs);
        for (int id : cs) {
            if (m_kind[id] == Kind::Unknown && std::find(need.begin(), need.end(), id) == need.end()) need.push_back(id);
        }
    }
    auto distance = [&](int id) {
        SequencePoint p = unflat(id);
        double d = 0.0;
        for (int i = 0; i < m_n; ++i) {
            int o = m_axes[i].origin;
            int reach = std::max(1, std::max(o, m_dim[i] - 1 - o));
            double x = static_cast<double>(p[i] - o) / static_cast<double>(reach);
            d += x * x;
        }
        return d;
    };
    std::stable_sort(need.begin(), need.end(), [&](int a, int b) {
        double da = distance(a), db = distance(b);
        if (std::fabs(da - db) > 1e-12) return da > db;
        return a < b;
    });
    m_valid = true;
    for (int id : need) {
        if (m_requested >= sampleCap()) {
            m_budget = true;
            break;
        }
        request(id);
    }
}

int SequencePlanner::sampleCap() const { return m_n == 2 ? m_cfg.maxSamplesPair : m_cfg.maxSamplesTriple; }

int SequencePlanner::flat(SequencePoint const& p) const { return p[0] + m_dim[0] * (p[1] + m_dim[1] * p[2]); }

SequencePoint SequencePlanner::unflat(int id) const {
    SequencePoint p{};
    p[0] = id % m_dim[0];
    id /= m_dim[0];
    p[1] = id % m_dim[1];
    p[2] = id / m_dim[1];
    return p;
}

std::array<double, kSequenceMaxInputs> SequencePlanner::shiftsOf(int id) const {
    std::array<double, kSequenceMaxInputs> s{};
    if (!m_valid || id < 0 || id >= m_total) return s;
    SequencePoint p = unflat(id);
    for (int i = 0; i < m_n; ++i) s[i] = m_axes[i].positions[static_cast<size_t>(p[i])];
    return s;
}

bool SequencePlanner::crossedPoint(SequencePoint const& p) const {
    for (int i = 0; i + 1 < m_n; ++i) {
        double a = m_times[i] + m_axes[i].positions[static_cast<size_t>(p[i])];
        double b = m_times[i + 1] + m_axes[i + 1].positions[static_cast<size_t>(p[i + 1])];
        if (b - a < m_cfg.neighbourMarginFrames - kEps) return true;
    }
    return false;
}

bool SequencePlanner::onAxis(SequencePoint const& p) const {
    int moved = 0;
    for (int i = 0; i < m_n; ++i) {
        if (p[i] != m_axes[i].origin) ++moved;
    }
    return moved <= 1;
}

void SequencePlanner::corners(Box const& b, std::vector<int>& out) const {
    out.clear();
    for (int mask = 0; mask < (1 << m_n); ++mask) {
        SequencePoint p{};
        bool skip = false;
        for (int i = 0; i < m_n; ++i) {
            bool high = (mask >> i) & 1;
            if (high && b.lo[i] == b.hi[i]) {
                skip = true;   // a flat axis has one corner, not two
                break;
            }
            p[i] = high ? b.hi[i] : b.lo[i];
        }
        if (!skip) out.push_back(flat(p));
    }
}

bool SequencePlanner::mixed(Box const& b) const {
    std::vector<int> cs;
    corners(b, cs);
    bool pass = false, fail = false;
    for (int id : cs) {
        if (passKind(m_kind[id])) pass = true;
        else if (failKind(m_kind[id])) fail = true;
    }
    return pass && fail;
}

bool SequencePlanner::splittable(Box const& b) const {
    for (int i = 0; i < m_n; ++i) {
        if (b.hi[i] - b.lo[i] >= 2) return true;
    }
    return false;
}

// Corner kinds of a box by mask (bit i = the high end of axis i; a flat axis repeats its one end).
// 1 = pass, 0 = fail, -1 = not known.
static int cornerValue(bool pass, bool fail) { return pass ? 1 : (fail ? 0 : -1); }

int SequencePlanner::splitAxis(Box const& b) const {
    int best = -1, bestDisagree = -1, bestSpan = 0;
    for (int i = 0; i < m_n; ++i) {
        int span = b.hi[i] - b.lo[i];
        if (span < 2) continue;
        // corner pairs that differ only along axis i and have different outcomes
        int disagree = 0;
        for (int mask = 0; mask < (1 << m_n); ++mask) {
            if ((mask >> i) & 1) continue;
            SequencePoint lo{}, hi{};
            bool skip = false;
            for (int k = 0; k < m_n; ++k) {
                bool high = (mask >> k) & 1;
                if (high && b.lo[k] == b.hi[k]) {
                    skip = true;
                    break;
                }
                lo[k] = hi[k] = high ? b.hi[k] : b.lo[k];
            }
            if (skip) continue;
            lo[i] = b.lo[i];
            hi[i] = b.hi[i];
            Kind a = m_kind[flat(lo)], c = m_kind[flat(hi)];
            int va = cornerValue(passKind(a), failKind(a)), vc = cornerValue(passKind(c), failKind(c));
            if (va >= 0 && vc >= 0 && va != vc) ++disagree;
        }
        if (disagree > bestDisagree || (disagree == bestDisagree && span > bestSpan)) {
            best = i;
            bestDisagree = disagree;
            bestSpan = span;
        }
    }
    return best;
}

void SequencePlanner::split(Box const& b, int axis, Box& low, Box& high) const {
    low = b;
    high = b;
    int mid = b.lo[axis] + (b.hi[axis] - b.lo[axis]) / 2;
    low.hi[axis] = mid;
    high.lo[axis] = mid;
    low.level = high.level = b.level + 1;
    // what result() falls back to when the job is cut before this cut's new corners are decided
    low.child = high.child = true;
    low.parentLo = high.parentLo = b.lo;
    low.parentHi = high.parentHi = b.hi;
}

double SequencePlanner::boxMeasure(Box const& b) const {
    double m = 1.0;
    for (int i = 0; i < m_n; ++i) {
        double w = 0.0;
        for (int a = b.lo[i]; a <= b.hi[i]; ++a) w += m_axes[i].weights[static_cast<size_t>(a)];
        m *= w;
    }
    return m;
}

void SequencePlanner::request(int id) {
    if (id < 0 || id >= m_total || m_kind[id] != Kind::Unknown || m_asked[id]) return;
    m_asked[id] = 1;
    m_queue.push_back(id);
    ++m_requested;
}

// One round: every box whose corners disagree is cut in two along splitAxis(), the largest
// measure first (that is where a wrong guess costs most), as far as the sample cap goes.
void SequencePlanner::planRefinement() {
    std::vector<int> cs;
    for (;;) {
        if (m_done) return;
        std::vector<std::pair<double, size_t>> open;
        for (size_t i = 0; i < m_leaves.size(); ++i) {
            if (mixed(m_leaves[i]) && splittable(m_leaves[i])) open.emplace_back(boxMeasure(m_leaves[i]), i);
        }
        if (open.empty()) {
            m_done = true;
            return;
        }
        if (m_rounds >= m_cfg.maxRefineRounds) {
            m_budget = true;
            m_done = true;
            return;
        }
        // stable: equal measures keep the leaf order, so the plan is deterministic
        std::stable_sort(open.begin(), open.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        std::vector<char> removed(m_leaves.size(), 0);
        std::vector<Box> added;
        bool any = false;
        for (auto const& entry : open) {
            Box const box = m_leaves[entry.second];
            int const axis = splitAxis(box);
            if (axis < 0) continue;
            Box low, high;
            split(box, axis, low, high);
            std::vector<int> need;
            for (Box const* kid : {&low, &high}) {
                corners(*kid, cs);
                for (int id : cs) {
                    if (m_kind[id] == Kind::Unknown && !m_asked[id] && std::find(need.begin(), need.end(), id) == need.end()) need.push_back(id);
                }
            }
            if (m_requested + static_cast<int>(need.size()) > sampleCap()) {
                m_budget = true;   // the cap does not cover this cut: the box stays as it is
                continue;
            }
            for (int id : need) request(id);
            removed[entry.second] = 1;
            added.push_back(low);
            added.push_back(high);
            any = true;
        }
        std::vector<Box> next;
        next.reserve(m_leaves.size() + added.size());
        for (size_t i = 0; i < m_leaves.size(); ++i) {
            if (!removed[i]) next.push_back(m_leaves[i]);
        }
        next.insert(next.end(), added.begin(), added.end());
        m_leaves = std::move(next);
        if (!any) {
            m_done = true;   // every open box is beyond the cap
            return;
        }
        ++m_rounds;
        if (m_queueHead < m_queue.size()) return;   // new points to simulate first
        // every new corner was already known (order / local): look at the children right away
    }
}

std::vector<SequenceTrial> SequencePlanner::nextBatch(int maxTrials) {
    std::vector<SequenceTrial> out;
    if (!m_valid || m_finished || maxTrials <= 0) return out;
    if (m_queueHead >= m_queue.size() && m_outstanding == 0 && !m_done) planRefinement();
    while (static_cast<int>(out.size()) < maxTrials && m_queueHead < m_queue.size()) {
        int id = m_queue[m_queueHead++];
        SequenceTrial t;
        t.id = id;
        t.atom = unflat(id);
        t.shiftFrames = shiftsOf(id);
        out.push_back(t);
        ++m_outstanding;
    }
    return out;
}

void SequencePlanner::ingest(SequenceOutcome const& o) {
    if (!m_valid || o.id < 0 || o.id >= m_total || !m_asked[o.id] || m_kind[o.id] != Kind::Unknown) return;
    m_kind[o.id] = o.invalid ? Kind::Invalid : (o.pass ? Kind::Pass : Kind::Fail);
    if (m_outstanding > 0) --m_outstanding;
}

void SequencePlanner::finish() { m_finished = true; }

bool SequencePlanner::done() const { return !m_valid || m_finished || (m_done && m_queueHead >= m_queue.size()); }

SequenceResult SequencePlanner::result() const {
    SequenceResult r;
    r.solverVersion = m_cfg.version;
    r.inputs = m_n;
    if (!m_valid) {
        r.invalidReason = m_invalidReason.empty() ? "invalid sequence" : m_invalidReason;
        return r;
    }
    r.latticePoints = m_total;
    r.productMeasure = 1.0;
    for (int i = 0; i < m_n; ++i) {
        r.localWidthsMs.push_back(m_axes[i].localWidthMs);
        r.productMeasure *= m_axes[i].localWidthMs;
    }
    for (int id = 0; id < m_total; ++id) {
        switch (m_kind[id]) {
            case Kind::Pass: ++r.samples; ++r.passed; break;
            case Kind::Fail: ++r.samples; ++r.failed; break;
            case Kind::Invalid: ++r.invalidTrials; break;
            case Kind::KnownPass: ++r.knownLocal; break;
            case Kind::Crossed: ++r.crossed; break;
            case Kind::Unknown: break;
        }
    }
    r.trivial = m_requested == 0;
    r.refineRounds = m_rounds;
    r.budgetExhausted = m_budget;
    r.boxes = static_cast<int>(m_leaves.size());

    // Every atom gets a vote from each box it lies in: the box's outcome when its corners agree;
    // in a box whose corners disagree, the side of 0.5 that the multilinear interpolation of the
    // corner outcomes falls on at the atom's position (a straight boundary between the corners;
    // the plain pass fraction of the corners would give a box with one passing corner a quarter
    // of its measure, twice what a straight cut through it leaves on average). A box with a corner
    // that could not be simulated (an invalid trial) falls back to the pass fraction of the corners
    // it knows.
    //
    // A corner that was never DECIDED (the job was cut short by finish(), or the sample cap is
    // below the coarse grid) is different: nothing was measured there. A box from the last
    // refinement round is evaluated on the box it was cut from (all of its corners are decided:
    // the cut is undone, and that box's size is what `resolutionMs` reports). A COARSE box has
    // nothing to fall back to: the grid itself is incomplete and the result is not valid - the
    // corners that are known (the local axes pass by definition) would stand in for the whole
    // box and pull the share towards 1.
    std::vector<double> sum(static_cast<size_t>(m_total), 0.0);
    std::vector<int> cnt(static_cast<size_t>(m_total), 0);
    std::vector<int> cs;
    double mixedSpan = 0.0, anySpan = 0.0;
    bool open = false;
    int undecidedGrid = 0;
    int const cornerCount = 1 << m_n;
    auto undecidedCorner = [&](Box const& b) {
        corners(b, cs);
        for (int id : cs) {
            if (m_kind[id] == Kind::Unknown) return true;
        }
        return false;
    };
    for (auto const& leaf : m_leaves) {
        // `box`: the corners the leaf is evaluated on (its own, or its parent's when it was cut
        // short); the atoms that receive the vote are always the leaf's own
        Box box = leaf;
        if (undecidedCorner(leaf)) {
            if (leaf.child) {
                box.lo = leaf.parentLo;
                box.hi = leaf.parentHi;
            }
            if (!leaf.child || undecidedCorner(box)) {
                ++undecidedGrid;
                continue;
            }
        }
        corners(box, cs);
        int pass = 0, known = 0;
        for (int id : cs) {
            if (passKind(m_kind[id])) { ++pass; ++known; }
            else if (failKind(m_kind[id])) ++known;
        }
        bool const isMixed = pass > 0 && pass < known;
        double span = 0.0;
        for (int i = 0; i < m_n; ++i) {
            span = std::max(span, m_axes[i].positions[static_cast<size_t>(box.hi[i])] - m_axes[i].positions[static_cast<size_t>(box.lo[i])]);
        }
        anySpan = std::max(anySpan, span);
        if (isMixed) {
            ++r.mixedBoxes;
            mixedSpan = std::max(mixedSpan, span);
            if (splittable(box)) open = true;
        }
        if (known == 0) continue;
        double const fraction = static_cast<double>(pass) / static_cast<double>(known);
        bool const interpolate = isMixed && known == static_cast<int>(cs.size());
        // corner outcomes by mask (a flat axis repeats its single end)
        std::array<double, 1 << kSequenceMaxInputs> value{};
        if (interpolate) {
            for (int mask = 0; mask < cornerCount; ++mask) {
                SequencePoint c{};
                for (int i = 0; i < m_n; ++i) c[i] = ((mask >> i) & 1) ? box.hi[i] : box.lo[i];
                value[static_cast<size_t>(mask)] = passKind(m_kind[flat(c)]) ? 1.0 : 0.0;
            }
        }
        SequencePoint p{};
        for (p[2] = leaf.lo[2]; p[2] <= leaf.hi[2]; ++p[2]) {
            for (p[1] = leaf.lo[1]; p[1] <= leaf.hi[1]; ++p[1]) {
                for (p[0] = leaf.lo[0]; p[0] <= leaf.hi[0]; ++p[0]) {
                    double v = fraction;
                    if (interpolate) {
                        std::array<double, kSequenceMaxInputs> u{};
                        for (int i = 0; i < m_n; ++i) {
                            double a = m_axes[i].positions[static_cast<size_t>(box.lo[i])];
                            double b = m_axes[i].positions[static_cast<size_t>(box.hi[i])];
                            u[static_cast<size_t>(i)] = b > a ? (m_axes[i].positions[static_cast<size_t>(p[i])] - a) / (b - a) : 0.0;
                        }
                        double f = 0.0;
                        for (int mask = 0; mask < cornerCount; ++mask) {
                            double w = 1.0;
                            for (int i = 0; i < m_n; ++i) w *= ((mask >> i) & 1) ? u[static_cast<size_t>(i)] : 1.0 - u[static_cast<size_t>(i)];
                            f += w * value[static_cast<size_t>(mask)];
                        }
                        v = f > 0.5 + 1e-12 ? 1.0 : (f < 0.5 - 1e-12 ? 0.0 : 0.5);
                    }
                    int id = flat(p);
                    sum[id] += v;
                    ++cnt[id];
                }
            }
        }
    }
    // ... unless it was measured (or is known) itself
    double num = 0.0, den = 0.0;
    for (int id = 0; id < m_total; ++id) {
        SequencePoint p = unflat(id);
        double w = 1.0;
        for (int i = 0; i < m_n; ++i) w *= m_axes[i].weights[static_cast<size_t>(p[i])];
        double f;
        if (passKind(m_kind[id])) f = 1.0;
        else if (failKind(m_kind[id])) f = 0.0;
        else if (cnt[id] > 0) f = sum[id] / static_cast<double>(cnt[id]);
        else continue;   // nothing known around it: left out of both sums
        num += w * f;
        den += w;
    }
    r.jointFeasibleShare = den > 0.0 ? std::clamp(num / den, 0.0, 1.0) : 0.0;
    r.jointMeasure = r.jointFeasibleShare * r.productMeasure;
    double span = r.mixedBoxes > 0 ? mixedSpan : anySpan;
    if (!(span > 0.0)) {
        for (int i = 0; i < m_n; ++i) span = std::max(span, m_axes[i].widthFrames());
    }
    r.resolutionMs = span * kTickMs;
    r.complete = !open && undecidedGrid == 0 && m_outstanding == 0 && m_queueHead >= m_queue.size();
    r.debug.push_back(describe());
    int const ran = r.samples + r.invalidTrials;
    if (r.trivial) r.invalidReason = "nothing to simulate: every combination is a local trial or swaps the input order";
    else if (undecidedGrid > 0)
        r.invalidReason = "cut short before the coarse grid was complete (" + std::to_string(r.samples) + " combinations simulated, " + std::to_string(undecidedGrid) + " of "
                          + std::to_string(r.boxes) + " boxes with a corner that was never decided)";
    else if (r.samples < std::max(1, m_cfg.minSamples)) r.invalidReason = "too few simulated combinations (" + std::to_string(r.samples) + ")";
    else if (ran > 0 && static_cast<double>(r.invalidTrials) > m_cfg.maxInvalidShare * static_cast<double>(ran))
        r.invalidReason = std::to_string(r.invalidTrials) + " of " + std::to_string(ran) + " combinations could not be simulated";
    else if (!(r.resolutionMs > 0.0) || !std::isfinite(r.resolutionMs)) r.invalidReason = "no resolution";
    else r.valid = true;
    return r;
}

std::string SequencePlanner::describe() const {
    if (!m_valid) return "invalid: " + m_invalidReason;
    int sim = 0, pass = 0, fail = 0, invalid = 0, local = 0, crossed = 0, mixedBoxes = 0;
    for (int id = 0; id < m_total; ++id) {
        switch (m_kind[id]) {
            case Kind::Pass: ++sim; ++pass; break;
            case Kind::Fail: ++sim; ++fail; break;
            case Kind::Invalid: ++invalid; break;
            case Kind::KnownPass: ++local; break;
            case Kind::Crossed: ++crossed; break;
            case Kind::Unknown: break;
        }
    }
    for (auto const& b : m_leaves) if (mixed(b)) ++mixedBoxes;
    char buf[256];
    std::snprintf(buf, sizeof buf, "sim %d (pass %d, fail %d%s), local %d, crossed %d of %d points; %d boxes (%d mixed), %d refine round(s)%s",
                  sim, pass, fail, invalid ? (", invalid " + std::to_string(invalid)).c_str() : "", local, crossed, m_total,
                  static_cast<int>(m_leaves.size()), mixedBoxes, m_rounds, m_budget ? ", sample cap reached" : "");
    return buf;
}

std::vector<std::string> SequencePlanner::map() const {
    std::vector<std::string> rows;
    if (!m_valid || m_n != 2) return rows;
    for (int j = m_dim[1] - 1; j >= 0; --j) {
        std::string row;
        for (int i = 0; i < m_dim[0]; ++i) {
            char c = ' ';
            switch (m_kind[flat({i, j, 0})]) {
                case Kind::Pass: c = '#'; break;
                case Kind::Fail: c = '.'; break;
                case Kind::Crossed: c = 'x'; break;
                case Kind::KnownPass: c = 'o'; break;
                case Kind::Invalid: c = '!'; break;
                case Kind::Unknown: c = ' '; break;
            }
            row.push_back(c);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---- engine-side arithmetic ----

namespace sequence {

std::vector<int> groupEndingAt(std::vector<double> const& frames, int last, int size, SequenceConfig const& cfg) {
    std::vector<int> out;
    if (size < kSequenceMinInputs || size > kSequenceMaxInputs || size > cfg.maxGroupSize) return out;
    int first = last - size + 1;
    if (first < 0 || last >= static_cast<int>(frames.size())) return out;
    for (int i = first; i < last; ++i) {
        double gap = frames[static_cast<size_t>(i + 1)] - frames[static_cast<size_t>(i)];
        if (!(gap >= cfg.neighbourMarginFrames) || gap > cfg.maxGapTicks + 1e-9) return out;
    }
    if (size >= 3 && frames[static_cast<size_t>(last)] - frames[static_cast<size_t>(first)] > cfg.maxTripleSpanTicks + 1e-9) return out;
    for (int i = first; i <= last; ++i) out.push_back(i);
    return out;
}

}  // namespace sequence

// ---- reference solver on the pull oracle ----

SequenceSolveResult SequenceSolver::solve(SnapshotId base, InputSchedule const& schedule, std::vector<size_t> const& group) {
    SequenceSolveResult out;
    out.inputIndices = group;
    auto& r = out.result;
    r.solverVersion = m_config.sequence.version;
    r.inputs = static_cast<int>(group.size());
    auto fail = [&](std::string why) {
        r.valid = false;
        r.invalidReason = std::move(why);
        r.debug.push_back(r.invalidReason);
        return out;
    };
    int const n = static_cast<int>(group.size());
    if (n < kSequenceMinInputs || n > kSequenceMaxInputs || n > m_config.sequence.maxGroupSize) return fail("a sequence is 2 to 3 inputs");
    for (int i = 0; i < n; ++i) {
        if (group[static_cast<size_t>(i)] >= schedule.inputs.size()) return fail("input index out of range");
        if (i > 0 && !(schedule.inputs[group[static_cast<size_t>(i)]].tMs > schedule.inputs[group[static_cast<size_t>(i - 1)]].tMs)) return fail("the group must be ascending in time");
    }
    LocalWindowSolver local(m_oracle, m_config.local);
    std::vector<SequenceAxis> axes;
    std::vector<double> times;
    int const maxAtoms = n == 2 ? m_config.sequence.maxAtomsPerAxisPair : m_config.sequence.maxAtomsPerAxisTriple;
    for (int i = 0; i < n; ++i) {
        WindowResult w = local.solve(base, schedule, group[static_cast<size_t>(i)]);
        LocalWindowInfo info = localWindowInfo(w);
        out.localWindows.push_back(std::move(w));
        SequenceAxis axis = makeAxis(info, m_config.continuous, maxAtoms);
        if (!axis.valid) return fail("input " + std::to_string(i + 1) + ": " + axis.invalidReason);
        axes.push_back(std::move(axis));
        times.push_back(schedule.inputs[group[static_cast<size_t>(i)]].tMs / kTickMs);
    }
    SequencePlanner planner(m_config.sequence, std::move(axes), std::move(times));
    if (!planner.valid()) return fail(planner.invalidReason());
    // control: the schedule as performed must pass
    Outcome control = m_oracle.trial(base, schedule, m_config.local.horizonSeconds);
    ++out.trials;
    if (!control.passed()) return fail(std::string("the unshifted schedule does not pass (") + name(control.kind) + ")");
    for (int guard = 0; guard < 100000; ++guard) {
        auto batch = planner.nextBatch(64);
        if (batch.empty()) break;
        for (auto const& t : batch) {
            InputSchedule s = schedule;
            for (int i = 0; i < n; ++i) s.inputs[group[static_cast<size_t>(i)]].tMs += t.shiftFrames[static_cast<size_t>(i)] * kTickMs;
            Outcome o = m_oracle.trial(base, s, m_config.local.horizonSeconds);
            ++out.trials;
            planner.ingest({t.id, o.passed(), o.invalid()});
        }
    }
    out.result = planner.result();
    return out;
}

// ---- telemetry ----

SequenceEventResult buildSequenceEvent(SequenceResult const& r, std::vector<int64_t> const& inputSeqs) {
    SequenceEventResult e;
    if (!r.valid) {
        e.error = r.invalidReason.empty() ? "sequence result invalid" : r.invalidReason;
        return e;
    }
    size_t const n = inputSeqs.size();
    if (n < static_cast<size_t>(kSequenceMinInputs) || n > static_cast<size_t>(kSequenceMaxInputs) || r.localWidthsMs.size() != n || static_cast<size_t>(r.inputs) != n) {
        e.error = "input count mismatch";
        return e;
    }
    for (size_t i = 0; i < n; ++i) {
        if (inputSeqs[i] < 0 || (i > 0 && inputSeqs[i] <= inputSeqs[i - 1])) {
            e.error = "input seqs must be strictly increasing";
            return e;
        }
        if (!std::isfinite(r.localWidthsMs[i]) || !(r.localWidthsMs[i] > 0.0)) {
            e.error = "local width not positive";
            return e;
        }
    }
    if (!std::isfinite(r.jointFeasibleShare) || r.jointFeasibleShare < 0.0 || r.jointFeasibleShare > 1.0) {
        e.error = "share outside [0,1]";
        return e;
    }
    if (r.samples < 1) {
        e.error = "no samples";
        return e;
    }
    if (!std::isfinite(r.resolutionMs) || !(r.resolutionMs > 0.0)) {
        e.error = "resolution not positive";
        return e;
    }
    e.payload.inputSeqs = inputSeqs;
    e.payload.localWidthsMs = r.localWidthsMs;
    e.payload.jointFeasibleShare = r.jointFeasibleShare;
    e.payload.samples = r.samples;
    e.payload.resolutionMs = r.resolutionMs;
    e.payload.solverVersion = r.solverVersion.empty() ? std::string(kSequenceSolverVersion) : r.solverVersion;
    e.ok = true;
    return e;
}

SequenceCheck checkSequencePayload(telemetry::SequenceWindowPayload const& p, SequenceCheckParams const& params) {
    SequenceCheck c;
    auto reject = [&](char const* why) {
        c.accepted = false;
        c.reasons.emplace_back(why);
    };
    bool finite = std::isfinite(p.jointFeasibleShare) && std::isfinite(p.resolutionMs);
    for (double w : p.localWidthsMs) finite = finite && std::isfinite(w);
    if (!finite) {
        reject("non_finite_value");
        return c;
    }
    size_t const n = p.inputSeqs.size();
    if (n < static_cast<size_t>(kSequenceMinInputs) || n > static_cast<size_t>(kSequenceMaxInputs)) reject("input_count_out_of_range");
    if (p.localWidthsMs.size() != n) reject("length_mismatch");
    for (size_t i = 1; i < n; ++i) {
        if (p.inputSeqs[i] <= p.inputSeqs[i - 1]) {
            reject("input_seqs_not_increasing");
            break;
        }
    }
    for (double w : p.localWidthsMs) {
        if (!(w > 0.0) || w > params.maxLocalWidthMs) {
            reject("local_width_out_of_range");
            break;
        }
    }
    if (p.jointFeasibleShare < 0.0 || p.jointFeasibleShare > 1.0) reject("share_out_of_range");
    if (p.samples < 1 || p.samples > params.maxSamples) reject("samples_out_of_range");
    if (!(p.resolutionMs > 0.0) || p.resolutionMs > params.maxResolutionMs) reject("resolution_out_of_range");
    return c;
}

}  // namespace gprl::solver
