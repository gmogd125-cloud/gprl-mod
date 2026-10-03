#include "classify.hpp"

#include <algorithm>
#include <cmath>

namespace gprl::classify {

Gamemode gamemodeFromFlags(ModeFlags const& f) {
    if (f.ship) return Gamemode::Ship;
    if (f.ball) return Gamemode::Ball;
    if (f.ufo) return Gamemode::Ufo;
    if (f.wave) return Gamemode::Wave;
    if (f.robot) return Gamemode::Robot;
    if (f.spider) return Gamemode::Spider;
    if (f.swing) return Gamemode::Swing;
    return Gamemode::Cube;
}

Speed speedFromMultiplier(double v, ClassifyParams const& p) {
    if (std::isnan(v)) return Speed::Normal;   // no ordering: never fall through to Fastest
    if (v < p.boundarySlowNormal) return Speed::Slow;
    if (v < p.boundaryNormalFast) return Speed::Normal;
    if (v < p.boundaryFastFaster) return Speed::Fast;
    if (v < p.boundaryFasterFastest) return Speed::Faster;
    return Speed::Fastest;
}

double multiplierOf(Speed s, ClassifyParams const& p) {
    switch (s) {
        case Speed::Slow: return p.speedSlow;
        case Speed::Normal: return p.speedNormal;
        case Speed::Fast: return p.speedFast;
        case Speed::Faster: return p.speedFaster;
        case Speed::Fastest: return p.speedFastest;
    }
    return p.speedNormal;
}

bool isMini(double vehicleSize, ClassifyParams const& p) { return vehicleSize < p.miniVehicleSizeBelow; }

std::optional<ClassifiedInput> classifyButton(bool down, int gdButton, bool isPlayer1) {
    Button b;
    switch (gdButton) {
        case 1: b = Button::Jump; break;
        case 2: b = Button::Left; break;
        case 3: b = Button::Right; break;
        default: return std::nullopt;
    }
    ClassifiedInput in;
    in.player = isPlayer1 ? 1 : 2;
    in.button = b;
    in.down = down;
    in.kind = down ? InputKind::Press : InputKind::Release;
    return in;
}

double tickDtFor(bool tpsBypass, double tps, ClassifyParams const& p) {
    if (tpsBypass && std::isfinite(tps) && tps > p.minBypassTps) return p.updateUnitsPerSecond / tps;
    return p.updateUnitsPerSecond / p.ticksPerSecond;
}

void SubTickClock::reset(double tickDt) {
    m_tickDt = tickDt > 0.0 && std::isfinite(tickDt) ? tickDt : tickDtFor(false, 0.0, m_p);
    m_elapsed = 0.0;
}

void SubTickClock::onPlayerUpdate(double dt) {
    if (!std::isfinite(dt) || dt <= 0.0) return;
    m_elapsed += dt;
    if (m_elapsed >= m_tickDt - m_p.tickBoundaryEpsilon) m_elapsed = 0.0;   // tick boundary
}

double SubTickClock::fraction() const {
    if (m_tickDt <= 0.0) return 0.0;
    return std::clamp(m_elapsed / m_tickDt, 0.0, m_p.maxSubTick);
}

DestroyVerdict classifyDestroy(DestroyFacts const& f) {
    if (!f.attemptOpen || f.wasDeadBefore) return DestroyVerdict::Ignore;
    if (f.playerSlot != 1 && f.playerSlot != 2) return DestroyVerdict::Ignore;   // other mods' clones
    if (f.anticheatSpike) return DestroyVerdict::Ignore;                          // never a real death
    return f.deadAfter ? DestroyVerdict::Death : DestroyVerdict::WouldBeDeath;
}

bool startsWouldBeDeath(WouldBeDeathStreak& streak, int64_t tick) {
    // the same tick, the next one, or an older one (clamped clocks) continue the current death
    bool contiguous = tick <= streak.lastTick + 1;
    streak.lastTick = std::max(tick, streak.lastTick);
    return !contiguous;
}

Integrity integrityFor(TrustState trust, bool modulesReadable) {
    if (!modulesReadable) return Integrity::Unknown;
    switch (trust) {
        case TrustState::Botting:
        case TrustState::PhysicsChanged: return Integrity::Flagged;
        case TrustState::NoclipModified:
        case TrustState::UnknownMod: return Integrity::Warnings;
        case TrustState::Allowed: return Integrity::Clean;
    }
    return Integrity::Unknown;
}

bool environmentChanged(telemetry::EnvironmentPayload const& a, telemetry::EnvironmentPayload const& b, ClassifyParams const& p) {
    if (a.trust != b.trust) return true;
    if (a.noclip.value_or(false) != b.noclip.value_or(false)) return true;
    if (a.bot.value_or(false) != b.bot.value_or(false)) return true;
    if (a.tpsBypass.value_or(false) != b.tpsBypass.value_or(false)) return true;
    if (std::fabs(a.tps - b.tps) > p.tpsChangeEpsilon) return true;
    if (a.cbf != b.cbf) return true;
    if (a.integrity != b.integrity) return true;
    return false;
}

}  // namespace gprl::classify
