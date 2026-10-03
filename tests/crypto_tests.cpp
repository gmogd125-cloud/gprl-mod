// core/crypto host tests: SHA-256 (FIPS 180-4 examples), HMAC-SHA256 (RFC 4231 test cases 1-7),
// base64 (RFC 4648 §10 vectors) and the hex helpers.
#include "test_util.hpp"

#include "../core/crypto.hpp"

#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::crypto;

namespace {

std::string bytes(std::string_view hex) {
    std::string out;
    CHECK(fromHex(hex, out));
    return out;
}

std::string repeat(char c, size_t n) { return std::string(n, c); }

void testSha256Vectors() {
    SECTION("SHA-256 FIPS 180-4 vectors");
    CHECK(toHex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(toHex(sha256(repeat('a', 1'000'000))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // streaming update in odd chunk sizes gives the same digest
    std::string million = repeat('a', 1'000'000);
    Sha256 h;
    size_t pos = 0;
    size_t chunk = 1;
    while (pos < million.size()) {
        size_t n = std::min(chunk, million.size() - pos);
        h.update(million.data() + pos, n);
        pos += n;
        chunk = (chunk * 7 + 3) % 1000 + 1;
    }
    CHECK(toHex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 55 / 56 / 64 byte messages exercise the padding boundaries
    CHECK(toHex(sha256(repeat('x', 55))) == toHex(sha256(std::string_view(repeat('x', 55)))));
    Sha256 a, b;
    std::string s56 = repeat('b', 56);
    a.update(s56);
    b.update(s56.substr(0, 20));
    b.update(s56.substr(20));
    CHECK(toHex(a.finish()) == toHex(b.finish()));
}

void testHmacRfc4231() {
    SECTION("HMAC-SHA256 RFC 4231 test cases");
    // TC1
    CHECK(toHex(hmacSha256(bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"), "Hi There"))
          == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    // TC2
    CHECK(toHex(hmacSha256("Jefe", "what do ya want for nothing?"))
          == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // TC3
    CHECK(toHex(hmacSha256(bytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), repeat('\xdd', 50)))
          == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    // TC4
    CHECK(toHex(hmacSha256(bytes("0102030405060708090a0b0c0d0e0f10111213141516171819"), repeat('\xcd', 50)))
          == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    // TC5 (truncated to 128 bits)
    CHECK(toHex(hmacSha256(bytes("0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c"), "Test With Truncation")).substr(0, 32)
          == "a3b6167473100ee06e0c796c2955552b");
    // TC6: key longer than the block size
    CHECK(toHex(hmacSha256(repeat('\xaa', 131), "Test Using Larger Than Block-Size Key - Hash Key First"))
          == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    // TC7
    CHECK(toHex(hmacSha256(repeat('\xaa', 131),
                           "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed "
                           "before being used by the HMAC algorithm."))
          == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

void testBase64() {
    SECTION("base64 RFC 4648 vectors + decoding variants");
    CHECK(toBase64("") == "");
    CHECK(toBase64("f") == "Zg==");
    CHECK(toBase64("fo") == "Zm8=");
    CHECK(toBase64("foo") == "Zm9v");
    CHECK(toBase64("foob") == "Zm9vYg==");
    CHECK(toBase64("fooba") == "Zm9vYmE=");
    CHECK(toBase64("foobar") == "Zm9vYmFy");
    std::string out;
    CHECK(fromBase64("Zm9vYmFy", out) && out == "foobar");
    CHECK(fromBase64("Zm9vYg==", out) && out == "foob");
    CHECK(fromBase64("Zm9vYg", out) && out == "foob");   // padding optional
    CHECK(fromBase64("-_-_", out) && out == bytes("fbffbf"));   // URL-safe alphabet
    CHECK(!fromBase64("Zm9v!", out));
    CHECK(!fromBase64("Z", out));   // a lone sextet cannot encode a byte
    // round trip of every byte value
    std::string all;
    for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
    CHECK(fromBase64(toBase64(all), out) && out == all);
}

void testHexAndConstantTime() {
    SECTION("hex + constant-time compare");
    std::string out;
    CHECK(fromHex("00ff7Aa5", out) && out == std::string("\x00\xff\x7a\xa5", 4));
    CHECK(!fromHex("abc", out));
    CHECK(!fromHex("zz", out));
    CHECK(toHex(reinterpret_cast<uint8_t const*>("\x01\xab"), 2) == "01ab");
    CHECK(constantTimeEquals("abc", "abc"));
    CHECK(!constantTimeEquals("abc", "abd"));
    CHECK(!constantTimeEquals("abc", "abcd"));
}

}  // namespace

int main() {
    testSha256Vectors();
    testHmacRfc4231();
    testBase64();
    testHexAndConstantTime();
    return gprl::test::finish("crypto_tests");
}
