#pragma once
// Rolling capture buffer (stream M3, MASTER §16, SPEC §28; docs/CLIPPING.md). The capture path of
// the owner's In-Game Clipper mod (D:\GeodeMods\ingame-clipper, Recorder.cpp), adapted:
//
//   GL thread   onFrame() right before the frame is shown: an asynchronous read-back (3 pixel
//               buffers, so the game never waits for the GPU) of the back buffer at a steady 60 fps,
//               handed to the writer thread. Costs one glReadPixels into a PBO + one memcpy per
//               captured frame; nothing at all while clipping is off or no level is open.
//   writer      pipes the raw frames into one ffmpeg child process per recording session, which
//               scales them to the chosen quality and encodes 1-second H.264 segments (hardware
//               encoder when one works, libx264 otherwise, constant bitrate). Housekeeping twice a
//               second: segment list -> core/clip BufferBook -> files that fell out of the buffer
//               are deleted; the disk cap and the free-space guard live here.
//   snapshot    (a clip thread) waits for the segment that holds the end of a range, hard-links
//               the segments of the range into a work folder and writes the sound tracks.
//
// A session ends when the window size changes, the game stalls for over 2 s (loading, minimised),
// the encoder dies, or the capture is no longer wanted. Everything that decides WHAT is kept is
// pure and host-tested (core/clip); this file only moves bytes.
//
// Threads: applySettings / setWanted / setOpenAttemptStart / pin / unpin / status / wipe from the
// main thread; onFrame on the GL thread (the main thread in GD); snapshot on a clip thread.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../../core/clip.hpp"
#include "Process.hpp"

namespace gprl::clip {

struct CaptureSettings {
    ClipConfig config;                  // sanitized
    std::filesystem::path ffmpeg;       // empty = not found
    std::filesystem::path bufferRoot;
};

struct CaptureStatus {
    bool enabled = false;
    bool wanted = false;
    bool recording = false;             // a session is encoding right now
    bool everRecorded = false;
    bool probed = false;                // the encoder self-test finished
    Encoder encoder = Encoder::None;    // what the next / current session uses
    Size source;                        // window size of the current session
    Size output;                        // encoded size of the current session
    double recordedSeconds = 0.0;       // footage in the buffer
    int64_t liveBytes = 0;
    bool capLimited = false;            // the disk cap made the buffer shorter than configured
    uint64_t droppedFrames = 0;         // frames the encoder could not take in time
    std::string problem;                // non-empty: why nothing is recorded
};

/// The raw material of one clip, in its own work folder.
struct ClipSource {
    std::string error;                  // non-empty: nothing usable
    std::filesystem::path dir;
    std::filesystem::path concatFile;
    std::filesystem::path gameWav;      // empty = no game sound
    std::filesystem::path micWav;       // empty = no microphone track
    std::filesystem::path desktopWav;   // empty = no desktop sound track (v0.9.0)
    double start = 0.0;                 // wall clock the clip starts at
    double duration = 0.0;              // seconds
    Size size;
    bool coversStart = false;
    int segments = 0;
};

class Capture {
public:
    static Capture& get();

    /// Main thread. Starts the writer thread on the first call with clipping enabled; probes the
    /// encoders again when ffmpeg or the quality changed; wipes the buffer when clipping is turned off.
    void applySettings(CaptureSettings settings);
    /// Main thread: record only while this is set (a level is open and the clipper wants footage).
    void setWanted(bool wanted);
    /// Main thread: the open attempt's start on the clip clock (its whole footage is kept).
    void setOpenAttemptStart(std::optional<double> start);

    /// GL thread, right before the frame is shown.
    void onFrame();

    void pin(std::string const& id, double from, double to);
    void unpin(std::string const& id);

    /// BLOCKING (clip thread, never the game thread): waits until the segment holding `to` is on
    /// disk (or the session ended), then links the segments of [from, to] into `dir` and writes
    /// the concat list and the sound tracks.
    ClipSource snapshot(double from, double to, std::filesystem::path const& dir);

    CaptureStatus status() const;
    /// Main thread: forget everything recorded so far (the level does not count, clipping off).
    void wipe();
    /// Game exit: stop feeding the encoder. Never blocks (the job object ends ffmpeg with the game).
    void shutdown();

private:
    Capture() = default;

    struct Frame {
        std::vector<std::uint8_t> pixels;   // BGRA, bottom row first
        int width = 0;
        int height = 0;
        std::uint64_t session = 0;
        double sessionStart = 0.0;
        std::int64_t index = 0;
    };

    struct Session {
        std::uint64_t id = 0;
        std::filesystem::path dir;
        double wallStart = 0.0;
        Size source;
        Size output;
        Encoder encoder = Encoder::None;
        std::unique_ptr<Process> process;
        std::int64_t frames = 0;
        enum class State { Running, Closing, Closed } state = State::Running;
        double closeAt = 0.0;
        std::size_t csvOffset = 0;
        int segments = 0;
        double writtenUntil = 0.0;   // wall clock of the last listed segment's end
    };

    struct Pbo {
        unsigned int id = 0;
        std::size_t capacity = 0;
        bool pending = false;
        std::uint64_t issuedFrame = 0;
        int width = 0;
        int height = 0;
        std::uint64_t session = 0;
        double sessionStart = 0.0;
        std::int64_t index = 0;
    };

    // writer thread
    void writerLoop();
    std::shared_ptr<Session> openSession(Frame const& frame);
    void closeSession(std::shared_ptr<Session> const& session, double t);
    void housekeeping(double t);
    void readSegmentList(Session& session);   // m_mutex held
    void removeFiles(std::vector<Segment> const& removed);   // m_mutex held
    void probeEncoders(CaptureSettings settings);

    std::unique_ptr<Frame> takeFrame(int width, int height);
    void recycle(std::unique_ptr<Frame> frame);
    CaptureSettings settingsCopy() const;

    // settings (main thread writes, others copy)
    mutable std::mutex m_cfgMutex;
    CaptureSettings m_settings;
    std::atomic<bool> m_enabled = false;
    std::atomic<bool> m_wanted = false;
    std::atomic<bool> m_encoderReady = false;   // probed and at least one encoder works
    std::atomic<bool> m_probed = false;
    std::atomic<bool> m_probing = false;
    std::atomic<bool> m_diskLow = false;
    std::atomic<bool> m_haveFfmpeg = false;
    std::atomic<bool> m_writerStarted = false;
    std::atomic<bool> m_exiting = false;
    std::atomic<bool> m_wipeRequested = false;
    std::atomic<double> m_openAttemptStart = 0.0;   // 0 = none

    // GL thread state
    Pbo m_pbos[3];
    std::uint64_t m_frameCounter = 0;
    std::uint64_t m_session = 0;
    std::uint64_t m_nextSession = 0;
    double m_sessionStart = 0.0;
    std::int64_t m_lastIndex = -1;
    double m_lastCapture = 0.0;
    double m_lastSwap = 0.0;
    double m_swapInterval = 1.0 / 60.0;
    int m_width = 0;
    int m_height = 0;
    std::atomic<bool> m_forceNewSession = false;
    std::atomic<std::uint64_t> m_drops = 0;

    // frame queue
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<std::unique_ptr<Frame>> m_queue;
    std::vector<std::unique_ptr<Frame>> m_pool;
    std::atomic<std::uint64_t> m_closeUpTo = 0;   // the writer closes sessions with ids up to this

    // sessions + bookkeeping (m_mutex)
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Session>> m_sessions;
    BufferBook m_book;
    EncoderLadder m_ladder;
    Encoder m_currentEncoder = Encoder::None;
    Size m_currentSource;
    Size m_currentOutput;
    bool m_recording = false;
    std::string m_lastError;
    std::atomic<double> m_failedUntil = 0.0;
    std::atomic<bool> m_everRecorded = false;
};

}  // namespace gprl::clip
