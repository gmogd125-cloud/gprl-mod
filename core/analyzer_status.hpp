#pragma once
// Background level analyzer: the PURE status texts (HUD bottom-left suffix, Session tab lines)
// (docs/BACKGROUND_ANALYZER_DESIGN.md §5, AN-D8 "status is computed"). PURE C++20; host-tested in
// tests/analyzer_recorder_tests.cpp. The mod (src/analyzer/Status.cpp) fills a `View` from the
// worker's published state and prints these lines; nothing here reads the game.
//
// READ-ONLY RULE (the whole analyzer, AN-D1 / AN-D11): nothing the analyzer adds ever writes a GD
// field; this file is plain text formatting.
//
// GD's bigFont.fnt has no glyphs beyond ASCII: every line is ASCII (`ascii()` folds what the
// simulator's own progress line may carry, e.g. a middle dot, into "-").
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gprl::analyzer::status {

enum class Stage : uint8_t {
    Off = 0,       // passive mode / disabled / nothing to show
    Extracting,    // reading the level's objects (main thread slices)
    Waiting,       // a job exists but may not run right now (see WaitReason)
    Running,       // the job runs (phase + percent)
    Done,          // the result is final
    Cached,        // a cached analysis (server or this computer) was used, nothing simulated
    Stopped,       // the isolation check found a live-state difference: stopped for this level visit
    Failed,        // extraction or job failed
};

enum class WaitReason : uint8_t {
    None = 0,
    RecordSafeAttempt,   // Record-Safe: only while no attempt is active
    FramePressure,       // the game is under frame pressure
    LevelClosed,         // the job's level is not open (kept for 10 minutes)
    Queued,              // waiting for the worker (cache check, start)
};

struct View {
    bool simulatorEnabled = false;
    bool recordSafe = false;
    Stage stage = Stage::Off;
    WaitReason wait = WaitReason::None;
    double extractPercent = 0.0;
    std::string phase;              // sim::name(JobPhase): verify / search / windows / assemble
    double phasePercent = 0.0;
    double solvedPercent = 0.0;
    bool hasVerification = false;
    double verifiedPercent = 0.0;
    double elapsedSeconds = 0.0;
    std::string cacheSource;        // "server" | "this computer"
    std::string upload;             // "uploaded (stored)" / "upload failed, retry in 30 s" / ""
    std::string failReason;
};

/// ASCII only (GD fonts): every multi-byte UTF-8 sequence becomes one '-'.
inline std::string ascii(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        size_t len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        out.push_back('-');
        i += len;
    }
    return out;
}

inline std::string pct(double v) {
    if (!std::isfinite(v)) v = 0.0;
    if (v < 0.0) v = 0.0;
    if (v > 100.0) v = 100.0;
    return std::to_string(static_cast<int>(std::floor(v + 1e-9))) + "%";
}

inline char const* phaseWord(std::string const& phase) {
    if (phase == "verify") return "verifying";
    if (phase == "search") return "searching";
    if (phase == "windows") return "measuring windows";
    if (phase == "assemble") return "finishing";
    if (phase == "idle") return "starting";
    return "working";
}

inline std::string seconds(double s) {
    if (!std::isfinite(s) || s < 0.0) s = 0.0;
    char b[32];
    if (s < 600.0) std::snprintf(b, sizeof b, "%.0f s", s);
    else std::snprintf(b, sizeof b, "%.0f min", s / 60.0);
    return b;
}

/// The bottom-left HUD suffix ("" = nothing to show).
inline std::string hudLine(View const& v) {
    if (!v.simulatorEnabled && v.stage == Stage::Off) return {};
    std::string out;
    switch (v.stage) {
        case Stage::Off: return {};
        case Stage::Extracting: out = "Level analysis: reading level " + pct(v.extractPercent); break;
        case Stage::Waiting:
            switch (v.wait) {
                case WaitReason::RecordSafeAttempt: out = "Record-Safe: waiting for the attempt to end"; break;
                case WaitReason::FramePressure: out = "Level analysis: paused (game busy)"; break;
                case WaitReason::LevelClosed: out = "Level analysis: paused (level closed)"; break;
                case WaitReason::Queued:
                case WaitReason::None: out = "Level analysis: queued"; break;
            }
            break;
        case Stage::Running:
            out = std::string("Level analysis: ") + phaseWord(v.phase) + " " + pct(v.phase == "search" ? v.solvedPercent : v.phasePercent) + ", "
                + seconds(v.elapsedSeconds);
            break;
        case Stage::Done:
            out = v.hasVerification ? "Level analysis: verified " + pct(v.verifiedPercent) : "Level analysis: done, solved " + pct(v.solvedPercent);
            break;
        case Stage::Cached: out = "Level analysis: cached"; break;
        case Stage::Stopped: out = "Level analysis: stopped (isolation)"; break;
        case Stage::Failed: out = "Level analysis: failed" + (v.failReason.empty() ? std::string() : " (" + v.failReason + ")"); break;
    }
    if (!v.upload.empty()) out += " - " + v.upload;
    return ascii(out);
}

}  // namespace gprl::analyzer::status
