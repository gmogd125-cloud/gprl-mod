#include "config.hpp"

#include <cctype>

namespace gprl::config {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool endsWith(std::string const& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Host part of "host[:port]" (brackets kept for IPv6 literals).
std::string hostOf(std::string const& authority) {
    if (!authority.empty() && authority.front() == '[') {
        auto close = authority.find(']');
        return close == std::string::npos ? authority : authority.substr(0, close + 1);
    }
    auto colon = authority.find(':');
    return colon == std::string::npos ? authority : authority.substr(0, colon);
}

bool isLocalHost(std::string const& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

}  // namespace

std::string normalizeApiBaseUrl(std::string_view input) {
    size_t b = 0, e = input.size();
    while (b < e && std::isspace(static_cast<unsigned char>(input[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(input[e - 1]))) --e;
    std::string s(input.substr(b, e - b));
    if (s.empty()) return {};

    std::string scheme = "https";
    auto sep = s.find("://");
    if (sep != std::string::npos) {
        scheme = lower(std::string_view(s).substr(0, sep));
        s = s.substr(sep + 3);
    }
    // cut query / fragment
    auto qf = s.find_first_of("?#");
    if (qf != std::string::npos) s = s.substr(0, qf);

    auto slash = s.find('/');
    std::string authority = lower(slash == std::string::npos ? s : s.substr(0, slash));
    std::string path = slash == std::string::npos ? std::string() : s.substr(slash);

    // trailing slashes, then one trailing /v1 or /api (the mod appends /v1/... itself)
    auto trimSlashes = [](std::string& p) {
        while (!p.empty() && p.back() == '/') p.pop_back();
    };
    trimSlashes(path);
    for (std::string_view suffix : {"/v1", "/api"}) {
        std::string lp = lower(path);
        if (endsWith(lp, suffix)) {
            path.resize(path.size() - suffix.size());
            trimSlashes(path);
            break;
        }
    }

    if (scheme == "http" && !isLocalHost(hostOf(authority))) scheme = "https";
    return scheme + "://" + authority + path;
}

bool apiBaseUrlIsValid(std::string_view normalized) {
    std::string s(normalized);
    std::string rest;
    if (s.rfind("https://", 0) == 0) rest = s.substr(8);
    else if (s.rfind("http://", 0) == 0) rest = s.substr(7);
    else return false;
    auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string host = hostOf(authority);
    if (host.empty()) return false;
    if (host.front() == '[') {
        if (host.back() != ']' || host.size() < 3) return false;
    } else {
        for (char c : host) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!ok) return false;
        }
        if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos) return false;
    }
    if (authority.size() > host.size()) {  // ":port"
        std::string port = authority.substr(host.size());
        if (port.size() < 2 || port[0] != ':') return false;
        for (size_t i = 1; i < port.size(); ++i)
            if (port[i] < '0' || port[i] > '9') return false;
    }
    for (char c : s)
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    return true;
}

bool apiBaseUrlIsPlaceholder(std::string_view normalized) {
    if (normalized.empty()) return true;
    if (normalized == kPlaceholderApi) return true;
    std::string s = lower(normalized);
    return s.find(".example.") != std::string::npos || s.find("example.workers.dev") != std::string::npos;
}

std::string effectiveApiBaseUrl(std::string_view settingValue) {
    std::string normalized = normalizeApiBaseUrl(settingValue);
    if (apiBaseUrlIsPlaceholder(normalized)) return kDefaultApi;
    return normalized;
}

}  // namespace gprl::config
