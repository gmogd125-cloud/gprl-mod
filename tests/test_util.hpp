#pragma once
// Minimal host test harness (no framework, no Geode). Each test file defines its own main().
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace gprl::test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool ok, char const* expr, char const* file, int line, std::string const& note = {}) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, note.empty() ? "" : " -- ", note.c_str());
}

inline bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

inline std::string readFile(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool fileExists(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    return static_cast<bool>(f);
}

inline int finish(char const* suite) {
    std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace gprl::test

#define CHECK(expr) ::gprl::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_MSG(expr, msg) ::gprl::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__, (msg))
#define CHECK_NEAR(a, b, tol) ::gprl::test::check(::gprl::test::near((a), (b), (tol)), #a " ~= " #b, __FILE__, __LINE__, \
    std::string("got ") + std::to_string(static_cast<double>(a)) + " expected " + std::to_string(static_cast<double>(b)))
#define SECTION(name) std::printf("- %s\n", name)
