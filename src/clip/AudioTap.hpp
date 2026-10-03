#pragma once
// Game sound for clips: a pass-through DSP on FMOD's master channel group (the game's final mix,
// after every effect and volume) that keeps one-second 16-bit chunks stamped with the clip clock.
// Adapted from the In-Game Clipper's AudioTap (same mapping of mixer samples to wall time).
//
// Only the GAME's own mix is heard: other apps (Discord, a browser) never pass through FMOD.
// The DSP exists only while clipping with game sound is on; turning it off removes it again.
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "../../core/clip.hpp"

namespace FMOD {
class System;
class DSP;
}

namespace gprl::clip {

class AudioTap {
public:
    static AudioTap& get();

    /// Any thread. The DSP is created / removed on the next tick().
    void setEnabled(bool on) { m_wanted = on; }
    /// Main thread, every FMODAudioEngine::update.
    void tick(FMOD::System* system);

    bool active() const { return m_active; }
    int rate() const { return m_rate; }

    /// Converts wall time to a sample index for one clip. Built once per clip so all of its
    /// segments share one smooth mapping (no clicks at segment boundaries).
    struct Mapping {
        bool ok = false;
        double offset = 0.0;  // sample index of wall time 0 in the mixer's clock
        double shift = 0.0;   // output latency, seconds
    };
    Mapping makeMapping(double rangeStart, double rangeEnd);

    /// Appends `count` stereo frames (as float) of what was heard from wall time t0; zeros where missing.
    void extract(Mapping const& map, double t0, std::int64_t count, std::vector<float>& out);

    /// Capture thread: drops sound no clip can use any more.
    void prune(std::vector<KeepRange> const& keep);

    // FMOD callbacks (mixer thread).
    void onMix(float const* in, unsigned int length, int channels);
    std::atomic<bool> m_idle = false;

private:
    AudioTap() = default;

    struct Chunk {
        std::int64_t first = 0;          // frame index of the first sample
        double wallTime = 0.0;           // when its first block was mixed
        std::vector<std::int16_t> pcm;   // interleaved stereo, up to one second
    };

    FMOD::DSP* m_dsp = nullptr;
    bool m_failed = false;
    std::atomic<bool> m_wanted = false;
    std::atomic<bool> m_active = false;
    int m_rate = 48000;
    double m_latency = 0.0;

    std::mutex m_mutex;
    std::deque<Chunk> m_chunks;
    std::vector<std::vector<std::int16_t>> m_spare;   // recycled buffers, so the mixer rarely allocates
    std::int64_t m_total = 0;                          // frames ever written
    std::deque<std::pair<std::int64_t, double>> m_anchors;   // (first frame of a block, time its mix ran)
};

}  // namespace gprl::clip
