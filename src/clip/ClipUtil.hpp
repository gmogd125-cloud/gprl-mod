#pragma once
// Small Windows / Geode helpers of the clipping buffer (stream M3, docs/CLIPPING.md). The capture
// path is adapted from the owner's In-Game Clipper mod (D:\GeodeMods\ingame-clipper, gmo12): the
// same steady clock for video and sound, the same child-process wrapper, the same ffmpeg lookup.
#include <filesystem>
#include <string>

namespace gprl::clip {

/// Seconds on the steady (QueryPerformanceCounter) clock. Video frames, the game sound and the
/// microphone are all stamped with it, so a clip's tracks line up.
double now();

std::wstring widen(std::string const& s);
std::string narrow(std::wstring const& s);
std::wstring quoted(std::filesystem::path const& p);
/// UTF-8 text of a path (for ffmpeg arguments, the log and the index).
std::string utf8(std::filesystem::path const& p);
std::filesystem::path fromUtf8(std::string const& s);
/// The last `maxChars` of a text file (an ffmpeg log), for the Geode log.
std::string readTail(std::filesystem::path const& file, size_t maxChars);

struct Paths {
    std::filesystem::path ffmpeg;       // empty when not found
    std::string ffmpegSource;           // "setting" | "mod save folder" | "In-Game Clipper" | "PATH" | ""
    std::filesystem::path root;         // the folder everything lives under (setting, else the mod save dir)
    std::filesystem::path bufferRoot;   // <root>/gprl-clip-buffer   (rolling segments; wiped at every game start)
    std::filesystem::path clipsDir;     // <root>/gprl-clips         (saved clips)
    std::filesystem::path pendingDir;   // <root>/gprl-clips/pending (prepared clips waiting for a choice)
    std::filesystem::path indexFile;    // <mod save dir>/clips.json
};
/// MAIN THREAD (reads Geode settings / mod dirs). ffmpeg is looked up like the In-Game Clipper
/// does: the `ffmpeg-path` setting, ffmpeg.exe in this mod's save / config folder, the In-Game
/// Clipper's own setting and save folder when that mod is installed, then PATH.
Paths resolvePaths(std::filesystem::path const& ffmpegSetting, std::filesystem::path const& folderSetting);

}  // namespace gprl::clip
