#pragma once
// Bottom-left HUD line: "GPRL: jumps 42 | attempts 7 | sigma/s LOCKED (calibrating 12%)"
// (SPEC §10: never a sigma/s figure while locked). Cheap: one label, refreshed twice a second from
// the tracker's session counters, the client's cached calibration state and the live trust
// classification. "sigma" is spelled out because GD's bitmap fonts have no Greek glyphs.
//
// v0.7.0 debug view of a timing (AUDIT §11, docs/TIMING_SOLVER_V2.md §2.12): with
// `solver-trace-max-ticks` > 0 the solver facade hands every new trace of the engine to showTrace
// (core/solver/trace_view fromEngine); with `solver-trace-overlay` on, tick() draws the last one
// in the level (object layer: the real run white, the earliest / latest valid trajectories green,
// the first invalid ones red with the hitbox where they died and the killer's rect orange, the
// sequence-adjusted edge trajectories cyan / magenta, the input point yellow) next to a
// middle-left panel with the AUDIT §11 fields (trace_view panelLines). Drawing only; physics is
// never touched.
#include <Geode/Geode.hpp>

#include "../core/solver/trace_view.hpp"

namespace gprl::hud {

void attach(PlayLayer* pl);
/// Removes the label from the (still valid) layer.
void detach();
/// Drops our reference without touching the layer (called while the PlayLayer is being destroyed).
void forget();
/// Called every frame from PlayLayer::postUpdate; does work only every 0.5 s.
void tick(float dt);
/// Text of the HUD line (also used by the popup header). `withSigma` false: without the
/// "sigma/s ..." part, which the v0.12.0 top-right panel shows instead.
std::string line(bool withSigma = true);

/// Game thread (solver facade, after the engine printed a `GPRL trace:` block): the new last
/// traced input. Kept until the next one or the next level; drawn on the next tick while the
/// overlay setting is on.
void showTrace(solver::trace_view::View view);
/// The last traced input (any == false before the first one of the current / last level), for
/// the popup's Session tab.
solver::trace_view::View const& lastTrace();

/// v0.9.0 (owner decision 2026-10-01): a top-right "!" notification on the running scene (level,
/// end screen or menu; independent of Show HUD): "Verify this run ...". Fades out after `seconds`.
void verificationToast(std::string const& text, float seconds = 8.f);

/// Level families (docs/LEVEL_FAMILY_DESIGN.md FA-D11): the server's "RELATED GAMEPLAY DETECTED"
/// lines as a small top-LEFT panel for `seconds`. Its own node id ("family-notice"), so it never
/// replaces the verification toast; no pulsing "!". Main thread only; does nothing without lines.
void familyNotice(std::vector<std::string> lines, float seconds = 6.f);

}  // namespace gprl::hud
