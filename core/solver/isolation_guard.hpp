#pragma once
// IsolationGuard (docs/LIVE_ISOLATION_DESIGN.md §3.2-§3.3): the pure state machine behind the
// development invariant check. PURE C++20; host-tested in tests/live_state_tests.cpp.
//
// The engine captures a LiveStateSnapshot before and after every analysis block (`every block`,
// the release default) or before and after every clone step (`every clone step`, the setting
// `isolation-check` / debug log, and automatically for the rest of the level visit after a
// breach). A difference is a BREACH: the engine logs the owner's block per field, aborts the
// stepped samples (`live_mutation_detected`), ends every open result of the attempt with it and
// stops clone stepping until the restart (the isolation breaker). The guard never repairs anything.
//
// Cost rule (§3.2): the capture cost is measured (EMA); while `every block` costs more than
// kIsolationConfig.maxCheckUsPerStep the guard checks every N-th block (sampleEveryNBlocks),
// never none. Every threshold lives in kIsolationConfig.
#include <cstdint>

namespace gprl::solver::isolation {

struct IsolationConfig {
    char const* version = "gprl-isolation/2";
    double maxCheckUsPerStep = 5.0;    // two captures per physics step above this -> sampled
    int sampleEveryNBlocks = 4;        // the fallback rate (never 0 = off)
    int maxLoggedFields = 12;          // owner blocks per breach, then "... N more"
    double costEmaAlpha = 0.05;        // EMA of one check's cost (us)
    int minChecksBeforeFallback = 60;  // the EMA must have settled before the rate drops
};
constexpr IsolationConfig kIsolationConfig{};

enum class CheckMode : uint8_t { EveryBlock, EveryCloneStep };

constexpr char const* name(CheckMode m) { return m == CheckMode::EveryBlock ? "every block" : "every clone step"; }

class IsolationGuard {
public:
    explicit IsolationGuard(IsolationConfig const& cfg = kIsolationConfig) : m_cfg(cfg) {}

    /// Level entry: the configured mode, nothing escalated yet.
    void setupLevel(CheckMode mode) {
        m_mode = mode;
        m_configured = mode;
        m_breachedAttempt = false;
        m_blocks = m_checks = m_breaches = m_skipped = 0;
        m_costUs = 0.0;
        m_sampleEvery = 1;
        m_fellBack = false;
    }
    /// resetLevel: the breaker clears; an escalated mode stays for the rest of the level visit.
    void resetAttempt() { m_breachedAttempt = false; }
    /// The setting changed mid-level: the configured mode follows; an escalated guard stays escalated.
    void setConfiguredMode(CheckMode mode) {
        m_configured = mode;
        if (m_breaches == 0) { m_mode = mode; m_sampleEvery = 1; m_fellBack = false; }
    }

    CheckMode mode() const { return m_mode; }
    CheckMode configuredMode() const { return m_configured; }
    bool breached() const { return m_breachedAttempt; }
    int blocks() const { return m_blocks; }
    int checks() const { return m_checks; }
    int breaches() const { return m_breaches; }
    int skipped() const { return m_skipped; }
    double costUs() const { return m_costUs; }
    int sampleEvery() const { return m_sampleEvery; }
    bool fellBack() const { return m_fellBack; }

    /// An analysis block starts: whether this one is checked (block mode, sampling rate).
    bool beginBlock() {
        ++m_blocks;
        if (m_mode != CheckMode::EveryBlock) return false;
        if (m_sampleEvery > 1 && (m_blocks % m_sampleEvery) != 0) { ++m_skipped; return false; }
        return true;
    }
    /// Per-step mode: every clone step is checked.
    bool checksEveryStep() const { return m_mode == CheckMode::EveryCloneStep; }

    /// A check finished (both captures + compare took `us`). Returns true when the guard just fell
    /// back to sampling (the caller logs it once).
    bool noteCheck(double us) {
        ++m_checks;
        m_costUs = m_checks == 1 ? us : m_costUs + m_cfg.costEmaAlpha * (us - m_costUs);
        if (m_mode == CheckMode::EveryBlock && m_sampleEvery == 1 && m_checks >= m_cfg.minChecksBeforeFallback && m_costUs > m_cfg.maxCheckUsPerStep) {
            m_sampleEvery = m_cfg.sampleEveryNBlocks > 1 ? m_cfg.sampleEveryNBlocks : 1;
            m_fellBack = m_sampleEvery > 1;
            return m_fellBack;
        }
        return false;
    }

    /// A difference was found: the attempt is breached, the mode escalates for the rest of the
    /// level visit (§3.2 "the mode switches to every clone step").
    void onBreach() {
        ++m_breaches;
        m_breachedAttempt = true;
        m_mode = CheckMode::EveryCloneStep;
        m_sampleEvery = 1;
    }

private:
    IsolationConfig m_cfg;
    CheckMode m_mode = CheckMode::EveryBlock;
    CheckMode m_configured = CheckMode::EveryBlock;
    bool m_breachedAttempt = false;
    int m_blocks = 0, m_checks = 0, m_breaches = 0, m_skipped = 0;
    double m_costUs = 0.0;
    int m_sampleEvery = 1;
    bool m_fellBack = false;
};

}  // namespace gprl::solver::isolation
