#pragma once
// Gameplay hash `gprl-gameplay-hash/1` (docs/BACKGROUND_ANALYZER_DESIGN.md AN-D7): the cache key
// part that identifies a GAMEPLAY version of a level. Two levels with the same hash play the same
// for the simulator: the same gameplay objects at the same quantized places with the same
// gameplay settings, the same start state, the same StartPos objects and the same unsupported
// spans. Decoration, colours, names, creator, metadata, object order and unique ids never enter.
//
//   stream   FNV-1a 64 over a canonical integer / string stream (geometry_hash.hpp mixing: every
//            integer as 8 little-endian bytes, strings as length + bytes)
//   objects  every non-decoration object as the tuple (objectId, gdType, kind, round(x*10),
//            round(y*10), round(rx*10), round(ry*10), round(rw*10), round(rh*10), round(rotation),
//            round(scaleX*100), round(scaleY*100), flipX, flipY, slope, mode, speed, flag, orb,
//            pad, multiActivate, hidden, groupsHash, groupCount), sorted by the tuple itself
//            (primary (round(x*10), round(y*10), objectId)) so World::objects order is irrelevant
//   start    the StartState fields; startPositions (count + each) unless ignoreStartPos
//   spans    unsupported spans (round(x0*10), round(x1*10), mechanic, objectId)
//   extent   groundY, ceilingY, lengthX, endX rounded to 0.1
//   output   16 lowercase hex digits
//
// StartPos objects ARE gameplay for the simulator (they decide where attempts start), so they are
// in the default hash. The level-family stream (next) needs a hash WITHOUT them: pass
// {ignoreStartPos = true}. Pure C++20, host-tested in tests/sim_hash_tests.cpp.
#include <cstdint>
#include <string>

#include "world.hpp"

namespace gprl::sim {

struct GameplayHashOptions {
    bool ignoreStartPos = false;   // leave the StartPos objects out (level-family identity)
};

/// 64-bit value of the gameplay hash.
uint64_t gameplayHashValue(World const& world, GameplayHashOptions const& options = {});
/// 16 lowercase hex digits (what LevelSimResult.gameplayHash carries).
std::string gameplayHash(World const& world, GameplayHashOptions const& options = {});
/// Formats a 64-bit hash as 16 lowercase hex digits.
std::string hashHex(uint64_t value);

}  // namespace gprl::sim
