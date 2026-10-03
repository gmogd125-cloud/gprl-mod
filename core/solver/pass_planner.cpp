#include "pass_planner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace gprl::solver {

namespace {

constexpr double kEps = 1e-9;

double roundKey(double v) { return std::round(v * 1e6) / 1e6; }

char kindChar(ShiftKind k) {
    switch (k) {
        case ShiftKind::Survived: return 'A';
        case ShiftKind::Resynced: return 'R';
        case ShiftKind::Died: return 'D';
        case ShiftKind::Invalid: return 'X';
        case ShiftKind::NotTested: return '-';
    }
    return '?';
}

std::string fmtShift(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+g", v);
    return buf;
}

}  // namespace

PassPlanner::PassPlanner(PlannerConfig cfg, double earlyLimitFrames) : m_cfg(cfg) {
    double const maxShift = static_cast<double>(m_cfg.maxShiftTicks) * static_cast<double>(m_cfg.coarseStepTicks);
    double const step = static_cast<double>(m_cfg.coarseStepTicks);
    m_early.limit = maxShift;
    if (!std::isnan(earlyLimitFrames) && earlyLimitFrames < maxShift) m_early.limit = std::max(0.0, earlyLimitFrames);
    m_early.limitKnown = true;
    m_early.blocked = m_early.limit < step - kEps;
    m_late.limit = maxShift;
    m_late.limitKnown = false;
    m_late.blocked = false;
}

void PassPlanner::setLateLimit(double frames) {
    if (std::isnan(frames)) return;
    double const step = static_cast<double>(m_cfg.coarseStepTicks);
    double lim = std::max(0.0, std::min(m_late.limit, frames));
    m_late.limit = lim;
    m_late.limitKnown = true;
    m_late.blocked = lim < step - kEps;
    if (m_passesIngested > 0) recompute();
}

void PassPlanner::markEarlyBlocked() {
    m_early.blocked = true;
    m_earlyHistoryMissing = true;
    if (m_passesIngested > 0) recompute();
}

void PassPlanner::setEarlyLimitKind(LimitKind k) {
    m_earlyLimitKind = k;
    if (m_passesIngested > 0) recompute();
}

std::vector<double> PassPlanner::coarseMagnitudes(SideState const& side, double offset) const {
    std::vector<double> out;
    double const step = static_cast<double>(m_cfg.coarseStepTicks);
    double const limit = side.limit;
    if (limit <= kEps) return out;
    for (int k = 0; k < m_cfg.maxShiftTicks; ++k) {
        double m = offset > kEps ? offset + static_cast<double>(k) * step : static_cast<double>(k + 1) * step;
        if (m > limit + kEps) break;
        if (m > kEps) out.push_back(m);
    }
    double last = out.empty() ? 0.0 : out.back();
    // The exact limit point when the limit is not on the grid (BoundarySearch `Limit` phase) -
    // only with sub-tick input placement (V2-D8, RC-minor 5): without Click Between Frames the
    // engine applies an input at the nearest step boundary, so the neighbour-limit point rounds
    // onto the neighbour's own step (`-7.9976(-8)A`) and the clone is wasted.
    if (m_cfg.subtick && limit - last > kEps) out.push_back(limit);
    return out;
}

std::vector<double> PassPlanner::nextPass() {
    if (m_finished) return {};
    if (m_passesPlanned == 0) {
        auto late = coarseMagnitudes(m_late, m_cfg.lateOffsetFrames);
        auto early = coarseMagnitudes(m_early, m_cfg.earlyOffsetFrames);
        m_late.coarseMax = late.empty() ? 0.0 : late.back();
        m_early.coarseMax = early.empty() ? 0.0 : early.back();
        m_late.limitPointPlanned = m_late.limitKnown && !late.empty() && std::fabs(late.back() - m_late.limit) < kEps;
        m_early.limitPointPlanned = true;
        std::vector<double> out;
        size_t n = std::max(late.size(), early.size());
        for (size_t i = 0; i < n; ++i) {
            if (i < late.size()) out.push_back(late[i]);
            if (i < early.size()) out.push_back(-early[i]);
        }
        m_passesPlanned = 1;
        return out;
    }
    if (m_passesIngested < m_passesPlanned) return {};   // the last pass has not been ingested yet
    auto out = planRefinement(true);
    if (!out.empty()) ++m_passesPlanned;
    return out;
}

std::vector<double> PassPlanner::planRefinement(bool commit) {
    std::vector<double> out;
    if (m_finished || m_passesIngested == 0 || !m_cfg.subtick) return out;
    if (m_early.invalid || m_late.invalid || m_noPass) return out;
    // A late limit that arrived after pass 0: test the exact limit point once (Limit phase) while
    // the side is still open and the point lies inside the planned range.
    if (m_late.limitKnown && !m_late.limitPointPlanned && !m_late.blocked && !m_late.bounded() && !m_miss
        && m_late.limit - m_late.lastPass > kEps && m_late.limit < m_late.coarseMax - kEps) {
        out.push_back(m_late.limit);
        if (commit) {
            m_late.limitPointPlanned = true;
            m_late.coarseMax = m_late.limit;
        }
    }
    int n = std::max(1, m_cfg.pointsPerPass);
    bool subdivides = false;
    for (SideState* side : {&m_late, &m_early}) {
        if (!side->bounded() || side->widthFrames() <= m_cfg.resolutionFrames + kEps) continue;
        if (side->refinePasses >= m_cfg.maxRefinePasses) continue;
        double stepF = (side->failEdge - side->passEdge) / static_cast<double>(n + 1);
        for (int i = 1; i <= n; ++i) out.push_back(side->passEdge + static_cast<double>(i) * stepF);
        subdivides = true;
        if (commit) ++side->refinePasses;
    }
    // a pass made only of the late limit point (whole-tick) does not refine anything: the window
    // stays "gprl-clone/1" at tick resolution unless a bracket was subdivided
    if (commit) m_lastPassSubdivides = subdivides;
    return out;
}

bool PassPlanner::done() const {
    if (m_finished) return true;
    if (m_passesIngested == 0 || m_passesIngested < m_passesPlanned) return false;
    return const_cast<PassPlanner*>(this)->planRefinement(false).empty();
}

void PassPlanner::ingest(std::vector<ShiftOutcome> const& outcomes, bool controlOk, bool controlDiedWithReal, bool spawnComplete) {
    int pass = std::max(0, m_passesPlanned - 1);
    for (auto o : outcomes) {
        o.pass = pass;
        m_all.push_back(std::move(o));
    }
    ++m_passesIngested;
    if (!spawnComplete) m_spawnIncomplete = true;
    if (pass >= 1 && m_cfg.subtick && m_lastPassSubdivides && !outcomes.empty()) m_refined = true;
    if (!m_controlSeen) {
        m_controlSeen = true;
        m_miss = !controlOk && controlDiedWithReal;
    }
    if (!controlOk && !controlDiedWithReal) {
        m_controlFailed = true;
        m_finished = true;
    }
    else if (controlOk == m_miss) {
        // a later pass disagrees with the first about hit / miss: the simulation is not stable here
        m_controlFailed = true;
        m_finished = true;
    }
    recompute();
}

void PassPlanner::setControlDeath(ShiftOutcome const& control) {
    m_controlDeath = control;
    m_controlDeath.kind = ShiftKind::Died;
    m_controlDeath.nominalFrames = m_controlDeath.appliedFrames = 0.0;
    m_haveControlDeath = true;
    if (m_passesIngested > 0) recompute();
}

void PassPlanner::attribute(SideState& side, ShiftOutcome const* fail) const {
    side.cause = EdgeCause::None;
    side.laterInputs = -1;
    side.failAfterFrames = kNaN;
    side.failObjectId = -1;
    side.failDeathX = kNaN;
    side.failExtension = false;
    if (!fail) return;
    side.failAfterFrames = fail->deathAfterFrames;
    side.failObjectId = fail->objectId;
    side.failDeathX = fail->deathX;
    side.failExtension = fail->extension;
    side.laterInputs = fail->laterFixed;
    if (fail->extension) side.cause = EdgeCause::Extension;
    else side.cause = fail->laterFixed >= 1 ? EdgeCause::Downstream : EdgeCause::Self;
}

EdgeStop PassPlanner::openStop(SideState const& side, bool late, bool untestedBeyond) const {
    double const maxShift = static_cast<double>(m_cfg.maxShiftTicks) * static_cast<double>(m_cfg.coarseStepTicks);
    if (!late && m_earlyHistoryMissing) return EdgeStop::History;
    if (untestedBeyond) return EdgeStop::Untested;
    if (side.limit < maxShift - kEps) {
        if (late) return side.limitKnown ? EdgeStop::Neighbour : EdgeStop::Range;
        return stopOf(m_earlyLimitKind);
    }
    return EdgeStop::Range;
}

void PassPlanner::walkSide(SideState& side, bool late) {
    // latest outcome per applied |shift| on this side, sorted by distance from the actual input
    std::map<double, ShiftOutcome const*> byShift;
    for (auto const& o : m_all) {
        double s = o.appliedFrames;
        if (late ? s <= kEps : s >= -kEps) continue;
        double mag = std::fabs(s);
        if (mag > side.limit + kEps) continue;   // beyond the limit: never counts (Limit / NotTested)
        double key = roundKey(mag);
        auto it = byShift.find(key);
        if (it == byShift.end() || it->second->pass <= o.pass) byShift[key] = &o;
    }
    side.lastPass = 0.0;
    side.firstFail = kNaN;
    side.invalid = false;
    side.invalidReason.clear();
    side.nonMonotonic = false;
    side.islands.clear();
    side.budgetCut = false;
    bool inIsland = false;
    Island island{};
    double sign = late ? 1.0 : -1.0;
    ShiftOutcome const* failOutcome = nullptr;
    for (auto const& [key, o] : byShift) {
        double mag = std::fabs(o->appliedFrames);   // the exact value, not the rounded map key
        if (!std::isnan(side.firstFail)) {
            if (passed(o->kind)) {
                side.nonMonotonic = true;
                if (!inIsland) {
                    inIsland = true;
                    island.fromShiftMs = sign * mag;
                }
                island.toShiftMs = sign * mag;
            }
            else if (o->kind == ShiftKind::Died && inIsland) {
                side.islands.push_back(island);
                inIsland = false;
            }
            continue;
        }
        switch (o->kind) {
            case ShiftKind::Survived:
            case ShiftKind::Resynced:
                side.lastPass = mag;
                break;
            case ShiftKind::Died:
                side.firstFail = mag;
                failOutcome = o;
                break;
            case ShiftKind::Invalid:
                side.invalid = true;
                side.invalidReason = o->reason.empty() ? "invalid trial" : o->reason;
                return;
            case ShiftKind::NotTested:
                break;   // a gap: nothing known here (widens the bracket)
        }
    }
    if (inIsland) side.islands.push_back(island);
    side.passEdge = sign * side.lastPass;
    side.failEdge = std::isnan(side.firstFail) ? kNaN : sign * side.firstFail;
    side.exhausted = std::isnan(side.firstFail) && side.coarseMax > kEps && side.lastPass >= side.coarseMax - kEps;
    // a NotTested point strictly inside the bracket = a cut pass (pool ran out / cancelled)
    bool untestedBeyond = false;
    for (auto const& [key, o] : byShift) {
        if (o->kind != ShiftKind::NotTested) continue;
        double mag = std::fabs(o->appliedFrames);
        bool inside = mag > side.lastPass + kEps && (std::isnan(side.firstFail) || mag < side.firstFail - kEps);
        if (inside) side.budgetCut = true;
        if (mag > side.lastPass + kEps) untestedBeyond = true;
    }
    attribute(side, failOutcome);
    side.stop = side.bounded() ? EdgeStop::Fail : openStop(side, late, untestedBeyond);
}

void PassPlanner::findMissRun() {
    // Every tested outcome as a signed applied shift (the control at 0 died with the real player).
    struct Pt { double s; ShiftKind kind; ShiftOutcome const* o; };
    std::map<double, Pt> pts;
    pts[0.0] = {0.0, ShiftKind::Died, m_haveControlDeath ? &m_controlDeath : nullptr};
    for (auto const& o : m_all) {
        double s = o.appliedFrames;
        SideState const& side = s < 0 ? m_early : m_late;
        if (std::fabs(s) > side.limit + kEps) continue;
        pts[roundKey(s)] = {s, o.kind, &o};   // later passes overwrite (ingested in order)
    }
    for (auto const& [k, p] : pts) {
        if (p.kind == ShiftKind::Invalid) {
            SideState& side = p.s < 0 ? m_early : m_late;
            side.invalid = true;
            // v0.7.1: name the trial's own reason like the hit path does ("teleport portal",
            // "dual portal", "history lost"): the engine maps the drop by it; without it a
            // teleport-invalid miss window was labelled payload_invalid (owner's 19.47.37 log, job 602)
            side.invalidReason = "invalid trial in the miss window (" + (p.o && !p.o->reason.empty() ? p.o->reason : std::string("invalid")) + ")";
        }
    }
    // contiguous pass runs (NotTested neither breaks nor extends a run)
    struct Run { double a, b; };
    std::vector<Run> runs;
    bool open = false;
    Run cur{};
    std::vector<Pt> ordered;
    for (auto const& [k, p] : pts) ordered.push_back(p);
    for (auto const& p : ordered) {
        if (passed(p.kind)) {
            if (!open) { open = true; cur.a = p.s; }
            cur.b = p.s;
        }
        else if (p.kind == ShiftKind::Died || p.kind == ShiftKind::Invalid) {
            if (open) { runs.push_back(cur); open = false; }
        }
    }
    if (open) runs.push_back(cur);
    m_noPass = runs.empty();
    m_early.passEdge = 0.0;
    m_early.failEdge = kNaN;
    m_late.passEdge = 0.0;
    m_late.failEdge = kNaN;
    m_early.exhausted = m_late.exhausted = false;
    m_early.budgetCut = m_late.budgetCut = false;
    attribute(m_early, nullptr);
    attribute(m_late, nullptr);
    if (m_noPass) {
        m_early.stop = m_late.stop = EdgeStop::Untested;
        return;
    }
    Run best = runs.front();
    double bestD = std::min(std::fabs(best.a), std::fabs(best.b));
    for (auto const& r : runs) {
        double d = std::min(std::fabs(r.a), std::fabs(r.b));
        if (d < bestD - kEps) { best = r; bestD = d; }
    }
    double failBelow = kNaN, failAbove = kNaN;
    ShiftOutcome const* failBelowO = nullptr;
    ShiftOutcome const* failAboveO = nullptr;
    bool untestedBelow = false, untestedAbove = false;
    for (auto const& p : ordered) {
        if (p.kind == ShiftKind::NotTested) {
            if (p.s < best.a - kEps && (std::isnan(failBelow) || p.s > failBelow + kEps)) untestedBelow = true;
            continue;
        }
        if (p.kind != ShiftKind::Died) continue;
        if (p.s < best.a - kEps) {   // last fail below the run
            failBelow = p.s;
            failBelowO = p.o;
            untestedBelow = false;   // a NotTested point below THIS fail does not touch the bracket
        }
        if (p.s > best.b + kEps && std::isnan(failAbove)) {
            failAbove = p.s;
            failAboveO = p.o;
        }
    }
    for (auto const& p : ordered) {
        if (p.kind != ShiftKind::NotTested) continue;
        if (p.s > best.b + kEps && (std::isnan(failAbove) || p.s < failAbove - kEps)) untestedAbove = true;
    }
    m_early.passEdge = best.a;
    m_early.failEdge = failBelow;
    m_late.passEdge = best.b;
    m_late.failEdge = failAbove;
    m_early.lastPass = std::fabs(best.a);
    m_late.lastPass = std::fabs(best.b);
    m_early.firstFail = std::isnan(failBelow) ? kNaN : std::fabs(failBelow);
    m_late.firstFail = std::isnan(failAbove) ? kNaN : std::fabs(failAbove);
    m_early.exhausted = std::isnan(failBelow) && best.a <= -m_early.coarseMax + kEps;
    m_late.exhausted = std::isnan(failAbove) && best.b >= m_late.coarseMax - kEps;
    // untested points between the run and its bracketing fail (the v0.6.x pruning left those)
    m_early.budgetCut = untestedBelow && !std::isnan(failBelow);
    m_late.budgetCut = untestedAbove && !std::isnan(failAbove);
    attribute(m_early, failBelowO);
    attribute(m_late, failAboveO);
    if (!std::isnan(failBelow) && !failBelowO) {
        // the control itself bounds the run: the real death (no attribution recorded for it)
        m_early.cause = EdgeCause::Self;
        m_early.laterInputs = 0;
    }
    if (!std::isnan(failAbove) && !failAboveO) {
        m_late.cause = EdgeCause::Self;
        m_late.laterInputs = 0;
    }
    m_early.stop = !std::isnan(failBelow) ? EdgeStop::Fail : openStop(m_early, false, untestedBelow);
    m_late.stop = !std::isnan(failAbove) ? EdgeStop::Fail : openStop(m_late, true, untestedAbove);
    // islands = the other runs (logged only)
    m_early.islands.clear();
    m_late.islands.clear();
    m_early.nonMonotonic = m_late.nonMonotonic = runs.size() > 1;
    for (auto const& r : runs) {
        if (r.a == best.a && r.b == best.b) continue;
        Island is{r.a, r.b};
        (r.b < best.a ? m_early : m_late).islands.push_back(is);
    }
}

void PassPlanner::recompute() {
    if (m_miss) {
        m_early.invalid = m_late.invalid = false;
        findMissRun();
        return;
    }
    walkSide(m_late, true);
    walkSide(m_early, false);
}

bool PassPlanner::anyShiftTested() const {
    for (auto const& o : m_all) {
        if (o.kind == ShiftKind::NotTested) continue;
        SideState const& side = o.appliedFrames < 0 ? m_early : m_late;
        if (std::fabs(o.appliedFrames) > side.limit + kEps) continue;
        if (std::fabs(o.appliedFrames) <= kEps) continue;
        return true;
    }
    return false;
}

WindowResult PassPlanner::result(double actualMs) const {
    WindowResult w;
    w.solverVersion = "pass-planner/0.2.0";
    w.actualMs = actualMs;
    auto fail = [&](std::string why) {
        w.valid = false;
        w.invalidReason = std::move(why);
        return w;
    };
    if (m_passesIngested == 0) return fail("no pass ingested");
    if (m_controlFailed) return fail("control clone did not reproduce the real player");
    if (m_early.invalid) return fail("early: " + m_early.invalidReason);
    if (m_late.invalid) return fail("late: " + m_late.invalidReason);
    if (m_miss && m_noPass) return fail("miss without a passing shift within the tested range");
    // RC-minor 2: a window without a single tested shift within the limits is no measurement
    // (v0.6.2 emitted `#610 press t=0.0000 ... +0A +1l ... +10l` as a 0 ms window)
    if (!m_miss && !anyShiftTested()) return fail(kNoShiftTested);

    auto fillSide = [&](SideState const& s, Side which) {
        BoundaryResult r;
        r.version = "pass-planner/0.2.0";
        r.side = which;
        r.valid = true;
        r.limitMs = s.limit * kTickMs;
        r.blocked = s.blocked;
        r.bounded = s.bounded();
        r.passShiftMs = s.passEdge * kTickMs;
        r.failShiftMs = s.bounded() ? s.failEdge * kTickMs : kNaN;
        r.bracketMs = s.bounded() ? std::fabs(s.failEdge - s.passEdge) * kTickMs : 0.0;
        r.nonMonotonic = s.nonMonotonic;
        for (auto const& is : s.islands) r.islands.push_back({is.fromShiftMs * kTickMs, is.toShiftMs * kTickMs});
        r.budgetExhausted = s.budgetCut || (s.bounded() && m_cfg.subtick && s.widthFrames() > m_cfg.resolutionFrames + kEps && m_spawnIncomplete);
        r.edge.attributed = true;
        r.edge.stop = s.stop;
        r.edge.cause = s.bounded() ? s.cause : EdgeCause::None;
        r.edge.laterInputs = s.bounded() ? s.laterInputs : -1;
        r.edge.failAfterMs = s.bounded() && !std::isnan(s.failAfterFrames) ? s.failAfterFrames * kTickMs : kNaN;
        r.edge.failObjectId = s.bounded() ? s.failObjectId : -1;
        r.edge.failDeathX = s.bounded() ? s.failDeathX : kNaN;
        r.edge.extension = s.bounded() && s.failExtension;
        for (auto const& o : m_all) {
            bool onSide = which == Side::Earlier ? o.appliedFrames < -kEps : o.appliedFrames > kEps;
            if (!onSide || o.kind == ShiftKind::NotTested) continue;
            Trial t;
            t.index = r.trialCount++;
            t.shiftMs = o.appliedFrames * kTickMs;
            t.tMs = actualMs + t.shiftMs;
            // deaths / re-joins are measured from the UNSHIFTED input (ShiftOutcome convention)
            switch (o.kind) {
                case ShiftKind::Survived: t.outcome = Outcome::survived(); break;
                case ShiftKind::Resynced: t.outcome = Outcome::resynced(actualMs + o.deathAfterFrames * kTickMs); break;
                case ShiftKind::Died: t.outcome = Outcome::died(actualMs + o.deathAfterFrames * kTickMs, o.objectId); break;
                default: t.outcome = Outcome::invalid(o.reason); break;
            }
            t.pass = t.outcome.passed();
            double mag = std::fabs(o.appliedFrames);
            bool limitPt = std::fabs(mag - s.limit) < kEps && std::fabs(mag - std::round(mag)) > kEps;
            if (limitPt) t.phase = TrialPhase::Limit;
            else if (o.pass >= 1) t.phase = TrialPhase::Bisect;
            else if (!std::isnan(s.firstFail) && mag > s.firstFail + kEps && !m_miss) t.phase = TrialPhase::IslandScan;
            else t.phase = TrialPhase::Coarse;
            r.trials.push_back(t);
        }
        char buf[240];
        std::snprintf(buf, sizeof buf, "limit %.4f frames%s, pass edge %+.4f, fail edge %+.4f, %s%s, stop %s%s%s", s.limit, s.blocked ? " (blocked)" : "",
                      s.passEdge, s.bounded() ? s.failEdge : std::numeric_limits<double>::quiet_NaN(), s.exhausted ? "unbounded" : (s.bounded() ? "bounded" : "open"),
                      s.budgetCut ? ", cut" : "", name(s.stop), s.bounded() ? " cause " : "", s.bounded() ? name(s.cause) : "");
        r.debug.emplace_back(buf);
        return r;
    };
    w.early = fillSide(m_early, Side::Earlier);
    w.late = fillSide(m_late, Side::Later);
    w.trials = w.early.trialCount + w.late.trialCount;
    w.earliestMs = actualMs + w.early.passShiftMs;
    w.latestMs = actualMs + w.late.passShiftMs;
    w.earliestFailMs = std::isnan(w.early.failShiftMs) ? kNaN : actualMs + w.early.failShiftMs;
    w.latestFailMs = std::isnan(w.late.failShiftMs) ? kNaN : actualMs + w.late.failShiftMs;
    w.boundedEarly = w.early.bounded;
    w.boundedLate = w.late.bounded;
    w.blockedEarly = w.early.blocked;
    w.blockedLate = w.late.blocked;
    w.resolutionMs = std::max(w.early.bracketMs, w.late.bracketMs);
    w.nonMonotonic = w.early.nonMonotonic || w.late.nonMonotonic;
    w.budgetExhausted = w.early.budgetExhausted || w.late.budgetExhausted;
    for (auto const& d : w.early.debug) w.debug.push_back("early: " + d);
    for (auto const& d : w.late.debug) w.debug.push_back("late: " + d);
    w.debug.push_back(std::string(m_miss ? "miss " : "hit ") + "window " + formatWindow(w.widthMs()));
    return w;
}

std::string PassPlanner::describe() const {
    std::vector<ShiftOutcome const*> sorted;
    for (auto const& o : m_all) sorted.push_back(&o);
    std::stable_sort(sorted.begin(), sorted.end(), [](ShiftOutcome const* a, ShiftOutcome const* b) { return a->nominalFrames < b->nominalFrames; });
    std::string out;
    int cancelled = 0;
    for (auto const* o : sorted) {
        if (o->kind == ShiftKind::NotTested) { ++cancelled; continue; }
        if (!out.empty()) out += ' ';
        out += fmtShift(o->nominalFrames);
        if (std::fabs(o->appliedFrames - o->nominalFrames) > 1e-6) out += "(" + fmtShift(o->appliedFrames) + ")";
        out += kindChar(o->kind);
        if (o->kind == ShiftKind::Died) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "@%.1f/%dL%c", o->deathAfterFrames, std::max(0, o->laterFixed), o->laterFixed >= 1 ? 'd' : 's');
            out += buf;
            if (o->objectId >= 0) out += "#" + std::to_string(o->objectId);
        }
        if (o->extension) out += "e";
    }
    if (cancelled) out += " (" + std::to_string(cancelled) + " not tested)";
    return out;
}

double hitPruneLimit(std::vector<PruneProbe> const& side, double scanFrames) {
    // bounded by the probe count (one pass)
    PruneProbe const* nearest = nullptr;
    double firstFail = kNaN;
    for (auto const& p : side) {
        if (!nearest || p.mag < nearest->mag - kEps) nearest = &p;
        if (p.state == ProbeState::Died && (std::isnan(firstFail) || p.mag < firstFail)) firstFail = p.mag;
    }
    if (!nearest || nearest->state != ProbeState::Passed || std::isnan(firstFail)) return kNaN;
    return firstFail + std::max(0.0, scanFrames);
}

int laterFixedBefore(InputSchedule const& reference, size_t movingIndex, std::vector<size_t> const& moved, double deathMs) {
    if (movingIndex >= reference.inputs.size() || !std::isfinite(deathMs)) return 0;
    double const t0 = reference.inputs[movingIndex].tMs;
    int n = 0;
    for (size_t i = 0; i < reference.inputs.size(); ++i) {
        if (i == movingIndex) continue;
        if (std::find(moved.begin(), moved.end(), i) != moved.end()) continue;
        double t = reference.inputs[i].tMs;
        if (t > t0 + 1e-9 && t < deathMs - 1e-9) ++n;
    }
    return n;
}

int runAgainstOracle(IPhysicsOracle& oracle, PassPlanner& planner, SnapshotId base, InputSchedule const& schedule,
                     size_t movingIndex, double horizonSeconds) {
    int trials = 0;
    if (movingIndex >= schedule.inputs.size()) return 0;
    double actual = schedule.inputs[movingIndex].tMs;
    auto toShift = [&](Outcome const& o, double s) {
        ShiftOutcome so;
        so.nominalFrames = so.appliedFrames = s;
        switch (o.kind) {
            case OutcomeKind::Survived: so.kind = ShiftKind::Survived; break;
            case OutcomeKind::Resynced:
                so.kind = ShiftKind::Resynced;
                so.deathAfterFrames = (o.tMs - actual) / kTickMs;
                so.rejoinAfterFrames = so.deathAfterFrames;
                break;
            case OutcomeKind::Died:
                so.kind = ShiftKind::Died;
                so.deathAfterFrames = (o.tMs - actual) / kTickMs;   // from the UNSHIFTED input (engine convention)
                so.objectId = o.objectId;
                so.laterFixed = laterFixedBefore(schedule, movingIndex, {}, o.tMs);
                break;
            case OutcomeKind::Invalid:
                so.kind = ShiftKind::Invalid;
                so.reason = o.reason;
                break;
        }
        return so;
    };
    for (int guard = 0; guard < 16; ++guard) {
        auto shifts = planner.nextPass();
        if (shifts.empty()) break;
        Outcome control = oracle.trial(base, schedule, horizonSeconds);
        ++trials;
        std::vector<ShiftOutcome> outcomes;
        for (double s : shifts) {
            // the engine's horizon is fixed at the UNSHIFTED input + horizon (SD §3.3); the pull
            // oracle measures from the earliest input it sees, which an earlier shift moves back
            double h = horizonSeconds + (s < 0.0 ? -s / kTicksPerSecond : 0.0);
            Outcome o = oracle.trial(base, schedule.withMoved(movingIndex, actual + s * kTickMs), h);
            ++trials;
            outcomes.push_back(toShift(o, s));
        }
        double const step = static_cast<double>(planner.config().coarseStepTicks);
        if (planner.passesPlanned() == 1 && !planner.config().pruneNearShifts && planner.config().pruneBeyondHitFail) {
            // v0.7.1 (Fable D5): the engine prunes a side beyond its first fail + the island scan
            // only when the side's nearest tested shift passed (hitPruneLimit)
            for (int side = 0; side < 2; ++side) {
                std::vector<PruneProbe> probes;
                for (auto const& o : outcomes) {
                    bool onSide = side == 0 ? o.nominalFrames > 0 : o.nominalFrames < 0;
                    if (!onSide || o.kind == ShiftKind::NotTested) continue;
                    PruneProbe pr;
                    pr.mag = std::fabs(o.nominalFrames);
                    pr.state = passed(o.kind) ? ProbeState::Passed : o.kind == ShiftKind::Died ? ProbeState::Died : ProbeState::Other;
                    probes.push_back(pr);
                }
                double lim = hitPruneLimit(probes, planner.config().islandScanSteps * step);
                if (std::isnan(lim)) continue;
                for (auto& o : outcomes) {
                    bool onSide = side == 0 ? o.nominalFrames > 0 : o.nominalFrames < 0;
                    if (onSide && std::fabs(o.nominalFrames) > lim + 1e-9) {
                        o.kind = ShiftKind::NotTested;
                        o.reason = "cancelled";
                    }
                }
            }
        }
        if (planner.passesPlanned() == 1 && planner.config().pruneNearShifts) {
            // The v0.6.x engine pruning (advance): once a smaller shift died, clones more than
            // islandScanSteps coarse steps beyond that first fail were cancelled (BoundarySearch
            // scans exactly that far for islands). Mirrored only for comparison runs.
            for (int side = 0; side < 2; ++side) {
                double firstFail = kNaN;
                for (auto const& o : outcomes) {
                    bool onSide = side == 0 ? o.nominalFrames > 0 : o.nominalFrames < 0;
                    if (!onSide || o.kind != ShiftKind::Died) continue;
                    double mag = std::fabs(o.nominalFrames);
                    if (std::isnan(firstFail) || mag < firstFail) firstFail = mag;
                }
                if (std::isnan(firstFail)) continue;
                for (auto& o : outcomes) {
                    bool onSide = side == 0 ? o.nominalFrames > 0 : o.nominalFrames < 0;
                    if (onSide && std::fabs(o.nominalFrames) > firstFail + planner.config().islandScanSteps * step + 1e-9) {
                        o.kind = ShiftKind::NotTested;
                        o.reason = "cancelled";
                    }
                }
            }
        }
        if (!control.passed() && control.kind == OutcomeKind::Died && planner.passesIngested() == 0) {
            planner.setControlDeath(toShift(control, 0.0));
        }
        planner.ingest(outcomes, control.passed(), !control.passed() && control.kind == OutcomeKind::Died);
        if (planner.done()) break;
    }
    return trials;
}

}  // namespace gprl::solver
