#pragma once
// Microphone track for clips: a SEPARATE opt-in (mod setting "clip-mic", SPEC §29). Nothing here
// runs, and no audio device is opened, unless the player ticks it while clipping is on (and
// ffmpeg exists). While it is ticked the device is open, but its sound is only kept while a level
// is open (setStoring).
//
// Records Windows' default recording device only (WASAPI shared mode, event driven) into
// one-second 16-bit chunks stamped with the clip clock; a clip carries it as its own second
// sound track so it can be removed later. Adapted from the In-Game Clipper's InputCapture (which
// records every input; GPRL evidence only ever takes the default microphone).
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../core/clip.hpp"

namespace gprl::clip {

class MicCapture {
public:
    static MicCapture& get();
    /// v0.9.0: the computer's own sound (the default OUTPUT device, WASAPI loopback) as a third,
    /// removable track (mod setting "clip-desktop-audio"). Same rules: open only while the setting
    /// is on, kept only while a level is open.
    static MicCapture& desktop();

    /// Any thread. The first `true` starts the manager thread; `false` stops recording and
    /// forgets what was recorded.
    void setEnabled(bool on);
    /// Any thread. Sound is only KEPT while this is set (a level is open): outside a level the
    /// device stays open (no gap when the next level starts) but every buffer it delivers is
    /// dropped on arrival, so nothing said in a menu is ever in memory or in a clip.
    void setStoring(bool on) { m_storing = on; }
    bool enabled() const { return m_enabled; }
    bool recording() const { return m_running; }
    std::string deviceName() const;
    std::string problem() const;

    /// True when any sound was recorded between the two wall times.
    bool hasData(double from, double to) const;
    /// Appends `count` frames of what was heard from wall time t0, interleaved int16 in the
    /// device's rate / channels (returned); zeros where nothing was recorded.
    void extract(double t0, std::int64_t count, std::vector<std::int16_t>& out, int& rate, int& channels) const;
    /// Capture thread: drops sound no clip can use any more.
    void prune(std::vector<KeepRange> const& keep);

private:
    explicit MicCapture(bool loopback = false) : m_loopback(loopback) {}
    char const* label() const { return m_loopback ? "desktop sound" : "microphone"; }

    struct Chunk {
        double wallTime = 0.0;   // when its first frame was captured
        std::vector<std::int16_t> pcm;
    };

    void managerLoop();
    void captureLoop(std::wstring deviceId, std::string name);
    void push(std::int16_t const* frames, std::size_t count, double t, bool discontinuity);
    void stopCapture();

    bool const m_loopback = false;   // eRender endpoint + AUDCLNT_STREAMFLAGS_LOOPBACK, polled
    std::atomic<bool> m_started = false;
    std::atomic<bool> m_enabled = false;
    std::atomic<bool> m_storing = false;   // a level is open (src/Clipper): chunks are kept
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_stop = false;
    std::atomic<double> m_lastStop = -1e9;
    std::thread m_thread;   // manager thread only

    mutable std::mutex m_mutex;   // chunks, format, names, error
    std::deque<Chunk> m_chunks;
    int m_rate = 48000;
    int m_channels = 1;
    std::string m_name;
    std::string m_error;
};

}  // namespace gprl::clip
