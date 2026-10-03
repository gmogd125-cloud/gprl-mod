// Helper of tests/clip_ffmpeg_check.ps1 (optional check, needs a real ffmpeg.exe): prints the exact
// ffmpeg command lines core/clip builds and runs the pure selection / hashing steps on real files,
// so the strings the mod would run inside Geometry Dash can be run outside it.
//
//   clip_args_tool probe <encoder> <quality>
//   clip_args_tool segment <srcW> <srcH> <quality> <encoder> <csvPath> <segmentPattern>
//   clip_args_tool select <sessionDir> <wallStart> <from> <to> <outConcatFile>
//        reads <sessionDir>/segments.csv like the capture does, keeps what BufferBook keeps, selects
//        [from, to] and writes the ffconcat list; prints "picks N duration D start S end E coversStart B"
//   clip_args_tool mux <concat> <gameWav|-> <micWav|-> <output> <clipId> <attemptId> <title>
//   clip_args_tool hash <file>
// Encoders: h264_nvenc | h264_amf | h264_qsv | libx264.  Quality: 480p | 720p | 1080p.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../core/clip.hpp"
#include "../core/clip_flow.hpp"

using namespace gprl::clip;

namespace {

Encoder parseEncoder(std::string const& s) {
    for (Encoder e : {Encoder::NvencH264, Encoder::AmfH264, Encoder::QsvH264, Encoder::X264}) {
        if (s == name(e)) return e;
    }
    return Encoder::None;
}

int usage() {
    std::fprintf(stderr, "usage: clip_args_tool probe|segment|select|mux|hash ... (see the file header)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string mode = argv[1];
    if (mode == "probe" && argc == 4) {
        Encoder e = parseEncoder(argv[2]);
        auto const& q = spec(parseQuality(argv[3]));
        if (e == Encoder::None) return usage();
        std::printf("%s\n", probeArgs(e, outputSize(1920, 1080, q.maxHeight), q.videoKbps, kClip.fps, kClip.segmentSeconds, kClip.probeFrames).c_str());
        return 0;
    }
    if (mode == "segment" && argc == 8) {
        Size src{std::atoi(argv[2]), std::atoi(argv[3])};
        auto const& q = spec(parseQuality(argv[4]));
        Encoder e = parseEncoder(argv[5]);
        Size out = outputSize(src.width, src.height, q.maxHeight);
        if (e == Encoder::None || out.width == 0) return usage();
        std::fprintf(stderr, "window %dx%d -> %dx%d %s %d kbit/s\n", src.width, src.height, out.width, out.height, name(e), q.videoKbps);
        std::printf("%s\n", segmenterArgs(src, out, e, q.videoKbps, kClip.fps, kClip.segmentSeconds, argv[6], argv[7]).c_str());
        return 0;
    }
    if (mode == "select" && argc == 7) {
        std::filesystem::path dir = argv[2];
        double wallStart = std::atof(argv[3]);
        double from = std::atof(argv[4]);
        double to = std::atof(argv[5]);
        std::ifstream csv(dir / "segments.csv", std::ios::binary);
        if (!csv) {
            std::fprintf(stderr, "no segments.csv in %s\n", argv[2]);
            return 1;
        }
        BufferBook book;
        book.configure(kClip.maxBufferSeconds, 1ll << 40);
        std::string line;
        int lines = 0;
        while (std::getline(csv, line)) {
            auto parsed = parseSegmentCsvLine(line);
            if (!parsed) continue;
            Segment seg;
            seg.session = 1;
            seg.file = parsed->file;
            seg.start = wallStart + parsed->start;
            seg.end = wallStart + parsed->end;
            std::error_code ec;
            seg.bytes = static_cast<int64_t>(std::filesystem::file_size(dir / parsed->file, ec));
            seg.size = {1280, 720};
            seg.encoder = Encoder::X264;
            book.add(seg);
            ++lines;
        }
        book.prune(std::nullopt);
        auto sel = book.select(from, to);
        if (!sel.error.empty()) {
            std::fprintf(stderr, "select: %s (%d csv lines)\n", sel.error.c_str(), lines);
            return 1;
        }
        std::vector<std::pair<std::string, double>> files;
        for (auto const& p : sel.picks) files.emplace_back((dir / p.file).string(), p.end - p.start);
        std::ofstream out(argv[6], std::ios::binary);
        out << concatList(files);
        std::printf("picks %zu duration %.6f start %.6f end %.6f coversStart %d bytes %lld\n", sel.picks.size(), sel.duration, sel.start, sel.end,
                    sel.coversStart ? 1 : 0, static_cast<long long>(book.liveBytes()));
        return 0;
    }
    if (mode == "mux" && argc == 9) {
        MuxInputs in;
        in.concatFile = argv[2];
        in.gameWav = std::string(argv[3]) == "-" ? "" : argv[3];
        in.micWav = std::string(argv[4]) == "-" ? "" : argv[4];
        in.output = argv[5];
        in.clipId = argv[6];
        in.attemptId = argv[7];
        in.title = argv[8];
        std::printf("%s\n", muxArgs(in, kClip.audioKbps).c_str());
        return 0;
    }
    if (mode == "hash" && argc == 3) {
        std::string hex;
        int64_t size = 0;
        if (!hashFile(argv[2], hex, size)) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 1;
        }
        std::printf("%s %lld\n", hex.c_str(), static_cast<long long>(size));
        return 0;
    }
    return usage();
}
