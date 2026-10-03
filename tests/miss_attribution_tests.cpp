// Miss attribution host tests (docs/TIMING_SOLVER_V2_FABLE.md §3.3, Fable review D6): among the
// jobs whose control died with the real player, the one with the LATEST input frame that has a
// passing run is the player's miss; a job without a passing run neither wins nor blocks; no
// passing run anywhere -> nobody is the miss. The real-log case (jobs 595-599 of the owner's
// Deadlocked attempt a6 -> exactly one miss) is T-ATT-3 in attribution_tests.cpp.
#include "test_util.hpp"

#include "../core/solver/miss_attribution.hpp"

#include <vector>

using namespace gprl::solver;

namespace {

void testAttributeMiss() {
    SECTION("attributeMiss: the latest input with a passing run wins; none -> nobody; no-pass jobs never block");
    // the latest input with a pass run is the miss
    CHECK(attributeMiss({{595, 3930, true}, {596, 3952, true}, {597, 3969, true}, {598, 3989, true}, {599, 4004, true}}) == std::optional<int>(599));
    // no passing run anywhere: nothing is the player's miss (every job stays no_pass_death_unrelated)
    CHECK(!attributeMiss({{1, 100, false}, {2, 120, false}}).has_value());
    CHECK(!attributeMiss({}).has_value());
    // the latest job WITHOUT a pass run does not block an earlier one
    CHECK(attributeMiss({{1, 100, true}, {2, 120, true}, {3, 140, false}}) == std::optional<int>(2));
    // order of the candidates does not matter
    CHECK(attributeMiss({{3, 140, false}, {2, 120, true}, {1, 100, true}}) == std::optional<int>(2));
    // two inputs in one step (same frame): the later job (the later input) wins
    CHECK(attributeMiss({{7, 200, true}, {8, 200, true}}) == std::optional<int>(8));
    CHECK(attributeMiss({{8, 200, true}, {7, 200, true}}) == std::optional<int>(8));
    // a single candidate with a pass run is the miss
    CHECK(attributeMiss({{4, 50, true}}) == std::optional<int>(4));
    // at most one miss per death: every other candidate is miss_downstream
    std::vector<MissCandidate> many;
    for (int i = 0; i < 40; ++i) many.push_back({i + 1, 1000.0 + i * 7.0, i % 3 != 0});
    auto who = attributeMiss(many);
    CHECK(who.has_value() && *who == 39);   // the latest with a pass run (index 38: 38 % 3 != 0)
}

void testDiedWithReal() {
    SECTION("diedWithReal: same step +-1 and the same killer; two object -1 deaths must also be at the same place (|dx| <= 2)");
    CHECK(diedWithReal(4025.0, 1717, 5600.0, 4025.0, 1717, 5610.0));    // an object names the death: x not needed
    CHECK(diedWithReal(4026.0, 8, 0.0, 4025.0, 8, 0.0));                 // one step later
    CHECK(!diedWithReal(4027.5, 8, 0.0, 4025.0, 8, 0.0));                // too late
    CHECK(!diedWithReal(4025.0, 8, 0.0, 4025.0, 9, 0.0));                // another object
    CHECK(diedWithReal(4025.0, -1, 5623.4, 4025.0, -1, 5622.1));         // no object, the same place
    CHECK(!diedWithReal(4025.0, -1, 5630.0, 4025.0, -1, 5622.1));        // no object, elsewhere: not the real death
    CHECK(kMissMatch.frameTolerance == 1.0 && kMissMatch.noObjectXTolerance == 2.0);
}

}  // namespace

int main() {
    testAttributeMiss();
    testDiedWithReal();
    return gprl::test::finish("miss_attribution_tests");
}
