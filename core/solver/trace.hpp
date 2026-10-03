#pragma once
// Debug trajectories (docs/TIMING_SOLVER_V2.md §2.12, V2-D12; AUDIT §11): make a claimed "4-frame
// timing" visually inspectable from the Geode log alone.
//
// Setting `solver-trace-max-ticks` (0 = off): every input whose local window is at most this many
// ticks wide is traced, at most TraceCaps::maxTracesPerAttempt per attempt. A traced local job
// records, for every clone of pass 0, per step {frame, x, y, yVel, rotation, mode, onGround,
// hitbox w/h} (at most maxTraceSteps per clone) and at a death the killer {id, type, x, y, rect}
// plus the four lastCollision ids. After the job resolves five trajectories are kept:
//   reference (the control = the real run), earliest valid, just-invalid early, latest valid,
//   just-invalid late
// and printed as `GPRL trace:` lines (one header, one line per trajectory, <= 2 KB each, every
// second step with 1 decimal). tools/timing-report --svg renders them offline.
//
// PURE C++20 (selection, encoding, caps; the engine records); host-tested in tests/trace_tests.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "budget.hpp"

namespace gprl::solver::trace {

struct TraceStep {
    double frame = 0.0;      // frame at the END of the step
    float x = 0.f, y = 0.f;
    float yVel = 0.f;
    float rotation = 0.f;
    char mode = 'C';         // C S B U W R P G (cube ship ball ufo wave robot spider swing)
    bool onGround = false;
    float w = 0.f, h = 0.f;  // hitbox size
};

struct TraceDeath {
    bool died = false;
    double frame = 0.0;      // end of the step the clone died in
    int killerId = -1;       // object id, -1 = none / level boundary
    int killerType = -1;     // GameObjectType
    float killerX = 0.f, killerY = 0.f;
    float rectX = 0.f, rectY = 0.f, rectW = 0.f, rectH = 0.f;   // the killer's hitbox
    float x = 0.f, y = 0.f;                                     // the clone at the death
    float boxW = 0.f, boxH = 0.f;
    int lastCollision[4] = {-1, -1, -1, -1};                    // top, bottom, left, right
    int laterInputs = -1;                                       // later fixed inputs applied before it
    bool extension = false;
};

struct TraceClone {
    double shiftFrames = 0.0;
    std::vector<TraceStep> steps;
    TraceDeath death;
    bool passed = false;
    bool capped = false;     // maxTraceSteps reached (append-only, stops at the cap)
};

/// Append-only recording, capped (T-trace: never grows past maxSteps).
inline bool record(TraceClone& c, TraceStep const& s, int maxSteps = budget::kTraceCaps.maxTraceSteps) {
    if (static_cast<int>(c.steps.size()) >= maxSteps) {
        c.capped = true;
        return false;
    }
    c.steps.push_back(s);
    return true;
}

/// Whether a finished local window is traced: tracing on, window bounded on both sides and at
/// most `maxTicks` wide, and the attempt's cap not reached.
inline bool wanted(double widthFrames, bool bothBounded, int maxTicksSetting, int tracedThisAttempt,
                   budget::TraceCaps const& caps = budget::kTraceCaps) {
    if (maxTicksSetting <= 0 || !bothBounded) return false;
    int maxTicks = std::min(maxTicksSetting, caps.maxTraceTicksSetting);
    if (!(widthFrames <= static_cast<double>(maxTicks) + 1e-9)) return false;
    return tracedThisAttempt < caps.maxTracesPerAttempt;
}

/// The five trajectories of §2.12 as indices into `clones` (-1 = not available). Targets are
/// signed frames: the conservative pass edges and the first fails of the local window.
struct Selection {
    int reference = -1;
    int earliestValid = -1;
    int earlyInvalid = -1;
    int latestValid = -1;
    int lateInvalid = -1;
};

inline Selection select(std::vector<TraceClone> const& clones, double earlyPass, double earlyFail, double latePass, double lateFail) {
    auto at = [&](double s) -> int {
        if (std::isnan(s)) return -1;
        for (size_t i = 0; i < clones.size(); ++i) {
            if (std::fabs(clones[i].shiftFrames - s) < 1e-6) return static_cast<int>(i);
        }
        return -1;
    };
    Selection sel;
    sel.reference = at(0.0);
    sel.earliestValid = std::fabs(earlyPass) < 1e-9 ? sel.reference : at(earlyPass);
    sel.earlyInvalid = at(earlyFail);
    sel.latestValid = std::fabs(latePass) < 1e-9 ? sel.reference : at(latePass);
    sel.lateInvalid = at(lateFail);
    return sel;
}

inline std::vector<std::pair<char const*, int>> ordered(Selection const& s) {
    return {{"reference", s.reference}, {"earliest-valid", s.earliestValid}, {"early-invalid", s.earlyInvalid},
            {"latest-valid", s.latestValid}, {"late-invalid", s.lateInvalid}};
}

struct Header {
    int inputIndex = 0;
    bool down = true;
    int64_t seq = -1;
    double t = 0.0;
    int64_t tick = 0;
    double x = 0.0;
    double percent = 0.0;
    std::string mode = "cube";
    double speed = 1.0;      // the GD speed multiplier label: 0.5 (slow), 1 (normal), 2, 3, 4
    bool flipped = false;
    bool mini = false;
    std::string local;       // "16.67 ms [-6.25,+10.42]"
    std::string sequence;    // "45.83 ms [-20.83,+25.00] pair decided" or "-"
    std::string hold;        // "-" or "[83.33, 100.00] ms"
};

/// The last traced input as the engine keeps it for the in-game overlay (MOD item M9, setting
/// `solver-trace-overlay`): the header and the printed trajectories with their roles ("reference",
/// "earliest-valid", "early-invalid", "latest-valid", "late-invalid", then "sa-..." for the
/// sequence-adjusted edges), world coordinates. `serial` grows with every new trace (0 = none yet).
struct View {
    int serial = 0;
    Header header;
    std::vector<std::pair<std::string, TraceClone>> trajectories;
};

/// The sequence-adjusted edge trajectories (§2.12 second block) among the SA trials recorded for
/// one member: `trials` = (adaptation 0..6: local, pair, chain2, chain3, comp1..3; signed shift frames, trajectory). Valid roles take the
/// passing trial at the SA pass edge (skipped when that pass is inside the local window: the local
/// block shows it); invalid roles take the LAST family member tried at the SA fail shift (the one
/// whose death decided it). Roles "sa-earliest-valid", "sa-early-invalid", "sa-latest-valid",
/// "sa-late-invalid"; the adaptation name is appended in brackets by the caller.
struct SATraced {
    int adaptation = 0;
    double shiftFrames = 0.0;
    TraceClone clone;
};
struct SASelection {
    int earliestValid = -1, earlyInvalid = -1, latestValid = -1, lateInvalid = -1;   // indices into the trials
};
inline SASelection selectSA(std::vector<SATraced> const& trials, double earlyPass, double earlyFail, double latePass, double lateFail,
                            double localEarlyPass, double localLatePass) {
    auto pick = [&](double s, bool wantPass) -> int {
        if (std::isnan(s)) return -1;
        int best = -1;
        for (size_t i = 0; i < trials.size(); ++i) {
            auto const& t = trials[i];
            if (std::fabs(t.shiftFrames - s) >= 1e-6) continue;
            if (wantPass && !t.clone.passed) continue;
            if (!wantPass && t.clone.passed) continue;
            if (best < 0 || t.adaptation > trials[static_cast<size_t>(best)].adaptation) best = static_cast<int>(i);
        }
        return best;
    };
    SASelection sel;
    if (earlyPass < localEarlyPass - 1e-6) sel.earliestValid = pick(earlyPass, true);
    sel.earlyInvalid = pick(earlyFail, false);
    if (latePass > localLatePass + 1e-6) sel.latestValid = pick(latePass, true);
    sel.lateInvalid = pick(lateFail, false);
    return sel;
}

inline std::string header(Header const& h) {
    char buf[640];
    std::snprintf(buf, sizeof buf, "GPRL trace: input #%d %s seq %lld t=%.4f tick=%lld x=%.0f %.3f%% %s spd %.2f grav %s mini %d | local %s | sequence %s | hold %s",
                  h.inputIndex, h.down ? "press" : "release", static_cast<long long>(h.seq), h.t, static_cast<long long>(h.tick), h.x, h.percent,
                  h.mode.c_str(), h.speed, h.flipped ? "flipped" : "normal", h.mini ? 1 : 0, h.local.c_str(), h.sequence.empty() ? "-" : h.sequence.c_str(),
                  h.hold.empty() ? "-" : h.hold.c_str());
    return buf;
}

/// One trajectory line: every `stride`-th step with one decimal, the death (killer, hitbox,
/// lastCollision, later inputs) at the end; never longer than maxBytes (points are dropped first,
/// the count is kept).
inline std::string trajectory(int inputIndex, char const* role, TraceClone const& c, int stride = 2,
                              int maxBytes = budget::kTraceCaps.maxLineBytes) {
    char head[160];
    double frame0 = c.steps.empty() ? 0.0 : c.steps.front().frame;
    std::snprintf(head, sizeof head, "GPRL trace: #%d %s shift %+g: frame0 %.1f pts %zu x,y:", inputIndex, role, c.shiftFrames, frame0, c.steps.size());
    std::string tail;
    if (c.death.died) {
        char d[400];
        auto const& k = c.death;
        std::snprintf(d, sizeof d, " | died frame %.1f on #%d type %d at (%.1f,%.1f) rect %.1f,%.1f %.1fx%.1f clone (%.1f,%.1f) box %.0fx%.0f lastCollision top %d bottom %d left %d right %d laterInputs %d (%s)%s",
                      k.frame, k.killerId, k.killerType, k.killerX, k.killerY, k.rectX, k.rectY, k.rectW, k.rectH, k.x, k.y, k.boxW, k.boxH, k.lastCollision[0],
                      k.lastCollision[1], k.lastCollision[2], k.lastCollision[3], k.laterInputs, k.laterInputs >= 1 ? "downstream" : "self",
                      k.extension ? " [ext]" : "");
        tail = d;
    }
    else tail = c.passed ? " | passed" : " | not resolved";
    if (c.capped) tail += " (capped)";
    std::string out = head;
    size_t budgetBytes = static_cast<size_t>(std::max(256, maxBytes));
    int step = std::max(1, stride);
    for (size_t i = 0; i < c.steps.size(); i += static_cast<size_t>(step)) {
        char p[48];
        std::snprintf(p, sizeof p, " %.1f,%.1f", c.steps[i].x, c.steps[i].y);
        if (out.size() + std::char_traits<char>::length(p) + tail.size() + 8 > budgetBytes) {
            out += " ...";
            break;
        }
        out += p;
    }
    if (out.size() + tail.size() > budgetBytes) tail = tail.substr(0, budgetBytes > out.size() ? budgetBytes - out.size() : 0);
    out += tail;
    return out;
}

}  // namespace gprl::solver::trace
