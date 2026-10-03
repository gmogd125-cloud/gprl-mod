#pragma once
// Log / summary helpers of the clone engine as PURE functions (host-tested in
// tests/tuning_tests.cpp): the kind of a control mismatch (for the 5 s summary's per-kind
// counts) and the classification of a job's drop reason into the summary buckets.
#include <string>
#include <string_view>

namespace gprl::solver::diagnostics {

/// The kind of a control-vs-real mismatch line ("step 339: touching rings 1 vs 0 | real ...")
/// as a short key: "rings", "speed", "position", "yvel", "control_died", "real_died", "ground",
/// "gravity", "mode", "dash", "slope", "size", "buttons", "lastpos", "other".
inline std::string mismatchKind(std::string_view why) {
    auto at = why.find(": ");
    if (at != std::string_view::npos) why.remove_prefix(at + 2);
    auto has = [&](std::string_view s) { return why.substr(0, s.size()) == s; };
    if (has("touching rings")) return "rings";
    if (has("speed")) return "speed";
    if (has("position off")) return "position";
    if (has("y velocity")) return "yvel";
    if (has("the control died")) return "control_died";
    if (has("the real player died")) return "real_died";
    if (has("the control clone")) return "control_invalid";
    if (has("ground flag")) return "ground";
    if (has("gravity")) return "gravity";
    if (has("game mode")) return "mode";
    if (has("dash")) return "dash";
    if (has("slope flag")) return "slope";
    if (has("size")) return "size";
    if (has("held buttons")) return "buttons";
    if (has("last position")) return "lastpos";
    return "other";
}

/// Summary bucket of a drop reason (CloneEngine::dropJob / finalize).
enum class DropBucket { Mismatch, Invalid, NoPass, Pool, Budget, History, Blocked, Unbound, Reset, LevelEnd, Other };

inline DropBucket dropBucket(std::string_view reason) {
    if (reason == "mismatch") return DropBucket::Mismatch;
    if (reason == "invalid") return DropBucket::Invalid;
    if (reason == "no_pass") return DropBucket::NoPass;
    if (reason == "pool") return DropBucket::Pool;
    if (reason == "budget") return DropBucket::Budget;
    if (reason == "history lost") return DropBucket::History;
    // v0.7.0: no tested shift within the limits (RC-minor 2) is a blocked input, never a 0 ms window
    if (reason == "blocked" || reason == "no_shift_tested") return DropBucket::Blocked;
    if (reason == "unbound") return DropBucket::Unbound;
    if (reason == "reset" || reason == "teardown") return DropBucket::Reset;
    if (reason == "level_end") return DropBucket::LevelEnd;
    return DropBucket::Other;
}

/// Not-windowable buckets: the input could never have had a window (they leave the coverage
/// denominator, see budget.hpp coveragePercent).
inline bool notWindowable(DropBucket b) { return b == DropBucket::NoPass || b == DropBucket::Reset || b == DropBucket::LevelEnd; }

}  // namespace gprl::solver::diagnostics
