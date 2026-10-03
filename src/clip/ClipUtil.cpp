#include "ClipUtil.hpp"

#include <Geode/Geode.hpp>

#include <Windows.h>

#include <chrono>
#include <fstream>

using namespace geode::prelude;

namespace gprl::clip {

namespace {

constexpr char const* kInGameClipperId = "gmo12.ingame_clipper";

bool isFile(std::filesystem::path const& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

}  // namespace

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::wstring widen(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring const& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring quoted(std::filesystem::path const& p) { return L"\"" + p.wstring() + L"\""; }

std::string utf8(std::filesystem::path const& p) { return narrow(p.wstring()); }

std::filesystem::path fromUtf8(std::string const& s) { return std::filesystem::path(widen(s)); }

std::string readTail(std::filesystem::path const& file, size_t maxChars) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(in.tellg());
    size_t start = size > maxChars ? size - maxChars : 0;
    in.seekg(static_cast<std::streamoff>(start));
    std::string out(size - start, '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    // one log line: the Geode log is read line by line
    for (char& c : out) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return out;
}

Paths resolvePaths(std::filesystem::path const& ffmpegSetting, std::filesystem::path const& folderSetting) {
    auto* mod = Mod::get();
    Paths paths;
    std::error_code ec;

    if (isFile(ffmpegSetting)) {
        paths.ffmpeg = ffmpegSetting;
        paths.ffmpegSource = "setting";
    }
    if (paths.ffmpeg.empty()) {
        for (auto const& candidate : {mod->getSaveDir() / "ffmpeg.exe", mod->getConfigDir() / "ffmpeg.exe"}) {
            if (isFile(candidate)) {
                paths.ffmpeg = candidate;
                paths.ffmpegSource = "mod save folder";
                break;
            }
        }
    }
    if (paths.ffmpeg.empty()) {
        // the owner's In-Game Clipper keeps its own ffmpeg.exe: reuse it instead of asking twice
        std::filesystem::path theirs;
        if (auto* clipper = Loader::get()->getLoadedMod(kInGameClipperId)) {
            theirs = clipper->getSettingValue<std::filesystem::path>("ffmpeg-path");
            if (!isFile(theirs)) theirs = clipper->getSaveDir() / "ffmpeg.exe";
        }
        if (!isFile(theirs)) theirs = mod->getSaveDir().parent_path() / kInGameClipperId / "ffmpeg.exe";
        if (isFile(theirs)) {
            paths.ffmpeg = theirs;
            paths.ffmpegSource = "In-Game Clipper";
        }
    }
    if (paths.ffmpeg.empty()) {
        wchar_t found[MAX_PATH * 2] = {};
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr) > 0) {
            paths.ffmpeg = found;
            paths.ffmpegSource = "PATH";
        }
    }

    paths.root = folderSetting;
    if (paths.root.empty() || !std::filesystem::is_directory(paths.root, ec)) paths.root = mod->getSaveDir();
    // named after the mod: the folder may be shared with other tools (the In-Game Clipper's D:\GD Clips)
    paths.bufferRoot = paths.root / "gprl-clip-buffer";
    paths.clipsDir = paths.root / "gprl-clips";
    paths.pendingDir = paths.clipsDir / "pending";
    paths.indexFile = mod->getSaveDir() / "clips.json";
    return paths;
}

}  // namespace gprl::clip
