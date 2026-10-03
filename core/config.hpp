#pragma once
// Pure helpers for the API base URL the player may type into the mod settings (the site origin
// and the connection rules live in core/identity). No Geode includes: compiled by the mod AND by the
// host tests (tests/config_tests.cpp).
#include <string>
#include <string_view>

namespace gprl::config {

/// PLACEHOLDER API base URL that mod.json shipped before the GPRL Worker was deployed (v0.1.0-v0.1.1).
/// Geode keeps a saved setting value, so players who ran those builds still have it saved.
inline constexpr char const* kPlaceholderApi = "https://gprl-api.example.workers.dev";

/// The deployed GPRL Worker (production). mod.json's default for api-base-url.
inline constexpr char const* kDefaultApi = "https://gprl-api.gprl.workers.dev";

/// The URL the mod actually uses: the normalised setting, except that an empty value or a
/// placeholder (see apiBaseUrlIsPlaceholder) means kDefaultApi. Local-only mode has its own toggle.
std::string effectiveApiBaseUrl(std::string_view settingValue);

/// Cleans a typed / pasted API base URL so path joins are predictable:
///   - trims whitespace; lowercases the scheme and host
///   - no scheme -> "https://" (e.g. "gprl-api.me.workers.dev")
///   - "http://" on a non-local host -> "https://" (Workers redirect http, which would turn a POST
///     into a GET); http stays for localhost / 127.0.0.1 (wrangler dev)
///   - drops a query / fragment, trailing slashes and a trailing "/v1" or "/api" (the mod appends
///     "/v1/..." itself; pasting the /v1 or /api URL from the docs must still work)
/// Returns "" for input that is empty after trimming.
std::string normalizeApiBaseUrl(std::string_view input);

/// True when a NORMALISED URL is usable: http(s)://host[:port][/path], host made of
/// [a-z0-9.-] (or a bracketed IPv6 literal), no spaces.
bool apiBaseUrlIsValid(std::string_view normalized);

/// True while the URL is the shipped placeholder / any *.example.* host / empty.
bool apiBaseUrlIsPlaceholder(std::string_view normalized);

}  // namespace gprl::config
