#pragma once
// LiveStateSnapshot capture (docs/LIVE_ISOLATION_DESIGN.md §3.1, Appendix B): fills the pure
// core/solver/live_state.hpp struct from the live PlayLayer. Read-only: nothing here writes.
#include <Geode/Geode.hpp>

#include <vector>

#include "../../core/solver/live_state.hpp"

namespace gprl::clone {

/// Both players, the camera, the layer and the activation bytes of the orbs / pads / portals in
/// `act` (sorted by `actX`) within [P1 x - 130, P1 x + 600] (at most live::kMaxObjects).
void captureLive(PlayLayer* pl, std::vector<EnhancedGameObject*> const& act, std::vector<float> const& actX, int step,
                 gprl::solver::live::Snapshot& out);

}  // namespace gprl::clone
