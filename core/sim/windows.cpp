#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

#include "../geometry_hash.hpp"
#include "../solver/settle.hpp"
#include "engine.hpp"
#include "search.hpp"

namespace gprl::sim {

namespace settle = gprl::solver::settle;

namespace {

constexpr double kTickMs = 1000.0 / 240.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct ControlTick {
    uint64_t hash = 0;
    float x = 0.f;
    bool dead = false;
    bool flying = false;
    bool onGround = false;
    bool dashing = false;
    bool ring = false;
    Gamemode mode = Gamemode::Cube;
    Speed speed = Speed::Normal;
    bool mini = false;
};

enum class Outcome : uint8_t { Pass, Fail, NotTested };

struct Trial {
    Outcome outcome = Outcome::NotTested;
    bool resynced = false;
    double deathX = 0.0;
};

/// Sorted, sanitised copy of the inputs with their frame (tick coordinate of the application point).
struct Input {
    double frame = 0.0;   // (step - 1) + subTick
    int step = 0;
    bool down = true;
    uint8_t button = 1;
    int original = 0;     // index in the caller's list
};

struct Side {
    double limit = 0.0;         // |shift| limit in frames (neighbour / start / maxShift)
    bool neighbourLimited = false;
    double lastPass = 0.0;
    double firstFail = kNaN;
    double bracket = 0.0;       // firstFail - lastPass when bounded
    int tested = 0;
    bool bounded() const { return !std::isnan(firstFail); }
};

/// Where the walk of one side stands (resumable between trials, H7).
struct SideWalk {
    Side side;
    int stage = 0;              // 0 whole-tick shifts, 1 sub-tick refinement, 2 finished
    int mag = 1;                // next whole-tick magnitude
    int pass = 0;               // refinement pass
    bool passStarted = false;
    int j = 1;                  // next refinement point of the pass
    double lo = 0.0, width = 0.0, newPass = 0.0, newFail = 0.0;
};

}  // namespace

struct WindowsState {
    std::vector<Input> inputs;
    std::vector<ControlTick> control;                // index = tick (0 = the start state)
    std::map<int, EngineSnapshot> snapshots;         // by tick
    std::vector<int> baseTick;                       // per input
    std::vector<char> measurable;                    // per input
    bool completed = false;                          // the control reached endX
    int endTick = 0;                                 // last control tick
    // the window being measured (resumable between trials)
    bool windowOpen = false;
    int sideIndex = 0;                               // 0 early, 1 late
    SideWalk early, late;
};

gprl::GeometryHash worldGeometryHash(World const& world, double playerX) {
    std::vector<gprl::GeometryObject> objs;
    gprl::GeometryHashParams params;
    for (auto const& o : world.objects) {
        if (o.kind == ObjKind::Decoration) continue;
        if (o.x < playerX - params.rangeBefore - 1.0 || o.x > playerX + params.rangeAfter + 1.0) continue;
        objs.push_back({o.objectId, static_cast<double>(o.x), static_cast<double>(o.y), false});
    }
    return geometryHash(objs, playerX, params);
}

namespace {

ControlTick controlTickOf(Engine const& e) {
    ControlTick t;
    t.hash = e.stateHash();
    t.x = static_cast<float>(e.player().x);
    t.dead = e.dead();
    t.flying = e.flying();
    t.onGround = e.player().onGround;
    t.dashing = e.player().dashing;
    t.ring = e.touchingRing();
    t.mode = e.player().mode;
    t.speed = e.player().speed;
    t.mini = e.player().mini;
    return t;
}

/// Builds the control run: replays every input, keeps the per-tick facts, stores the base
/// snapshots, and decides measurability per input.
void buildControl(World const& world, std::vector<RecordedInput> const& raw, StartState const& start, WindowsConfig const& cfg,
                  WindowsState& st, WindowsProgress& progress) {
    st.inputs.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i].step < 1) continue;
        Input in;
        in.step = raw[i].step;
        in.frame = static_cast<double>(raw[i].step - 1) + std::clamp(raw[i].subTick, 0.0, 0.999999);
        in.down = raw[i].down;
        in.button = raw[i].button;
        in.original = static_cast<int>(i);
        st.inputs.push_back(in);
    }
    std::stable_sort(st.inputs.begin(), st.inputs.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });

    std::vector<int> needed;
    st.baseTick.assign(st.inputs.size(), 0);
    for (size_t i = 0; i < st.inputs.size(); ++i) {
        int base = std::max(0, st.inputs[i].step - 1 - cfg.maxShift - 1);
        st.baseTick[i] = base;
        needed.push_back(base);
    }
    needed.push_back(0);
    std::sort(needed.begin(), needed.end());
    needed.erase(std::unique(needed.begin(), needed.end()), needed.end());

    Engine engine(&world);
    engine.reset(start);
    st.control.clear();
    st.control.push_back(controlTickOf(engine));
    st.snapshots.clear();
    size_t needIdx = 0;
    if (needIdx < needed.size() && needed[needIdx] == 0) {
        st.snapshots[0] = engine.save();
        ++needIdx;
    }
    size_t idx = 0;
    std::vector<TickInput> tickInputs;
    int const lookahead = static_cast<int>(cfg.lookaheadSeconds * 240.0);
    int lastInputTick = st.inputs.empty() ? 0 : st.inputs.back().step;
    int const hardCap = lastInputTick + std::max(lookahead, 240) + 1;
    for (int tick = 0; tick < hardCap; ++tick) {
        tickInputs.clear();
        while (idx < st.inputs.size() && st.inputs[idx].frame < static_cast<double>(tick + 1)) {
            tickInputs.push_back({st.inputs[idx].frame, st.inputs[idx].down});
            ++idx;
        }
        stepTickWithInputs(engine, tickInputs);
        ++progress.ticks;
        st.control.push_back(controlTickOf(engine));
        if (needIdx < needed.size() && needed[needIdx] == engine.tick()) {
            st.snapshots[engine.tick()] = engine.save();
            ++needIdx;
        }
        if (engine.dead()) break;
        if (engine.completed()) {
            st.completed = true;
            break;
        }
        if (engine.inUnsupportedSpan()) break;
    }
    st.endTick = static_cast<int>(st.control.size()) - 1;

    // measurability: the control settles after the input, or completes the level
    st.measurable.assign(st.inputs.size(), 0);
    for (size_t i = 0; i < st.inputs.size(); ++i) {
        if (st.snapshots.find(st.baseTick[i]) == st.snapshots.end()) continue;   // the control ended before the base tick
        double moved = st.inputs[i].frame;
        settle::SettleState s;
        bool ok = false;
        for (int t = st.inputs[i].step; t <= st.endTick; ++t) {
            ControlTick const& c = st.control[static_cast<size_t>(t)];
            if (c.dead) break;
            settle::StepFacts f;
            f.flyingMode = c.flying;
            f.onGround = c.onGround;
            f.dashing = c.dashing;
            f.touchingRing = c.ring;
            f.frameDone = static_cast<double>(t);
            f.lastMovedFrame = moved;
            if (settle::update(s, f)) {
                ok = true;
                break;
            }
        }
        if (!ok && st.completed && !st.control.back().dead) ok = true;
        st.measurable[i] = ok ? 1 : 0;
    }
    progress.controlReady = true;
    progress.total = static_cast<int>(st.inputs.size());
    char buf[160];
    std::snprintf(buf, sizeof buf, "control: %d ticks, %zu inputs, %s", st.endTick, st.inputs.size(),
                  st.completed ? "completed" : (st.control.back().dead ? "died" : "ended"));
    if (progress.debug.size() < 50) progress.debug.push_back(buf);
}

/// One shifted run of input `i` moved to `movedFrame`.
Trial runTrial(World const& world, WindowsConfig const& cfg, WindowsState const& st, size_t i, double movedFrame, Engine& engine, WindowsProgress& progress) {
    Trial out;
    int const base = st.baseTick[i];
    auto snapIt = st.snapshots.find(base);
    if (snapIt == st.snapshots.end()) return out;
    engine.restore(snapIt->second);

    // the schedule from the base tick on, with input i moved (relative order preserved by the
    // neighbour limits; a stable sort keeps same-frame inputs in their recorded order)
    std::vector<Input> sched;
    for (size_t j = 0; j < st.inputs.size(); ++j) {
        if (st.inputs[j].frame < static_cast<double>(base) && j != i) continue;
        Input in = st.inputs[j];
        if (j == i) in.frame = movedFrame;
        if (in.frame < static_cast<double>(base)) continue;
        sched.push_back(in);
    }
    std::stable_sort(sched.begin(), sched.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });

    int const lookahead = static_cast<int>(cfg.lookaheadSeconds * 240.0);
    settle::SettleState settleState;
    int rejoin = 0;
    size_t idx = 0;
    std::vector<TickInput> tickInputs;
    int guard = 0;
    while (guard++ < lookahead + cfg.maxShift + 4) {
        int const tick = engine.tick();
        tickInputs.clear();
        while (idx < sched.size() && sched[idx].frame < static_cast<double>(tick + 1)) {
            tickInputs.push_back({std::max(sched[idx].frame, static_cast<double>(tick)), sched[idx].down});
            ++idx;
        }
        stepTickWithInputs(engine, tickInputs);
        ++progress.ticks;
        int const done = engine.tick();
        if (engine.dead()) {
            out.outcome = Outcome::Fail;
            out.deathX = engine.player().x;
            return out;
        }
        if (engine.completed()) {
            out.outcome = Outcome::Pass;
            return out;
        }
        if (engine.inUnsupportedSpan()) {
            out.outcome = Outcome::NotTested;
            return out;
        }
        settle::StepFacts f;
        f.flyingMode = engine.flying();
        f.onGround = engine.player().onGround;
        f.dashing = engine.player().dashing;
        f.touchingRing = engine.touchingRing();
        f.frameDone = static_cast<double>(done);
        f.lastMovedFrame = movedFrame;
        if (static_cast<double>(done) > movedFrame && settle::update(settleState, f)) {
            out.outcome = Outcome::Pass;
            return out;
        }
        if (static_cast<double>(done) > movedFrame + 1.0 && done <= st.endTick && !st.control[static_cast<size_t>(done)].dead) {
            if (engine.stateHash() == st.control[static_cast<size_t>(done)].hash) {
                if (++rejoin >= cfg.rejoinTicks) {
                    out.outcome = Outcome::Pass;
                    out.resynced = true;
                    return out;
                }
            }
            else rejoin = 0;
        }
        if (static_cast<double>(done) - movedFrame > static_cast<double>(lookahead)) break;
    }
    out.outcome = Outcome::NotTested;
    return out;
}

/// Walks one side of input i, resumable at trial granularity: mayContinue() is polled before every
/// trial; on a stop the walk keeps its position and the next call continues with the same trial
/// sequence (the result equals an uninterrupted walk). Returns true once the side is finished.
bool walkSide(World const& world, WindowsConfig const& cfg, WindowsState const& st, size_t i, double sign, SideWalk& sw, Engine& engine, WindowsProgress& progress,
              std::function<bool()> const& mayContinue) {
    double const frame = st.inputs[i].frame;
    Side& side = sw.side;
    if (sw.stage == 0) {
        int const maxMag = static_cast<int>(std::floor(std::min(static_cast<double>(cfg.maxShift), side.limit) + 1e-9));
        while (sw.mag <= maxMag) {
            if (mayContinue && !mayContinue()) return false;
            int const mag = sw.mag++;
            Trial t = runTrial(world, cfg, st, i, frame + sign * mag, engine, progress);
            ++progress.trials;
            ++side.tested;
            if (t.outcome == Outcome::Pass) side.lastPass = mag;
            else if (t.outcome == Outcome::Fail) {
                side.firstFail = mag;
                break;
            }
            else ++progress.notTested;
        }
        if (!side.bounded() || !cfg.subTick) {
            if (side.bounded()) side.bracket = side.firstFail - side.lastPass;
            sw.stage = 2;
            return true;
        }
        side.bracket = side.firstFail - side.lastPass;
        sw.stage = 1;
        sw.pass = 0;
        sw.passStarted = false;
    }
    while (sw.stage == 1 && sw.pass < cfg.subTickPasses) {
        if (!sw.passStarted) {
            sw.lo = side.lastPass;
            sw.width = side.firstFail - side.lastPass;
            if (sw.width <= 1e-9) break;
            sw.newPass = sw.lo;
            sw.newFail = side.firstFail;
            sw.j = 1;
            sw.passStarted = true;
        }
        int const n = std::max(1, cfg.pointsPerPass);
        while (sw.j <= n) {
            if (mayContinue && !mayContinue()) return false;
            double const mag = sw.lo + sw.width * static_cast<double>(sw.j) / static_cast<double>(n + 1);
            ++sw.j;
            Trial t = runTrial(world, cfg, st, i, frame + sign * mag, engine, progress);
            ++progress.trials;
            ++side.tested;
            if (t.outcome == Outcome::Pass) sw.newPass = mag;
            else if (t.outcome == Outcome::Fail) {
                sw.newFail = mag;
                break;
            }
            else ++progress.notTested;
        }
        side.lastPass = sw.newPass;
        side.firstFail = sw.newFail;
        side.bracket = sw.newFail - sw.newPass;
        sw.passStarted = false;
        ++sw.pass;
    }
    sw.stage = 2;
    return true;
}

}  // namespace

int coarseSideToDemote(double earlyLastPass, double earlyFirstFail, double earlyBracket, double lateLastPass, double lateFirstFail, double lateBracket) {
    if (std::isnan(earlyFirstFail) || std::isnan(lateFirstFail)) return 0;
    double const width = 0.5 * (earlyLastPass + earlyFirstFail) + 0.5 * (lateLastPass + lateFirstFail);
    double const res = std::max(earlyBracket, lateBracket);
    if (width + 1e-9 >= res) return 0;
    return earlyBracket >= lateBracket ? -1 : +1;
}

bool measureWindows(World const& world, std::vector<RecordedInput> const& inputs, StartState const& start, WindowsConfig const& cfg,
                    std::function<bool()> const& mayContinue, uint64_t tickBudget, std::vector<SimWindow>& out, WindowsProgress& progress) {
    if (progress.done) return true;
    if (!progress.state) progress.state = std::make_shared<WindowsState>();
    WindowsState& st = *progress.state;
    uint64_t const ticksAtStart = progress.ticks;
    if (!progress.controlReady) buildControl(world, inputs, start, cfg, st, progress);

    Engine engine(&world);
    double const endX = world.endX > 0.f ? static_cast<double>(world.endX) : 1.0;
    while (progress.next < static_cast<int>(st.inputs.size())) {
        if (tickBudget > 0 && progress.ticks - ticksAtStart >= tickBudget) return false;
        size_t const i = static_cast<size_t>(progress.next);
        Input const& in = st.inputs[i];
        if (!st.windowOpen) {
            if (mayContinue && !mayContinue()) return false;
            if (static_cast<int>(out.size()) >= cfg.maxWindows) {
                ++progress.skippedCap;
                ++progress.next;
                continue;
            }
            if (!st.measurable[i]) {
                ++progress.skippedControl;
                ++progress.next;
                continue;
            }
            // neighbour limits (same button): the moved input never crosses them, nor the start
            st.early = SideWalk{};
            st.late = SideWalk{};
            Side& early = st.early.side;
            Side& late = st.late.side;
            early.limit = in.frame;   // down to frame 0 (the first step)
            for (size_t j = i; j-- > 0;)
                if (st.inputs[j].button == in.button) {
                    double d = in.frame - st.inputs[j].frame - cfg.neighbourMarginFrames;
                    if (d < early.limit) {
                        early.limit = d;
                        early.neighbourLimited = true;
                    }
                    break;
                }
            late.limit = static_cast<double>(cfg.maxShift);
            for (size_t j = i + 1; j < st.inputs.size(); ++j)
                if (st.inputs[j].button == in.button) {
                    double d = st.inputs[j].frame - in.frame - cfg.neighbourMarginFrames;
                    if (d < late.limit) {
                        late.limit = d;
                        late.neighbourLimited = true;
                    }
                    break;
                }
            if (early.limit < 1.0 && late.limit < 1.0) {
                ++progress.skippedNeighbour;
                ++progress.next;
                continue;
            }
            st.windowOpen = true;
            st.sideIndex = 0;
        }
        if (st.sideIndex == 0) {
            if (!walkSide(world, cfg, st, i, -1.0, st.early, engine, progress, mayContinue)) return false;   // resumes here
            st.sideIndex = 1;
        }
        if (!walkSide(world, cfg, st, i, +1.0, st.late, engine, progress, mayContinue)) return false;
        st.windowOpen = false;
        Side early = st.early.side;
        Side late = st.late.side;
        ++progress.next;
        if (early.tested == 0 && late.tested == 0) {
            ++progress.skippedNeighbour;
            continue;
        }
        // M9: never bounded on both sides with a width under the resolution - the coarser side
        // (its refinement was not tested) is reported unbounded at its pass edge
        int const demote = coarseSideToDemote(early.lastPass, early.firstFail, early.bracket, late.lastPass, late.firstFail, late.bracket);
        if (demote != 0) {
            (demote < 0 ? early : late).firstFail = kNaN;
            ++progress.demoted;
        }

        SimWindow w;
        w.source = cfg.source;
        w.tick = in.step + cfg.tickOffset;
        w.frame = in.frame + 1.0 + cfg.tickOffset;
        w.tSeconds = static_cast<double>(w.tick) / 240.0;
        ControlTick const& ct = st.control[static_cast<size_t>(std::min(in.step, st.endTick))];
        w.percent = std::clamp(static_cast<double>(ct.x) / endX * 100.0, 0.0, 100.0);
        w.down = in.down;
        w.gamemode = ct.mode;
        w.speed = ct.speed;
        double earlyEdge = early.bounded() ? -0.5 * (early.lastPass + early.firstFail) : -early.lastPass;
        double lateEdge = late.bounded() ? 0.5 * (late.lastPass + late.firstFail) : late.lastPass;
        w.earliestMs = earlyEdge * kTickMs;
        w.latestMs = lateEdge * kTickMs;
        w.windowMs = w.latestMs - w.earliestMs;
        double res = 0.0;
        if (early.bounded()) res = std::max(res, early.bracket);
        if (late.bounded()) res = std::max(res, late.bracket);
        w.resolutionMs = (res > 0.0 ? res : 1.0) * kTickMs;
        w.boundedEarly = early.bounded();
        w.boundedLate = late.bounded();
        w.verified = false;
        w.supported = true;
        for (auto const& u : world.unsupported)
            if (ct.x >= u.x0 && ct.x < u.x1) w.supported = false;
        gprl::GeometryHash gh = worldGeometryHash(world, ct.x);
        w.geometryHash = gh.low32;
        w.geometryHashHex = gh.hex;
        w.trials = early.tested + late.tested;
        out.push_back(std::move(w));
        ++progress.measured;
    }
    progress.done = true;
    if (!cfg.keepState) progress.state.reset();   // M4: the control run and its snapshots are not needed any more
    char buf[200];
    std::snprintf(buf, sizeof buf, "windows: %d measured, %d control-skipped, %d neighbour-skipped, %d trials, %d not tested, %d demoted, %llu ticks",
                  progress.measured, progress.skippedControl, progress.skippedNeighbour, progress.trials, progress.notTested, progress.demoted,
                  static_cast<unsigned long long>(progress.ticks));
    if (progress.debug.size() < 50) progress.debug.push_back(buf);
    return true;
}

// ---- centring (resumable, review H7) ----

struct CentreState {
    enum class Phase : uint8_t { Measure, Apply, Replay, Done };
    Phase phase = Phase::Measure;
    int round = 0;                            // rounds completed (validated)
    std::vector<RecordedInput> roundInputs;   // seg.inputs at the round start (what `measure` measured)
    std::vector<RecordedInput> current;       // with this round's accepted moves
    WindowsProgress measure;                  // the round's probe windows; its state (the control) is kept for Apply
    std::vector<SimWindow> windows;
    std::map<int, int> shiftByStep;           // desired whole-tick shift by the round-start step
    size_t applyIndex = 0;
    int movedThisRound = 0;
    std::unique_ptr<Engine> live;             // the CURRENT inputs replayed forward only (validation snapshots)
    size_t liveInput = 0;
    std::unique_ptr<Engine> trial;
    std::unique_ptr<Engine> replay;           // the round's final replay (chunked)
    size_t replayInput = 0;
    std::vector<RecordedTick> traj;
};

namespace {

constexpr int kReplayChunk = 4096;

/// One whole tick of `engine` with the inputs of the sorted list whose step is the next one; `idx`
/// walks the list. Exactly replayTrajectory's per-step rule (inputs before step 1 are dropped).
void stepWithList(Engine& engine, std::vector<RecordedInput> const& inputs, size_t& idx, std::vector<TickInput>& buf) {
    int const s = engine.tick() + 1;
    buf.clear();
    while (idx < inputs.size() && inputs[idx].step <= s) {
        if (inputs[idx].step == s) buf.push_back({static_cast<double>(s - 1) + std::clamp(inputs[idx].subTick, 0.0, 0.999999), inputs[idx].down});
        ++idx;
    }
    stepTickWithInputs(engine, buf);
}

/// Validates moving input i of `cs.current` to `newStep` from the live engine's state just before
/// it: the moved run must re-join the round's control (equal stateHash for `rejoinTicks` ticks after
/// both positions), complete the level or reach the segment's end alive.
bool validateMove(World const& world, ReferenceSegment const& seg, CentreState& cs, WindowsState const& control, size_t i, int newStep, int rejoinTicks,
                  int lookaheadTicks, uint64_t& ticks) {
    int const oldStep = cs.current[i].step;
    int const base = std::max(0, std::min(oldStep, newStep) - 2);
    std::vector<TickInput> buf;
    while (cs.live->tick() < base && !cs.live->dead()) {
        stepWithList(*cs.live, cs.current, cs.liveInput, buf);
        ++ticks;
    }
    if (cs.live->dead() || cs.live->tick() != base) return false;
    if (!cs.trial) cs.trial = std::make_unique<Engine>(&world);
    Engine& e = *cs.trial;
    e.restore(cs.live->save());
    std::vector<RecordedInput> sched;
    for (size_t j = 0; j < cs.current.size(); ++j) {
        RecordedInput in = cs.current[j];
        if (j == i) in.step = newStep;
        if (in.step <= base) continue;
        sched.push_back(in);
    }
    std::stable_sort(sched.begin(), sched.end(), [](RecordedInput const& a, RecordedInput const& b) { return a.step < b.step; });
    size_t idx = 0;
    int const after = std::max(oldStep, newStep) + 1;
    int rejoin = 0;
    for (int guard = 0; guard < lookaheadTicks + (after - base) + 4; ++guard) {
        stepWithList(e, sched, idx, buf);
        ++ticks;
        int const done = e.tick();
        if (e.dead()) return false;
        if (e.completed()) return true;
        // the segment's end first: a segment that ended by entering a span ends inside it
        if (done >= seg.ticks) return e.player().x >= static_cast<double>(seg.x1) - 1.0;
        if (e.inUnsupportedSpan()) return false;
        if (done > after && done <= control.endTick && !control.control[static_cast<size_t>(done)].dead) {
            if (e.stateHash() == control.control[static_cast<size_t>(done)].hash) {
                if (++rejoin >= rejoinTicks) return true;
            }
            else rejoin = 0;
        }
        if (done - after > lookaheadTicks) return false;
    }
    return false;
}

}  // namespace

bool centreInputsResumable(World const& world, ReferenceSegment& seg, int probeShift, int maxRounds, std::function<bool()> const& mayContinue,
                           CentreProgress& progress) {
    if (progress.done) return true;
    if (seg.inputs.empty() || seg.ticks <= 0 || probeShift < 1) {
        progress.done = true;
        progress.state.reset();
        return true;
    }
    if (!progress.state) {
        progress.state = std::make_shared<CentreState>();
        progress.state->roundInputs = seg.inputs;
        progress.state->current = seg.inputs;
    }
    CentreState& cs = *progress.state;
    WindowsConfig const defaults;
    int const lookaheadTicks = static_cast<int>(defaults.lookaheadSeconds * 240.0);
    int const rounds = std::max(1, maxRounds);
    using Phase = CentreState::Phase;
    while (cs.phase != Phase::Done) {
        if (cs.phase == Phase::Measure) {
            WindowsConfig cfg;
            cfg.maxShift = probeShift;
            cfg.subTick = false;
            cfg.maxWindows = static_cast<int>(cs.roundInputs.size()) + 1;
            cfg.keepState = true;
            uint64_t before = cs.measure.ticks;
            // progress.ticks follows the measurement trial by trial: the caller's yield rule counts it
            auto inner = [&]() {
                progress.ticks += cs.measure.ticks - before;
                before = cs.measure.ticks;
                return !mayContinue || mayContinue();
            };
            bool const measured = measureWindows(world, cs.roundInputs, seg.start, cfg, inner, 0, cs.windows, cs.measure);
            progress.ticks += cs.measure.ticks - before;
            if (!measured) return false;
            cs.shiftByStep.clear();
            for (auto const& w : cs.windows) {
                if (!(w.boundedEarly && w.boundedLate)) continue;
                double earlyPass = std::round(-w.earliestMs / kTickMs - 0.5);
                double latePass = std::round(w.latestMs / kTickMs - 0.5);
                int s = static_cast<int>((latePass - earlyPass) / 2.0);   // towards zero
                if (s != 0) cs.shiftByStep[w.tick] = s;
            }
            if (cs.shiftByStep.empty() || !cs.measure.state) {
                cs.phase = Phase::Done;
                break;
            }
            cs.applyIndex = 0;
            cs.movedThisRound = 0;
            cs.current = cs.roundInputs;
            cs.live = std::make_unique<Engine>(&world);
            cs.live->reset(seg.start);
            cs.liveInput = 0;
            cs.phase = Phase::Apply;
        }
        else if (cs.phase == Phase::Apply) {
            WindowsState const& control = *cs.measure.state;
            while (cs.applyIndex < cs.current.size()) {
                if (mayContinue && !mayContinue()) return false;
                size_t const i = cs.applyIndex++;
                auto it = cs.shiftByStep.find(cs.roundInputs[i].step);
                if (it == cs.shiftByStep.end()) continue;
                int const newStep = cs.current[i].step + it->second;
                if (newStep < 1) continue;
                bool crosses = false;
                for (size_t j = i; j-- > 0;)
                    if (cs.current[j].button == cs.current[i].button) {
                        if (newStep <= cs.current[j].step) crosses = true;
                        break;
                    }
                for (size_t j = i + 1; j < cs.current.size() && !crosses; ++j)
                    if (cs.current[j].button == cs.current[i].button) {
                        if (newStep >= cs.current[j].step) crosses = true;
                        break;
                    }
                if (crosses) {
                    ++progress.rejected;
                    continue;
                }
                if (validateMove(world, seg, cs, control, i, newStep, defaults.rejoinTicks, lookaheadTicks, progress.ticks)) {
                    cs.current[i].step = newStep;
                    ++cs.movedThisRound;
                }
                else ++progress.rejected;
            }
            cs.live.reset();
            cs.trial.reset();
            if (cs.movedThisRound == 0) {
                cs.phase = Phase::Done;
                break;
            }
            cs.replay.reset();
            cs.replayInput = 0;
            cs.traj.clear();
            cs.phase = Phase::Replay;
        }
        else if (cs.phase == Phase::Replay) {
            if (!cs.replay) {
                cs.replay = std::make_unique<Engine>(&world);
                cs.replay->reset(seg.start);
            }
            int const maxTicks = std::max(1, seg.ticks);
            std::vector<TickInput> buf;
            bool ended = !cs.traj.empty() && (cs.traj.back().dead || cs.replay->completed());
            while (!ended && cs.replay->tick() < maxTicks) {
                if (cs.replay->tick() % kReplayChunk == 0 && mayContinue && !mayContinue()) return false;
                stepWithList(*cs.replay, cs.current, cs.replayInput, buf);
                ++progress.ticks;
                cs.traj.push_back(recordTick(*cs.replay));
                ended = cs.replay->dead() || cs.replay->completed();
            }
            bool const ok = !cs.traj.empty() && !cs.traj.back().dead &&
                            (seg.completed ? cs.traj.back().x >= world.endX - 1e-3f
                                           : static_cast<int>(cs.traj.size()) >= seg.ticks && cs.traj.back().x >= seg.x1 - 1.0f);
            cs.replay.reset();
            if (!ok) {
                // a re-join by state hash was only approximate: undo the whole round, stop centring
                ++progress.revertedRounds;
                if (progress.debug.size() < 50) progress.debug.push_back("centre: round reverted (the moved inputs did not replay to the segment's end)");
                cs.phase = Phase::Done;
                break;
            }
            seg.inputs = cs.current;
            seg.trajectory = std::move(cs.traj);
            cs.traj.clear();
            progress.moved += cs.movedThisRound;
            progress.round = ++cs.round;
            if (cs.round >= rounds) {
                cs.phase = Phase::Done;
                break;
            }
            cs.roundInputs = seg.inputs;
            cs.current = seg.inputs;
            cs.measure = WindowsProgress{};   // frees the previous round's control (M4)
            cs.windows.clear();
            cs.phase = Phase::Measure;
        }
    }
    if (progress.moved > 0 && !seg.trajectory.empty()) seg.x1 = std::max(seg.x1, seg.trajectory.back().x);
    if (progress.debug.size() < 50) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "centre: %d of %zu inputs moved, %d rejected, %d rounds%s (probe +-%d ticks)", progress.moved, seg.inputs.size(), progress.rejected,
                      progress.round, progress.revertedRounds ? ", last round reverted" : "", probeShift);
        progress.debug.push_back(buf);
    }
    progress.state.reset();
    progress.done = true;
    return true;
}

int centreInputs(World const& world, ReferenceSegment& seg, int probeShift, int maxRounds, uint64_t& ticks, std::vector<std::string>* debug) {
    CentreProgress p;
    centreInputsResumable(world, seg, probeShift, maxRounds, {}, p);
    ticks += p.ticks;
    if (debug)
        for (auto const& line : p.debug)
            if (debug->size() < 50) debug->push_back(line);
    return p.moved;
}

}  // namespace gprl::sim
