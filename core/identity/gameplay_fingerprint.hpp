#pragma once
// gprl-gameplay-fp/2 (docs/LEVEL_FAMILY_DESIGN.md Â§1-Â§2): the tuple hash of one gameplay object,
// the FNV over sorted tuple hashes, the MinHash signature and the level's gameplay fingerprint.
//
//   tuple(o)     = (objectId, gdType, kind, q(rx - xFrom), q(ry), q(rw), q(rh), round(rotation),
//                   round(scaleX * 100), round(scaleY * 100), flipX, flipY, slope, mode, speed, flag,
//                   orb, pad, multiActivate, hidden, groupsHash)         q(v) = round(v * 10)
//   tupleHash(o) = FNV-1a 64 over the 21 integers, each as 8 little-endian bytes
//   setHash      = FNV-1a 64 over the SORTED tuple hashes (8 LE bytes each): object order never matters
//   minhash[k]   = min over objects of high32(splitmix64(tupleHash ^ seed_k)), seed_k = splitmix64(k + 1)
//
// This file does not depend on core/sim/gameplay_hash (the analyzer's own hash keeps StartPos
// objects and may evolve separately); the fingerprint of FA-D1 is defined here.
#include <cstdint>
#include <string>
#include <vector>

#include "identity.hpp"

namespace gprl::identity {

/// splitmix64 finaliser (Steele / Lea / Flood), the MinHash mixing function.
uint64_t splitmix64(uint64_t x);

/// 16 lowercase hex digits.
std::string hex16(uint64_t v);

/// q(v) = round(v / quantum); 0 for non-finite input.
int64_t quantize(double v, double quantum);

/// StartPos objects (kind StartPos / id 31) never enter a fingerprint or a section hash.
bool hashedForIdentity(SimObject const& o);

/// Â§2 tuple hash of one object; `xFrom` = the section start (0 for the absolute fingerprint).
uint64_t tupleHash(SimObject const& o, double xFrom, IdentityConfig const& cfg = kIdentityConfig);

/// FNV-1a 64 over the hashes after sorting them in place.
uint64_t hashSortedTuples(std::vector<uint64_t>& hashes);

/// The MinHash signature of a tuple-hash set (every position 0xffffffff for an empty set).
MinHash minhashOf(std::vector<uint64_t> const& tupleHashes);

/// gprl-gameplay-fp/2 as a value / as 16 hex digits.
uint64_t gameplayFingerprintValue(World const& world, IdentityConfig const& cfg = kIdentityConfig);
std::string gameplayFingerprint(World const& world, IdentityConfig const& cfg = kIdentityConfig);

}  // namespace gprl::identity
