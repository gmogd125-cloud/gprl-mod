#include "Capture.hpp"

#include <Geode/Geode.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

#include "../Settings.hpp"
#include "AudioTap.hpp"
#include "ClipUtil.hpp"
#include "MicCapture.hpp"

using namespace geode::prelude;

namespace gprl::clip {

namespace {

// PCM WAV: 32-bit float or 16-bit integer, any channel count.
bool writeWav(std::filesystem::path const& path, void const* data, std::size_t bytes, int rate, int channels, bool isFloat) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<char const*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<char const*>(&v), 2); };
    auto dataBytes = static_cast<std::uint32_t>(bytes);
    int bits = isFloat ? 32 : 16;
    int blockAlign = channels * bits / 8;
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(isFloat ? 3 : 1);
    u16(static_cast<std::uint16_t>(channels));
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * static_cast<std::uint32_t>(blockAlign));
    u16(static_cast<std::uint16_t>(blockAlign));
    u16(static_cast<std::uint16_t>(bits));
    out.write("data", 4);
    u32(dataBytes);
    out.write(static_cast<char const*>(data), static_cast<std::streamsize>(dataBytes));
    return static_cast<bool>(out);
}

}  // namespace

Capture& Capture::get() {
    static Capture* capture = new Capture();   // never destroyed: its threads live until the game exits
    return *capture;
}

CaptureSettings Capture::settingsCopy() const {
    std::lock_guard lock(m_cfgMutex);
    return m_settings;
}

// ---------------------------------------------------------------------------
// Settings (main thread)
// ---------------------------------------------------------------------------

void Capture::applySettings(CaptureSettings settings) {
    settings.config = sanitized(settings.config);
    bool enable = settings.config.enabled;
    bool reprobe = false;
    bool first = false;
    bool changed = false;
    {
        std::lock_guard lock(m_cfgMutex);
        first = m_settings.bufferRoot.empty();
        reprobe = settings.ffmpeg != m_settings.ffmpeg || settings.config.quality != m_settings.config.quality;
        changed = reprobe || settings.bufferRoot != m_settings.bufferRoot;
        m_settings = settings;
    }
    m_haveFfmpeg = !settings.ffmpeg.empty();
    {
        std::lock_guard lock(m_mutex);
        m_book.configure(settings.config.bufferSeconds, static_cast<int64_t>(settings.config.diskCapMb) * 1024 * 1024);
    }
    std::error_code ec;
    if (first && !settings.bufferRoot.empty()) {
        // Leftovers of the last run (the game can exit mid-recording): the buffer never survives a
        // restart, whether clipping is on or off now.
        std::filesystem::remove_all(settings.bufferRoot, ec);
    }

    if (!enable) {
        if (m_enabled.exchange(false)) {
            m_wipeRequested = true;
            log::info("GPRL clip: clipping turned off - the recording stops and the buffer is deleted");
        }
        return;
    }

    std::filesystem::create_directories(settings.bufferRoot, ec);
    if (ec) log::warn("GPRL clip: cannot create the buffer folder {}: {}", utf8(settings.bufferRoot), ec.message());
    if (!m_writerStarted.exchange(true)) std::thread([this] { writerLoop(); }).detach();
    if (changed) m_forceNewSession = true;   // the next session uses the new folder / quality
    if (m_haveFfmpeg && (reprobe || (!m_probed && !m_probing)) && !m_probing.exchange(true)) {
        m_probed = false;
        m_encoderReady = false;
        std::thread([this, settings] { probeEncoders(settings); }).detach();
    }
    if (!m_enabled.exchange(true)) {
        log::info("GPRL clip: clipping ON - {}; buffer folder {}; ffmpeg {}", costLine(settings.config), utf8(settings.bufferRoot),
                  settings.ffmpeg.empty() ? std::string("NOT FOUND") : utf8(settings.ffmpeg));
    }
}

void Capture::probeEncoders(CaptureSettings settings) {
    auto const& q = spec(settings.config.quality);
    Size probe = outputSize(1920, 1080, q.maxHeight);
    bool results[kEncoderCount] = {};
    for (Encoder e : {Encoder::NvencH264, Encoder::AmfH264, Encoder::QsvH264, Encoder::X264}) {
        if (m_exiting) break;
        // the EXACT options the recording uses: an encoder that rejects one of them is not used
        auto args = probeArgs(e, probe, q.videoKbps, kClip.fps, kClip.segmentSeconds, kClip.probeFrames);
        ProcessOptions opts;
        opts.lowPriority = true;
        opts.logFile = settings.bufferRoot / fmt::format("probe-{}.log", name(e));
        auto proc = Process::start(settings.ffmpeg, widen(args), opts);
        bool ok = false;
        if (proc) {
            auto code = proc->wait(kClip.probeTimeoutMs);
            if (!code) proc->kill();
            ok = code && *code == 0;
        }
        results[static_cast<int>(e)] = ok;
        if (!ok) GPRL_DEBUG("GPRL clip: encoder {} not usable: {}", name(e), readTail(opts.logFile, 300));
    }
    Encoder chosen;
    {
        std::lock_guard lock(m_mutex);
        m_ladder = EncoderLadder{};
        for (int i = 1; i < kEncoderCount; ++i) m_ladder.setAvailable(static_cast<Encoder>(i), results[i]);
        chosen = m_ladder.current();
        m_currentEncoder = chosen;
    }
    log::info("GPRL clip: encoders h264_nvenc {}, h264_amf {}, h264_qsv {}, libx264 {} -> using {} ({} {} kbit/s constant bitrate)",
              results[static_cast<int>(Encoder::NvencH264)], results[static_cast<int>(Encoder::AmfH264)], results[static_cast<int>(Encoder::QsvH264)],
              results[static_cast<int>(Encoder::X264)], name(chosen), q.label, q.videoKbps);
    m_encoderReady = chosen != Encoder::None;
    m_probed = true;
    m_probing = false;
    // settings changed while probing (another ffmpeg / quality): probe again on the next applySettings
    auto current = settingsCopy();
    if (current.ffmpeg != settings.ffmpeg || current.config.quality != settings.config.quality) m_probed = false;
}

void Capture::setWanted(bool wanted) {
    if (m_wanted.exchange(wanted) != wanted) GPRL_DEBUG("GPRL clip: capture {}", wanted ? "wanted (a level is open)" : "not wanted");
}

void Capture::setOpenAttemptStart(std::optional<double> start) { m_openAttemptStart = start ? *start : 0.0; }

void Capture::pin(std::string const& id, double from, double to) {
    std::lock_guard lock(m_mutex);
    m_book.pin(id, from, to);
}

void Capture::unpin(std::string const& id) {
    std::lock_guard lock(m_mutex);
    m_book.unpin(id);
}

void Capture::wipe() { m_wipeRequested = true; }

void Capture::shutdown() {
    m_exiting = true;
    m_enabled = false;
    m_queueCv.notify_all();
}

// ---------------------------------------------------------------------------
// GL thread
// ---------------------------------------------------------------------------

std::unique_ptr<Capture::Frame> Capture::takeFrame(int width, int height) {
    std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    std::unique_ptr<Frame> frame;
    {
        std::lock_guard lock(m_queueMutex);
        for (auto it = m_pool.begin(); it != m_pool.end(); ++it) {
            if ((*it)->pixels.size() == bytes) {
                frame = std::move(*it);
                m_pool.erase(it);
                break;
            }
        }
        if (!frame && m_pool.size() > 4) m_pool.clear();   // wrong sizes after a resize
    }
    if (!frame) {
        frame = std::make_unique<Frame>();
        frame->pixels.resize(bytes);
    }
    frame->width = width;
    frame->height = height;
    return frame;
}

void Capture::recycle(std::unique_ptr<Frame> frame) {
    if (!frame) return;
    std::lock_guard lock(m_queueMutex);
    if (m_pool.size() < 8) m_pool.push_back(std::move(frame));
}

void Capture::onFrame() {
    if (!m_writerStarted) return;   // clipping was never on in this game session: nothing to do
    double t = now();
    if (m_lastSwap > 0.0) m_swapInterval = m_swapInterval * 0.9 + std::clamp(t - m_lastSwap, 0.0, 0.1) * 0.1;
    m_lastSwap = t;
    ++m_frameCounter;

    bool wantNow = m_enabled && m_wanted && !m_exiting && m_encoderReady && m_haveFfmpeg && !m_diskLow && t >= m_failedUntil;
    bool anyPending = false;
    for (auto const& p : m_pbos) anyPending = anyPending || p.pending;
    if (!wantNow && !anyPending) {
        if (m_session != 0) {
            m_closeUpTo = std::max<std::uint64_t>(m_closeUpTo, m_session);
            m_session = 0;
        }
        return;
    }

    int width = 0, height = 0;
    HWND hwnd = WindowFromDC(wglGetCurrentDC());
    RECT rc{};
    if (hwnd && !IsIconic(hwnd) && GetClientRect(hwnd, &rc)) {
        width = rc.right - rc.left;
        height = rc.bottom - rc.top;
    }
    bool usable = wantNow && width >= 16 && height >= 16;
    if (!usable && m_session != 0) {
        m_closeUpTo = std::max<std::uint64_t>(m_closeUpTo, m_session);
        m_session = 0;
    }
    if (!usable && !anyPending) return;

    GLint prevPack = 0, prevFramebuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);

    // 1. Hand the oldest finished read-back to the writer (issued on an earlier frame, so it is ready).
    Pbo* ready = nullptr;
    for (auto& p : m_pbos) {
        if (p.pending && p.issuedFrame < m_frameCounter && (!ready || p.issuedFrame < ready->issuedFrame)) ready = &p;
    }
    if (ready) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ready->id);
        auto* src = static_cast<std::uint8_t const*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
        if (src) {
            auto frame = takeFrame(ready->width, ready->height);
            std::memcpy(frame->pixels.data(), src, frame->pixels.size());
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            frame->session = ready->session;
            frame->sessionStart = ready->sessionStart;
            frame->index = ready->index;
            bool dropped = false;
            {
                std::lock_guard lock(m_queueMutex);
                if (m_queue.size() >= static_cast<std::size_t>(kClip.maxQueuedFrames)) dropped = true;
                else m_queue.push_back(std::move(frame));
            }
            if (dropped) {
                recycle(std::move(frame));
                auto drops = ++m_drops;
                if (drops % 120 == 1) log::warn("GPRL clip: the encoder is behind, {} frames dropped so far (the clip repeats the previous frame)", drops);
            }
            else {
                m_queueCv.notify_one();
            }
        }
        ready->pending = false;
    }

    // 2. Start a new read-back when the next 1/60 s slot is due.
    if (usable) {
        const double fps = static_cast<double>(kClip.fps);
        bool force = m_forceNewSession;
        bool fresh = m_session == 0 || force || width != m_width || height != m_height || t - m_lastCapture > kClip.maxGapSeconds;
        bool capture = fresh;
        std::int64_t index = 0;
        if (!fresh) {
            double due = m_sessionStart + static_cast<double>(m_lastIndex + 1) / fps;
            double half = std::min(m_swapInterval * 0.5, 1.0 / (2.0 * fps));
            if (t >= due - half) {
                capture = true;
                index = std::max<std::int64_t>(m_lastIndex + 1, std::llround((t - m_sessionStart) * fps));
            }
        }

        Pbo* slot = nullptr;
        if (capture) {
            for (auto& p : m_pbos) {
                if (!p.pending) {
                    slot = &p;
                    break;
                }
            }
        }
        if (capture && slot) {
            if (fresh) {
                if (m_session != 0) m_closeUpTo = std::max<std::uint64_t>(m_closeUpTo, m_session);
                m_session = ++m_nextSession;
                m_sessionStart = t;
                m_width = width;
                m_height = height;
                m_forceNewSession = false;
                index = 0;
            }
            std::size_t need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
            if (slot->id == 0) {
                GLuint id = 0;
                glGenBuffers(1, &id);
                slot->id = id;
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->id);
            if (slot->capacity != need) {
                glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(need), nullptr, GL_STREAM_READ);
                slot->capacity = need;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            slot->pending = true;
            slot->issuedFrame = m_frameCounter;
            slot->width = width;
            slot->height = height;
            slot->session = m_session;
            slot->sessionStart = m_sessionStart;
            slot->index = index;
            m_lastIndex = index;
            m_lastCapture = t;
        }
        else if (capture && !fresh) {
            // No free buffer: skip this slot, the writer repeats the previous frame.
            m_lastIndex = index;
            m_lastCapture = t;
        }
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPack));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFramebuffer));
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

std::shared_ptr<Capture::Session> Capture::openSession(Frame const& frame) {
    auto cfg = settingsCopy();
    if (cfg.ffmpeg.empty()) return nullptr;
    Encoder encoder;
    int64_t liveBytes = 0;
    {
        std::lock_guard lock(m_mutex);
        encoder = m_ladder.current();
        liveBytes = m_book.liveBytes();
    }
    if (encoder == Encoder::None) return nullptr;
    auto const& q = spec(cfg.config.quality);
    Size source{frame.width, frame.height};
    Size output = outputSize(source.width, source.height, q.maxHeight);
    if (output.width == 0) return nullptr;

    std::error_code ec;
    std::filesystem::create_directories(cfg.bufferRoot, ec);
    // free-space guard: never fill the drive (what the buffer already holds counts as available)
    auto space = std::filesystem::space(cfg.bufferRoot, ec);
    int64_t need = requiredFreeBytes(cfg.config);
    if (!ec && static_cast<int64_t>(space.available) + liveBytes < need) {
        std::string msg = fmt::format("not enough free disk space for the clip buffer: {} MB free on the drive of {}, {} MB needed (change the clip folder or lower the buffer / quality)",
                                      space.available / (1024 * 1024), utf8(cfg.bufferRoot), need / (1024 * 1024));
        bool wasLow = m_diskLow.exchange(true);
        std::lock_guard lock(m_mutex);
        m_lastError = msg;
        if (!wasLow) log::warn("GPRL clip: {}", msg);
        return nullptr;
    }

    // ffmpeg's segment muxer reads the output path as a printf pattern ("seg_%05d.mkv")
    if (utf8(cfg.bufferRoot).find('%') != std::string::npos) {
        static bool warned = false;
        std::lock_guard lock(m_mutex);
        m_lastError = "the clip folder's path contains a % sign, which ffmpeg cannot write to: pick another clip folder in the mod settings";
        if (!warned) log::warn("GPRL clip: {}", m_lastError);
        warned = true;
        return nullptr;
    }

    auto session = std::make_shared<Session>();
    session->id = frame.session;
    session->dir = cfg.bufferRoot / fmt::format("s{}", frame.session);
    session->wallStart = frame.sessionStart;
    session->writtenUntil = frame.sessionStart;
    session->source = source;
    session->output = output;
    session->encoder = encoder;
    // A session id is used once per game run, so anything already in this folder is a leftover of
    // an earlier run (the clip folder was switched to one that still held an old buffer): its
    // segments.csv / seg_*.mkv must never be read as this session's footage.
    std::filesystem::remove_all(session->dir, ec);
    ec.clear();
    std::filesystem::create_directories(session->dir, ec);

    auto args = segmenterArgs(source, output, encoder, q.videoKbps, kClip.fps, kClip.segmentSeconds, utf8(session->dir / "segments.csv"),
                              utf8(session->dir / "seg_%05d.mkv"));
    ProcessOptions opts;
    opts.pipeStdin = true;
    opts.logFile = session->dir / "ffmpeg.log";
    session->process = Process::start(cfg.ffmpeg, widen(args), opts);
    if (!session->process) {
        std::filesystem::remove_all(session->dir, ec);
        std::lock_guard lock(m_mutex);
        m_lastError = "could not start ffmpeg";
        return nullptr;
    }
    log::info("GPRL clip: recording session {}: window {}x{} -> {}x{} {} fps, {} {} kbit/s", session->id, source.width, source.height,
              output.width, output.height, kClip.fps, name(encoder), q.videoKbps);
    GPRL_DEBUG("GPRL clip: ffmpeg {}", args);
    std::lock_guard lock(m_mutex);
    m_sessions.push_back(session);
    m_recording = true;
    m_currentEncoder = encoder;
    m_currentSource = source;
    m_currentOutput = output;
    m_lastError.clear();
    return session;
}

void Capture::closeSession(std::shared_ptr<Session> const& session, double t) {
    if (!session) return;
    std::lock_guard lock(m_mutex);
    if (session->state != Session::State::Running) return;
    if (session->process) session->process->closeStdin();
    session->state = Session::State::Closing;
    session->closeAt = t;
}

void Capture::writerLoop() {
    std::shared_ptr<Session> cur;
    std::unique_ptr<Frame> last;
    std::int64_t lastIndex = -1;
    std::uint64_t highestDone = 0;
    double nextHousekeeping = 0.0;

    auto endCurrent = [&](double t) {
        closeSession(cur, t);
        highestDone = std::max(highestDone, cur->id);
        cur.reset();
        recycle(std::move(last));
        lastIndex = -1;
    };

    for (;;) {
        std::unique_ptr<Frame> frame;
        bool queueEmpty = true;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !m_queue.empty(); });
            if (!m_queue.empty()) {
                frame = std::move(m_queue.front());
                m_queue.pop_front();
            }
            queueEmpty = m_queue.empty();
        }
        double t = now();

        if (frame && cur && frame->session != cur->id) {
            if (frame->session < cur->id) recycle(std::move(frame));
            else endCurrent(t);
        }
        if (frame && !cur) {
            if (frame->session <= highestDone || t < m_failedUntil || !m_enabled) {
                recycle(std::move(frame));
            }
            else {
                cur = openSession(*frame);
                if (!cur) {
                    highestDone = std::max(highestDone, frame->session);
                    m_failedUntil = t + kClip.encoderRetrySeconds;
                    recycle(std::move(frame));
                }
            }
        }
        if (frame && cur) {
            if (frame->index <= lastIndex) {
                recycle(std::move(frame));
            }
            else {
                // Keep the timeline exact: every 1/60 s slot gets a frame.
                Frame const* filler = last ? last.get() : frame.get();
                std::int64_t missing = std::min<std::int64_t>(frame->index - lastIndex - 1, kClip.maxFillFrames);
                bool ok = true;
                for (std::int64_t i = 0; ok && i < missing; ++i) ok = cur->process->write(filler->pixels.data(), filler->pixels.size());
                ok = ok && cur->process->write(frame->pixels.data(), frame->pixels.size());
                if (ok) {
                    cur->frames += missing + 1;
                    lastIndex = frame->index;
                    recycle(std::move(last));
                    last = std::move(frame);
                }
                else {
                    // the encoder died: say why, maybe fall back to the next encoder, retry later
                    auto tail = readTail(cur->dir / "ffmpeg.log", 600);
                    Encoder failedEncoder = cur->encoder;
                    bool demoted = false;
                    Encoder next = Encoder::None;
                    {
                        std::lock_guard lock(m_mutex);
                        demoted = m_ladder.failed(failedEncoder);
                        next = m_ladder.current();
                        m_lastError = fmt::format("the {} encoder stopped", name(failedEncoder));
                    }
                    log::warn("GPRL clip: the recording encoder ({}) stopped after {} frames. ffmpeg said: {}", name(failedEncoder), cur->frames, tail);
                    if (demoted) log::warn("GPRL clip: {} failed {} times in a row, switching to {}", name(failedEncoder), kClip.encoderFailuresBeforeDemote, name(next));
                    recycle(std::move(frame));
                    endCurrent(t);
                    m_failedUntil = t + kClip.encoderRetrySeconds;
                }
            }
        }

        if (cur && queueEmpty && (cur->id <= m_closeUpTo || !m_enabled)) endCurrent(t);

        if (t >= nextHousekeeping) {
            housekeeping(t);
            nextHousekeeping = t + kClip.housekeepingSeconds;
        }
    }
}

void Capture::readSegmentList(Session& session) {
    std::ifstream in(session.dir / "segments.csv", std::ios::binary);
    if (!in) return;
    in.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(in.tellg());
    if (size <= session.csvOffset) return;
    in.seekg(static_cast<std::streamoff>(session.csvOffset));
    std::string text(size - session.csvOffset, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));

    std::size_t consumed = 0;
    std::error_code ec;
    for (;;) {
        auto nl = text.find('\n', consumed);
        if (nl == std::string::npos) break;   // a line still being written is read next time
        auto line = parseSegmentCsvLine(std::string_view(text).substr(consumed, nl - consumed));
        consumed = nl + 1;
        if (!line) continue;
        Segment seg;
        seg.session = session.id;
        seg.file = line->file;
        seg.start = session.wallStart + line->start;
        seg.end = session.wallStart + line->end;
        auto bytes = std::filesystem::file_size(session.dir / fromUtf8(line->file), ec);
        seg.bytes = ec ? 0 : static_cast<int64_t>(bytes);
        seg.size = session.output;
        seg.encoder = session.encoder;
        session.writtenUntil = std::max(session.writtenUntil, seg.end);
        ++session.segments;
        m_book.add(std::move(seg));
        m_ladder.succeeded(session.encoder);
        m_everRecorded = true;
    }
    session.csvOffset += consumed;
}

void Capture::removeFiles(std::vector<Segment> const& removed) {
    std::error_code ec;
    for (auto const& seg : removed) {
        for (auto const& s : m_sessions) {
            if (s->id != seg.session) continue;
            std::filesystem::remove(s->dir / fromUtf8(seg.file), ec);
            break;
        }
    }
}

void Capture::housekeeping(double t) {
    static double nextDiskCheck = 0.0;
    static bool wasCapLimited = false;
    std::vector<KeepRange> sound;
    {
        std::lock_guard lock(m_mutex);
        std::error_code ec;
        bool anyOpen = false;
        for (auto& s : m_sessions) {
            if (s->state == Session::State::Closing && s->process) {
                if (auto code = s->process->wait(0)) {
                    if (*code != 0) log::warn("GPRL clip: the encoder of session {} exited with {}: {}", s->id, *code, readTail(s->dir / "ffmpeg.log", 600));
                    s->process.reset();
                    s->state = Session::State::Closed;
                }
                else if (t - s->closeAt > kClip.closingTimeoutSeconds) {
                    log::warn("GPRL clip: the encoder of session {} did not finish, stopping it", s->id);
                    s->process->kill();
                    s->process.reset();
                    s->state = Session::State::Closed;
                }
            }
            else if (s->state == Session::State::Running && s->process) {
                if (auto code = s->process->wait(0)) {
                    // quit on its own: the writer notices on its next write and reports / retries
                    GPRL_DEBUG("GPRL clip: the encoder of session {} quit early ({})", s->id, *code);
                }
            }
            readSegmentList(*s);
            if (s->state != Session::State::Closed) anyOpen = true;
        }
        m_recording = std::any_of(m_sessions.begin(), m_sessions.end(), [](auto const& s) { return s->state == Session::State::Running; });

        // "forget everything": once no encoder is still writing (or clipping is on again)
        if (m_wipeRequested && (!anyOpen || m_enabled)) {
            m_wipeRequested = false;
            auto all = m_book.clear();
            removeFiles(all);
            log::info("GPRL clip: buffer wiped ({} segments deleted)", all.size());
        }

        double open = m_openAttemptStart;
        auto removed = m_book.prune(open > 0.0 ? std::optional<double>(open) : std::nullopt);
        removeFiles(removed);
        if (m_book.capLimited() != wasCapLimited) {
            wasCapLimited = m_book.capLimited();
            if (wasCapLimited)
                log::warn("GPRL clip: the disk cap ({} MB) is reached - the buffer now holds {:.0f} s instead of the configured {} s", m_book.capBytes() / (1024 * 1024),
                          m_book.recordedSeconds(), m_book.bufferSeconds());
        }

        // session folders nothing refers to any more
        std::set<std::uint64_t> live;
        for (auto const& seg : m_book.segments()) live.insert(seg.session);
        std::erase_if(m_sessions, [&](std::shared_ptr<Session> const& s) {
            if (s->state != Session::State::Closed || live.count(s->id)) return false;
            if (s->segments == 0 && t - s->closeAt < 10.0) return false;   // keep a failed session's ffmpeg.log for a moment
            std::filesystem::remove_all(s->dir, ec);
            return true;
        });

        sound = m_book.soundRanges();
        sound.push_back({t - 5.0, 1e18});
        if (open > 0.0) sound.push_back({open - kClip.leadSeconds - 2.0, 1e18});
    }
    AudioTap::get().prune(sound);
    if (MicCapture::get().enabled()) MicCapture::get().prune(sound);
    if (MicCapture::desktop().enabled()) MicCapture::desktop().prune(sound);

    // the drive filled up (or has room again)
    if (t >= nextDiskCheck) {
        nextDiskCheck = t + kClip.diskCheckSeconds;
        auto cfg = settingsCopy();
        if (m_enabled && !cfg.bufferRoot.empty()) {
            std::error_code ec;
            auto space = std::filesystem::space(cfg.bufferRoot, ec);
            if (!ec) {
                int64_t liveBytes = 0;
                {
                    std::lock_guard lock(m_mutex);
                    liveBytes = m_book.liveBytes();
                }
                bool low = m_diskLow;
                if (!low && static_cast<int64_t>(space.available) < kClip.freeSpaceReserveBytes) {
                    m_diskLow = true;
                    std::lock_guard lock(m_mutex);
                    m_lastError = fmt::format("the drive of {} is almost full ({} MB free): the recording is paused", utf8(cfg.bufferRoot),
                                              space.available / (1024 * 1024));
                    log::warn("GPRL clip: {}", m_lastError);
                }
                else if (low && static_cast<int64_t>(space.available) + liveBytes >= requiredFreeBytes(cfg.config)) {
                    m_diskLow = false;
                    log::info("GPRL clip: enough free disk space again ({} MB), the recording resumes", space.available / (1024 * 1024));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Clips
// ---------------------------------------------------------------------------

ClipSource Capture::snapshot(double from, double to, std::filesystem::path const& dir) {
    ClipSource src;
    src.dir = dir;

    // Wait until the segment that contains the range end has been written (about one segment
    // plus encoder delay), or the session holding it has ended.
    double deadline = std::max(now(), to) + kClip.segmentSeconds + kClip.finalizeGraceSeconds;
    for (;;) {
        bool waiting = false;
        {
            std::lock_guard lock(m_mutex);
            for (auto& s : m_sessions) {
                if (s->state == Session::State::Closed) continue;
                readSegmentList(*s);
                if (s->state == Session::State::Closing) waiting = true;
                else if (s->wallStart < to && s->writtenUntil < to) waiting = true;
            }
        }
        if (!waiting || now() > deadline || m_exiting) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    struct Pick {
        std::filesystem::path path;
        Segment seg;
    };
    std::vector<Pick> picks;
    Selection sel;
    {
        std::lock_guard lock(m_mutex);
        sel = m_book.select(from, to);
        for (auto const& seg : sel.picks) {
            for (auto const& s : m_sessions) {
                if (s->id != seg.session) continue;
                picks.push_back({s->dir / fromUtf8(seg.file), seg});
                break;
            }
        }
    }
    if (!sel.error.empty() || picks.empty()) {
        src.error = sel.error.empty() ? std::string("nothing was recorded for this attempt") : sel.error;
        return src;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::vector<std::pair<std::string, double>> files;
    double duration = 0.0;
    for (std::size_t i = 0; i < picks.size(); ++i) {
        auto target = dir / fmt::format("part_{:04}.mkv", i);
        if (!CreateHardLinkW(target.wstring().c_str(), picks[i].path.wstring().c_str(), nullptr)) {
            std::filesystem::copy_file(picks[i].path, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                src.error = fmt::format("could not copy a part of the clip: {}", ec.message());
                return src;
            }
        }
        double d = picks[i].seg.end - picks[i].seg.start;
        files.emplace_back(utf8(target), d);
        duration += d;
    }
    if (duration < 0.5) {
        src.error = "the clip is too short";
        return src;
    }
    src.concatFile = dir / "source.ffconcat";
    {
        std::ofstream list(src.concatFile, std::ios::binary);
        list << concatList(files);
        if (!list) {
            src.error = "could not write the clip's segment list";
            return src;
        }
    }

    // Sound tracks cover the whole concat timeline (every pick), like the video.
    auto cfg = settingsCopy();
    auto& tap = AudioTap::get();
    if (cfg.config.gameAudio && tap.active()) {
        auto map = tap.makeMapping(sel.start, sel.end);
        if (map.ok) {
            std::vector<float> pcm;
            int rate = tap.rate();
            double cum = 0.0;
            for (auto const& p : picks) {
                double d = p.seg.end - p.seg.start;
                auto n = std::llround((cum + d) * rate) - std::llround(cum * rate);
                tap.extract(map, p.seg.start, n, pcm);
                cum += d;
            }
            auto wav = dir / "game.wav";
            if (writeWav(wav, pcm.data(), pcm.size() * sizeof(float), rate, 2, true)) src.gameWav = wav;
        }
        else {
            log::info("GPRL clip: not enough sound timing data yet, this clip has no game sound");
        }
    }
    // The microphone and (v0.9.0) the desktop sound: one WAV each, the same timeline as the video.
    auto pcmTrack = [&](MicCapture const& tap, bool wanted, char const* file, std::filesystem::path& out) {
        if (!wanted || !tap.enabled() || !tap.hasData(sel.start, sel.end)) return;
        std::vector<std::int16_t> pcm;
        int rate = 48000, channels = 1;
        tap.extract(sel.start, 0, pcm, rate, channels);   // a zero-length read: the device's rate / channels
        double cum = 0.0;
        for (auto const& p : picks) {
            double d = p.seg.end - p.seg.start;
            auto n = std::llround((cum + d) * rate) - std::llround(cum * rate);
            int r = rate, c = channels;
            tap.extract(p.seg.start, n, pcm, r, c);
            cum += d;
        }
        auto wav = dir / file;
        if (!pcm.empty() && writeWav(wav, pcm.data(), pcm.size() * sizeof(std::int16_t), rate, channels, false)) out = wav;
    };
    pcmTrack(MicCapture::get(), cfg.config.micAudio, "mic.wav", src.micWav);
    pcmTrack(MicCapture::desktop(), cfg.config.desktopAudio, "desktop.wav", src.desktopWav);

    src.start = sel.start;
    src.duration = duration;
    src.size = picks.back().seg.size;
    src.coversStart = sel.coversStart;
    src.segments = static_cast<int>(picks.size());
    return src;
}

CaptureStatus Capture::status() const {
    CaptureStatus st;
    st.enabled = m_enabled;
    st.wanted = m_wanted;
    st.probed = m_probed;
    st.everRecorded = m_everRecorded;
    st.droppedFrames = m_drops;
    std::string lastError;
    {
        std::lock_guard lock(m_mutex);
        st.recording = m_recording;
        st.encoder = m_currentEncoder;
        st.source = m_currentSource;
        st.output = m_currentOutput;
        st.recordedSeconds = m_book.recordedSeconds();
        st.liveBytes = m_book.liveBytes();
        st.capLimited = m_book.capLimited();
        lastError = m_lastError;
    }
    if (!st.enabled) return st;
    if (!m_haveFfmpeg) st.problem = "ffmpeg.exe was not found: put it in the mod's save folder or pick it in the mod settings";
    else if (!st.probed) st.problem = "checking the video encoders...";
    else if (!m_encoderReady) st.problem = "ffmpeg has no usable H.264 encoder (no NVENC / AMF / Quick Sync and no libx264)";
    else if (m_diskLow) st.problem = lastError.empty() ? std::string("not enough free disk space") : lastError;
    else if (now() < m_failedUntil && !lastError.empty()) st.problem = lastError + " (retrying)";
    return st;
}

}  // namespace gprl::clip
