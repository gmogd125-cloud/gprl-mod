// core/updater host tests (v0.12.1 self-update): version parsing / ordering, the GitHub
// "latest release" answer (what is taken, what is refused), and the download check.
//
// Live check after a release (tools/release-mod.ps1 saves the API answer):
//   updater_tests.exe <repo> --live <latest-release.json> <downloaded gmo12.gprl.geode> [installed version]
// runs the real answer and the real file through the same functions the mod uses.
#include "test_util.hpp"

#include "../core/crypto.hpp"
#include "../core/json.hpp"
#include "../core/updater.hpp"

#include <string>

using namespace gprl;
using namespace gprl::updater;

namespace {

Version v(std::string_view s) {
    Version out{-1, -1, -1};
    CHECK_MSG(parseVersion(s, out), std::string(s));
    return out;
}

void testVersions() {
    SECTION("parseVersion / compare / isNewer");
    Version x;
    CHECK(parseVersion("v0.12.1", x) && x.major == 0 && x.minor == 12 && x.patch == 1);
    CHECK(parseVersion("1.2.3", x) && x.major == 1 && x.minor == 2 && x.patch == 3);
    CHECK(parseVersion("V10.0.0", x) && x.major == 10);
    CHECK(!parseVersion("", x));
    CHECK(!parseVersion("v", x));
    CHECK(!parseVersion("v1.2", x));
    CHECK(!parseVersion("v1.2.3-beta.1", x));
    CHECK(!parseVersion("v1.2.3+build", x));
    CHECK(!parseVersion("v1.2.3.4", x));
    CHECK(!parseVersion("v01.2.3", x));
    CHECK(!parseVersion("v1.2.x", x));
    CHECK(!parseVersion(" v1.2.3", x));
    CHECK(!parseVersion("v1.2.3 ", x));
    CHECK(!parseVersion("v123456.0.0", x));

    CHECK(compare(v("v0.12.1"), v("v0.12.0")) > 0);
    CHECK(compare(v("v0.12.0"), v("v0.12.1")) < 0);
    CHECK(compare(v("v0.12.1"), v("0.12.1")) == 0);
    CHECK(compare(v("v0.13.0"), v("v0.12.9")) > 0);
    CHECK(compare(v("v1.0.0"), v("v0.99.99")) > 0);
    CHECK(compare(v("v0.12.10"), v("v0.12.9")) > 0);   // numeric, not text order

    CHECK(isNewer(v("v0.12.0"), v("v0.12.1")));
    CHECK(!isNewer(v("v0.12.1"), v("v0.12.1")));       // same version: nothing to do
    CHECK(!isNewer(v("v0.12.2"), v("v0.12.1")));       // never a downgrade (a newer dev build stays)
    CHECK(toString(v("1.2.3")) == "v1.2.3");
}

std::string const kSha = "3b6f4a2e9c1d0b8a7f6e5d4c3b2a19087f6e5d4c3b2a19087f6e5d4c3b2a1908";

/// Shape of a real GET /repos/<repo>/releases/latest answer (trimmed).
std::string releaseJson(std::string const& tag = "v0.12.1", std::string const& asset = "gmo12.gprl.geode",
                        std::string const& url = "https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode",
                        std::string const& digest = "sha256:3B6F4A2E9C1D0B8A7F6E5D4C3B2A19087F6E5D4C3B2A19087F6E5D4C3B2A1908",
                        std::string const& extra = "", int64_t size = 4769791) {
    return R"({"url":"https://api.github.com/repos/gmogd125-cloud/gprl-mod/releases/1","tag_name":")" + tag +
           R"(","target_commitish":"main","name":"GPRL )" + tag + R"(","draft":false,"prerelease":false,)" + extra +
           R"("assets":[{"name":"notes.txt","size":10,"digest":"sha256:)" + kSha +
           R"(","state":"uploaded","browser_download_url":"https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/notes.txt"},)" +
           R"({"name":")" + asset + R"(","label":null,"content_type":"application/octet-stream","state":"uploaded","size":)" +
           std::to_string(size) + R"(,"digest":")" + digest + R"(","download_count":0,"browser_download_url":")" + url +
           R"("}],"body":"## Changes\n- disclaimer"})";
}

bool parse(std::string const& text, Release& r, std::string& why) {
    json::Value body;
    json::ParseError pe;
    if (!json::parse(text, body, &pe)) {
        why = "test json: " + pe.message;
        return false;
    }
    return parseLatestRelease(body, r, why);
}

void testRelease() {
    SECTION("parseLatestRelease");
    Release r;
    std::string why;
    CHECK_MSG(parse(releaseJson(), r, why), why);
    CHECK(r.tag == "v0.12.1");
    CHECK(compare(r.version, v("v0.12.1")) == 0);
    CHECK(r.title == "GPRL v0.12.1");
    CHECK(r.assetSize == 4769791);
    CHECK(r.sha256Hex == kSha);   // upper-case digest from the server is normalised
    CHECK(r.assetUrl == "https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode");

    CHECK(!parse(releaseJson("v0.12.1-beta.1"), r, why));
    CHECK(!parse(releaseJson("nightly"), r, why));
    CHECK(!parse(releaseJson("v0.12.1", "gprl.zip"), r, why));
    CHECK(why.find("no gmo12.gprl.geode") != std::string::npos);
    // a download URL outside this repo's releases (another repo / another host / plain http)
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", "https://github.com/someone-else/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode"), r, why));
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", "https://evil.example/gmogd125-cloud/gprl-mod/releases/download/x"), r, why));
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", "http://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode"), r, why));
    // the digest is required and must be sha256 of the right length
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", "https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode", ""), r, why));
    CHECK(why.find("digest") != std::string::npos);
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", "https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode", "md5:abcd"), r, why));
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", "https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode", "sha256:abc"), r, why));
    // sizes
    std::string const okUrl = "https://github.com/gmogd125-cloud/gprl-mod/releases/download/v0.12.1/gmo12.gprl.geode";
    std::string const okDigest = "sha256:" + kSha;
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", okUrl, okDigest, "", 0), r, why));
    CHECK(!parse(releaseJson("v0.12.1", "gmo12.gprl.geode", okUrl, okDigest, "", kMaxAssetBytes + 1), r, why));
    // drafts / prereleases are never taken
    json::Value body;
    CHECK(json::parse(releaseJson(), body));
    body.set("prerelease", true);
    CHECK(!parseLatestRelease(body, r, why));
    CHECK(json::parse(releaseJson(), body));
    body.set("draft", true);
    CHECK(!parseLatestRelease(body, r, why));
    // not an object at all (e.g. GitHub's {"message":"Not Found"} is an object without a tag)
    CHECK(json::parse(R"({"message":"Not Found","documentation_url":"https://docs.github.com"})", body));
    CHECK(!parseLatestRelease(body, r, why));
    CHECK(json::parse("[]", body));
    CHECK(!parseLatestRelease(body, r, why));
}

void testVerify() {
    SECTION("verifyAsset");
    std::string bytes = std::string("PK\x03\x04", 4) + "rest of a zip file";
    Release r;
    r.assetSize = static_cast<int64_t>(bytes.size());
    r.sha256Hex = crypto::toHex(crypto::sha256(bytes));
    std::string why;
    CHECK_MSG(verifyAsset(bytes, r, why), why);

    std::string changed = bytes;
    changed.back() ^= 1;
    CHECK(!verifyAsset(changed, r, why));
    CHECK(why.find("SHA-256") != std::string::npos);

    CHECK(!verifyAsset(bytes + "x", r, why));
    CHECK(why.find("bytes") != std::string::npos);

    std::string notZip = "<html>rate limited</html>";
    Release r2;
    r2.assetSize = static_cast<int64_t>(notZip.size());
    r2.sha256Hex = crypto::toHex(crypto::sha256(notZip));
    CHECK(!verifyAsset(notZip, r2, why));
    CHECK(why.find("zip") != std::string::npos);
}

}  // namespace

int live(char const* jsonPath, char const* assetPath, char const* installed) {
    SECTION("live release");
    std::string text = gprl::test::readFile(jsonPath);
    std::string bytes = gprl::test::readFile(assetPath);
    CHECK_MSG(!text.empty(), jsonPath);
    CHECK_MSG(!bytes.empty(), assetPath);
    json::Value body;
    json::ParseError pe;
    CHECK_MSG(json::parse(text, body, &pe), pe.message);
    Release r;
    std::string why;
    bool ok = parseLatestRelease(body, r, why);
    CHECK_MSG(ok, why);
    if (ok) {
        std::printf("  release %s, %lld bytes, sha256 %s\n  %s\n", r.tag.c_str(), static_cast<long long>(r.assetSize), r.sha256Hex.c_str(), r.assetUrl.c_str());
        CHECK_MSG(verifyAsset(bytes, r, why), why);
        if (installed) {
            Version iv;
            CHECK(parseVersion(installed, iv));
            std::printf("  installed %s -> %s\n", installed, isNewer(iv, r.version) ? "UPDATE" : "up to date");
        }
    }
    return gprl::test::finish("updater_tests --live");
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::string(argv[2]) == "--live") return live(argv[3], argv[4], argc >= 6 ? argv[5] : nullptr);
    testVersions();
    testRelease();
    testVerify();
    return gprl::test::finish("updater_tests");
}
