#pragma once
// SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and base64 (RFC 4648), implemented here so core/
// has no dependency. Used for: telemetry batch signatures (ARCHITECTURE §4, X-GPRL-Signature =
// hex HMAC-SHA256(sessionKey bytes, canonical body)), level string hashes, and loaded-module
// fingerprints (SPEC §22/§23). Host-tested against the FIPS, RFC 4231 and RFC 4648 vectors.
//
// Not used for: anything that needs to be secret from the client. The session key it signs with
// is per-session, issued over HTTPS (base64 in V1CreateSessionResponse.sessionKey) and lives only
// in memory (SPEC §23, §51). Signatures provide replay protection and evidence consistency, not
// proof of honesty.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gprl::crypto {

using Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(void const* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    Digest finish();

private:
    void block(uint8_t const* p);
    std::array<uint32_t, 8> m_h{};
    std::array<uint8_t, 64> m_buf{};
    size_t m_bufLen = 0;
    uint64_t m_total = 0;
};

Digest sha256(void const* data, size_t len);
inline Digest sha256(std::string_view s) { return sha256(s.data(), s.size()); }

Digest hmacSha256(void const* key, size_t keyLen, void const* msg, size_t msgLen);
inline Digest hmacSha256(std::string_view key, std::string_view msg) {
    return hmacSha256(key.data(), key.size(), msg.data(), msg.size());
}

std::string toHex(uint8_t const* data, size_t len);
inline std::string toHex(Digest const& d) { return toHex(d.data(), d.size()); }
/// Hex -> bytes; returns false on odd length or non-hex characters.
bool fromHex(std::string_view hex, std::string& out);

/// Standard base64 (RFC 4648 §4) with padding.
std::string toBase64(std::string_view bytes);
/// Decodes standard or URL-safe base64, padding optional; false on any other character.
bool fromBase64(std::string_view text, std::string& out);

/// Constant-time comparison of two byte strings of equal length.
bool constantTimeEquals(std::string_view a, std::string_view b);

}  // namespace gprl::crypto
