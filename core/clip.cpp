#include "clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gprl::clip {

namespace {

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

/// Double-quoted for an ffmpeg command line (named dquote: an unqualified `quoted` would find std::quoted).
std::string dquote(std::string const& s) { return "\"" + s + "\""; }

/// Names a sound track: `title` for Matroska-style players, `handler_name` is what MP4 stores.
std::string trackName(int track, char const* name) {
    std::string t = std::to_string(track);
    return " -metadata:s:a:" + t + " title=\"" + name + "\" -metadata:s:a:" + t + " handler_name=\"" + name + "\"";
}

constexpr char const* kColorTags = "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";

double duration(Segment const& s) { return std::max(0.0, s.end - s.start); }

}  // namespace

// ---- settings ----

Quality parseQuality(std::string_view label) {
    for (int i = 0; i < 3; ++i) {
        if (label == kQualities[i].label) return static_cast<Quality>(i);
    }
    return Quality::P720;
}

ClipConfig sanitized(ClipConfig c) {
    c.bufferSeconds = std::clamp(c.bufferSeconds, kClip.minBufferSeconds, kClip.maxBufferSeconds);
    c.diskCapMb = std::clamp(c.diskCapMb, kClip.minDiskCapMb, kClip.maxDiskCapMb);
    if (static_cast<int>(c.quality) < 0 || static_cast<int>(c.quality) > 2) c.quality = Quality::P720;
    return c;
}

Size outputSize(int srcWidth, int srcHeight, int maxHeight) {
    if (srcWidth < 16 || srcHeight < 16 || maxHeight < 16) return {};
    // the segmenter crops the window to even dimensions first (H.264 4:2:0)
    int w = srcWidth & ~1;
    int h = srcHeight & ~1;
    if (h <= maxHeight) return {w, h};
    int outH = maxHeight & ~1;
    // aspect kept, rounded to the nearest even width
    int outW = static_cast<int>(std::lround(static_cast<double>(w) * outH / h / 2.0)) * 2;
    return {std::max(outW, 16), outH};
}

int64_t bufferBytesPerSecond(Quality q) { return static_cast<int64_t>(spec(q).videoKbps) * 1000 / 8; }

int64_t clipBytes(Quality q, double seconds, int audioTracks) {
    audioTracks = std::clamp(audioTracks, 0, 2);
    double kbps = spec(q).videoKbps + audioTracks * kClip.audioKbps;
    // + 2 % container overhead (index, faststart)
    return static_cast<int64_t>(std::ceil(std::max(0.0, seconds) * kbps * 1000.0 / 8.0 * 1.02));
}

int64_t requiredFreeBytes(ClipConfig const& raw) {
    ClipConfig c = sanitized(raw);
    int64_t cap = static_cast<int64_t>(c.diskCapMb) * 1024 * 1024;
    int64_t footage = static_cast<int64_t>(2.0 * (c.bufferSeconds + kClip.keepMarginSeconds) * static_cast<double>(bufferBytesPerSecond(c.quality)));
    return std::min(cap, footage) + clipBytes(c.quality, c.bufferSeconds, 2) + kClip.freeSpaceReserveBytes;
}

std::string costLine(ClipConfig const& raw) {
    ClipConfig c = sanitized(raw);
    double mbPerMinute = static_cast<double>(bufferBytesPerSecond(c.quality)) * 60.0 / (1024.0 * 1024.0);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %d fps, about %.0f MB of disk per minute kept, buffer %d s, cap %d MB", spec(c.quality).label, kClip.fps,
                  mbPerMinute, c.bufferSeconds, c.diskCapMb);
    return buf;
}

// ---- encoders ----

char const* name(Encoder e) {
    switch (e) {
        case Encoder::NvencH264: return "h264_nvenc";
        case Encoder::AmfH264: return "h264_amf";
        case Encoder::QsvH264: return "h264_qsv";
        case Encoder::X264: return "libx264";
        case Encoder::None: break;
    }
    return "none";
}

bool isHardware(Encoder e) { return e == Encoder::NvencH264 || e == Encoder::AmfH264 || e == Encoder::QsvH264; }

std::string encoderArgs(Encoder e, int videoKbps, int fps, double segmentSeconds) {
    int k = std::max(videoKbps, 100);
    std::string rate = "-b:v " + std::to_string(k) + "k -maxrate " + std::to_string(k) + "k -bufsize " + std::to_string(k * 2) + "k";
    // closed GOPs of one segment, no B-frames, a forced keyframe at every segment boundary
    int gop = std::max(1, static_cast<int>(std::lround(fps * segmentSeconds)));
    std::string keys = "-g " + std::to_string(gop) + " -bf 0 -force_key_frames \"expr:gte(t,n_forced*" + num(segmentSeconds) + ")\"";
    switch (e) {
        case Encoder::NvencH264:
            return "-c:v h264_nvenc -preset p4 -tune hq -rc cbr " + rate + " -profile:v high -forced-idr 1 " + keys;
        case Encoder::AmfH264:
            return "-c:v h264_amf -usage transcoding -quality speed -rc cbr " + rate + " -profile:v high -max_b_frames 0 " + keys;
        case Encoder::QsvH264:
            return "-c:v h264_qsv -preset veryfast " + rate + " -profile:v high -forced_idr 1 " + keys;
        case Encoder::X264:
            return "-c:v libx264 -preset ultrafast " + rate + " " + keys;
        case Encoder::None: break;
    }
    return {};
}

std::string probeArgs(Encoder e, Size out, int videoKbps, int fps, double segmentSeconds, int frames) {
    return "-hide_banner -loglevel error -f lavfi -i color=black:s=" + std::to_string(out.width) + "x" + std::to_string(out.height) + ":r="
        + std::to_string(fps) + " -frames:v " + std::to_string(frames) + " -pix_fmt yuv420p " + encoderArgs(e, videoKbps, fps, segmentSeconds)
        + " -f null -";
}

std::string segmenterArgs(Size src, Size out, Encoder e, int videoKbps, int fps, double segmentSeconds, std::string const& csvPath,
                          std::string const& segmentPattern) {
    std::string filter = "vflip,crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0,scale=" + std::to_string(out.width) + ":" + std::to_string(out.height)
        + ":flags=bilinear:out_color_matrix=bt709:out_range=tv,format=yuv420p";
    return "-hide_banner -loglevel warning -f rawvideo -pix_fmt bgra -video_size " + std::to_string(src.width) + "x" + std::to_string(src.height)
        + " -framerate " + std::to_string(fps) + " -i pipe:0 -vf " + dquote(filter) + " " + encoderArgs(e, videoKbps, fps, segmentSeconds) + " "
        + kColorTags + " -f segment -segment_time " + num(segmentSeconds) + " -segment_format matroska -reset_timestamps 1 -segment_list "
        + dquote(csvPath) + " -segment_list_type csv " + dquote(segmentPattern);
}

void EncoderLadder::setAvailable(Encoder e, bool ok) {
    if (e == Encoder::None) return;
    m_available[static_cast<int>(e)] = ok;
}

Encoder EncoderLadder::current() const {
    for (Encoder e : {Encoder::NvencH264, Encoder::AmfH264, Encoder::QsvH264, Encoder::X264}) {
        int i = static_cast<int>(e);
        if (m_available[i] && !m_demoted[i]) return e;
    }
    return Encoder::None;
}

bool EncoderLadder::failed(Encoder e) {
    if (e == Encoder::None) return false;
    int i = static_cast<int>(e);
    if (m_demoted[i]) return false;
    if (++m_failures[i] < kClip.encoderFailuresBeforeDemote) return false;
    // the last working encoder is never demoted: a transient failure must not end clipping for good
    int others = 0;
    for (int k = 1; k < kEncoderCount; ++k) {
        if (k != i && m_available[k] && !m_demoted[k]) ++others;
    }
    if (others == 0) {
        m_failures[i] = 0;
        return false;
    }
    m_demoted[i] = true;
    return true;
}

void EncoderLadder::succeeded(Encoder e) {
    if (e == Encoder::None) return;
    m_failures[static_cast<int>(e)] = 0;
}

// ---- clip mux ----

std::string muxArgs(MuxInputs const& in, int audioKbps) {
    std::string args = "-y -hide_banner -loglevel warning -f concat -safe 0 -i " + dquote(in.concatFile);
    int inputs = 1;
    int gameInput = -1, micInput = -1, desktopInput = -1;
    if (!in.gameWav.empty()) {
        args += " -i " + dquote(in.gameWav);
        gameInput = inputs++;
    }
    if (!in.micWav.empty()) {
        args += " -i " + dquote(in.micWav);
        micInput = inputs++;
    }
    if (!in.desktopWav.empty()) {
        args += " -i " + dquote(in.desktopWav);
        desktopInput = inputs++;
    }
    args += " -map 0:v:0";
    int track = 0;
    std::string meta;
    if (gameInput >= 0) {
        args += " -map " + std::to_string(gameInput) + ":a:0";
        meta += trackName(track, "Game") + " -disposition:a:" + std::to_string(track) + " default";
        ++track;
    }
    if (micInput >= 0) {
        args += " -map " + std::to_string(micInput) + ":a:0";
        meta += trackName(track, "Microphone") + " -disposition:a:" + std::to_string(track) + (track == 0 ? " default" : " 0");
        ++track;
    }
    if (desktopInput >= 0) {
        args += " -map " + std::to_string(desktopInput) + ":a:0";
        meta += trackName(track, "Desktop") + " -disposition:a:" + std::to_string(track) + (track == 0 ? " default" : " 0");
        ++track;
    }
    args += " -c:v copy";
    if (track > 0) args += " -c:a aac -b:a " + std::to_string(audioKbps) + "k -ar 48000 -ac 2" + meta;
    else args += " -an";
    args += " -metadata title=" + dquote(safeText(in.title)) + " -metadata comment="
        + dquote("gprl clip " + safeText(in.clipId, 64) + " attempt " + safeText(in.attemptId, 64)) + " -movflags +faststart " + dquote(in.output);
    return args;
}

std::string concatPath(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\\') out += '/';
        else if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out;
}

std::string concatList(std::vector<std::pair<std::string, double>> const& files) {
    std::string out = "ffconcat version 1.0\n";
    char buf[64];
    for (auto const& [file, seconds] : files) {
        out += "file '" + concatPath(file) + "'\n";
        std::snprintf(buf, sizeof(buf), "duration %.6f\n", seconds);
        out += buf;
    }
    return out;
}

std::string safeText(std::string_view text, size_t maxLen) {
    std::string out;
    for (char c : text) {
        if (out.size() >= maxLen) break;
        auto u = static_cast<unsigned char>(c);
        bool ok = u >= 32 && u < 127 && c != '"' && c != '\\' && c != '%';
        out += ok ? c : '_';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---- segment bookkeeping ----

std::optional<CsvSegment> parseSegmentCsvLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    auto c2 = line.rfind(',');
    if (c2 == std::string_view::npos || c2 == 0) return std::nullopt;
    auto c1 = line.rfind(',', c2 - 1);
    if (c1 == std::string_view::npos || c1 == 0) return std::nullopt;
    CsvSegment seg;
    seg.file = std::string(line.substr(0, c1));
    std::string a(line.substr(c1 + 1, c2 - c1 - 1));
    std::string b(line.substr(c2 + 1));
    char* endA = nullptr;
    char* endB = nullptr;
    seg.start = std::strtod(a.c_str(), &endA);
    seg.end = std::strtod(b.c_str(), &endB);
    if (a.empty() || b.empty() || endA != a.c_str() + a.size() || endB != b.c_str() + b.size()) return std::nullopt;
    if (!std::isfinite(seg.start) || !std::isfinite(seg.end) || seg.start < 0.0 || seg.end <= seg.start) return std::nullopt;
    return seg;
}

void BufferBook::configure(int bufferSeconds, int64_t capBytes) {
    m_bufferSeconds = std::clamp(bufferSeconds, kClip.minBufferSeconds, kClip.maxBufferSeconds);
    m_capBytes = std::max<int64_t>(capBytes, static_cast<int64_t>(kClip.minDiskCapMb) * 1024 * 1024);
}

void BufferBook::add(Segment s) {
    if (s.end <= s.start) return;
    auto it = std::upper_bound(m_segments.begin(), m_segments.end(), s.start, [](double t, Segment const& g) { return t < g.start; });
    m_segments.insert(it, std::move(s));
}

void BufferBook::pin(std::string id, double from, double to) {
    for (auto& [pid, range] : m_pins) {
        if (pid == id) {
            range = {from, to};
            return;
        }
    }
    m_pins.emplace_back(std::move(id), KeepRange{from, to});
}

void BufferBook::unpin(std::string const& id) {
    std::erase_if(m_pins, [&](auto const& p) { return p.first == id; });
}

bool BufferBook::hasPin(std::string const& id) const {
    return std::any_of(m_pins.begin(), m_pins.end(), [&](auto const& p) { return p.first == id; });
}

bool BufferBook::pinned(Segment const& s) const {
    for (auto const& [id, range] : m_pins) {
        if (s.end > range.from && s.start < range.to) return true;
    }
    return false;
}

std::vector<Segment> BufferBook::prune(std::optional<double> openAttemptStart) {
    m_capLimited = false;
    size_t n = m_segments.size();
    std::vector<char> keep(n, 0);
    std::vector<char> pins(n, 0);
    double window = m_bufferSeconds + kClip.keepMarginSeconds;
    double attemptLimit = kClip.maxBufferSeconds + kClip.keepMarginSeconds;
    double newer = 0.0;   // footage newer than segment i
    for (size_t i = n; i-- > 0;) {
        auto const& s = m_segments[i];
        pins[i] = pinned(s) ? 1 : 0;
        bool inWindow = newer < window;
        bool inAttempt = openAttemptStart && s.end > *openAttemptStart - kClip.leadSeconds && newer < attemptLimit;
        keep[i] = (inWindow || inAttempt || pins[i]) ? 1 : 0;
        newer += duration(s);
    }
    int64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        if (keep[i]) total += m_segments[i].bytes;
    }
    for (size_t i = 0; i < n && total > m_capBytes; ++i) {
        if (!keep[i] || pins[i]) continue;
        keep[i] = 0;
        total -= m_segments[i].bytes;
        m_capLimited = true;
    }
    std::vector<Segment> removed;
    std::vector<Segment> kept;
    kept.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        (keep[i] ? kept : removed).push_back(std::move(m_segments[i]));
    }
    m_segments.swap(kept);
    return removed;
}

std::vector<Segment> BufferBook::clear() {
    std::vector<Segment> removed;
    removed.swap(m_segments);
    m_capLimited = false;
    return removed;
}

Selection BufferBook::select(double from, double to) const {
    Selection sel;
    if (!(to > from)) {
        sel.error = "empty range";
        return sel;
    }
    std::vector<Segment const*> hits;
    for (auto const& s : m_segments) {
        if (s.end > from && s.start < to) hits.push_back(&s);
    }
    if (hits.empty()) {
        sel.error = "nothing was recorded for this range";
        return sel;
    }
    // one clip = one encoded size and one encoder (stream copy): keep the newest such run
    size_t first = hits.size() - 1;
    while (first > 0 && hits[first - 1]->size == hits.back()->size && hits[first - 1]->encoder == hits.back()->encoder) --first;
    for (size_t i = first; i < hits.size(); ++i) {
        sel.picks.push_back(*hits[i]);
        sel.duration += duration(*hits[i]);
    }
    sel.start = sel.picks.front().start;
    sel.end = sel.picks.back().end;
    sel.coversStart = sel.start <= from + kClip.segmentSeconds + 0.25;
    return sel;
}

int64_t BufferBook::liveBytes() const {
    int64_t total = 0;
    for (auto const& s : m_segments) total += s.bytes;
    return total;
}

double BufferBook::recordedSeconds() const {
    double total = 0.0;
    for (auto const& s : m_segments) total += duration(s);
    return total;
}

std::optional<double> BufferBook::oldestStart() const {
    if (m_segments.empty()) return std::nullopt;
    return m_segments.front().start;
}

std::vector<KeepRange> BufferBook::soundRanges() const {
    std::vector<KeepRange> out;
    if (auto oldest = oldestStart()) out.push_back({*oldest - 2.0, 1e18});
    for (auto const& [id, range] : m_pins) out.push_back({range.from - 2.0, range.to + 2.0});
    return out;
}

// ---- attempts ----

void AttemptBook::started(AttemptMark mark) {
    // the previous attempt should have been closed by the tracker; never leave two open
    for (auto& m : m_marks) {
        if (m.end == 0.0) m.end = mark.start;
    }
    mark.end = 0.0;
    m_marks.push_back(std::move(mark));
    while (m_marks.size() > static_cast<size_t>(kClip.attemptsTracked)) m_marks.erase(m_marks.begin());
}

bool AttemptBook::ended(std::string const& attemptId, double end, double percent, bool completed, bool legit, double endT, int64_t endTick) {
    for (auto& m : m_marks) {
        if (m.attemptId != attemptId) continue;
        if (m.end != 0.0) return false;
        m.end = std::max(end, m.start);
        m.percent = std::clamp(percent, 0.0, 100.0);
        m.completed = completed;
        m.legit = legit;
        m.endT = endT;
        m.endTick = endTick;
        return true;
    }
    return false;
}

AttemptMark const* AttemptBook::find(std::string const& attemptId) const {
    for (auto const& m : m_marks) {
        if (m.attemptId == attemptId) return &m;
    }
    return nullptr;
}

std::optional<double> AttemptBook::openStart() const {
    for (auto it = m_marks.rbegin(); it != m_marks.rend(); ++it) {
        if (it->end == 0.0) return it->start;
    }
    return std::nullopt;
}

void AttemptBook::closeOpen(double now) {
    for (auto& m : m_marks) {
        if (m.end == 0.0) m.end = std::max(now, m.start);
    }
}

KeepRange preserveRange(AttemptMark const& mark, double now) {
    double end = mark.end > 0.0 ? mark.end : now;
    return {mark.start - kClip.leadSeconds, end + kClip.tailSeconds};
}

// ---- when is a run exceptional ----

PreserveDecision decidePreserve(PreserveFacts const& f) {
    PreserveDecision d;
    if (!f.clippingEnabled) {
        d.why = "clipping is off";
        return d;
    }
    if (!f.recording) {
        d.why = "nothing was recorded for this attempt";
        return d;
    }
    if (f.serverHinted) {
        d.preserve = true;
        d.rule = "server_hint";
        d.why = "the server asked for evidence of this attempt";
        return d;
    }
    if (f.serverHintsSupported) {
        d.why = "the server decides which runs need evidence and did not name this one";
        return d;
    }
    if (!f.completed) {
        d.why = "not a completion";
        return d;
    }
    if (f.practice) {
        d.why = "practice mode completion";
        return d;
    }
    if (!f.fromStart) {
        d.why = "completion from a StartPos";
        return d;
    }
    if (!f.legit) {
        d.why = "noclip / bot / modified physics seen during the attempt";
        return d;
    }
    bool counts = f.serverLevelCounts ? *f.serverLevelCounts : f.localRatedDemon;
    if (!counts) {
        d.why = f.serverLevelCounts ? "the server says this level does not count" : "not a rated demon";
        return d;
    }
    d.preserve = true;
    d.rule = "local_completion";
    d.why = f.serverLevelCounts ? "completion of a counting level" : "completion of a rated demon (no server verdict)";
    return d;
}

}  // namespace gprl::clip
