// core/config host tests: API base URL normalisation / validation / placeholder rule.
#include "test_util.hpp"

#include "../core/config.hpp"

#include <string>

using namespace gprl::config;

namespace {

void testNormalizeApiBaseUrl() {
    SECTION("normalizeApiBaseUrl");
    CHECK(normalizeApiBaseUrl("https://gprl-api.gmo.workers.dev") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("  https://gprl-api.gmo.workers.dev/  ") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("gprl-api.gmo.workers.dev") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("HTTPS://GPRL-API.Gmo.Workers.Dev") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("http://gprl-api.gmo.workers.dev") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("https://gprl-api.gmo.workers.dev/v1") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("https://gprl-api.gmo.workers.dev/v1/") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("https://gprl-api.gmo.workers.dev/api") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("https://gprl-api.gmo.workers.dev/?x=1#top") == "https://gprl-api.gmo.workers.dev");
    CHECK(normalizeApiBaseUrl("https://example.com/gprl/v1") == "https://example.com/gprl");
    CHECK(normalizeApiBaseUrl("http://127.0.0.1:8787") == "http://127.0.0.1:8787");
    CHECK(normalizeApiBaseUrl("http://localhost:8787/v1") == "http://localhost:8787");
    CHECK(normalizeApiBaseUrl("   ").empty());
    CHECK(normalizeApiBaseUrl("").empty());
}

void testValidity() {
    SECTION("apiBaseUrlIsValid");
    CHECK(apiBaseUrlIsValid(normalizeApiBaseUrl("gprl-api.gmo.workers.dev")));
    CHECK(apiBaseUrlIsValid("http://127.0.0.1:8787"));
    CHECK(apiBaseUrlIsValid("http://[::1]:8787"));
    CHECK(!apiBaseUrlIsValid(""));
    CHECK(!apiBaseUrlIsValid("https://"));
    CHECK(!apiBaseUrlIsValid("ftp://x.dev"));
    CHECK(!apiBaseUrlIsValid(normalizeApiBaseUrl("gprl api.workers.dev")));
    CHECK(!apiBaseUrlIsValid("https://bad_host.dev"));
    CHECK(!apiBaseUrlIsValid("https://x.dev:80a"));
    CHECK(!apiBaseUrlIsValid("https://.dev"));
}

void testPlaceholder() {
    SECTION("apiBaseUrlIsPlaceholder");
    CHECK(apiBaseUrlIsPlaceholder(""));
    CHECK(apiBaseUrlIsPlaceholder(kPlaceholderApi));
    CHECK(apiBaseUrlIsPlaceholder(normalizeApiBaseUrl("https://gprl-api.example.workers.dev/")));
    CHECK(apiBaseUrlIsPlaceholder("https://api.example.com"));
    CHECK(!apiBaseUrlIsPlaceholder("https://gprl-api.gmo.workers.dev"));
    CHECK(!apiBaseUrlIsPlaceholder(kDefaultApi));
}

void testEffective() {
    SECTION("effectiveApiBaseUrl: saved placeholder / empty -> deployed Worker");
    CHECK(effectiveApiBaseUrl(kPlaceholderApi) == kDefaultApi);
    CHECK(effectiveApiBaseUrl("https://gprl-api.example.workers.dev/") == kDefaultApi);
    CHECK(effectiveApiBaseUrl("") == kDefaultApi);
    CHECK(effectiveApiBaseUrl("   ") == kDefaultApi);
    CHECK(effectiveApiBaseUrl(kDefaultApi) == kDefaultApi);
    CHECK(effectiveApiBaseUrl("gprl-api.gprl.workers.dev/v1") == kDefaultApi);
    CHECK(effectiveApiBaseUrl("http://127.0.0.1:8787") == "http://127.0.0.1:8787");
    CHECK(effectiveApiBaseUrl("https://gprl-api.other.workers.dev") == "https://gprl-api.other.workers.dev");
    CHECK(apiBaseUrlIsValid(kDefaultApi));
}

}  // namespace

int main() {
    testNormalizeApiBaseUrl();
    testValidity();
    testPlaceholder();
    testEffective();
    return gprl::test::finish("config_tests");
}
