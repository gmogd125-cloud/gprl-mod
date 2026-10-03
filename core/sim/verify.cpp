#include "verify.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "engine.hpp"
#include "physics.hpp"
#include "search.hpp"

namespace gprl::sim {

int verifyBinIndex(double percent, double binPercent) {
    if (binPercent <= 0.0) binPercent = 2.0;
    int const bins = static_cast<int>(std::ceil(100.0 / binPercent - 1e-9));
    if (!(percent >= 0.0)) return 0;
    int idx = static_cast<int>(std::floor(percent / binPercent));
    return std::clamp(idx, 0, bins - 1);
}

namespace {

std::vector<VerifyBin> emptyBins(World const& world, double binPercent) {
    int const bins = static_cast<int>(std::ceil(100.0 / binPercent - 1e-9));
    std::vector<VerifyBin> out(static_cast<size_t>(bins));
    double const endX = world.endX > 0.f ? static_cast<double>(world.endX) : 1.0;
    for (int i = 0; i < bins; ++i) {
        VerifyBin& b = out[static_cast<size_t>(i)];
        b.from = i * binPercent;
        b.to = std::min(100.0, (i + 1) * binPercent);
        double x0 = b.from / 100.0 * endX, x1 = b.to / 100.0 * endX;
        for (auto const& u : world.unsupported)
            if (u.x0 < x1 && u.x1 > x0) b.unsupported = true;
    }
    return out;
}

void finishShare(VerifyResult& r) {
    int counted = 0, verified = 0;
    for (auto const& b : r.bins) {
        if (b.ticks <= 0 || b.unsupported) continue;
        ++counted;
        if (b.verified) ++verified;
    }
    r.countedBins = counted;
    r.verifiedShare = counted > 0 ? static_cast<double>(verified) / counted : 0.0;
}

/// The union of the world's unsupported spans, sorted and merged (for a binary-search lookup).
std::vector<std::pair<double, double>> spanUnion(World const& world) {
    std::vector<std::pair<double, double>> v;
    for (auto const& u : world.unsupported)
        if (u.x1 > u.x0) v.emplace_back(static_cast<double>(u.x0), static_cast<double>(u.x1));
    std::sort(v.begin(), v.end());
    std::vector<std::pair<double, double>> out;
    for (auto const& s : v) {
        if (!out.empty() && s.first <= out.back().second) out.back().second = std::max(out.back().second, s.second);
        else out.push_back(s);
    }
    return out;
}

bool inUnion(std::vector<std::pair<double, double>> const& u, double x) {
    auto it = std::upper_bound(u.begin(), u.end(), x, [](double v, std::pair<double, double> const& s) { return v < s.first; });
    if (it == u.begin()) return false;
    --it;
    return x >= it->first && x < it->second;
}

/// Places the engine on a recorded tick (the state AFTER step rec.step) so the replay continues
/// from the real player's state past an unsupported span. Fields a RecordedTick does not carry are
/// inferred: the button held = jumpBuffered (the press already consumed), boosted = airborne and
/// rising faster than the falling-bugged threshold, the robot hover spent.
void reseed(Engine& engine, RecordedTick const& rec, RecordedTick const* prev) {
    StartState s;
    s.x = rec.x;
    s.y = rec.y;
    s.mode = rec.mode;
    s.mini = rec.mini;
    s.upsideDown = rec.upsideDown;
    s.speed = rec.speed;
    engine.reset(s);
    EngineSnapshot snap = engine.save();
    PlayerState& p = snap.player;
    p.yVelocity = static_cast<double>(rec.yVelocity);
    p.onGround = rec.onGround;
    p.onGround2 = rec.onGround;
    p.held = rec.held;
    p.jumpBuffered = rec.held;
    p.ringJumpArmed = false;
    p.boosted = !rec.onGround && !playerIsFallingBugged(p);
    p.robotHold = kConstants.robotHoldOnEnter;
    p.lastX = prev ? static_cast<double>(prev->x) : p.x;
    p.lastY = prev ? static_cast<double>(prev->y) : p.y;
    snap.tick = rec.step;
    snap.subTick = 0.0;
    engine.restore(snap);
}

}  // namespace

VerifyResult verifyAttempt(World const& world, RecordedAttempt const& attempt, double tolerance, VerifyConfig const& cfgIn) {
    VerifyConfig cfg = cfgIn;
    cfg.tolerance = tolerance;
    VerifyResult r;
    r.attempts = 1;
    r.tolerancePosition = cfg.tolerance;
    r.bins = emptyBins(world, cfg.binPercent);
    std::vector<char> binSeen(r.bins.size(), 0);   // a bin with ticks starts verified until a mismatch
    for (auto& b : r.bins) b.verified = false;

    std::vector<RecordedTick> recorded = attempt.ticks;
    std::stable_sort(recorded.begin(), recorded.end(), [](RecordedTick const& a, RecordedTick const& b) { return a.step < b.step; });
    std::vector<RecordedInput> inputs = attempt.inputs;
    std::stable_sort(inputs.begin(), inputs.end(), [](RecordedInput const& a, RecordedInput const& b) {
        if (a.step != b.step) return a.step < b.step;
        return a.subTick < b.subTick;
    });
    if (recorded.empty()) {
        finishShare(r);
        return r;
    }

    double const endX = world.endX > 0.f ? static_cast<double>(world.endX) : 1.0;
    auto binOf = [&](float x) { return verifyBinIndex(static_cast<double>(x) / endX * 100.0, cfg.binPercent); };
    std::vector<std::pair<double, double>> const spans = spanUnion(world);

    // Comparing: the engine follows the recording; Diverged: it lost it (after a stop) - the
    // recorded ticks count as after_divergence until the next span exit; InSpan: the recorded x is
    // inside an unsupported span - nothing is compared, the engine waits for the exit
    enum class Phase { Comparing, Diverged, InSpan };
    Phase phase = Phase::Comparing;
    Engine engine(&world);
    engine.reset(attempt.start);
    size_t inIdx = 0, recIdx = 0;
    int const lastStep = recorded.back().step;
    RecordedTick const* prevRec = nullptr;
    std::vector<TickInput> tickInputs;
    auto markAfter = [&](VerifyBin& bin, int bi, int step) {
        if (!binSeen[static_cast<size_t>(bi)] || bin.verified) {
            binSeen[static_cast<size_t>(bi)] = 1;
            bin.verified = false;
            bin.reason = "after_divergence";
            bin.firstDivergenceStep = step;
        }
    };
    for (int s = 1; s <= lastStep; ++s) {
        tickInputs.clear();
        while (inIdx < inputs.size() && inputs[inIdx].step <= s) {
            if (inputs[inIdx].step == s) tickInputs.push_back({static_cast<double>(s - 1) + std::clamp(inputs[inIdx].subTick, 0.0, 0.999999), inputs[inIdx].down});
            ++inIdx;
        }
        if (phase == Phase::Comparing) stepTickWithInputs(engine, tickInputs);   // a lost engine is re-seeded, not stepped
        while (recIdx < recorded.size() && recorded[recIdx].step < s) ++recIdx;
        if (recIdx >= recorded.size() || recorded[recIdx].step != s) {
            if (phase == Phase::Comparing && engine.dead()) phase = Phase::Diverged;   // nothing recorded here and the sim died
            continue;
        }
        RecordedTick const& rec = recorded[recIdx];
        ++recIdx;
        int const bi = binOf(rec.x);
        VerifyBin& bin = r.bins[static_cast<size_t>(bi)];
        ++bin.ticks;
        RecordedTick const* const before = prevRec;
        prevRec = &rec;
        if (inUnion(spans, static_cast<double>(rec.x))) {
            // inside an unsupported span: the mechanic is not modelled, nothing is compared
            if (!binSeen[static_cast<size_t>(bi)]) {
                binSeen[static_cast<size_t>(bi)] = 1;
                bin.verified = true;   // the bin is `unsupported` (emptyBins) and never counted either way
            }
            phase = Phase::InSpan;
            continue;
        }
        if (phase == Phase::InSpan) {
            // the first recorded tick past the span: continue from the real player's state
            reseed(engine, rec, before);
            ++r.reseeds;
            phase = Phase::Comparing;
            if (!binSeen[static_cast<size_t>(bi)]) {
                binSeen[static_cast<size_t>(bi)] = 1;
                bin.verified = true;
            }
            continue;
        }
        if (phase == Phase::Diverged) {
            markAfter(bin, bi, rec.step);
            continue;
        }
        ++r.ticksCompared;
        PlayerState const& p = engine.player();
        if (!binSeen[static_cast<size_t>(bi)]) {
            binSeen[static_cast<size_t>(bi)] = 1;
            bin.verified = true;
        }
        double const err = std::fabs(p.x - static_cast<double>(rec.x)) + std::fabs(p.y - static_cast<double>(rec.y));
        bin.maxError = std::max(bin.maxError, static_cast<float>(err));
        char const* reason = nullptr;
        if (err > cfg.tolerance) reason = "position";
        else {
            double const dv = std::fabs(p.yVelocity - static_cast<double>(rec.yVelocity));
            if (dv > cfg.yVelocityAbs && dv > cfg.yVelocityRel * std::fabs(static_cast<double>(rec.yVelocity))) reason = "y_velocity";
            else if (p.dead != rec.dead) reason = "dead";
            else if (p.mode != rec.mode) reason = "gamemode";
            else if (p.upsideDown != rec.upsideDown) reason = "gravity";
            else if (p.mini != rec.mini) reason = "size";
            else if (p.speed != rec.speed) reason = "speed";
            else if (p.onGround != rec.onGround) reason = "on_ground";
            else if (p.held != rec.held) reason = "held";
        }
        if (reason) {
            if (bin.verified) {
                bin.verified = false;
                bin.reason = reason;
                bin.firstDivergenceStep = s;
            }
        }
        if (engine.dead() && rec.dead) break;   // both died here: the attempt ends
        if (err > cfg.divergenceStop || (p.dead && !rec.dead)) phase = Phase::Diverged;
    }
    // recorded ticks past the replay's end (the loop broke on a shared death, or steps beyond the
    // last compared one): only the ones of a lost replay count, as after_divergence
    if (phase == Phase::Diverged) {
        while (recIdx < recorded.size()) {
            RecordedTick const& rec = recorded[recIdx++];
            if (inUnion(spans, static_cast<double>(rec.x))) continue;
            int const bi = binOf(rec.x);
            VerifyBin& bin = r.bins[static_cast<size_t>(bi)];
            markAfter(bin, bi, rec.step);
            ++bin.ticks;
        }
    }
    finishShare(r);
    return r;
}

VerifyResult mergeVerify(std::vector<VerifyResult> const& results) {
    VerifyResult out;
    if (results.empty()) return out;
    out.bins = results.front().bins;
    for (auto& b : out.bins) {
        b.ticks = 0;
        b.verified = false;
        b.maxError = 0.f;
        b.firstDivergenceStep = 0;
        b.reason.clear();
    }
    out.tolerancePosition = results.front().tolerancePosition;
    std::vector<char> seen(out.bins.size(), 0);
    for (auto const& r : results) {
        out.attempts += r.attempts;
        out.ticksCompared += r.ticksCompared;
        out.reseeds += r.reseeds;
        for (size_t i = 0; i < r.bins.size() && i < out.bins.size(); ++i) {
            VerifyBin const& b = r.bins[i];
            VerifyBin& o = out.bins[i];
            if (b.unsupported) o.unsupported = true;
            if (b.ticks <= 0) continue;
            o.ticks += b.ticks;
            o.maxError = std::max(o.maxError, b.maxError);
            if (!seen[i]) {
                seen[i] = 1;
                o.verified = b.verified;
            }
            else o.verified = o.verified && b.verified;
            if (!b.verified && o.reason.empty()) {
                o.reason = b.reason;
                o.firstDivergenceStep = b.firstDivergenceStep;
            }
        }
    }
    finishShare(out);
    return out;
}

}  // namespace gprl::sim
