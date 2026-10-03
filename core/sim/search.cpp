#include "search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "../geometry_hash.hpp"
#include "../solver/settle.hpp"
#include "collision.hpp"
#include "physics.hpp"

namespace gprl::sim {

namespace settle = gprl::solver::settle;

// ---- level time over stretches the run did not simulate (M1 / M2) ----

double unitsPerTick(Speed speed) {
    SpeedParams const& sp = speedParams(speed);
    return gdFloat(sp.playerSpeed) * sp.speedMultiplier * kStepDt;
}

Speed speedAtX(World const& world, double x) {
    Speed s = world.start.speed;
    double bestX = -1e300;
    for (auto const& o : world.objects) {
        if (o.kind != ObjKind::SpeedChange || o.hidden) continue;
        double const ox = static_cast<double>(o.x);
        if (ox <= x && ox >= bestX) {
            bestX = ox;
            s = o.speed;
        }
    }
    return s;
}

double ticksAcross(World const& world, double x0, double x1, Speed speed) {
    if (!(x1 > x0)) return 0.0;
    std::vector<std::pair<double, Speed>> changes;
    for (auto const& o : world.objects) {
        if (o.kind != ObjKind::SpeedChange || o.hidden) continue;
        double const ox = static_cast<double>(o.x);
        if (ox > x0 && ox <= x1) changes.emplace_back(ox, o.speed);
    }
    std::stable_sort(changes.begin(), changes.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    double t = 0.0, x = x0;
    Speed s = speed;
    for (auto const& c : changes) {
        t += (c.first - x) / unitsPerTick(s);
        x = c.first;
        s = c.second;
    }
    t += (x1 - x) / unitsPerTick(s);
    return t;
}

// ---- shared helpers ----

RecordedTick recordTick(Engine const& engine) {
    PlayerState const& p = engine.player();
    RecordedTick t;
    t.step = engine.tick();
    t.x = static_cast<float>(p.x);
    t.y = static_cast<float>(p.y);
    t.yVelocity = static_cast<float>(p.yVelocity);
    t.upsideDown = p.upsideDown;
    t.mode = p.mode;
    t.mini = p.mini;
    t.speed = p.speed;
    t.onGround = p.onGround;
    t.held = p.held;
    t.dead = p.dead;
    return t;
}

void stepTickWithInputs(Engine& engine, std::vector<TickInput> const& inputs) {
    if (inputs.empty()) {
        engine.step();
        return;
    }
    double const tickStart = static_cast<double>(engine.tick());
    double pos = 0.0;
    bool wholeTickPending = true;
    for (auto const& in : inputs) {
        double frac = in.frame - tickStart;
        if (frac < 0.0) frac = 0.0;
        if (frac > 0.999999) frac = 0.999999;
        if (frac <= 1e-9 && pos <= 0.0) {
            // applied at the start of the step (processCommands of this step)
            engine.queueInput(in.down);
            continue;
        }
        if (frac > pos + 1e-9) {
            engine.stepPart(frac - pos);
            pos = frac;
            wholeTickPending = false;
        }
        engine.queueInput(in.down);
        engine.applyQueuedInput();
    }
    if (wholeTickPending && pos <= 0.0) engine.step();
    else if (pos < 1.0) engine.stepPart(1.0 - pos);
}

std::vector<RecordedTick> replayTrajectory(World const& world, StartState const& start, std::vector<RecordedInput> const& inputs, int maxTicks,
                                           uint64_t* ticksSimulated) {
    std::vector<RecordedTick> out;
    Engine engine(&world);
    engine.reset(start);
    size_t idx = 0;
    std::vector<TickInput> tickInputs;
    for (int s = 1; s <= maxTicks; ++s) {
        tickInputs.clear();
        while (idx < inputs.size() && inputs[idx].step <= s) {
            if (inputs[idx].step == s) tickInputs.push_back({static_cast<double>(s - 1) + std::clamp(inputs[idx].subTick, 0.0, 0.999999), inputs[idx].down});
            ++idx;   // inputs before the first step (step < 1) are dropped
        }
        stepTickWithInputs(engine, tickInputs);
        if (ticksSimulated) ++*ticksSimulated;
        out.push_back(recordTick(engine));
        if (engine.dead() || engine.completed()) break;
    }
    return out;
}

namespace {

void mixInt(uint64_t& h, int64_t v) {
    unsigned char bytes[8];
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
    h = fnv1a64(bytes, 8, h);
}

int64_t qr(double v, double scale) { return std::isfinite(v) ? static_cast<int64_t>(std::llround(v * scale)) : 0; }

}  // namespace

uint64_t trajectoryDigest(std::vector<RecordedTick> const& ticks, std::vector<RecordedInput> const& inputs) {
    uint64_t h = 14695981039346656037ull;
    mixInt(h, static_cast<int64_t>(ticks.size()));
    for (auto const& t : ticks) {
        mixInt(h, t.step);
        mixInt(h, qr(t.x, 100.0));
        mixInt(h, qr(t.y, 100.0));
        mixInt(h, qr(t.yVelocity, 1000.0));
        int64_t flags = (t.upsideDown ? 1 : 0) | (t.mini ? 2 : 0) | (t.onGround ? 4 : 0) | (t.held ? 8 : 0) | (t.dead ? 16 : 0);
        mixInt(h, flags | (static_cast<int64_t>(t.mode) << 8) | (static_cast<int64_t>(t.speed) << 16));
    }
    mixInt(h, static_cast<int64_t>(inputs.size()));
    for (auto const& in : inputs) {
        mixInt(h, in.step);
        mixInt(h, qr(in.subTick, 64.0));
        mixInt(h, in.down ? 1 : 0);
    }
    return h;
}

void refreshRun(World const& world, ReferenceRun& run) {
    run.inputs.clear();
    run.ticks.clear();
    int offset = 0;
    double covered = 0.0;
    for (auto& seg : run.segments) {
        offset += std::max(0, seg.gapTicksBefore);   // the skipped stretch's level time (M2)
        seg.tickOffset = offset;
        for (auto const& in : seg.inputs) {
            RecordedInput g = in;
            g.step += offset;
            run.inputs.push_back(g);
        }
        for (auto const& t : seg.trajectory) {
            RecordedTick g = t;
            g.step += offset;
            run.ticks.push_back(g);
        }
        offset += seg.ticks;
        covered += std::max(0.0, static_cast<double>(seg.x1) - static_cast<double>(seg.x0));
    }
    double const endX = world.endX > 0.f ? static_cast<double>(world.endX) : 1.0;
    run.solvedPercent = std::clamp(covered / endX * 100.0, 0.0, 100.0);
    run.trajectoryDigest = trajectoryDigest(run.ticks, run.inputs);
}

// ---- the search ----

namespace {

struct InputNode {
    int step = 0;
    bool down = true;
    int parent = -1;
};

struct Branch {
    EngineSnapshot snap;
    int node = -1;             // last input (InputNode index), -1 = none
    int inputs = 0;
    int lastInputTick = 0;     // tick (after the step) of the last input; 0 = none since the segment start
    settle::SettleState settle{};
    bool settled = false;
    int coast = 0;             // rollout survival in ticks, capped at H
    int coastAge = 0;          // ticks since the rollout (only meaningful when coast == H)
    double x = 0.0;
    double score = 0.0;
    uint64_t hash = 0;
};

struct Checkpoint {
    EngineSnapshot snap;
    std::vector<RecordedInput> inputs;
    settle::SettleState settle{};
    bool settled = false;
    int lastInputTick = 0;
    int inputCount = 0;
    double x = 0.0;
    int tick = 0;
    int bucket = 0;
    int maxWidthRestarts = 0;
};

constexpr size_t kCompactAt = size_t(1) << 20;

}  // namespace

struct Search::Impl {
    World const* world;
    SearchConfig cfg;
    Engine engine;
    ReferenceRun run;
    std::vector<std::string> dbg;

    std::vector<InputNode> nodes;
    std::vector<Branch> beam;
    std::vector<Branch> next;
    std::vector<Checkpoint> cps;         // checkpoints of the current segment
    int width = 48;
    int backtracked = 0;
    uint64_t ticks = 0;
    bool finished = false;
    double lastX = 0.0;

    // the current segment
    ReferenceSegment seg;
    int lastBucket = -1;
    int maxBucket = -1;                  // the farthest checkpoint bucket reached in this segment (real progress)
    int maxSettledBucket = -1;           // the farthest SETTLED checkpoint bucket
    Checkpoint farthest;                 // the farthest settled checkpoint: the give-up commit point (a run that ends
                                         // mid-air inside the impossible stretch would understate the unsolved span)
    bool hasFarthest = false;
    double farthestDeathX = 0.0;
    PlayerState lastState;               // the beam leader's state (its y orders a flyer's restart candidates)
    std::vector<int> stateObjects;       // gamemode / speed / size / gravity objects, sorted by x (stable)

    Impl(World const* w, SearchConfig c) : world(w), cfg(c), engine(w) {
        width = std::max(1, cfg.beamWidth);
        if (cfg.beamWidthMax < width) cfg.beamWidthMax = width;
        if (cfg.lookaheadTicks < 1) cfg.lookaheadTicks = 1;
        if (cfg.checkpointEveryPercent <= 0.0) cfg.checkpointEveryPercent = 1.0;
        if (cfg.startProbeTicks < 1) cfg.startProbeTicks = 1;
        for (int i = 0; i < static_cast<int>(world->objects.size()); ++i) {
            ObjKind const k = world->objects[static_cast<size_t>(i)].kind;
            if (k == ObjKind::GamemodePortal || k == ObjKind::SpeedChange || k == ObjKind::SizePortal || k == ObjKind::GravityPortal) stateObjects.push_back(i);
        }
        std::stable_sort(stateObjects.begin(), stateObjects.end(),
                         [&](int a, int b) { return world->objects[static_cast<size_t>(a)].x < world->objects[static_cast<size_t>(b)].x; });
        beginSegment(world->start, "level", false);
    }

    void log(std::string const& s) {
        if (dbg.size() < 200) dbg.push_back(s);
    }

    double endX() const { return world->endX > 0.f ? static_cast<double>(world->endX) : 1.0; }
    int bucketOf(double x) const { return static_cast<int>(std::floor(x / endX() * 100.0 / cfg.checkpointEveryPercent)); }

    // ---- input chains ----

    std::vector<RecordedInput> materialize(int node) const {
        std::vector<RecordedInput> out;
        for (int n = node; n >= 0; n = nodes[static_cast<size_t>(n)].parent) {
            RecordedInput in;
            in.step = nodes[static_cast<size_t>(n)].step;
            in.subTick = 0.0;
            in.down = nodes[static_cast<size_t>(n)].down;
            in.button = 1;
            out.push_back(in);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    int chainOf(std::vector<RecordedInput> const& inputs) {
        int node = -1;
        for (auto const& in : inputs) {
            nodes.push_back({in.step, in.down, node});
            node = static_cast<int>(nodes.size()) - 1;
        }
        return node;
    }

    void compactNodes() {
        if (nodes.size() < kCompactAt) return;
        std::vector<int> remap(nodes.size(), -1);
        std::vector<char> live(nodes.size(), 0);
        for (auto const& b : beam)
            for (int n = b.node; n >= 0 && !live[static_cast<size_t>(n)]; n = nodes[static_cast<size_t>(n)].parent) live[static_cast<size_t>(n)] = 1;
        std::vector<InputNode> fresh;
        fresh.reserve(nodes.size() / 4 + 16);
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!live[i]) continue;
            remap[i] = static_cast<int>(fresh.size());
            InputNode n = nodes[i];
            n.parent = n.parent >= 0 ? remap[static_cast<size_t>(n.parent)] : -1;   // parents precede children
            fresh.push_back(n);
        }
        for (auto& b : beam) b.node = b.node >= 0 ? remap[static_cast<size_t>(b.node)] : -1;
        nodes.swap(fresh);
    }

    // ---- engine helpers ----

    int rollout(EngineSnapshot const& s) {
        int const H = cfg.lookaheadTicks;
        engine.restore(s);
        for (int i = 1; i <= H; ++i) {
            engine.step();
            ++ticks;
            if (engine.dead()) return i - 1;
            if (engine.completed() || engine.inUnsupportedSpan()) return H;
        }
        return H;
    }

    double scoreOf(Branch const& b) const {
        return static_cast<double>(std::min(b.coast, cfg.lookaheadTicks)) - cfg.inputPenalty * b.inputs + (b.settled ? cfg.settledBonus : 0.0);
    }

    void updateSettle(Branch& b) {
        settle::StepFacts f;
        f.flyingMode = engine.flying();
        f.onGround = engine.player().onGround;
        f.dashing = engine.player().dashing;
        f.touchingRing = engine.touchingRing();
        f.frameDone = static_cast<double>(engine.tick());
        f.lastMovedFrame = static_cast<double>(b.lastInputTick);
        b.settled = settle::update(b.settle, f);
    }

    Branch freshBranch() {
        Branch b;
        b.snap = engine.save();
        b.x = engine.player().x;
        b.hash = engine.stateHash();
        b.coast = rollout(b.snap);
        b.coastAge = 0;
        b.score = scoreOf(b);
        return b;
    }

    // ---- segments ----

    void beginSegment(StartState const& start, char const* kind, bool synthetic) {
        seg = ReferenceSegment{};
        seg.start = start;
        seg.startKind = kind;
        seg.syntheticStart = synthetic;
        if (!run.segments.empty()) {
            // M2: the level time of the skipped stretch is integrated over its speed objects, so
            // window ticks / tSeconds after a gap stay level ticks (not "simulated ticks so far")
            ReferenceSegment const& prev = run.segments.back();
            double const fromX = static_cast<double>(prev.x1);
            double const gap = ticksAcross(*world, fromX, static_cast<double>(start.x), speedAtX(*world, fromX));
            seg.gapTicksBefore = static_cast<int>(std::llround(std::max(0.0, gap)));
            seg.tickOffset = prev.tickOffset + prev.ticks + seg.gapTicksBefore;
        }
        seg.x0 = start.x;
        seg.x1 = start.x;
        cps.clear();
        beam.clear();
        backtracked = 0;
        hasFarthest = false;
        farthestDeathX = start.x;
        lastX = start.x;
        engine.reset(start);
        lastState = engine.player();
        lastBucket = bucketOf(start.x);
        maxBucket = lastBucket;
        maxSettledBucket = lastBucket;
        if (engine.inUnsupportedSpan()) {
            // the start itself lies in a span: nothing to simulate here
            skipUnsupportedFrom(start.x, nullptr);
            return;
        }
        beam.push_back(freshBranch());
    }

    // ---- restart states after a skipped stretch (H1) ----

    struct JumpStart {
        StartState s;
        char const* kind = "synthetic";
        bool synthetic = true;
    };

    bool inAnySpan(double x) const {
        for (auto const& u : world->unsupported)
            if (x >= static_cast<double>(u.x0) && x < static_cast<double>(u.x1)) return true;
        return false;
    }

    /// The player state the level's objects imply at x: the level start's, then every gamemode /
    /// speed / size / gravity object with o.x <= x in x order (a speed object consumed by
    /// Engine::reset behind the start is thereby applied, not lost).
    StartState stateAtX(double x) const {
        StartState s = world->start;
        for (int i : stateObjects) {
            SimObject const& o = world->objects[static_cast<size_t>(i)];
            if (static_cast<double>(o.x) > x) break;
            if (o.hidden) continue;
            switch (o.kind) {
                case ObjKind::GamemodePortal: s.mode = o.mode; break;
                case ObjKind::SpeedChange: s.speed = o.speed; break;
                case ObjKind::SizePortal: s.mini = o.flag; break;
                case ObjKind::GravityPortal: s.upsideDown = o.flag; break;
                default: break;
            }
        }
        s.x = static_cast<float>(x);
        s.percent = static_cast<float>(x / endX() * 100.0);
        return s;
    }

    /// The player rect at (x, y), inset by 0.5 (standing ON a block touches it), overlaps no solid /
    /// hazard / slope.
    bool freeAt(double x, double y, double half) const {
        double const h = std::max(0.0, half - 0.5);
        Rect const pr{x - h, y - h, x + h, y + h};
        for (auto const& o : world->objects) {
            if (static_cast<double>(o.left()) > pr.x1) break;   // sorted by left
            if (o.hidden) continue;
            if (o.kind != ObjKind::Solid && o.kind != ObjKind::Hazard && o.kind != ObjKind::Slope) continue;
            if (intersects(pr, objectRect(o))) return false;
        }
        return true;
    }

    /// y candidates of a synthetic start, most plausible first (see search.hpp).
    std::vector<double> candidateYs(StartState const& s, double x) const {
        double const floorY = world->groundY > 0.f ? static_cast<double>(world->groundY) : kConstants.floorY;
        double const half = playerHalfSize(s.mode, s.mini);
        std::vector<double> out;
        if (!flyingMode(s.mode)) {
            std::vector<double> surfaces;
            for (auto const& o : world->objects) {
                if (static_cast<double>(o.left()) > x) break;
                if (o.hidden || o.kind != ObjKind::Solid || static_cast<double>(o.right()) < x) continue;
                surfaces.push_back(s.upsideDown ? static_cast<double>(o.bottom()) - half : static_cast<double>(o.top()) + half);
            }
            if (!s.upsideDown) {
                out.push_back(floorCentreY(s.mode, s.mini, floorY));
                std::sort(surfaces.begin(), surfaces.end());
            } else {
                if (world->ceilingY > 0.f) out.push_back(ceilingCentreY(s.mode, s.mini, world->ceilingY));
                std::sort(surfaces.begin(), surfaces.end(), std::greater<double>());
            }
            for (double y : surfaces)
                if (out.empty() || std::fabs(out.back() - y) > 0.5) out.push_back(y);
        } else {
            double const top = world->ceilingY > 0.f ? static_cast<double>(world->ceilingY) : floorY + 600.0;
            double const wx0 = x - half, wx1 = x + half + 60.0;
            std::vector<std::pair<double, double>> blocked;
            for (auto const& o : world->objects) {
                if (static_cast<double>(o.left()) > wx1) break;
                if (o.hidden || (o.kind != ObjKind::Solid && o.kind != ObjKind::Hazard && o.kind != ObjKind::Slope)) continue;
                if (static_cast<double>(o.right()) < wx0) continue;
                blocked.emplace_back(static_cast<double>(o.bottom()), static_cast<double>(o.top()));
            }
            std::sort(blocked.begin(), blocked.end());
            std::vector<std::pair<double, double>> gaps;
            double a = floorY;
            for (auto const& b : blocked) {
                if (b.first > a) gaps.emplace_back(a, std::min(b.first, top));
                a = std::max(a, b.second);
                if (a >= top) break;
            }
            if (a < top) gaps.emplace_back(a, top);
            std::vector<std::pair<double, double>> mids;   // (order key, centre)
            bool const haveLast = flyingMode(lastState.mode) && lastState.y > 0.0;
            for (auto const& g : gaps) {
                if (g.second - g.first <= 2.0 * half + 2.0) continue;
                double const c = 0.5 * (g.first + g.second);
                mids.emplace_back(haveLast ? std::fabs(c - lastState.y) : -(g.second - g.first), c);
            }
            std::stable_sort(mids.begin(), mids.end(), [](auto const& p, auto const& q) { return p.first < q.first; });
            for (auto const& m : mids) out.push_back(m.second);
            if (out.empty()) out.push_back(floorCentreY(s.mode, s.mini, floorY));
        }
        return out;
    }

    /// The candidate survives `startProbeTicks` ticks with no input (hold = false) or held.
    bool probeStart(StartState const& c, bool hold) {
        engine.reset(c);
        if (hold) engine.queueInput(true);
        for (int i = 0; i < cfg.startProbeTicks; ++i) {
            engine.step();
            ++ticks;
            if (engine.dead()) return false;
            if (engine.completed() || engine.inUnsupportedSpan()) return true;
        }
        return true;
    }

    /// Where and how a new segment starts at `x` after a stretch ending at `stretchEnd` (H1).
    JumpStart jumpStart(double x, double stretchEnd) {
        JumpStart j;
        for (auto const& sp : world->startPositions) {
            double const sx = static_cast<double>(sp.x);
            if (sx < stretchEnd - 1e-3 || sx > x + static_cast<double>(cfg.startPosReach) || sx >= endX()) continue;
            if (sp.dual || sp.mirror || sp.reversed || sp.platformer || sp.twoPlayer) continue;
            if (inAnySpan(sx)) continue;
            j.s = sp;
            j.s.percent = static_cast<float>(sx / endX() * 100.0);
            j.kind = "startpos";
            j.synthetic = false;
            char buf[160];
            std::snprintf(buf, sizeof buf, "startpos start x %.0f: %s%s%s speed %d y %.1f", sx, gamemodeName(sp.mode), sp.mini ? " mini" : "",
                          sp.upsideDown ? " flipped" : "", static_cast<int>(sp.speed), static_cast<double>(sp.y));
            log(buf);
            return j;
        }
        j.s = stateAtX(x);
        std::vector<double> cands = candidateYs(j.s, x);
        double const half = playerHalfSize(j.s.mode, j.s.mini);
        std::vector<double> free;
        for (double y : cands)
            if (freeAt(x, y, half)) free.push_back(y);
        if (free.empty()) free = cands;
        size_t chosen = free.size();
        for (size_t k = 0; k < free.size() && chosen == free.size(); ++k) {
            StartState c = j.s;
            c.y = static_cast<float>(free[k]);
            if (probeStart(c, false) || probeStart(c, true)) chosen = k;
        }
        bool const survived = chosen < free.size();
        if (!survived) chosen = 0;
        j.s.y = static_cast<float>(free.empty() ? static_cast<double>(world->start.y) : free[chosen]);
        char buf[200];
        std::snprintf(buf, sizeof buf, "synthetic start x %.0f: %s%s%s speed %d y %.1f (candidate %zu of %zu%s)", x, gamemodeName(j.s.mode), j.s.mini ? " mini" : "",
                      j.s.upsideDown ? " flipped" : "", static_cast<int>(j.s.speed), static_cast<double>(j.s.y), chosen + 1, free.size(),
                      survived ? "" : ", none survived the probe");
        log(buf);
        return j;
    }

    /// Commits the current segment ending at `branchInputs` / `tick` / `x` and appends it to the run.
    void commitSegment(std::vector<RecordedInput> inputs, int tick, double x, bool completed, char const* reason) {
        seg.inputs = std::move(inputs);
        seg.ticks = tick;
        seg.x1 = static_cast<float>(x);
        seg.completed = completed;
        seg.endReason = reason;
        seg.trajectory = tick > 0 ? replayTrajectory(*world, seg.start, seg.inputs, tick, &ticks) : std::vector<RecordedTick>{};
        if (tick > 0) {
            // invariant: the committed inputs replayed plainly reach the same end the beam reached
            // through save / restore; a miss is an engine identity defect, reported, never hidden
            bool const reproduced = !seg.trajectory.empty() && !seg.trajectory.back().dead && static_cast<int>(seg.trajectory.size()) == tick
                && (completed ? seg.trajectory.back().x >= endX() - 1e-3 : seg.trajectory.back().x >= x - 1.0);
            if (!reproduced) {
                ++run.replayMismatches;
                char buf[200];
                std::snprintf(buf, sizeof buf, "REPLAY MISMATCH: the beam reached x %.0f at tick %d but the plain replay of its %zu inputs %s at tick %zu (x %.0f)", x, tick,
                              seg.inputs.size(), seg.trajectory.empty() || !seg.trajectory.back().dead ? "ends" : "dies", seg.trajectory.size(),
                              seg.trajectory.empty() ? static_cast<double>(seg.x0) : static_cast<double>(seg.trajectory.back().x));
                log(buf);
            }
        }
        if (!seg.trajectory.empty()) seg.x1 = std::max(seg.x1, seg.trajectory.back().x);
        if (seg.x1 < seg.x0) seg.x1 = seg.x0;
        int const offset = seg.tickOffset;
        for (auto const& in : seg.inputs) {
            RecordedInput g = in;
            g.step += offset;
            run.inputs.push_back(g);
        }
        for (auto const& t : seg.trajectory) {
            RecordedTick g = t;
            g.step += offset;
            run.ticks.push_back(g);
        }
        run.segments.push_back(seg);
        char buf[200];
        std::snprintf(buf, sizeof buf, "segment %zu: x %.0f..%.0f, %d ticks, %zu inputs, %s", run.segments.size(), static_cast<double>(seg.x0),
                      static_cast<double>(seg.x1), seg.ticks, seg.inputs.size(), reason);
        log(buf);
    }

    CoverageSpan spanOf(double x0, double x1, std::string mechanic) const {
        CoverageSpan s;
        s.x0 = static_cast<float>(x0);
        s.x1 = static_cast<float>(x1);
        s.percentFrom = std::clamp(x0 / endX() * 100.0, 0.0, 100.0);
        s.percentTo = std::clamp(x1 / endX() * 100.0, 0.0, 100.0);
        s.mechanic = std::move(mechanic);
        return s;
    }

    /// Where a new segment may start after `x`: past every unsupported span that covers it.
    double pastSpans(double x) const {
        bool moved = true;
        int guard = 0;
        while (moved && guard++ < 1000) {
            moved = false;
            for (auto const& u : world->unsupported) {
                if (x >= u.x0 && x < u.x1) {
                    x = static_cast<double>(u.x1) + cfg.jumpMargin;
                    moved = true;
                }
            }
        }
        return x;
    }

    void jumpTo(double x, double stretchEnd) {
        if (x >= endX()) {
            finalize();
            return;
        }
        JumpStart const j = jumpStart(x, stretchEnd);
        if (static_cast<double>(j.s.x) >= endX()) {
            finalize();
            return;
        }
        beginSegment(j.s, j.kind, j.synthetic);
    }

    /// A branch entered an UnsupportedSpan at `x` (or the segment start lies in one).
    void skipUnsupportedFrom(double x, Branch const* entered) {
        UnsupportedSpan const* span = nullptr;
        for (auto const& u : world->unsupported)
            if (x >= u.x0 - 1e-6 && x < u.x1) {
                span = &u;
                break;
            }
        if (!span) {
            // the engine says "in a span" but the world lists none at x: take the nearest ahead
            for (auto const& u : world->unsupported)
                if (u.x1 > x && (!span || u.x0 < span->x0)) span = &u;
        }
        double x0 = span ? std::max(static_cast<double>(span->x0), std::min(x, static_cast<double>(span->x1))) : x;
        double x1 = span ? static_cast<double>(span->x1) : x + cfg.jumpMargin;
        std::string mechanic = span ? span->mechanic : std::string("unsupported");
        if (entered) {
            lastState = entered->snap.player;
            commitSegment(materialize(entered->node), entered->snap.tick, entered->x, false, "unsupported");
        }
        else {
            commitSegment({}, 0, x, false, "unsupported");
        }
        run.skippedUnsupported.push_back(spanOf(x0, x1, mechanic));
        jumpTo(pastSpans(x1 + cfg.jumpMargin), x1);
    }

    void recordUnsolved(double x0, double x1, char const* mechanic) {
        if (x1 < x0 + 1.0) x1 = x0 + 1.0;
        run.unsolved.push_back(spanOf(x0, std::min(x1, endX()), mechanic));
        char buf[160];
        std::snprintf(buf, sizeof buf, "unsolved %.0f..%.0f (%s), beam %d", x0, x1, mechanic, width);
        log(buf);
    }

    // ---- checkpoints / restarts ----

    void setCheckpoint(int bucket) {
        Branch const* pick = nullptr;
        for (auto const& b : beam)
            if (b.settled) {
                pick = &b;
                break;
            }
        bool settled = pick != nullptr;
        if (!pick) pick = &beam.front();
        Checkpoint cp;
        cp.snap = pick->snap;
        cp.inputs = materialize(pick->node);
        cp.settle = pick->settle;
        cp.settled = pick->settled;
        cp.lastInputTick = pick->lastInputTick;
        cp.inputCount = pick->inputs;
        cp.x = pick->x;
        cp.tick = pick->snap.tick;
        cp.bucket = bucket;
        if (bucket > maxBucket) {
            // real progress (not a re-run over ground already covered): the backtrack budget renews
            maxBucket = bucket;
            backtracked = 0;
        }
        if (settled && bucket > maxSettledBucket) {
            maxSettledBucket = bucket;
            farthest = cp;
            hasFarthest = true;
        }
        cps.push_back(std::move(cp));
        lastBucket = bucket;
        ++run.checkpoints;
        if (!settled) ++run.unsettledCheckpoints;
        width = relaxed(width);
    }

    int relaxed(int w) const { return w > cfg.beamWidth ? std::max(cfg.beamWidth, w / 2) : cfg.beamWidth; }

    void restartFrom(Checkpoint const& cp) {
        beam.clear();
        next.clear();
        nodes.clear();
        Branch b;
        b.snap = cp.snap;
        b.node = chainOf(cp.inputs);
        b.inputs = cp.inputCount;
        b.lastInputTick = cp.lastInputTick;
        b.settle = cp.settle;
        b.settled = cp.settled;
        b.x = cp.x;
        engine.restore(cp.snap);
        b.hash = engine.stateHash();
        b.coast = rollout(cp.snap);
        b.score = scoreOf(b);
        beam.push_back(std::move(b));
        lastBucket = cp.bucket;
        lastX = cp.x;
    }

    void restartFromSegmentStart() {
        beam.clear();
        next.clear();
        nodes.clear();
        engine.reset(seg.start);
        beam.push_back(freshBranch());
        lastBucket = bucketOf(seg.start.x);
        lastX = seg.start.x;
    }

    void handleEmptyBeam() {
        ++run.restarts;
        if (run.restarts > cfg.maxRestarts) {
            giveUpSegment("search_exhausted");
            return;
        }
        if (cps.empty()) {
            if (width < cfg.beamWidthMax) {
                width = widened(width);
                char buf[120];
                std::snprintf(buf, sizeof buf, "restart from the segment start (x %.0f) with beam %d", static_cast<double>(seg.start.x), width);
                log(buf);
                restartFromSegmentStart();
                return;
            }
            giveUpSegment("search_exhausted");
            return;
        }
        if (width < cfg.beamWidthMax) {
            width = widened(width);
            char buf[120];
            std::snprintf(buf, sizeof buf, "restart from checkpoint x %.0f with beam %d", cps.back().x, width);
            log(buf);
            restartFrom(cps.back());
            return;
        }
        // At the maximum width a restart from the same checkpoint repeats the same deterministic
        // failure, so every checkpoint is restarted at the cap once; after that the search backs
        // up one checkpoint, at most maxCheckpointBacktrack times per stretch of real progress.
        while (cps.size() > 1 && cps.back().maxWidthRestarts >= 1 && backtracked < cfg.maxCheckpointBacktrack) {
            cps.pop_back();
            ++backtracked;
        }
        Checkpoint& cp = cps.back();
        if (cp.maxWidthRestarts >= 1) {
            giveUpSegment("search_exhausted");
            return;
        }
        ++cp.maxWidthRestarts;
        char buf[120];
        std::snprintf(buf, sizeof buf, "restart from checkpoint x %.0f at the maximum beam %d (backtracked %d)", cp.x, width, backtracked);
        log(buf);
        restartFrom(cp);
    }

    int widened(int w) const { return w >= cfg.beamWidthMax ? cfg.beamWidthMax : std::min(cfg.beamWidthMax, w * 2); }

    /// The segment cannot be solved past its last checkpoint: commit up to there, record the
    /// unsolved stretch to the farthest death, and jump past it.
    void giveUpSegment(char const* mechanic) {
        double from;
        if (hasFarthest) {
            Checkpoint const& cp = farthest;
            engine.restore(cp.snap);
            lastState = engine.player();
            commitSegment(cp.inputs, cp.tick, cp.x, false, "unsolved");
            from = cp.x;
        }
        else {
            commitSegment({}, 0, seg.start.x, false, "unsolved");
            from = seg.start.x;
        }
        double to = std::max(farthestDeathX, from);
        recordUnsolved(from, to, mechanic);
        width = cfg.beamWidth;
        jumpTo(pastSpans(to + cfg.jumpMargin), to);
    }

    void completeAt(Branch const& b) {
        lastState = b.snap.player;
        commitSegment(materialize(b.node), b.snap.tick, endX(), true, "completed");
        run.reachedEnd = true;
        finalize();
    }

    void finalize() {
        if (finished) return;
        finished = true;
        beam.clear();
        next.clear();
        double covered = 0.0;
        for (auto const& s : run.segments) covered += std::max(0.0, static_cast<double>(s.x1) - static_cast<double>(s.x0));
        run.solvedPercent = std::clamp(covered / endX() * 100.0, 0.0, 100.0);
        run.completed = run.reachedEnd && run.unsolved.empty() && run.skippedUnsupported.empty();
        run.beamWidthFinal = width;
        run.ticksSimulated = ticks;
        run.trajectoryDigest = trajectoryDigest(run.ticks, run.inputs);
        char buf[200];
        std::snprintf(buf, sizeof buf, "done: %s, solved %.1f %%, %zu inputs, %zu ticks, %d restarts, %d checkpoints (%d unsettled), %llu engine ticks",
                      run.completed ? "completed" : (run.reachedEnd ? "reached the end with gaps" : "stopped"), run.solvedPercent, run.inputs.size(),
                      run.ticks.size(), run.restarts, run.checkpoints, run.unsettledCheckpoints, static_cast<unsigned long long>(ticks));
        log(buf);
    }

    // ---- one beam tick ----

    uint64_t cellOf(Branch const& b) const {
        PlayerState const& p = b.snap.player;
        uint64_t h = 14695981039346656037ull;
        mixInt(h, static_cast<int64_t>(p.mode));
        mixInt(h, p.held ? 1 : 0);
        mixInt(h, p.upsideDown ? 1 : 0);
        mixInt(h, qr(p.y, 1.0 / 16.0));
        mixInt(h, qr(p.yVelocity, 0.5));
        return h;
    }

    void stepBeam() {
        next.clear();
        bool entered = false;
        bool completedNow = false;
        Branch winner;
        for (auto const& b : beam) {
            for (int d = 0; d < 2 && !completedNow; ++d) {
                bool const held = b.snap.player.held;
                engine.restore(b.snap);
                if (d == 1) engine.queueInput(!held);   // press when up, release when down
                engine.step();
                ++ticks;
                if (engine.dead()) {
                    farthestDeathX = std::max(farthestDeathX, engine.player().x);
                    continue;
                }
                Branch c;
                c.node = b.node;
                c.inputs = b.inputs;
                c.lastInputTick = b.lastInputTick;
                c.settle = b.settle;
                if (d == 1) {
                    nodes.push_back({engine.tick(), !held, b.node});
                    c.node = static_cast<int>(nodes.size()) - 1;
                    c.inputs = b.inputs + 1;
                    c.lastInputTick = engine.tick();
                }
                c.snap = engine.save();
                c.x = engine.player().x;
                c.hash = engine.stateHash();
                updateSettle(c);
                if (engine.completed()) {
                    winner = c;
                    completedNow = true;
                    break;
                }
                if (engine.inUnsupportedSpan()) {
                    if (!entered) {
                        next.insert(next.begin(), c);   // the first in rank order wins below
                        entered = true;
                    }
                    continue;
                }
                if (d == 0 && b.coast < cfg.lookaheadTicks) {
                    c.coast = std::max(0, b.coast - 1);
                    c.coastAge = 0;
                }
                else if (d == 0 && b.coastAge + 1 < std::max(1, cfg.lookaheadTicks / 2)) {
                    c.coast = b.coast;
                    c.coastAge = b.coastAge + 1;
                }
                else {
                    c.coast = rollout(c.snap);
                    c.coastAge = 0;
                }
                c.score = scoreOf(c);
                next.push_back(std::move(c));
            }
            if (completedNow) break;
        }
        if (completedNow) {
            completeAt(winner);
            return;
        }
        if (entered) {
            Branch e = next.front();
            skipUnsupportedFrom(e.x, &e);
            return;
        }
        if (next.empty()) {
            handleEmptyBeam();
            return;
        }
        // rank: x progress (whole units), then the score; stable for the rest
        std::stable_sort(next.begin(), next.end(), [](Branch const& a, Branch const& b) {
            int64_t xa = static_cast<int64_t>(std::llround(a.x));
            int64_t xb = static_cast<int64_t>(std::llround(b.x));
            if (xa != xb) return xa > xb;
            return a.score > b.score;
        });
        // de-duplicate by state hash (first = best wins); the held flag is mixed in explicitly so
        // a release child is never folded into its held twin whatever the engine's hash covers
        auto dedupKey = [](Branch const& c) { return c.hash ^ (c.snap.player.held ? 0x9e3779b97f4a7c15ull : 0ull); };
        std::unordered_set<uint64_t> seen;
        seen.reserve(next.size() * 2);
        beam.clear();
        beam.reserve(static_cast<size_t>(width));
        if (next.size() > static_cast<size_t>(width) && cfg.diversityCap > 0) {
            std::unordered_map<uint64_t, int> cells;
            std::vector<Branch> overflow;
            for (auto& c : next) {
                if (!seen.insert(dedupKey(c)).second) continue;
                int& n = cells[cellOf(c)];
                if (n < cfg.diversityCap) {
                    ++n;
                    if (beam.size() < static_cast<size_t>(width)) beam.push_back(std::move(c));
                }
                else overflow.push_back(std::move(c));
            }
            for (auto& c : overflow) {
                if (beam.size() >= static_cast<size_t>(width)) break;
                beam.push_back(std::move(c));
            }
        }
        else {
            for (auto& c : next) {
                if (!seen.insert(dedupKey(c)).second) continue;
                if (beam.size() >= static_cast<size_t>(width)) break;
                beam.push_back(std::move(c));
            }
        }
        next.clear();
        lastState = beam.front().snap.player;
        lastX = beam.front().x;
        int bucket = bucketOf(lastX);
        if (bucket > lastBucket) setCheckpoint(bucket);
        compactNodes();
    }

    bool runSome(std::function<bool()> const& mayContinue, uint64_t tickBudget) {
        uint64_t const start = ticks;
        while (!finished) {
            if (cfg.maxTicks > 0 && ticks >= cfg.maxTicks) {
                stopNow("budget");
                break;
            }
            if (tickBudget > 0 && ticks - start >= tickBudget) break;
            if (mayContinue && !mayContinue()) break;
            if (beam.empty()) {
                handleEmptyBeam();
                continue;
            }
            stepBeam();
        }
        return finished;
    }

    void stopNow(char const* reason) {
        if (finished) return;
        if (!beam.empty()) {
            Branch const& b = beam.front();
            lastState = b.snap.player;
            commitSegment(materialize(b.node), b.snap.tick, b.x, false, reason);
            recordUnsolved(b.x, endX(), reason);
        }
        else {
            commitSegment({}, 0, seg.start.x, false, reason);
            recordUnsolved(seg.start.x, endX(), reason);
        }
        finalize();
    }
};

Search::Search(World const* world, SearchConfig cfg) : m_impl(new Impl(world, cfg)) {}
Search::~Search() { delete m_impl; }

bool Search::run(std::function<bool()> const& mayContinue, uint64_t tickBudget) { return m_impl->runSome(mayContinue, tickBudget); }
void Search::stop(char const* reason) { m_impl->stopNow(reason ? reason : "stopped"); }
bool Search::done() const { return m_impl->finished; }
double Search::progress() const {
    if (m_impl->finished) return 1.0;
    return std::clamp(m_impl->lastX / m_impl->endX(), 0.0, 1.0);
}
uint64_t Search::ticksSimulated() const { return m_impl->ticks; }
int Search::beamWidth() const { return m_impl->width; }
ReferenceRun const& Search::result() const {
    if (!m_impl->finished) {
        m_impl->run.ticksSimulated = m_impl->ticks;
        m_impl->run.beamWidthFinal = m_impl->width;
    }
    return m_impl->run;
}
std::vector<std::string> const& Search::debug() const { return m_impl->dbg; }

}  // namespace gprl::sim
