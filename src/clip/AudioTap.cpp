#include "AudioTap.hpp"

#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>
#include <Geode/fmod/fmod_errors.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../Settings.hpp"
#include "ClipUtil.hpp"

using namespace geode::prelude;

namespace gprl::clip {

namespace {

// Hard memory cap whatever the keep ranges say: the longest clip + margin (~120 MB at 48 kHz).
constexpr double kMaxStoredSeconds = kClip.soundMaxStoredSeconds;

FMOD_RESULT F_CALL readCallback(FMOD_DSP_STATE*, float* in, float* out, unsigned int length, int inChannels, int* outChannels) {
    auto& tap = AudioTap::get();
    std::size_t samples = static_cast<std::size_t>(length) * static_cast<std::size_t>(std::max(inChannels, 0));
    if (tap.m_idle) {
        std::memset(out, 0, samples * sizeof(float));
        tap.onMix(nullptr, length, inChannels);
    }
    else {
        std::memcpy(out, in, samples * sizeof(float));
        tap.onMix(in, length, inChannels);
    }
    if (outChannels) *outChannels = inChannels;
    return FMOD_OK;
}

// Always process, even when everything is silent, so the sample clock never skips.
FMOD_RESULT F_CALL shouldProcessCallback(FMOD_DSP_STATE*, FMOD_BOOL inputsIdle, unsigned int, FMOD_CHANNELMASK, int, FMOD_SPEAKERMODE) {
    AudioTap::get().m_idle = inputsIdle != 0;
    return FMOD_OK;
}

std::int16_t toPcm(float v) {
    v = std::clamp(v, -1.f, 1.f);
    return static_cast<std::int16_t>(std::lrint(v * 32767.f));
}

bool overlaps(std::vector<KeepRange> const& keep, double a, double b) {
    for (auto const& r : keep) {
        if (a <= r.to && b >= r.from) return true;
    }
    return false;
}

}  // namespace

AudioTap& AudioTap::get() {
    static AudioTap* tap = new AudioTap();   // never destroyed: FMOD's mixer thread may still call it at exit
    return *tap;
}

void AudioTap::tick(FMOD::System* system) {
    if (!system) return;
    bool wanted = m_wanted;

    if (!wanted) {
        if (!m_dsp) return;
        // clipping (or its game sound) was turned off: take the DSP out of the game's mix again
        FMOD::ChannelGroup* master = nullptr;
        if (system->getMasterChannelGroup(&master) == FMOD_OK && master) master->removeDSP(m_dsp);
        m_dsp->release();
        m_dsp = nullptr;
        m_active = false;
        {
            std::lock_guard lock(m_mutex);
            m_chunks.clear();
            m_anchors.clear();
            m_total = 0;
        }
        log::info("GPRL clip: game sound tap removed");
        return;
    }
    if (m_dsp || m_failed) return;

    int rate = 0, raw = 0;
    FMOD_SPEAKERMODE mode{};
    if (system->getSoftwareFormat(&rate, &mode, &raw) != FMOD_OK || rate <= 0) rate = 44100;
    unsigned int bufferLength = 0;
    int buffers = 0;
    system->getDSPBufferSize(&bufferLength, &buffers);

    {
        std::lock_guard lock(m_mutex);
        m_rate = rate;
        m_chunks.clear();
        m_total = 0;
        m_anchors.clear();
    }
    m_latency = static_cast<double>(bufferLength) * std::max(buffers, 1) / rate;

    FMOD_DSP_DESCRIPTION desc{};
    desc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    std::strncpy(desc.name, "GPRL clip tap", sizeof(desc.name) - 1);
    desc.version = 0x00010000;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = readCallback;
    desc.shouldiprocess = shouldProcessCallback;

    FMOD::ChannelGroup* master = nullptr;
    FMOD_RESULT r = system->getMasterChannelGroup(&master);
    if (r == FMOD_OK && master) r = system->createDSP(&desc, &m_dsp);
    if (r == FMOD_OK && m_dsp) r = master->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, m_dsp);
    if (r != FMOD_OK) {
        log::warn("GPRL clip: could not tap the game sound ({}); clips will have no game sound", FMOD_ErrorString(r));
        if (m_dsp) m_dsp->release();
        m_dsp = nullptr;
        m_failed = true;
        return;
    }
    m_active = true;
    log::info("GPRL clip: game sound tap on the master mix, {} Hz, mixer buffer {} x {} (~{:.0f} ms latency)", rate, bufferLength, buffers,
              m_latency * 1000.0);
}

void AudioTap::onMix(float const* in, unsigned int length, int channels) {
    double t = now();
    std::lock_guard lock(m_mutex);
    auto const chunkSamples = static_cast<std::size_t>(m_rate) * 2;
    for (unsigned int i = 0; i < length; ++i) {
        if (m_chunks.empty() || m_chunks.back().pcm.size() >= chunkSamples) {
            Chunk chunk;
            chunk.first = m_total + i;
            chunk.wallTime = t;
            if (!m_spare.empty()) {
                chunk.pcm = std::move(m_spare.back());
                m_spare.pop_back();
                chunk.pcm.clear();
            }
            else {
                chunk.pcm.reserve(chunkSamples);
            }
            m_chunks.push_back(std::move(chunk));
        }
        float l = 0.f, r = 0.f;
        if (in && channels >= 2) {
            l = in[i * channels];
            r = in[i * channels + 1];
        }
        else if (in && channels == 1) {
            l = r = in[i];
        }
        auto& pcm = m_chunks.back().pcm;
        pcm.push_back(toPcm(l));
        pcm.push_back(toPcm(r));
    }
    m_anchors.emplace_back(m_total, t);
    m_total += length;
    // safety net while nothing prunes (chunks are one second each)
    while (m_chunks.size() > static_cast<std::size_t>(kMaxStoredSeconds) + 60) {
        if (m_spare.size() < 8) m_spare.push_back(std::move(m_chunks.front().pcm));
        m_chunks.pop_front();
    }
}

void AudioTap::prune(std::vector<KeepRange> const& keep) {
    std::lock_guard lock(m_mutex);
    // Never drop the newest two chunks (the mixer is writing into the last one).
    std::size_t n = m_chunks.size();
    std::vector<bool> keepFlags(n);
    std::size_t keptCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        keepFlags[i] = i + 2 >= n || overlaps(keep, m_chunks[i].wallTime - 2.0, m_chunks[i].wallTime + 3.0);
        if (keepFlags[i]) ++keptCount;
    }
    // Hard memory cap: drop the oldest kept chunks first.
    auto const maxChunks = static_cast<std::size_t>(kMaxStoredSeconds);
    for (std::size_t i = 0; i + 2 < n && keptCount > maxChunks; ++i) {
        if (keepFlags[i]) {
            keepFlags[i] = false;
            --keptCount;
        }
    }
    std::deque<Chunk> survivors;
    for (std::size_t i = 0; i < n; ++i) {
        if (keepFlags[i]) survivors.push_back(std::move(m_chunks[i]));
        else if (m_spare.size() < 8) m_spare.push_back(std::move(m_chunks[i].pcm));
    }
    m_chunks.swap(survivors);

    double oldest = m_chunks.empty() ? now() : m_chunks.front().wallTime;
    while (!m_anchors.empty() && m_anchors.front().second < oldest - 10.0) m_anchors.pop_front();
    while (m_anchors.size() > 400000) m_anchors.pop_front();
}

AudioTap::Mapping AudioTap::makeMapping(double rangeStart, double rangeEnd) {
    Mapping map;
    std::lock_guard lock(m_mutex);
    if (m_anchors.size() < 8) return map;
    // Each block's mix can only start late, never early, so the highest (sample - rate*time)
    // values come from the least-delayed callbacks. A high percentile ignores outliers.
    std::vector<double> offsets;
    auto collect = [&](double from, double to) {
        offsets.clear();
        for (auto const& [frame, time] : m_anchors) {
            if (time < from || time > to) continue;
            offsets.push_back(static_cast<double>(frame) - m_rate * time);
        }
    };
    collect(rangeStart - 5.0, rangeEnd + 5.0);
    if (offsets.size() < 8) collect(-1e18, 1e18);
    if (offsets.size() < 8) return map;
    auto nth = offsets.begin() + static_cast<std::ptrdiff_t>(offsets.size() * 9 / 10);
    std::nth_element(offsets.begin(), nth, offsets.end());
    map.offset = *nth;
    map.shift = m_latency;
    map.ok = true;
    if (settings::debugEnabled()) {
        auto [lo, hi] = std::minmax_element(offsets.begin(), offsets.end());
        log::debug("GPRL clip: sound mapping from {} blocks, spread {:.1f} ms, shift {:.0f} ms", offsets.size(), (*hi - *lo) / m_rate * 1000.0,
                   map.shift * 1000.0);
    }
    return map;
}

void AudioTap::extract(Mapping const& map, double t0, std::int64_t count, std::vector<float>& out) {
    std::size_t base = out.size();
    out.resize(base + static_cast<std::size_t>(std::max<std::int64_t>(count, 0)) * 2, 0.f);
    if (!map.ok || count <= 0) return;
    std::lock_guard lock(m_mutex);
    if (m_chunks.empty()) return;
    auto start = static_cast<std::int64_t>(std::llround(map.offset + m_rate * (t0 - map.shift)));

    // First chunk that could hold `start`.
    auto it = std::upper_bound(m_chunks.begin(), m_chunks.end(), start, [](std::int64_t s, Chunk const& c) { return s < c.first; });
    if (it != m_chunks.begin()) --it;
    constexpr float scale = 1.f / 32767.f;
    for (std::int64_t i = 0; i < count && it != m_chunks.end(); ++i) {
        std::int64_t s = start + i;
        while (it != m_chunks.end() && s >= it->first + static_cast<std::int64_t>(it->pcm.size() / 2)) ++it;
        if (it == m_chunks.end()) break;
        auto frames = static_cast<std::int64_t>(it->pcm.size() / 2);
        if (s < it->first || s >= it->first + frames) continue;
        auto pos = static_cast<std::size_t>(s - it->first) * 2;
        out[base + static_cast<std::size_t>(i) * 2] = it->pcm[pos] * scale;
        out[base + static_cast<std::size_t>(i) * 2 + 1] = it->pcm[pos + 1] * scale;
    }
}

}  // namespace gprl::clip
