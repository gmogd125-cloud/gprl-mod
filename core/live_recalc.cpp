#include "live_recalc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ranks.hpp"

namespace gprl::live {

int64_t effectiveIntervalMs(LiveRecalcParams const& p) {
    return std::max<int64_t>({p.intervalMs, p.serverCooldownMs, 1000});
}

int64_t effectiveBackoffMs(LiveRecalcParams const& p) {
    return std::max(p.errorBackoffMs, effectiveIntervalMs(p));
}

LiveRecalcSchedule::LiveRecalcSchedule(LiveRecalcParams params) : m_params(std::move(params)) {}

void LiveRecalcSchedule::sessionStarted(int64_t nowMs) {
    m_session = true;
    m_queued = false;
    m_warnedThisSession = false;
    m_pendingWindows = 0;
    m_nextAttemptMs = nowMs + effectiveIntervalMs(m_params);
    if (m_backingOff) m_nextAttemptMs = std::max(m_nextAttemptMs, m_backoffUntilMs);
}

void LiveRecalcSchedule::sessionEnded() {
    m_session = false;
    m_queued = false;
    m_pendingWindows = 0;
}

void LiveRecalcSchedule::windowsSent(int64_t count) {
    if (!m_session || count <= 0) return;
    m_pendingWindows += count;
}

bool LiveRecalcSchedule::due(int64_t nowMs, LiveRecalcGate const& gate) const {
    // `gate.levelCounts` is NOT a condition: every level feeds the calibration since 2026-10-01
    // (docs/RATING.md §11). While it still was one, a session on a level that is not on the
    // levels list never asked, and the calibration only moved when the session ended.
    if (!gate.enabled || !gate.sessionOpen || !gate.remote) return false;
    if (!m_session || m_queued || m_pendingWindows <= 0) return false;
    return nowMs >= m_nextAttemptMs;
}

void LiveRecalcSchedule::onAnswer(int64_t nowMs, bool recalculated) {
    m_queued = false;
    m_backingOff = false;
    m_backoffUntilMs = 0;
    if (recalculated) m_pendingWindows = 0;
    m_nextAttemptMs = nowMs + effectiveIntervalMs(m_params);
}

bool LiveRecalcSchedule::onError(int64_t nowMs) {
    m_queued = false;
    m_backingOff = true;
    m_backoffUntilMs = nowMs + effectiveBackoffMs(m_params);
    m_nextAttemptMs = m_backoffUntilMs;
    bool first = !m_warnedThisSession;
    m_warnedThisSession = true;
    return first;
}

bool parseLiveRecalcAnswer(json::Value const& body, LiveRecalcAnswer& out) {
    if (!body.isObject()) return false;
    auto const* rec = body.find("recalculated");
    if (!rec || !rec->isBool()) return false;
    LiveRecalcAnswer a;
    a.recalculated = rec->asBool();
    auto str = [&](char const* key) {
        auto const* v = body.find(key);
        return v && v->isString() ? v->asString() : std::string();
    };
    auto count = [&](char const* key) -> std::optional<int64_t> {
        auto const* v = body.find(key);
        if (!v || !v->isNumber() || !std::isfinite(v->asNumber()) || v->asNumber() < 0) return std::nullopt;
        return static_cast<int64_t>(std::llround(v->asNumber()));
    };
    a.computedAt = str("computedAt");
    a.nextAllowedAt = str("nextAllowedAt");
    a.skipped = str("skipped");
    a.cooldownSeconds = count("cooldownSeconds").value_or(0);
    a.ratableSamples = count("ratableSamples");
    a.sampleCount = count("sampleCount");
    out = std::move(a);
    return true;
}

namespace {

std::string percentText(double fraction) {
    char buf[32];
    double pct = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) * 100.0 : 0.0;
    std::snprintf(buf, sizeof buf, "%.1f%%", pct);
    return buf;
}

}  // namespace

std::string liveRecalcLogLine(LiveRecalcAnswer const& answer, CalibrationState const& overall, CalibrationDisplay const& display) {
    double eff = std::isfinite(overall.effectiveSamples.current) ? overall.effectiveSamples.current : 0.0;
    std::string effective = std::to_string(static_cast<long long>(std::llround(std::max(0.0, eff)))) + " effective";
    std::string what;
    if (answer.recalculated) {
        what = answer.ratableSamples ? std::to_string(*answer.ratableSamples) + " rated, " + effective : effective;
    }
    else if (answer.skipped == "no_sessions") what = "no session on the server, stored; " + effective;
    else what = "server cooldown, stored; " + effective;

    std::string shown = "display " + std::string(name(display.state));
    if (!display.locked()) {
        // public figure (the HUD shows it anyway); a locked display never names one
        if (auto s = display.headlineSigma()) shown += " " + ranks::formatSigma(*s);
    }
    // the private estimate is owner-only: present or not, never its number
    std::string priv = display.privatePresent && display.privateHeadline() ? "private yes" : "private -";
    return "live recalc -> calibration " + percentText(overall.percent) + " (" + what + "), " + shown + ", " + priv;
}

}  // namespace gprl::live
