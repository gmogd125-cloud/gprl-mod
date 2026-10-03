#include "analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "../vocab.hpp"
#include "verify.hpp"

namespace gprl::sim {

namespace {

std::string cut(std::string s, size_t max = 128) {
    if (s.size() > max) s.resize(max);
    return s;
}

double clampPercent(double p) { return std::clamp(std::isfinite(p) ? p : 0.0, 0.0, 100.0); }

double finiteOr(double v, double def) { return std::isfinite(v) ? v : def; }

/// Verified verdict of a percent: the verify bin holding it is verified. False without bins.
bool binVerified(VerifyResult const* v, double percent) {
    if (!v || v->bins.empty()) return false;
    double const binPercent = v->bins.size() > 1 ? v->bins[1].from - v->bins[0].from : 100.0;
    int const idx = verifyBinIndex(percent, binPercent > 0.0 ? binPercent : 2.0);
    if (idx < 0 || idx >= static_cast<int>(v->bins.size())) return false;
    VerifyBin const& b = v->bins[static_cast<size_t>(idx)];
    return b.ticks > 0 && !b.unsupported && b.verified;
}

/// The server's span cap (shared LEVEL_SIM_LIMITS.maxSpans).
constexpr size_t kMaxSpans = 1000;

/// At most `max` spans: overlapping spans are merged first (mechanics joined with '+'), then the
/// tail is folded into one "multiple" span. A folded span may cover gaps between the originals:
/// windows in it are marked unsupported too (assemble checks the emitted spans), so the result
/// stays valid and honest (less coverage claimed, never more).
std::vector<CoverageSpan> capSpans(std::vector<CoverageSpan> spans, size_t max) {
    if (spans.size() <= max) return spans;
    std::stable_sort(spans.begin(), spans.end(), [](CoverageSpan const& a, CoverageSpan const& b) { return a.x0 < b.x0; });
    std::vector<CoverageSpan> merged;
    for (auto& s : spans) {
        if (!merged.empty() && s.x0 <= merged.back().x1) {
            CoverageSpan& m = merged.back();
            m.x1 = std::max(m.x1, s.x1);
            m.percentTo = std::max(m.percentTo, s.percentTo);
            if (m.mechanic.find(s.mechanic) == std::string::npos) m.mechanic = cut(m.mechanic + "+" + s.mechanic);
        }
        else merged.push_back(std::move(s));
    }
    if (merged.size() <= max || max == 0) return merged;
    CoverageSpan tail = merged[max - 1];
    for (size_t i = max; i < merged.size(); ++i) {
        tail.x1 = std::max(tail.x1, merged[i].x1);
        tail.percentTo = std::max(tail.percentTo, merged[i].percentTo);
    }
    tail.mechanic = "multiple";
    merged.resize(max - 1);
    merged.push_back(std::move(tail));
    return merged;
}

/// Sorted, merged [x0, x1) intervals of the spans, clamped to [0, endX].
std::vector<std::pair<double, double>> unionOf(std::vector<CoverageSpan> const& spans, double endX) {
    std::vector<std::pair<double, double>> v;
    for (auto const& s : spans) {
        double const a = std::clamp(static_cast<double>(s.x0), 0.0, endX), b = std::clamp(static_cast<double>(s.x1), 0.0, endX);
        if (b > a) v.emplace_back(a, b);
    }
    std::sort(v.begin(), v.end());
    std::vector<std::pair<double, double>> out;
    for (auto const& s : v) {
        if (!out.empty() && s.first <= out.back().second) out.back().second = std::max(out.back().second, s.second);
        else out.push_back(s);
    }
    return out;
}

/// Length of [a, b] outside the (sorted, merged) union.
double lengthOutside(double a, double b, std::vector<std::pair<double, double>> const& u) {
    if (!(b > a)) return 0.0;
    double len = b - a;
    for (auto const& s : u) {
        double const lo = std::max(a, s.first), hi = std::min(b, s.second);
        if (hi > lo) len -= hi - lo;
    }
    return std::max(0.0, len);
}

}  // namespace

SimDensity clickDensity(std::vector<RecordedInput> const& inputs, double seconds) {
    SimDensity d;
    std::vector<double> presses;
    for (auto const& in : inputs)
        if (in.down && in.step >= 1) presses.push_back((static_cast<double>(in.step - 1) + in.subTick) / 240.0);
    std::sort(presses.begin(), presses.end());
    if (presses.empty() || !(seconds > 0.0)) return d;
    d.avgCps = static_cast<double>(presses.size()) / seconds;
    auto countIn = [&](double t0, double t1) {
        auto lo = std::lower_bound(presses.begin(), presses.end(), t0);
        auto hi = std::lower_bound(presses.begin(), presses.end(), t1);
        return static_cast<int>(hi - lo);
    };
    std::vector<double> oneSecond;
    double peak1 = 0.0, peak5 = 0.0;
    double const last = std::max(seconds, presses.back() + 1e-9);
    for (double t = 0.0; t < last; t += 0.25) {
        int c1 = countIn(t, t + 1.0);
        oneSecond.push_back(c1);
        peak1 = std::max(peak1, static_cast<double>(c1));
        peak5 = std::max(peak5, countIn(t, t + 5.0) / 5.0);
    }
    std::sort(oneSecond.begin(), oneSecond.end());
    size_t const idx = std::min(oneSecond.size() - 1, static_cast<size_t>(std::floor(0.9 * static_cast<double>(oneSecond.size() - 1) + 0.5)));
    d.p90Cps = oneSecond[idx];
    d.peak1sCps = peak1;
    d.peak5sCps = peak5;
    return d;
}

void applyVerification(LevelSimResult& r, VerifyResult const* verifyIn) {
    // M5: recorded attempts whose every tick fell in unsupported bins verify nothing (0 / 0): the
    // result then has NO verification (hasVerification false, verifiedPercent 0 = "null" for the
    // server); the verification block is still serialised with its countedBins for diagnostics
    bool const has = verifyIn != nullptr && verifyIn->attempts > 0 && verifyIn->countedBins > 0;
    VerifyResult const* verify = has ? verifyIn : nullptr;
    r.hasVerification = has;
    r.coverage.hasVerification = has;
    r.verification = verifyIn ? *verifyIn : VerifyResult{};
    r.coverage.verifiedPercent = has ? clampPercent(verifyIn->verifiedShare * 100.0) : 0.0;
    for (auto& w : r.windows) w.verified = binVerified(verify, w.percent);
    for (auto& s : r.sections) {
        bool any = false, all = true;
        if (verify) {
            for (auto const& b : verify->bins) {
                if (b.to <= s.from + 1e-9 || b.from >= s.to - 1e-9) continue;
                if (b.ticks <= 0 || b.unsupported) continue;
                any = true;
                if (!b.verified) all = false;
            }
        }
        s.verified = any && all;
    }
}

LevelSimResult assemble(World const& world, ReferenceRun const& run, std::vector<SimWindow> const& referenceWindows,
                        std::vector<SimWindow> const& recordedWindows, VerifyResult const* verify, SimBudget budget) {
    LevelSimResult r;
    r.gdLevelId = world.gdLevelId;
    r.levelHash = world.levelHash;
    r.gameplayHash = world.gameplayHash;
    r.gameplayObjects = world.gameplayObjects > 0 ? world.gameplayObjects : static_cast<int>(world.objects.size());
    r.decorationObjects = world.decorationObjects > 0 ? world.decorationObjects : world.decoration.objects;
    r.objects = r.gameplayObjects + r.decorationObjects;
    r.lengthX = world.lengthX > 0.f ? world.lengthX : world.endX;
    r.startPositions = static_cast<int>(world.startPositions.size());
    r.tooLarge = world.tooLarge;

    double const endX = world.endX > 0.f ? static_cast<double>(world.endX) : 1.0;
    // M1: the level's length in time = the last segment's end tick (its offset carries every
    // skipped stretch, M2) plus, when the run did not reach the end, the speed-integrated rest
    if (!run.segments.empty()) {
        ReferenceSegment const& last = run.segments.back();
        double ticks = static_cast<double>(last.tickOffset + last.ticks);
        if (!run.reachedEnd) ticks += ticksAcross(world, static_cast<double>(last.x1), endX, speedAtX(world, static_cast<double>(last.x1)));
        r.lengthSeconds = ticks / 240.0;
    }
    else r.lengthSeconds = ticksAcross(world, static_cast<double>(world.start.x), endX, world.start.speed) / 240.0;

    // coverage: the spans as the server receives them (<= 1000 each, LOW) ...
    std::vector<CoverageSpan> unsupported;
    for (auto const& u : world.unsupported) {
        double x0 = std::clamp(static_cast<double>(u.x0), 0.0, endX), x1 = std::clamp(static_cast<double>(u.x1), 0.0, endX);
        CoverageSpan s;
        s.x0 = u.x0;
        s.x1 = u.x1;
        s.percentFrom = clampPercent(x0 / endX * 100.0);
        s.percentTo = clampPercent(x1 / endX * 100.0);
        s.mechanic = cut(u.mechanic);
        unsupported.push_back(std::move(s));
    }
    r.coverage.unsupported = capSpans(std::move(unsupported), kMaxSpans);
    r.coverage.unsolved = run.unsolved;
    for (auto& s : r.coverage.unsolved) s.mechanic = cut(s.mechanic);
    r.coverage.unsolved = capSpans(std::move(r.coverage.unsolved), kMaxSpans);
    // ... C2: physics = the share outside the UNION of the unsupported spans (overlapping spans of
    // different mechanics were summed by /1: Deadlocked reported 0 %); solved = the reference run's
    // length outside that union, so physicsPercent >= solvedPercent always holds
    std::vector<std::pair<double, double>> const spanUnion = unionOf(r.coverage.unsupported, endX);
    double unsupportedLen = 0.0;
    for (auto const& s : spanUnion) unsupportedLen += s.second - s.first;
    r.coverage.physicsPercent = clampPercent((endX - std::min(unsupportedLen, endX)) / endX * 100.0);
    double solvedLen = 0.0;
    for (auto const& seg : run.segments)
        solvedLen += lengthOutside(std::clamp(static_cast<double>(seg.x0), 0.0, endX), std::clamp(static_cast<double>(seg.x1), 0.0, endX), spanUnion);
    r.coverage.solvedPercent = std::min(clampPercent(solvedLen / endX * 100.0), r.coverage.physicsPercent);

    r.referenceInputs = static_cast<int>(run.inputs.size());
    r.referenceTicks = static_cast<int>(run.ticks.size());
    r.trajectoryDigest = run.trajectoryDigest;

    // a window is unsupported inside any world span by x AND, with validate.ts's own rule, inside
    // any EMITTED span by the serialised percents ([percentFrom, percentTo): exactly at a span's
    // start = inside), so the server never sees a `supported` window in a span (LOW)
    r.windows = referenceWindows;
    r.windows.insert(r.windows.end(), recordedWindows.begin(), recordedWindows.end());
    for (auto& w : r.windows) {
        w.supported = true;
        double const x = w.percent / 100.0 * endX;
        for (auto const& u : world.unsupported)
            if (x >= u.x0 && x < u.x1) w.supported = false;
        double const p = clampPercent(w.percent);
        for (auto const& s : r.coverage.unsupported)
            if (p >= clampPercent(s.percentFrom) && p < clampPercent(s.percentTo)) w.supported = false;
    }

    r.density = clickDensity(run.inputs, r.lengthSeconds);

    // sections: cut at gamemode / speed / size portals, unsupported and unsolved span edges; a cut
    // closer than 1 unit to the previous one is dropped (no zero-width sections from the 1-unit
    // unsolved stretches of a failed restart, LOW), the level end always stays
    std::set<double> cutsX{0.0, endX};
    for (auto const& o : world.objects)
        if (o.kind == ObjKind::GamemodePortal || o.kind == ObjKind::SpeedChange || o.kind == ObjKind::SizePortal) cutsX.insert(std::clamp(static_cast<double>(o.x), 0.0, endX));
    for (auto const& u : world.unsupported) {
        cutsX.insert(std::clamp(static_cast<double>(u.x0), 0.0, endX));
        cutsX.insert(std::clamp(static_cast<double>(u.x1), 0.0, endX));
    }
    for (auto const& u : run.unsolved) {
        cutsX.insert(std::clamp(static_cast<double>(u.x0), 0.0, endX));
        cutsX.insert(std::clamp(static_cast<double>(u.x1), 0.0, endX));
    }
    std::vector<double> cuts;
    for (double c : cutsX) {
        if (!cuts.empty() && c - cuts.back() < 1.0) {
            if (c >= endX) cuts.back() = endX;   // keep the end; the previous cut gives way
            continue;
        }
        cuts.push_back(c);
    }
    if (cuts.size() == 1 && endX > 0.0) cuts.push_back(endX);
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        double const a = cuts[i], b = cuts[i + 1];
        if (b - a < 1e-6) continue;
        SimSection s;
        s.from = clampPercent(a / endX * 100.0);
        s.to = clampPercent(b / endX * 100.0);
        // state in the section: the reference trajectory's first tick a little past the cut (a
        // portal / speed object switches the player when its rect is touched, not at its centre)
        bool found = false;
        double const probe = a + std::min(16.0, 0.5 * (b - a));
        for (auto const& t : run.ticks)
            if (static_cast<double>(t.x) >= probe - 1e-6 && static_cast<double>(t.x) < b) {
                s.gamemode = t.mode;
                s.speed = t.speed;
                s.mini = t.mini;
                found = true;
                break;
            }
        if (!found) {
            s.gamemode = world.start.mode;
            s.speed = world.start.speed;
            s.mini = world.start.mini;
            for (auto const& o : world.objects) {   // the last portal before the section decides
                if (static_cast<double>(o.x) > a) break;
                if (o.kind == ObjKind::GamemodePortal) s.gamemode = o.mode;
                else if (o.kind == ObjKind::SpeedChange) s.speed = o.speed;
                else if (o.kind == ObjKind::SizePortal) s.mini = o.flag;
            }
        }
        for (auto const& u : world.unsupported)
            if (static_cast<double>(u.x0) < b && static_cast<double>(u.x1) > a) s.supported = false;
        s.solved = false;
        for (auto const& seg : run.segments)
            if (static_cast<double>(seg.x0) <= a + 1e-6 && static_cast<double>(seg.x1) >= b - 1e-6) s.solved = true;
        std::vector<double> bounded;
        for (auto const& w : referenceWindows) {
            if (w.percent < s.from || w.percent >= s.to) continue;
            ++s.inputs;
            if (w.boundedEarly && w.boundedLate) bounded.push_back(w.windowMs);
        }
        if (!bounded.empty()) {
            std::sort(bounded.begin(), bounded.end());
            s.narrowestMs = bounded.front();
            size_t const n = bounded.size();
            s.medianMs = n % 2 ? bounded[n / 2] : 0.5 * (bounded[n / 2 - 1] + bounded[n / 2]);
        }
        r.sections.push_back(std::move(s));
    }

    r.budget = std::move(budget);
    r.budget.mode = cut(r.budget.mode);
    applyVerification(r, verify);
    return r;
}

json::Value toJson(LevelSimResult const& r) {
    using json::Value;
    Value root = Value::object();
    root.set("analyzerVersion", cut(r.analyzerVersion));
    root.set("simVersion", cut(r.simVersion));
    root.set("gameplayHashVersion", cut(r.gameplayHashVersion));
    root.set("gdLevelId", r.gdLevelId);
    root.set("levelHash", cut(r.levelHash));
    root.set("gameplayHash", cut(r.gameplayHash));
    root.set("computedAt", cut(r.computedAt));
    root.set("build", cut(r.build));
    // world (flat, the result.hpp names: shared/src/level-sim/validate.ts)
    root.set("objects", r.objects);
    root.set("gameplayObjects", r.gameplayObjects);
    root.set("decorationObjects", r.decorationObjects);
    root.set("lengthX", finiteOr(static_cast<double>(r.lengthX), 0.0));
    root.set("lengthSeconds", finiteOr(r.lengthSeconds, 0.0));
    root.set("startPositions", r.startPositions);
    root.set("tooLarge", r.tooLarge);

    auto spans = [](std::vector<CoverageSpan> const& list) {
        Value arr = Value::array();
        for (auto const& s : list) {
            Value o = Value::object();
            o.set("percentFrom", clampPercent(s.percentFrom));
            o.set("percentTo", clampPercent(s.percentTo));
            o.set("x0", finiteOr(static_cast<double>(s.x0), 0.0));
            o.set("x1", finiteOr(static_cast<double>(s.x1), 0.0));
            o.set("mechanic", cut(s.mechanic));
            arr.push(std::move(o));
        }
        return arr;
    };
    Value coverage = Value::object();
    coverage.set("physicsPercent", clampPercent(r.coverage.physicsPercent));
    coverage.set("solvedPercent", clampPercent(r.coverage.solvedPercent));
    coverage.set("hasVerification", r.coverage.hasVerification);
    coverage.set("verifiedPercent", r.coverage.hasVerification ? clampPercent(r.coverage.verifiedPercent) : 0.0);
    coverage.set("unsupported", spans(r.coverage.unsupported));
    coverage.set("unsolved", spans(r.coverage.unsolved));
    root.set("coverage", std::move(coverage));

    root.set("referenceInputs", r.referenceInputs);
    root.set("referenceTicks", r.referenceTicks);
    root.set("trajectoryDigest", std::to_string(r.trajectoryDigest));   // uint64: a decimal string, never a JS number

    Value windows = Value::array();
    for (auto const& w : r.windows) {
        Value o = Value::object();
        o.set("source", name(w.source));
        o.set("tick", w.tick);
        o.set("frame", finiteOr(w.frame, 0.0));
        o.set("tSeconds", finiteOr(w.tSeconds, 0.0));
        o.set("percent", clampPercent(w.percent));
        o.set("down", w.down);
        o.set("gamemode", gamemodeName(w.gamemode));
        o.set("speed", static_cast<int>(w.speed));
        double const earliest = finiteOr(w.earliestMs, 0.0), latest = finiteOr(w.latestMs, 0.0);
        o.set("windowMs", latest - earliest);   // exactly the doubles the server subtracts
        o.set("earliestMs", earliest);
        o.set("latestMs", latest);
        o.set("resolutionMs", std::isfinite(w.resolutionMs) && w.resolutionMs > 0.0 ? w.resolutionMs : 1000.0 / 240.0);
        o.set("boundedEarly", w.boundedEarly);
        o.set("boundedLate", w.boundedLate);
        o.set("verified", w.verified);
        o.set("supported", w.supported);
        o.set("geometryHash", w.geometryHash);
        o.set("geometryHashHex", cut(w.geometryHashHex));
        o.set("trials", w.trials);
        windows.push(std::move(o));
    }
    root.set("windows", std::move(windows));

    root.set("hasVerification", r.hasVerification);
    // the block is written whenever attempts were replayed (with hasVerification false when no bin
    // could be counted, M5: countedBins 0); null when there was nothing to replay
    if (r.hasVerification || r.verification.attempts > 0) {
        Value v = Value::object();
        v.set("attempts", r.verification.attempts);
        v.set("ticksCompared", r.verification.ticksCompared);
        v.set("verifiedShare", r.hasVerification ? finiteOr(r.verification.verifiedShare, 0.0) : 0.0);
        v.set("tolerancePosition", finiteOr(r.verification.tolerancePosition, 0.5));
        v.set("countedBins", r.verification.countedBins);   // gprl-sim/2 (extra key; the validator ignores unknown keys)
        v.set("reseeds", r.verification.reseeds);
        Value bins = Value::array();
        for (auto const& b : r.verification.bins) {
            Value o = Value::object();
            o.set("from", clampPercent(b.from));
            o.set("to", clampPercent(b.to));
            o.set("ticks", b.ticks);
            o.set("verified", b.verified);
            o.set("unsupported", b.unsupported);
            o.set("maxError", finiteOr(static_cast<double>(b.maxError), 0.0));
            o.set("firstDivergenceStep", b.firstDivergenceStep);
            o.set("reason", cut(b.reason));
            bins.push(std::move(o));
        }
        v.set("bins", std::move(bins));
        root.set("verification", std::move(v));
    }
    else root.set("verification", nullptr);

    Value density = Value::object();
    density.set("avgCps", finiteOr(r.density.avgCps, 0.0));
    density.set("p90Cps", finiteOr(r.density.p90Cps, 0.0));
    density.set("peak1sCps", finiteOr(r.density.peak1sCps, 0.0));
    density.set("peak5sCps", finiteOr(r.density.peak5sCps, 0.0));
    root.set("density", std::move(density));

    Value sections = Value::array();
    for (auto const& s : r.sections) {
        Value o = Value::object();
        o.set("from", clampPercent(s.from));
        o.set("to", clampPercent(s.to));
        o.set("gamemode", gamemodeName(s.gamemode));
        o.set("speed", static_cast<int>(s.speed));
        o.set("mini", s.mini);
        o.set("inputs", s.inputs);
        o.set("narrowestMs", finiteOr(s.narrowestMs, 0.0));
        o.set("medianMs", finiteOr(s.medianMs, 0.0));
        o.set("supported", s.supported);
        o.set("solved", s.solved);
        o.set("verified", s.verified);
        sections.push(std::move(o));
    }
    root.set("sections", std::move(sections));

    Value budget = Value::object();
    budget.set("cpuMs", finiteOr(r.budget.cpuMs, 0.0));
    budget.set("wallMs", finiteOr(r.budget.wallMs, 0.0));
    budget.set("pausedMs", finiteOr(r.budget.pausedMs, 0.0));
    budget.set("ticksSimulated", static_cast<double>(r.budget.ticksSimulated));
    budget.set("trials", r.budget.trials);
    budget.set("searchRestarts", r.budget.searchRestarts);
    budget.set("mode", cut(r.budget.mode));
    budget.set("recordSafe", r.budget.recordSafe);
    budget.set("budgetExhausted", r.budget.budgetExhausted);
    root.set("budget", std::move(budget));

    Value debug = Value::array();
    size_t n = 0;
    for (auto const& line : r.debug) {
        if (n++ >= 50) break;
        debug.push(cut(line));
    }
    root.set("debug", std::move(debug));
    return root;
}

std::string toJsonString(LevelSimResult const& r) { return json::stringify(toJson(r)); }

}  // namespace gprl::sim
