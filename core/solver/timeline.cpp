// timeline.hpp is header-only (constexpr / inline helpers). This translation unit exists so the
// module has a compiled presence in the core list and the host tests; it deliberately only
// instantiates the template once as a compile check.
#include "timeline.hpp"

namespace gprl::solver::timeline {

int compileCheckStepForFrame(int fromStep, double targetFrame) {
    return stepForFrame(fromStep, targetFrame, [](int k, double& f) {
        if (k < 1) return false;
        f = static_cast<double>(k);
        return true;
    });
}

}  // namespace gprl::solver::timeline
