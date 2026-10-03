// core/clip host tests (geode v0.6.0, stream M3, MASTER §16 / SPEC §28): the clipping buffer's pure
// rules - settings clamp, encoded size, disk cost, the ffmpeg command lines, the encoder ladder,
// the segment bookkeeping (what is kept, the disk cap, what a clip is made of), attempt ranges and
// the "is this run exceptional" rule. The capture itself (GL, ffmpeg, FMOD, WASAPI) needs the game.
#include "test_util.hpp"

#include "../core/clip.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::clip;

namespace {

bool has(std::string const& text, char const* needle) { return text.find(needle) != std::string::npos; }

Segment seg(uint64_t session, int index, double start, double seconds = 1.0, int64_t bytes = 625000, Size size = {1280, 720},
            Encoder encoder = Encoder::NvencH264) {
    Segment s;
    s.session = session;
    char name[32];
    std::snprintf(name, sizeof(name), "seg_%05d.mkv", index);
    s.file = name;
    s.start = start;
    s.end = start + seconds;
    s.bytes = bytes;
    s.size = size;
    s.encoder = encoder;
    return s;
}

/// `count` one-second segments of session 1 starting at wall time `from`.
void fill(BufferBook& book, double from, int count, int64_t bytes = 625000) {
    for (int i = 0; i < count; ++i) book.add(seg(1, i, from + i, 1.0, bytes));
}

void testParamsAndConfig() {
    SECTION("ClipParams: one versioned object, the defaults of the spec, the database range");
    CHECK(std::string(kClip.version) == "gprl-clip/1");
    CHECK(kClip.defaultBufferSeconds == 120);
    CHECK(kClip.minBufferSeconds == 10 && kClip.maxBufferSeconds == 600);   // player_settings_clip_buffer CHECK
    CHECK(kClip.fps == 60 && kClip.segmentSeconds == 1.0);
    CHECK(kClip.leadSeconds == 1.0 && kClip.tailSeconds == 3.0 && kClip.maxQueuedFrames == 8);
    CHECK(kClip.encoderFailuresBeforeDemote == 2 && kClip.attemptsTracked == 8);
    CHECK(kClip.soundMaxStoredSeconds >= kClip.maxBufferSeconds + kClip.leadSeconds + kClip.tailSeconds);   // a whole clip's sound fits

    ClipConfig def;
    CHECK(!def.enabled);            // OFF until the player turns Clipping on
    CHECK(!def.micAudio);           // the microphone is its own opt-in
    CHECK(def.gameAudio);
    CHECK(def.bufferSeconds == 120 && def.quality == Quality::P720 && def.diskCapMb == 1024);

    ClipConfig c;
    c.bufferSeconds = 5;
    c.diskCapMb = 1;
    c = sanitized(c);
    CHECK(c.bufferSeconds == 10 && c.diskCapMb == 128);
    c.bufferSeconds = 100000;
    c.diskCapMb = 1 << 30;
    c = sanitized(c);
    CHECK(c.bufferSeconds == 600 && c.diskCapMb == 8192);
    c.quality = static_cast<Quality>(9);
    CHECK(sanitized(c).quality == Quality::P720);

    CHECK(parseQuality("480p") == Quality::P480);
    CHECK(parseQuality("720p") == Quality::P720);
    CHECK(parseQuality("1080p") == Quality::P1080);
    CHECK(parseQuality("4K") == Quality::P720);
    CHECK(spec(Quality::P720).videoKbps == 5000 && spec(Quality::P720).maxHeight == 720);
}

void testOutputSize() {
    SECTION("outputSize: never upscaled, aspect kept, even dimensions");
    CHECK((outputSize(1920, 1080, 720) == Size{1280, 720}));
    CHECK((outputSize(1850, 1041, 720) == Size{1280, 720}));    // the owner's window (odd height)
    CHECK((outputSize(1850, 1041, 1080) == Size{1850, 1040}));  // below the limit: cropped to even, not scaled
    CHECK((outputSize(1440, 810, 720) == Size{1280, 720}));
    CHECK((outputSize(1280, 720, 720) == Size{1280, 720}));
    CHECK((outputSize(800, 600, 720) == Size{800, 600}));
    CHECK((outputSize(2560, 1080, 720) == Size{1706, 720}));    // ultrawide: 21:9 kept
    CHECK((outputSize(3840, 2160, 1080) == Size{1920, 1080}));
    CHECK((outputSize(1001, 563, 480) == Size{854, 480}));
    CHECK((outputSize(8, 8, 720) == Size{0, 0}));
    CHECK((outputSize(0, 0, 720) == Size{0, 0}));
    for (int w : {641, 1000, 1366, 1921, 3440}) {
        for (int h : {361, 768, 1081, 1440}) {
            for (int limit : {480, 720, 1080}) {
                Size s = outputSize(w, h, limit);
                CHECK(s.width % 2 == 0 && s.height % 2 == 0);
                CHECK(s.height <= limit && s.height <= h && s.width <= w + 1);
            }
        }
    }
}

void testCost() {
    SECTION("disk cost: constant bitrate, so the size is bitrate x time");
    CHECK(bufferBytesPerSecond(Quality::P720) == 625000);     // 5 Mbit/s
    CHECK(bufferBytesPerSecond(Quality::P480) == 312500);
    CHECK(bufferBytesPerSecond(Quality::P1080) == 1125000);
    // a 120 s 720p clip with game sound: (5000 + 160) kbit/s * 120 s / 8 * 1.02 = 78.9 MB
    CHECK(clipBytes(Quality::P720, 120.0, 1) == static_cast<int64_t>(std::ceil(120.0 * 5160.0 * 1000.0 / 8.0 * 1.02)));
    CHECK(clipBytes(Quality::P720, 120.0, 1) < 95ll * 1024 * 1024);           // fits the default upload limit
    CHECK(clipBytes(Quality::P720, 120.0, 2) > clipBytes(Quality::P720, 120.0, 1));
    CHECK(clipBytes(Quality::P720, 120.0, 7) == clipBytes(Quality::P720, 120.0, 2));   // at most two tracks
    CHECK(clipBytes(Quality::P720, -5.0, 0) == 0);

    ClipConfig c;
    int64_t need = requiredFreeBytes(c);
    // twice the footage of the buffer (123 s at 625 kB/s) + one full clip with two tracks + the reserve
    int64_t footage = static_cast<int64_t>(2.0 * 123.0 * 625000.0);
    CHECK(need == footage + clipBytes(Quality::P720, 120.0, 2) + kClip.freeSpaceReserveBytes);
    c.diskCapMb = 128;   // the cap is smaller than the footage: the cap counts
    CHECK(requiredFreeBytes(c) == 128ll * 1024 * 1024 + clipBytes(Quality::P720, 120.0, 2) + kClip.freeSpaceReserveBytes);
    CHECK(has(costLine(ClipConfig{}), "720p 60 fps") && has(costLine(ClipConfig{}), "36 MB of disk per minute") && has(costLine(ClipConfig{}), "cap 1024 MB"));
}

void testEncoderArgs() {
    SECTION("encoder options: constant bitrate, closed 1 s GOPs, a forced keyframe per segment");
    for (Encoder e : {Encoder::NvencH264, Encoder::AmfH264, Encoder::QsvH264, Encoder::X264}) {
        std::string a = encoderArgs(e, 5000, 60, 1.0);
        CHECK_MSG(has(a, (std::string("-c:v ") + name(e)).c_str()), a);
        CHECK_MSG(has(a, "-b:v 5000k -maxrate 5000k -bufsize 10000k"), a);
        CHECK_MSG(has(a, "-g 60 -bf 0 -force_key_frames \"expr:gte(t,n_forced*1)\""), a);
    }
    CHECK(has(encoderArgs(Encoder::NvencH264, 5000, 60, 1.0), "-preset p4 -tune hq -rc cbr"));
    CHECK(has(encoderArgs(Encoder::NvencH264, 5000, 60, 1.0), "-forced-idr 1"));
    CHECK(has(encoderArgs(Encoder::X264, 5000, 60, 1.0), "-preset ultrafast"));
    CHECK(has(encoderArgs(Encoder::AmfH264, 5000, 60, 1.0), "-rc cbr") && has(encoderArgs(Encoder::AmfH264, 5000, 60, 1.0), "-max_b_frames 0"));
    CHECK(encoderArgs(Encoder::None, 5000, 60, 1.0).empty());
    CHECK(isHardware(Encoder::NvencH264) && isHardware(Encoder::AmfH264) && isHardware(Encoder::QsvH264) && !isHardware(Encoder::X264));
    CHECK(std::string(name(Encoder::None)) == "none");

    SECTION("probe = the exact encoder options on a synthetic source");
    std::string probe = probeArgs(Encoder::AmfH264, {1280, 720}, 5000, 60, 1.0, 10);
    CHECK_MSG(has(probe, "-f lavfi -i color=black:s=1280x720:r=60 -frames:v 10"), probe);
    CHECK(has(probe, encoderArgs(Encoder::AmfH264, 5000, 60, 1.0).c_str()));
    CHECK(has(probe, "-f null -"));

    SECTION("segmenter: raw BGRA on stdin -> flipped, scaled, BT.709 -> 1 s Matroska segments + CSV list");
    std::string args = segmenterArgs({1850, 1041}, {1280, 720}, Encoder::NvencH264, 5000, 60, 1.0, "D:\\GD Clips\\clip-buffer\\s1\\segments.csv",
                                     "D:\\GD Clips\\clip-buffer\\s1\\seg_%05d.mkv");
    CHECK_MSG(has(args, "-f rawvideo -pix_fmt bgra -video_size 1850x1041 -framerate 60 -i pipe:0"), args);
    CHECK_MSG(has(args, "-vf \"vflip,crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0,scale=1280:720:flags=bilinear:out_color_matrix=bt709:out_range=tv,format=yuv420p\""), args);
    CHECK(has(args, "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"));
    CHECK(has(args, "-f segment -segment_time 1 -segment_format matroska -reset_timestamps 1"));
    CHECK(has(args, "-segment_list \"D:\\GD Clips\\clip-buffer\\s1\\segments.csv\" -segment_list_type csv \"D:\\GD Clips\\clip-buffer\\s1\\seg_%05d.mkv\""));
    CHECK(!has(args, "-c:a") && !has(args, "-i audio"));   // the buffer is video only: sound stays in memory
}

void testEncoderLadder() {
    SECTION("encoder ladder: best available, demoted after two failures in a row, never below the last one");
    EncoderLadder none;
    CHECK(none.current() == Encoder::None);

    EncoderLadder l;
    l.setAvailable(Encoder::NvencH264, true);
    l.setAvailable(Encoder::X264, true);
    CHECK(l.current() == Encoder::NvencH264);
    CHECK(!l.failed(Encoder::NvencH264));          // first failure: retried
    CHECK(l.current() == Encoder::NvencH264);
    l.succeeded(Encoder::NvencH264);                // a good session in between resets the streak
    CHECK(!l.failed(Encoder::NvencH264));
    CHECK(l.failed(Encoder::NvencH264));            // two in a row: demoted
    CHECK(l.demoted(Encoder::NvencH264));
    CHECK(l.current() == Encoder::X264);
    // the last working encoder is never demoted (a transient failure must not end clipping)
    CHECK(!l.failed(Encoder::X264));
    CHECK(!l.failed(Encoder::X264));
    CHECK(!l.failed(Encoder::X264));
    CHECK(l.current() == Encoder::X264 && !l.demoted(Encoder::X264));

    EncoderLadder order;
    order.setAvailable(Encoder::X264, true);
    order.setAvailable(Encoder::QsvH264, true);
    order.setAvailable(Encoder::AmfH264, true);
    CHECK(order.current() == Encoder::AmfH264);     // NVENC not available: AMF before Quick Sync before libx264
    order.setAvailable(Encoder::AmfH264, false);
    CHECK(order.current() == Encoder::QsvH264);
    order.setAvailable(Encoder::None, true);        // ignored
    CHECK(!order.available(Encoder::None));
    CHECK(!order.failed(Encoder::None));
}

void testMuxArgs() {
    SECTION("mux: video stream copy, one AAC track per WAV, the game sound first, ids in the metadata");
    MuxInputs in;
    in.concatFile = "D:\\b\\work\\c\\source.ffconcat";
    in.output = "D:\\b\\clips\\pending\\clip-1.mp4";
    in.clipId = "clip-19a0b1c2d3e-5f6a7b8c";
    in.attemptId = "abc123-a7";
    in.title = "GPRL Bloodbath 100% attempt 2041";

    std::string silent = muxArgs(in, 160);
    CHECK_MSG(has(silent, "-f concat -safe 0 -i \"D:\\b\\work\\c\\source.ffconcat\" -map 0:v:0 -c:v copy -an"), silent);
    CHECK(!has(silent, "-c:a"));
    CHECK(has(silent, "-metadata comment=\"gprl clip clip-19a0b1c2d3e-5f6a7b8c attempt abc123-a7\""));
    CHECK(has(silent, "-movflags +faststart \"D:\\b\\clips\\pending\\clip-1.mp4\""));
    CHECK_MSG(has(silent, "-metadata title=\"GPRL Bloodbath 100_ attempt 2041\""), silent);   // '%' never reaches ffmpeg

    in.gameWav = "D:\\b\\work\\c\\game.wav";
    std::string game = muxArgs(in, 160);
    CHECK_MSG(has(game, "-i \"D:\\b\\work\\c\\game.wav\" -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 160k -ar 48000 -ac 2"), game);
    CHECK(has(game, "-metadata:s:a:0 title=\"Game\" -metadata:s:a:0 handler_name=\"Game\" -disposition:a:0 default"));
    CHECK(!has(game, "Microphone"));

    in.micWav = "D:\\b\\work\\c\\mic.wav";
    std::string both = muxArgs(in, 160);
    CHECK_MSG(has(both, "-map 0:v:0 -map 1:a:0 -map 2:a:0"), both);
    CHECK(has(both, "-metadata:s:a:0 title=\"Game\" -metadata:s:a:0 handler_name=\"Game\" -disposition:a:0 default"));
    // its own, non-default track: removable
    CHECK(has(both, "-metadata:s:a:1 title=\"Microphone\" -metadata:s:a:1 handler_name=\"Microphone\" -disposition:a:1 0"));

    // v0.9.0: the desktop sound is a third, removable track after the microphone
    in.desktopWav = "D:\\b\\work\\c\\desktop.wav";
    std::string three = muxArgs(in, 160);
    CHECK_MSG(has(three, "-map 0:v:0 -map 1:a:0 -map 2:a:0 -map 3:a:0"), three);
    CHECK(has(three, "-metadata:s:a:2 title=\"Desktop\" -metadata:s:a:2 handler_name=\"Desktop\" -disposition:a:2 0"));
    in.desktopWav.clear();

    in.gameWav.clear();
    std::string micOnly = muxArgs(in, 160);
    CHECK_MSG(has(micOnly, "-map 0:v:0 -map 1:a:0"), micOnly);
    CHECK(has(micOnly, "-metadata:s:a:0 title=\"Microphone\" -metadata:s:a:0 handler_name=\"Microphone\" -disposition:a:0 default"));
    CHECK(!has(micOnly, "\"Game\""));

    SECTION("concat list + text that is safe inside a quoted argument");
    CHECK(concatPath("D:\\GD Clips\\it's\\part_0001.mkv") == "D:/GD Clips/it'\\''s/part_0001.mkv");
    std::string list = concatList({{"D:\\w\\part_0000.mkv", 1.0}, {"D:\\w\\part_0001.mkv", 0.983333}});
    CHECK(list == "ffconcat version 1.0\nfile 'D:/w/part_0000.mkv'\nduration 1.000000\nfile 'D:/w/part_0001.mkv'\nduration 0.983333\n");
    CHECK(safeText("a\"b\\c\n\xc3\xa9%d") == "a_b_c____d");   // quote, backslash, newline, two UTF-8 bytes, percent
    CHECK(safeText("trailing   ") == "trailing");
    CHECK(safeText(std::string(200, 'x')).size() == 80);
    CHECK(safeText("abcdef", 3) == "abc");
}

void testCsv() {
    SECTION("segment list lines (ffmpeg -segment_list_type csv)");
    auto a = parseSegmentCsvLine("seg_00012.mkv,12.000000,13.000000");
    CHECK(a && a->file == "seg_00012.mkv" && a->start == 12.0 && a->end == 13.0);
    auto crlf = parseSegmentCsvLine("seg_00000.mkv,0.000000,1.016667\r\n");
    CHECK(crlf && crlf->end == 1.016667);
    auto comma = parseSegmentCsvLine("odd,name.mkv,3.5,4.5");       // the last two fields are the times
    CHECK(comma && comma->file == "odd,name.mkv" && comma->start == 3.5);
    CHECK(!parseSegmentCsvLine(""));
    CHECK(!parseSegmentCsvLine("seg_00001.mkv"));
    CHECK(!parseSegmentCsvLine("seg_00001.mkv,1.0"));
    CHECK(!parseSegmentCsvLine(",1.0,2.0"));
    CHECK(!parseSegmentCsvLine("seg.mkv,abc,2.0"));
    CHECK(!parseSegmentCsvLine("seg.mkv,1.0,2.0x"));
    CHECK(!parseSegmentCsvLine("seg.mkv,2.0,2.0"));   // empty
    CHECK(!parseSegmentCsvLine("seg.mkv,3.0,2.0"));   // backwards
    CHECK(!parseSegmentCsvLine("seg.mkv,-1.0,2.0"));
    CHECK(!parseSegmentCsvLine("seg.mkv,nan,2.0"));
    CHECK(!parseSegmentCsvLine("seg.mkv,1.0,inf"));
}

void testRollingWindow() {
    SECTION("rolling window: the newest bufferSeconds (+ margin) of footage stays, older segments go");
    BufferBook book;
    book.configure(10, 1ll << 40);   // 10 s, no cap in practice
    fill(book, 1000.0, 30);
    CHECK(book.segments().size() == 30 && book.recordedSeconds() == 30.0);
    auto removed = book.prune(std::nullopt);
    // kept: footage newer than the segment < 10 + 3 s  => the newest 13 segments
    CHECK(removed.size() == 17);
    CHECK(book.segments().size() == 13);
    CHECK(book.segments().front().start == 1017.0);
    CHECK(removed.front().file == "seg_00000.mkv" && removed.back().file == "seg_00016.mkv");
    CHECK(book.recordedSeconds() == 13.0);
    CHECK(book.liveBytes() == 13 * 625000);
    CHECK(*book.oldestStart() == 1017.0);
    CHECK(!book.capLimited());
    // nothing new: a second prune removes nothing
    CHECK(book.prune(std::nullopt).empty());
    // one more second of footage pushes exactly one segment out
    book.add(seg(1, 30, 1030.0));
    removed = book.prune(std::nullopt);
    CHECK(removed.size() == 1 && removed[0].start == 1017.0);

    SECTION("the window counts RECORDED footage, not wall time (a stall does not empty the buffer)");
    BufferBook gap;
    gap.configure(10, 1ll << 40);
    fill(gap, 0.0, 8);                    // 8 s, then the game was minimised for an hour
    for (int i = 0; i < 4; ++i) gap.add(seg(2, i, 3600.0 + i));
    CHECK(gap.prune(std::nullopt).empty());   // 12 s of footage < 13 s: everything stays
    CHECK(gap.segments().size() == 12);

    SECTION("segments arriving out of order are kept sorted by start");
    BufferBook sorted;
    sorted.configure(600, 1ll << 40);
    sorted.add(seg(2, 0, 50.0));
    sorted.add(seg(1, 0, 10.0));
    sorted.add(seg(1, 1, 11.0));
    sorted.add(seg(1, 2, 9.0, 0.0));   // empty: ignored
    CHECK(sorted.segments().size() == 3);
    CHECK(sorted.segments()[0].start == 10.0 && sorted.segments()[1].start == 11.0 && sorted.segments()[2].start == 50.0);

    SECTION("configure clamps like the settings");
    BufferBook clamp;
    clamp.configure(1, 1);
    CHECK(clamp.bufferSeconds() == 10 && clamp.capBytes() == 128ll * 1024 * 1024);
    clamp.configure(99999, 1ll << 40);
    CHECK(clamp.bufferSeconds() == 600);
}

void testOpenAttempt() {
    SECTION("the open attempt is kept whole beyond the window, up to 600 s of footage");
    BufferBook book;
    book.configure(10, 1ll << 40);
    fill(book, 1000.0, 100);                       // the attempt started at 1020 and is still running at 1100
    auto removed = book.prune(1020.0);
    // kept from 1020 - lead (1 s): segments ending after 1019 => starts 1019 .. 1099
    CHECK(book.segments().front().start == 1019.0);
    CHECK(book.segments().size() == 81 && removed.size() == 19);
    // the attempt ended (no open attempt any more): back to the rolling window
    removed = book.prune(std::nullopt);
    CHECK(book.segments().size() == 13 && removed.size() == 68);

    BufferBook longRun;
    longRun.configure(10, 1ll << 40);
    fill(longRun, 0.0, 700);                       // an attempt that has been running for 700 s
    longRun.prune(0.0);
    CHECK(longRun.segments().size() == 603);       // 600 + margin: the oldest 97 s are gone
    CHECK(longRun.segments().front().start == 97.0);
}

void testPins() {
    SECTION("a pinned range (a preserved attempt being cut) survives the window and the cap");
    BufferBook book;
    book.configure(10, 1ll << 40);
    fill(book, 1000.0, 60);
    book.pin("clip-a", 1005.0, 1012.0);
    CHECK(book.hasPin("clip-a") && !book.hasPin("clip-b"));
    auto removed = book.prune(std::nullopt);
    // window: starts 1047..1059 (13); pin: segments overlapping (1005, 1012) => starts 1005..1011 (7)
    CHECK(book.segments().size() == 20);
    CHECK(book.segments().front().start == 1005.0 && book.segments()[6].start == 1011.0 && book.segments()[7].start == 1047.0);
    CHECK(removed.size() == 40);
    // re-pinning the same id moves the range
    book.pin("clip-a", 1008.0, 1010.0);
    removed = book.prune(std::nullopt);
    CHECK(removed.size() == 5 && book.segments().front().start == 1008.0);
    book.unpin("clip-a");
    CHECK(!book.hasPin("clip-a"));
    removed = book.prune(std::nullopt);
    CHECK(removed.size() == 2 && book.segments().size() == 13);

    SECTION("sound ranges follow the kept footage and the pins");
    BufferBook sound;
    CHECK(sound.soundRanges().empty());
    sound.configure(10, 1ll << 40);
    fill(sound, 500.0, 5);
    sound.pin("p", 100.0, 110.0);
    auto ranges = sound.soundRanges();
    CHECK(ranges.size() == 2);
    CHECK(ranges[0].from == 498.0 && ranges[0].to > 1e17);
    CHECK(ranges[1].from == 98.0 && ranges[1].to == 112.0);
}

void testDiskCap() {
    SECTION("disk cap: the oldest unpinned footage goes first, pinned footage never");
    BufferBook book;
    int64_t mb = 1024 * 1024;
    book.configure(600, 128 * mb);
    fill(book, 0.0, 300, 1 * mb);                 // 300 MB of footage, all inside the 600 s window
    auto removed = book.prune(std::nullopt);
    CHECK(book.capLimited());
    CHECK(book.liveBytes() == 128 * mb);
    CHECK(removed.size() == 172 && removed.front().start == 0.0 && removed.back().start == 171.0);
    CHECK(book.segments().front().start == 172.0);
    // under the cap again: not limited
    book.configure(600, 4096 * mb);
    CHECK(book.prune(std::nullopt).empty());
    CHECK(!book.capLimited());

    BufferBook pinned;
    pinned.configure(600, 128 * mb);
    fill(pinned, 0.0, 300, 1 * mb);
    pinned.pin("clip", 0.0, 50.0);                 // the oldest 50 s are being cut into a clip
    removed = pinned.prune(std::nullopt);
    CHECK(pinned.capLimited());
    CHECK(pinned.liveBytes() == 128 * mb);
    CHECK(pinned.segments().front().start == 0.0 && pinned.segments()[49].start == 49.0);
    CHECK(pinned.segments()[50].start == 222.0);   // 78 MB of unpinned footage left: the newest 78 s
    for (auto const& r : removed) CHECK(r.start >= 50.0 && r.start < 222.0);

    SECTION("clear() forgets everything (clipping turned off)");
    auto all = pinned.clear();
    CHECK(all.size() == 128 && pinned.segments().empty() && pinned.liveBytes() == 0 && !pinned.oldestStart());

    // Verifier (stream M3): the cap as a property, not an example. Random segment sizes / lengths,
    // stalls, open attempts and pins; after EVERY prune the kept bytes are within the cap, the one
    // exception being footage pinned for a clip that is being cut (then nothing unpinned is left).
    SECTION("disk cap property: 300 000 prunes of random footage never keep unpinned bytes over the cap");
    uint32_t seed = 12345u;
    auto rnd = [&seed](uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    int prunes = 0, overCap = 0, overCapWithUnpinned = 0, windowViolations = 0, longViolations = 0;
    for (int trial = 0; trial < 400; ++trial) {
        BufferBook book;
        int bufferSeconds = 10 + static_cast<int>(rnd(591));
        int64_t cap = (128ll + rnd(2000)) * mb;
        book.configure(bufferSeconds, cap);
        double t = 1000.0;
        uint64_t session = 1;
        std::vector<std::pair<std::string, KeepRange>> pins;
        std::optional<double> open;
        for (int step = 0; step < 1500; ++step) {
            if (rnd(50) == 0) {   // a stall: a new recording session later
                t += 1.0 + rnd(20);
                ++session;
            }
            double d = 0.5 + rnd(1000) / 1000.0;
            book.add(seg(session, step, t, d, 100000 + static_cast<int64_t>(rnd(3000000))));
            t += d;
            if (rnd(97) == 0) open = t - rnd(700);
            if (rnd(131) == 0) open.reset();
            if (rnd(211) == 0) {
                std::string id = "p" + std::to_string(step);
                double from = t - rnd(650);
                book.pin(id, from, t + 3.0);
                pins.push_back({id, {from, t + 3.0}});
            }
            if (!pins.empty() && rnd(40) == 0) {
                book.unpin(pins.front().first);
                pins.erase(pins.begin());
            }
            if (step % 2) continue;
            book.prune(open);
            ++prunes;
            int64_t unpinnedBytes = 0;
            double recorded = 0.0;
            for (auto const& g : book.segments()) {
                bool isPinned = false;
                for (auto const& p : pins) isPinned = isPinned || (g.end > p.second.from && g.start < p.second.to);
                if (!isPinned) unpinnedBytes += g.bytes;
                recorded += g.end - g.start;
            }
            if (book.liveBytes() > cap) {
                ++overCap;
                if (unpinnedBytes > 0) ++overCapWithUnpinned;
            }
            // never more footage than the window (no open attempt, no pin) / 600 s (no pin), + one segment
            if (!open && pins.empty() && recorded > bufferSeconds + kClip.keepMarginSeconds + 1.5) ++windowViolations;
            if (pins.empty() && recorded > kClip.maxBufferSeconds + kClip.keepMarginSeconds + 1.5) ++longViolations;
        }
    }
    CHECK(prunes == 300000);
    CHECK(overCap > 0);   // the walk did reach "pinned footage alone is over the cap"
    CHECK_MSG(overCapWithUnpinned == 0, "unpinned footage was kept while the buffer was over its cap");
    CHECK(windowViolations == 0 && longViolations == 0);
}

void testSelect() {
    SECTION("select: the segments of a wall-clock range, their duration, whether the start is covered");
    BufferBook book;
    book.configure(600, 1ll << 40);
    fill(book, 1000.0, 60);
    auto sel = book.select(1010.4, 1020.2);
    CHECK(sel.error.empty());
    CHECK(sel.picks.size() == 11);                  // 1010 .. 1020 (the segments holding both edges)
    CHECK(sel.picks.front().start == 1010.0 && sel.picks.back().start == 1020.0);
    CHECK(sel.start == 1010.0 && sel.end == 1021.0 && sel.duration == 11.0);
    CHECK(sel.coversStart);
    // a range that starts before the buffer: what exists is used and the clip says so
    auto early = book.select(900.0, 1005.0);
    CHECK(early.error.empty() && early.picks.size() == 5 && !early.coversStart);
    // a range whose end is still in the future: everything recorded so far
    auto future = book.select(1055.5, 1070.0);
    CHECK(future.picks.size() == 5 && future.end == 1060.0 && future.coversStart);
    CHECK(!book.select(2000.0, 2010.0).error.empty());
    CHECK(!book.select(1010.0, 1010.0).error.empty());
    CHECK(!book.select(1020.0, 1010.0).error.empty());
    BufferBook empty;
    CHECK(!empty.select(0.0, 10.0).error.empty());

    SECTION("one clip = one encoded size and one encoder: the newest such run wins");
    BufferBook mixed;
    mixed.configure(600, 1ll << 40);
    for (int i = 0; i < 5; ++i) mixed.add(seg(1, i, 100.0 + i, 1.0, 1000, {1280, 720}));
    for (int i = 0; i < 5; ++i) mixed.add(seg(2, i, 105.0 + i, 1.0, 1000, {1706, 720}));   // the window was resized
    auto resized = mixed.select(100.0, 110.0);
    CHECK(resized.picks.size() == 5 && resized.picks.front().session == 2 && !resized.coversStart);
    BufferBook enc;
    enc.configure(600, 1ll << 40);
    for (int i = 0; i < 3; ++i) enc.add(seg(1, i, 100.0 + i, 1.0, 1000, {1280, 720}, Encoder::NvencH264));
    for (int i = 0; i < 3; ++i) enc.add(seg(2, i, 103.0 + i, 1.0, 1000, {1280, 720}, Encoder::X264));   // NVENC was demoted
    CHECK(enc.select(100.0, 106.0).picks.size() == 3);
    // two sessions of the same size and encoder (a stall in between) join into one clip
    BufferBook stall;
    stall.configure(600, 1ll << 40);
    for (int i = 0; i < 3; ++i) stall.add(seg(1, i, 100.0 + i));
    for (int i = 0; i < 3; ++i) stall.add(seg(2, i, 110.0 + i));
    auto joined = stall.select(100.0, 113.0);
    CHECK(joined.picks.size() == 6 && joined.duration == 6.0 && joined.start == 100.0 && joined.end == 113.0);
}

void testAttempts() {
    SECTION("attempt book: open / closed attempts and the range a preserved attempt covers");
    AttemptBook book;
    CHECK(!book.openStart());
    AttemptMark a;
    a.attemptId = "s1-a1";
    a.sessionLocalId = "s1";
    a.attemptNo = 41;
    a.start = 100.0;
    book.started(a);
    CHECK(book.openStart() && *book.openStart() == 100.0);
    CHECK(book.find("s1-a1") && book.find("s1-a1")->end == 0.0);
    CHECK(!book.find("nope"));
    // an open attempt's range ends at "now" + tail
    auto open = preserveRange(*book.find("s1-a1"), 130.0);
    CHECK(open.from == 99.0 && open.to == 133.0);
    CHECK(book.ended("s1-a1", 160.0, 100.0, true, true, 59.5, 14280));
    CHECK(!book.ended("s1-a1", 170.0, 50.0, false, true, 1.0, 1));   // already closed
    CHECK(!book.ended("unknown", 170.0, 50.0, false, true, 1.0, 1));
    auto const* m = book.find("s1-a1");
    CHECK(m->end == 160.0 && m->completed && m->percent == 100.0 && m->endT == 59.5 && m->endTick == 14280);
    CHECK(!book.openStart());
    auto range = preserveRange(*m, 999.0);
    CHECK(range.from == 100.0 - kClip.leadSeconds && range.to == 160.0 + kClip.tailSeconds);

    // a new attempt closes a stale open one; only the last 8 are tracked
    for (int i = 2; i <= 12; ++i) {
        AttemptMark n;
        n.attemptId = "s1-a" + std::to_string(i);
        n.start = 200.0 + i;
        book.started(n);
    }
    CHECK(book.size() == 8);
    CHECK(!book.find("s1-a1") && !book.find("s1-a4") && book.find("s1-a5"));
    CHECK(book.find("s1-a11")->end == 212.0);        // closed by the next start
    CHECK(*book.openStart() == 212.0);
    book.closeOpen(300.0);
    CHECK(!book.openStart() && book.find("s1-a12")->end == 300.0);
    // percent is clamped, an end before the start is pulled up
    AttemptMark odd;
    odd.attemptId = "odd";
    odd.start = 500.0;
    book.started(odd);
    CHECK(book.ended("odd", 400.0, 250.0, false, true, 0.0, 0));
    CHECK(book.find("odd")->end == 500.0 && book.find("odd")->percent == 100.0);
}

PreserveFacts completion() {
    PreserveFacts f;
    f.clippingEnabled = true;
    f.recording = true;
    f.completed = true;
    f.legit = true;
    f.fromStart = true;
    f.serverLevelCounts = true;
    return f;
}

void testPreserveRule() {
    SECTION("local rule (no server hint yet): legit completion from 0 % of a level that counts");
    auto d = decidePreserve(completion());
    CHECK(d.preserve && std::string(d.rule) == "local_completion");

    auto f = completion();
    f.clippingEnabled = false;
    CHECK(!decidePreserve(f).preserve && decidePreserve(f).why == "clipping is off");
    f = completion();
    f.recording = false;
    CHECK(!decidePreserve(f).preserve);
    f = completion();
    f.completed = false;
    CHECK(!decidePreserve(f).preserve && decidePreserve(f).why == "not a completion");
    f = completion();
    f.practice = true;
    CHECK(!decidePreserve(f).preserve);
    f = completion();
    f.fromStart = false;
    CHECK(!decidePreserve(f).preserve);
    f = completion();
    f.legit = false;                      // noclip / bot / modified physics seen
    CHECK(!decidePreserve(f).preserve);
    f = completion();
    f.serverLevelCounts = false;          // the server's "Not a rated demon: not counted"
    f.localRatedDemon = true;             // GD's own data never overrides the server's verdict
    CHECK(!decidePreserve(f).preserve);

    SECTION("no server verdict (not connected / local-only): GD's own rated-demon data stands in");
    f = completion();
    f.serverLevelCounts.reset();
    f.localRatedDemon = false;
    CHECK(!decidePreserve(f).preserve && decidePreserve(f).why == "not a rated demon");
    f.localRatedDemon = true;
    CHECK(decidePreserve(f).preserve && std::string(decidePreserve(f).rule) == "local_completion");

    SECTION("server hint: only named attempts, and the local rule is off once the server evaluates");
    f = completion();
    f.serverHintsSupported = true;        // the server sends preserveEvidence and did NOT name this completion
    CHECK(!decidePreserve(f).preserve);
    f.serverHinted = true;
    CHECK(decidePreserve(f).preserve && std::string(decidePreserve(f).rule) == "server_hint");
    // a named attempt is preserved whatever it is (a death, practice, from a StartPos): the server decides
    PreserveFacts death;
    death.clippingEnabled = true;
    death.recording = true;
    death.serverHintsSupported = true;
    death.serverHinted = true;
    death.completed = false;
    death.practice = true;
    death.fromStart = false;
    death.legit = false;
    CHECK(decidePreserve(death).preserve);
    // but never while clipping is off or nothing was recorded
    death.recording = false;
    CHECK(!decidePreserve(death).preserve);
    death.recording = true;
    death.clippingEnabled = false;
    CHECK(!decidePreserve(death).preserve);
    // every refusal explains itself (the Geode log line)
    CHECK(!decidePreserve(death).why.empty());
}

}  // namespace

int main() {
    testParamsAndConfig();
    testOutputSize();
    testCost();
    testEncoderArgs();
    testEncoderLadder();
    testMuxArgs();
    testCsv();
    testRollingWindow();
    testOpenAttempt();
    testPins();
    testDiskCap();
    testSelect();
    testAttempts();
    testPreserveRule();
    return gprl::test::finish("clip_tests");
}
