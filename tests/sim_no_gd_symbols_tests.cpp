// Isolation proof for the offline simulator (docs/BACKGROUND_ANALYZER_DESIGN.md §10.1, AN-D1):
// every file under geode/core/sim is scanned at RUN time and the suite fails when the CODE of any
// of them names a Geode / cocos2d / Geometry Dash symbol. Comments and string literals are
// stripped first: the sources CITE GD functions by name as the provenance of every constant
// ("PlayerObject::updateJump @0x38b900"), which is documentation, not a dependency. The host
// build itself is the second proof: tests/run_tests.ps1 compiles core/sim without the SDK.
//
// Usage: sim_no_gd_symbols_tests.exe <repo root>
#include "test_util.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Removes // and /* */ comments and the contents of string / char literals.
std::string stripCommentsAndStrings(std::string const& src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i += 2;
            out.push_back(' ');
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            out.push_back(q);
            ++i;
            while (i < n && src[i] != q) {
                if (src[i] == '\\') ++i;
                ++i;
            }
            out.push_back(q);
            ++i;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

/// Whole-identifier occurrence of `word` in `code`.
bool containsIdentifier(std::string const& code, std::string const& word) {
    size_t pos = 0;
    while ((pos = code.find(word, pos)) != std::string::npos) {
        bool leftOk = pos == 0 || !isIdentChar(code[pos - 1]);
        bool rightOk = pos + word.size() >= code.size() || !isIdentChar(code[pos + word.size()]);
        if (leftOk && rightOk) return true;
        pos += word.size();
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string repo = argc > 1 ? argv[1] : "D:/GPRL";
    fs::path dir = fs::path(repo) / "geode" / "core" / "sim";
    SECTION("every file under geode/core/sim is free of Geode / cocos2d / GD symbols in its code");
    CHECK_MSG(fs::exists(dir), "core/sim directory exists at " + dir.string());
    std::vector<std::string> const substrings = {"#include <Geode", "#include <cocos2d", "#include \"Geode", "gd::", "geode::", "cocos2d::"};
    std::vector<std::string> const identifiers = {"PlayLayer", "PlayerObject", "GJBaseGameLayer", "GameObject", "CCNode", "CCRect",
                                                  "CCPoint", "CCObject", "GJGameLevel", "GameManager", "EffectGameObject",
                                                  "RingObject", "StartPosObject", "LevelSettingsObject"};
    int files = 0;
    if (fs::exists(dir)) {
        for (auto const& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".hpp" && ext != ".cpp" && ext != ".h" && ext != ".inc") continue;
            ++files;
            std::string src = gprl::test::readFile(entry.path().string());
            CHECK_MSG(!src.empty(), "readable: " + entry.path().string());
            std::string code = stripCommentsAndStrings(src);
            for (auto const& s : substrings) {
                CHECK_MSG(code.find(s) == std::string::npos, entry.path().filename().string() + " contains " + s);
            }
            for (auto const& id : identifiers) {
                CHECK_MSG(!containsIdentifier(code, id), entry.path().filename().string() + " names " + id);
            }
        }
    }
    CHECK_MSG(files >= 3, "at least the physics / collision / engine sources were scanned (" + std::to_string(files) + ")");
    std::printf("  scanned %d files under %s\n", files, dir.string().c_str());

    SECTION("the stripper keeps code and drops comments and strings");
    std::string sample = "int a; // PlayLayer here\n/* GameObject */ char const* s = \"CCNode\"; GJBase x;";
    std::string code = stripCommentsAndStrings(sample);
    CHECK(!containsIdentifier(code, "PlayLayer"));
    CHECK(!containsIdentifier(code, "GameObject"));
    CHECK(!containsIdentifier(code, "CCNode"));
    CHECK(containsIdentifier(code, "GJBase"));
    CHECK(containsIdentifier(code, "int"));
    CHECK(!containsIdentifier("SimObject obj;", "Object"));
    return gprl::test::finish("sim_no_gd_symbols_tests");
}
