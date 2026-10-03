#pragma once
// IPhysicsOracle: "given snapshot S and an input schedule, does the player survive H seconds?"
// (ARCHITECTURE §6). The search algorithms in this directory only ever talk to this interface, so
// they are host-tested with a synthetic oracle (tests/boundary_search_tests.cpp) and, in Phase 3,
// driven by the real game through src/solver/GdOracle (hidden clone stepping, GD_PHYSICS_NOTES.md).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../vocab.hpp"

namespace gprl::solver {

/// Index of a deterministic snapshot in the oracle's history ring. The oracle owns the mapping.
using SnapshotId = int64_t;
constexpr SnapshotId kInvalidSnapshot = -1;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// One input in level time. Times are absolute milliseconds since the attempt start (t * 1000).
struct ScheduledInput {
    double tMs = 0.0;
    int player = 1;
    Button button = Button::Jump;
    bool down = true;
};

/// Every input that the trial replays after the snapshot, in time order.
struct InputSchedule {
    std::vector<ScheduledInput> inputs;

    void sortByTime() {
        std::stable_sort(inputs.begin(), inputs.end(), [](auto const& a, auto const& b) { return a.tMs < b.tMs; });
    }

    /// Copy with input `index` moved to `newTMs` (other inputs untouched: a LOCAL window trial).
    InputSchedule withMoved(size_t index, double newTMs) const {
        InputSchedule s = *this;
        if (index < s.inputs.size()) s.inputs[index].tMs = newTMs;
        return s;
    }
};

enum class OutcomeKind : uint8_t {
    Survived,   // alive at the horizon
    Died,       // died at tMs on objectId
    Invalid,    // the trial could not be run (dual mode, teleport, no history, adapter not implemented)
    Resynced,   // re-joined the unshifted trajectory exactly before the horizon (GD_PHYSICS_NOTES)
};

struct Outcome {
    OutcomeKind kind = OutcomeKind::Invalid;
    double tMs = kNaN;      // death time or resync time
    int objectId = 0;       // killer object id for Died
    std::string reason;     // for Invalid

    bool passed() const { return kind == OutcomeKind::Survived || kind == OutcomeKind::Resynced; }
    bool invalid() const { return kind == OutcomeKind::Invalid; }

    static Outcome survived() { return {OutcomeKind::Survived}; }
    static Outcome died(double atMs, int objectId = 0) { return {OutcomeKind::Died, atMs, objectId}; }
    static Outcome resynced(double atMs) { return {OutcomeKind::Resynced, atMs}; }
    static Outcome invalid(std::string why) { Outcome o; o.reason = std::move(why); return o; }
};

constexpr char const* name(OutcomeKind k) {
    switch (k) {
        case OutcomeKind::Survived: return "survived";
        case OutcomeKind::Died: return "died";
        case OutcomeKind::Invalid: return "invalid";
        case OutcomeKind::Resynced: return "resynced";
    }
    return "invalid";
}

class IPhysicsOracle {
public:
    virtual ~IPhysicsOracle() = default;

    /// Run the player from snapshot `base` with `schedule` applied and report what happened within
    /// `horizonSeconds` after the earliest input of the schedule. Must be deterministic.
    virtual Outcome trial(SnapshotId base, InputSchedule const& schedule, double horizonSeconds) = 0;

    /// Earliest absolute time (ms) any input may be moved to from `base` (the history limit). An
    /// input cannot be moved before the snapshot it re-runs from. Default: no history at all.
    virtual double historyStartMs(SnapshotId base) const { (void)base; return kNaN; }

    /// Physics tick length the coarse scan steps by (240 TPS unless the adapter says otherwise).
    virtual double tickMs() const { return kTickMs; }

    virtual std::string name() const = 0;
};

}  // namespace gprl::solver
