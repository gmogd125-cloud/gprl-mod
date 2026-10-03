#pragma once
// ResultLedger (docs/TIMING_SOLVER_V2.md §3.2, V2-D6): one entry per BOUND local job from its
// binding to its `timing_result`:
//
//   awaitingLocal --localDone(needsSA)--> awaitingSA --saDone--> done --drain--> emitted, erased
//                 --localDone(!needsSA) or close-----------------> done
//   flush(finalize): every entry that is not done gets its final state from `finalize` (cut by a
//   restart / the level end / teardown) and is emitted
//
// Guarantees EXACTLY ONE emission per opened job on every lifecycle path (host-tested in
// tests/result_ledger_tests.cpp): an entry is erased the moment it is emitted, a second open of
// the same job id is refused, transitions of unknown / already-emitted ids are ignored. Unbound
// jobs (no telemetry input event, nothing to attach a result to) never enter the ledger.
//
// PURE C++20. Every loop is bounded by the number of open entries (each flush / drain iteration
// emits and erases one entry).
#include <cstdint>
#include <utility>
#include <vector>

namespace gprl::solver {

enum class LedgerState : uint8_t { AwaitingLocal, AwaitingSA, Done };
constexpr char const* name(LedgerState s) {
    switch (s) {
        case LedgerState::AwaitingLocal: return "awaiting_local";
        case LedgerState::AwaitingSA: return "awaiting_sa";
        case LedgerState::Done: return "done";
    }
    return "done";
}

template <class T>
class ResultLedger {
public:
    struct Entry {
        int jobId = 0;
        LedgerState state = LedgerState::AwaitingLocal;
        T data{};
    };

    /// A job was bound to its input event: one result is now owed. False when the id is open already.
    bool open(int jobId, T data) {
        if (find(jobId)) return false;
        m_entries.push_back({jobId, LedgerState::AwaitingLocal, std::move(data)});
        ++m_opened;
        return true;
    }

    bool has(int jobId) const { return findConst(jobId) != nullptr; }
    T* data(int jobId) {
        Entry* e = find(jobId);
        return e ? &e->data : nullptr;
    }
    LedgerState state(int jobId) const {
        Entry const* e = findConst(jobId);
        return e ? e->state : LedgerState::Done;
    }

    /// The local window finished (or the job ended without one): wait for the SA job or be done.
    bool localDone(int jobId, bool needsSA) {
        Entry* e = find(jobId);
        if (!e || e->state != LedgerState::AwaitingLocal) return false;
        e->state = needsSA ? LedgerState::AwaitingSA : LedgerState::Done;
        return true;
    }
    /// The SA job produced (or gave up on) this input's sequence window.
    bool saDone(int jobId) {
        Entry* e = find(jobId);
        if (!e || e->state != LedgerState::AwaitingSA) return false;
        e->state = LedgerState::Done;
        return true;
    }
    /// Done from any state (a job dropped before its local window).
    bool close(int jobId) {
        Entry* e = find(jobId);
        if (!e || e->state == LedgerState::Done) return false;
        e->state = LedgerState::Done;
        return true;
    }

    /// Emits every done entry (opening order) through `emit(jobId, T&)` and erases it. Returns
    /// how many were emitted. The entry is moved out BEFORE `emit` runs, so an emit callback that
    /// touches the ledger never sees (or emits) it twice.
    template <class Emit>
    int drain(Emit&& emit) {
        int n = 0;
        for (size_t i = 0; i < m_entries.size();) {
            if (m_entries[i].state != LedgerState::Done) {
                ++i;
                continue;
            }
            Entry e = std::move(m_entries[i]);
            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
            ++m_emitted;
            ++n;
            emit(e.jobId, e.data);
        }
        return n;
    }

    /// Restart / level end / teardown: `finalize(jobId, T&, LedgerState before)` records the final
    /// state of every entry that is not done, then everything is emitted. Returns the count.
    template <class Finalize, class Emit>
    int flush(Finalize&& finalize, Emit&& emit) {
        for (auto& e : m_entries) {
            if (e.state == LedgerState::Done) continue;
            LedgerState before = e.state;
            e.state = LedgerState::Done;
            finalize(e.jobId, e.data, before);
        }
        return drain(std::forward<Emit>(emit));
    }

    size_t openCount() const { return m_entries.size(); }
    int64_t opened() const { return m_opened; }
    int64_t emitted() const { return m_emitted; }
    std::vector<Entry> const& entries() const { return m_entries; }
    /// Forget everything WITHOUT emitting (the layer died without teardown: nothing may be pushed).
    void abandon() { m_entries.clear(); }

private:
    Entry* find(int jobId) {
        for (auto& e : m_entries) if (e.jobId == jobId) return &e;
        return nullptr;
    }
    Entry const* findConst(int jobId) const {
        for (auto const& e : m_entries) if (e.jobId == jobId) return &e;
        return nullptr;
    }

    std::vector<Entry> m_entries;
    int64_t m_opened = 0;
    int64_t m_emitted = 0;
};

}  // namespace gprl::solver
