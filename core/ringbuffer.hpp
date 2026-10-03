#pragma once
// Single-producer / single-consumer ring buffer for gameplay events (SPEC §46: ring buffers,
// no per-click allocation on the game thread beyond what the event itself holds).
//
// "Lock-free-ish": head and tail are atomics with acquire/release ordering, so the game thread
// (producer) never blocks on the telemetry thread (consumer) and vice versa. Element moves are not
// atomic; T may own heap memory (strings), which is fine for SPSC use. Capacity is a power of two.
// When the buffer is full, tryPush returns false and the caller counts a drop (it must never wait).
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace gprl {

template <typename T, size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    SpscRing() = default;
    SpscRing(SpscRing const&) = delete;
    SpscRing& operator=(SpscRing const&) = delete;

    /// Producer side. Returns false (and drops nothing but the argument) when full.
    bool tryPush(T&& item) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t next = (head + 1) & kMask;
        if (next == m_tail.load(std::memory_order_acquire)) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[head] = std::move(item);
        m_head.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when empty.
    bool tryPop(T& out) {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return false;
        out = std::move(m_slots[tail]);
        m_tail.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    /// Approximate count (exact for whichever thread calls it between its own operations).
    size_t size() const {
        size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return Capacity - 1; }   // one slot is the full/empty sentinel

    /// Number of items refused because the buffer was full. Reported in telemetry as evidence of
    /// loss, never silently swallowed.
    uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    static constexpr size_t kMask = Capacity - 1;
    std::array<T, Capacity> m_slots{};
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    std::atomic<uint64_t> m_dropped{0};
};

}  // namespace gprl
