#pragma once
// Clipping buffer, pure rules (stream M3, geode v0.6.0; MASTER §16, SPEC §28-§31, docs/CLIPPING.md).
// No Geode / cocos / Windows includes: compiled by the mod AND by tests/clip_tests.cpp.
//
//   ClipParams      every constant of the clipping feature in one versioned object
//   ClipConfig      the player's settings, clamped (database CHECK 10..600 s)
//   outputSize      the encoded frame size for a window size (never upscaled, even dimensions)
//   Encoder ladder  h264_nvenc -> h264_amf -> h264_qsv -> libx264, demoted after repeated failures
//   ffmpeg args     the exact command lines of the rolling segmenter, the encoder probe and the
//                   clip mux (strings only; src/clip/Capture.cpp runs them)
//   BufferBook      bookkeeping of the 1 s segments on disk: what to keep (the last N recorded
//                   seconds, the whole open attempt, pinned ranges), the disk cap, what a clip of
//                   a wall-clock range is made of
//   AttemptBook     the wall-clock range of the last attempts (what preserveAttempt() cuts)
//   decidePreserve  when a run is exceptional: the server's hint, or (while the server sends none)
//                   the local rule "legit completion of a counting level"
//
// The capture itself (GL read-back, ffmpeg child process, FMOD / WASAPI taps) lives in src/clip/;
// the choice flow, upload contract, hashing and the clip index live in core/clip_flow.
//
// Wall clock: every time in this file is seconds on ONE steady clock (src/clip/Capture now()),
// shared by the video frames, the game sound and the microphone.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gprl::clip {

struct ClipParams {
    char const* version = "gprl-clip/1";
    // ---- rolling buffer (mod setting "clip-buffer-seconds"; database CHECK player_settings_clip_buffer) ----
    int defaultBufferSeconds = 120;
    int minBufferSeconds = 10;
    int maxBufferSeconds = 600;          // also the longest clip: the open attempt is kept up to this much footage
    double segmentSeconds = 1.0;         // one keyframe-aligned file per second
    int fps = 60;
    double keepMarginSeconds = 3.0;      // kept beyond the window: segment granularity + encoder delay
    // ---- disk (mod setting "clip-disk-cap-mb") ----
    int defaultDiskCapMb = 1024;
    int minDiskCapMb = 128;
    int maxDiskCapMb = 8192;
    int64_t freeSpaceReserveBytes = 256ll * 1024 * 1024;   // never record with less free space than need + this
    // ---- encode ----
    int audioKbps = 160;                 // AAC per sound track in the finished clip
    int probeFrames = 10;                // frames of the encoder self-test
    int encoderFailuresBeforeDemote = 2; // consecutive failed sessions before the next encoder is tried
    double encoderRetrySeconds = 5.0;    // pause after a failed session
    // ---- capture ----
    double maxGapSeconds = 2.0;          // a longer stall (loading, minimised, pause) starts a new session
    int maxFillFrames = 120;             // repeated frames that bridge a stall inside a session
    int maxQueuedFrames = 8;             // frames waiting for the encoder before new ones are dropped (8 MB each at 1080p)
    // ---- what a preserved attempt covers ----
    double leadSeconds = 1.0;            // before the attempt start
    double tailSeconds = 3.0;            // after the attempt end (the end screen / the respawn)
    double finalizeGraceSeconds = 6.0;   // longest wait for the segment holding the range end
    int attemptsTracked = 8;
    // ---- operational timings of src/clip ----
    double housekeepingSeconds = 0.5;    // segment list -> BufferBook -> prune
    double diskCheckSeconds = 5.0;       // free-space re-check
    double closingTimeoutSeconds = 20.0; // an encoder that does not finish after stdin closed is stopped
    unsigned long probeTimeoutMs = 20000;   // one encoder self-test
    unsigned long muxTimeoutMs = 120000;    // cutting one clip
    double soundMaxStoredSeconds = 660.0;   // hard memory cap of a sound tap (the longest clip + margin)
    double micRestartSeconds = 5.0;      // pause before the microphone is reopened after an error
};
inline constexpr ClipParams kClip{};

// ---- settings ----

enum class Quality : uint8_t { P480, P720, P1080 };

struct QualitySpec {
    char const* label;
    int maxHeight;
    int videoKbps;   // constant bitrate: size = bitrate x time, so the disk and upload caps are exact
};
/// Indexed by Quality. 720p / 5 Mbit is the default: a 120 s clip is about 77 MB with one sound track.
inline constexpr QualitySpec kQualities[] = {
    {"480p", 480, 2500},
    {"720p", 720, 5000},
    {"1080p", 1080, 9000},
};
inline constexpr QualitySpec const& spec(Quality q) { return kQualities[static_cast<int>(q)]; }
/// "480p" | "720p" | "1080p"; anything else = 720p.
Quality parseQuality(std::string_view label);

/// Mirrors shared PlayerClientSettings (clippingEnabled, clipBufferSeconds) plus the mod-only
/// knobs. `enabled` false is the default: nothing is captured until the player turns Clipping on.
struct ClipConfig {
    bool enabled = false;
    int bufferSeconds = kClip.defaultBufferSeconds;
    int diskCapMb = kClip.defaultDiskCapMb;
    Quality quality = Quality::P720;
    bool gameAudio = true;
    bool micAudio = false;   // separate opt-in (SPEC §29): never recorded unless the player ticks it
    bool desktopAudio = false;   // v0.9.0: the computer's own sound (default output, WASAPI loopback), its own track
};
/// Clamps bufferSeconds to [10, 600] and diskCapMb to [128, 8192].
ClipConfig sanitized(ClipConfig c);

struct Size {
    int width = 0;
    int height = 0;
    bool operator==(Size const&) const = default;
};
/// Encoded size for a `srcWidth` x `srcHeight` window at a quality: height min(src, maxHeight),
/// aspect kept, both dimensions even (H.264 4:2:0), never upscaled. {0, 0} for a window under 16 px.
Size outputSize(int srcWidth, int srcHeight, int maxHeight);

/// Bytes per recorded second of the BUFFER (video only; sound is kept in memory until a clip is cut).
int64_t bufferBytesPerSecond(Quality q);
/// Bytes of a finished clip of `seconds` with `audioTracks` AAC tracks (0..2).
int64_t clipBytes(Quality q, double seconds, int audioTracks);
/// Free disk space the buffer needs before it starts: the smaller of the disk cap and twice the
/// footage of `bufferSeconds`, plus one full clip and the reserve.
int64_t requiredFreeBytes(ClipConfig const& c);
/// "about 40 MB per minute, up to 1024 MB" - the cost text of the settings / Account tab.
std::string costLine(ClipConfig const& c);

// ---- encoders ----

enum class Encoder : uint8_t { None, NvencH264, AmfH264, QsvH264, X264 };
constexpr int kEncoderCount = 5;
char const* name(Encoder e);           // "h264_nvenc", "h264_amf", "h264_qsv", "libx264", "none"
bool isHardware(Encoder e);

/// ffmpeg output options of one encoder at a constant bitrate with a keyframe at every segment
/// start (closed 1 s GOPs, no B-frames) so segments can be concatenated without re-encoding.
std::string encoderArgs(Encoder e, int videoKbps, int fps, double segmentSeconds);
/// Self-test command line: the EXACT encoder options on a synthetic source ("-f null -").
std::string probeArgs(Encoder e, Size out, int videoKbps, int fps, double segmentSeconds, int frames);
/// The rolling segmenter: raw BGRA frames on stdin (bottom row first) -> flipped, scaled to `out`,
/// BT.709 -> 1 s Matroska segments + a CSV segment list. Paths are quoted.
std::string segmenterArgs(Size src, Size out, Encoder e, int videoKbps, int fps, double segmentSeconds, std::string const& csvPath,
                          std::string const& segmentPattern);

/// Which encoder to use. Order NvencH264, AmfH264, QsvH264, X264; only probed-available ones are
/// used; `failed()` demotes after ClipParams::encoderFailuresBeforeDemote consecutive failures.
class EncoderLadder {
public:
    void setAvailable(Encoder e, bool ok);
    bool available(Encoder e) const { return m_available[static_cast<int>(e)]; }
    /// Best encoder not demoted; None when nothing works.
    Encoder current() const;
    /// A recording session with `e` died. Returns true when `e` was demoted by this call.
    bool failed(Encoder e);
    /// A session with `e` produced a segment: its failure streak is over.
    void succeeded(Encoder e);
    bool demoted(Encoder e) const { return m_demoted[static_cast<int>(e)]; }

private:
    bool m_available[kEncoderCount] = {};
    bool m_demoted[kEncoderCount] = {};
    int m_failures[kEncoderCount] = {};
};

// ---- clip mux ----

struct MuxInputs {
    std::string concatFile;   // ffconcat list of the picked segments
    std::string gameWav;      // "" = no game sound track
    std::string micWav;       // "" = no microphone track (separate track, removable: SPEC §29)
    std::string desktopWav;   // "" = no desktop sound track (v0.9.0; its own track, removable)
    std::string output;       // .mp4
    std::string clipId;
    std::string attemptId;
    std::string title;        // plain ASCII, already sanitised (safeText)
};
/// Video stream copy (no re-encode) + one AAC track per WAV, faststart, the clip / attempt ids as
/// container metadata. The game sound is always the first (default) track.
std::string muxArgs(MuxInputs const& in, int audioKbps);

/// A path for an ffconcat file: forward slashes, single quotes escaped.
std::string concatPath(std::string_view path);
/// The ffconcat text for picked segment files with their durations.
std::string concatList(std::vector<std::pair<std::string, double>> const& files);
/// ASCII text safe inside a double-quoted ffmpeg argument: quotes, backslashes and control / non-
/// ASCII characters become '_'; at most `maxLen` characters.
std::string safeText(std::string_view text, size_t maxLen = 80);

// ---- segment bookkeeping ----

/// One finished segment file of a recording session.
struct Segment {
    uint64_t session = 0;
    std::string file;      // file name inside the session directory
    double start = 0.0;    // wall clock, seconds
    double end = 0.0;
    int64_t bytes = 0;
    Size size;             // encoded size of the session
    Encoder encoder = Encoder::None;   // a clip is a stream copy: one size and one encoder per clip
};

/// One line of ffmpeg's `-segment_list_type csv` file: "seg_00012.mkv,12.000000,13.000000".
struct CsvSegment {
    std::string file;
    double start = 0.0;    // seconds since the session start
    double end = 0.0;
};
std::optional<CsvSegment> parseSegmentCsvLine(std::string_view line);

struct KeepRange {
    double from = 0.0;
    double to = 0.0;
};

/// A clip of a wall-clock range: the segment files in order and what they cover.
struct Selection {
    std::vector<Segment> picks;
    double start = 0.0;      // wall clock of the first pick's start
    double end = 0.0;        // wall clock of the last pick's end
    double duration = 0.0;   // summed segment durations (pauses are not recorded, so <= end - start)
    bool coversStart = false;   // the first pick starts at or before the requested start (+ one segment)
    std::string error;       // non-empty: nothing usable
};

/// Bookkeeping of the segments on disk. Not synchronised (the capture's mutex guards it).
///
/// Kept after prune():
///   1. the newest `bufferSeconds` (+ margin) of RECORDED footage;
///   2. every segment of the open attempt (from `openAttemptStart - lead`), up to
///      ClipParams::maxBufferSeconds of footage, so a run longer than the buffer is still whole;
///   3. every segment overlapping a pinned range (a preserved attempt being cut).
/// Then the disk cap: while the kept bytes exceed it, the OLDEST unpinned segment goes
/// (`capLimited()` reports that the buffer is shorter than configured).
class BufferBook {
public:
    void configure(int bufferSeconds, int64_t capBytes);
    void add(Segment s);
    void pin(std::string id, double from, double to);
    void unpin(std::string const& id);
    bool hasPin(std::string const& id) const;

    /// Removes what is no longer kept and returns it (the caller deletes the files).
    std::vector<Segment> prune(std::optional<double> openAttemptStart);
    /// Removes and returns everything (buffer wiped: clipping turned off, level does not count).
    std::vector<Segment> clear();

    Selection select(double from, double to) const;

    std::vector<Segment> const& segments() const { return m_segments; }
    int64_t liveBytes() const;
    double recordedSeconds() const;
    /// Wall clock of the oldest kept segment's start; nullopt when empty.
    std::optional<double> oldestStart() const;
    /// Wall-clock ranges whose SOUND must be kept: [oldest kept segment - 2 s, +inf) and the pins.
    std::vector<KeepRange> soundRanges() const;
    bool capLimited() const { return m_capLimited; }
    int bufferSeconds() const { return m_bufferSeconds; }
    int64_t capBytes() const { return m_capBytes; }

private:
    bool pinned(Segment const& s) const;

    std::vector<Segment> m_segments;   // sorted by start
    std::vector<std::pair<std::string, KeepRange>> m_pins;
    int m_bufferSeconds = kClip.defaultBufferSeconds;
    int64_t m_capBytes = static_cast<int64_t>(kClip.defaultDiskCapMb) * 1024 * 1024;
    bool m_capLimited = false;
};

// ---- attempts ----

/// One attempt as the clipper sees it (src/Tracker tells it the start and the end).
struct AttemptMark {
    std::string attemptId;        // telemetry attemptId ("<session>-a<n>")
    std::string sessionLocalId;   // the tracker's level-session id the attempt belongs to
    int attemptNo = 0;
    double start = 0.0;           // wall clock
    double end = 0.0;             // 0 while the attempt is open
    double fromPercent = 0.0;
    double percent = 0.0;
    bool practice = false;
    bool startPos = false;
    bool completed = false;
    bool legit = true;            // no noclip / bot / physics change / unknown mod seen by the client
    double endT = 0.0;            // the attempt_end event's t / tick (what clip_available carries)
    int64_t endTick = 0;
};

class AttemptBook {
public:
    void started(AttemptMark mark);
    /// Closes the open attempt `attemptId`. Returns false when it is unknown or already closed.
    bool ended(std::string const& attemptId, double end, double percent, bool completed, bool legit, double endT, int64_t endTick);
    AttemptMark const* find(std::string const& attemptId) const;
    /// Wall clock of the open attempt's start; nullopt when none is open.
    std::optional<double> openStart() const;
    /// Ends any open attempt at `now` without a result (level left, game exit).
    void closeOpen(double now);
    size_t size() const { return m_marks.size(); }

private:
    std::vector<AttemptMark> m_marks;   // oldest first, at most ClipParams::attemptsTracked
};

/// The wall-clock range a preserved attempt covers: [start - lead, end + tail]; an open attempt
/// ends at `now`.
KeepRange preserveRange(AttemptMark const& mark, double now);

// ---- when is a run exceptional ----

struct PreserveFacts {
    bool clippingEnabled = false;
    bool recording = false;               // the buffer holds footage of this attempt
    bool serverHintsSupported = false;    // the server has sent `preserveEvidence` in this session
    bool serverHinted = false;            // ... and named this attempt
    bool completed = false;
    bool practice = false;
    bool fromStart = true;                // fromPercent 0 and no StartPos
    bool legit = true;
    std::optional<bool> serverLevelCounts;   // V1CreateSessionResponse.levelCounts (Remote sessions only)
    bool localRatedDemon = false;         // GD's own data: a demon with stars
};

struct PreserveDecision {
    bool preserve = false;
    char const* rule = "";      // "server_hint" | "local_completion" | ""
    std::string why;            // one line for the Geode log
};

/// SPEC §27 lives on the server: when it sends `preserveEvidence`, only the attempts it names are
/// preserved. Until a server sends the field, the local rule applies: a legit, non-practice
/// completion from 0 % of a level that counts (the server's `levelCounts`, or GD's own "rated
/// demon" data while there is no server verdict).
PreserveDecision decidePreserve(PreserveFacts const& f);

}  // namespace gprl::clip
