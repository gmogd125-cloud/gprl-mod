#include "MicCapture.hpp"

#include <Geode/Geode.hpp>

#include <Windows.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "ClipUtil.hpp"

using namespace geode::prelude;

namespace gprl::clip {

namespace {

constexpr int kWantRate = 48000;
constexpr double kMaxStoredSeconds = kClip.soundMaxStoredSeconds;   // the longest clip + margin (~120 MB stereo)
constexpr double kRestartSeconds = kClip.micRestartSeconds;

// GUIDs written out so no header needs INITGUID.
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// PKEY_Device_FriendlyName
PROPERTYKEY const kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(ComPtr const&) = delete;
    ComPtr& operator=(ComPtr const&) = delete;
    ~ComPtr() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Format {
    int rate = kWantRate;
    int inChannels = 1;
    int outChannels = 1;
    int bits = 16;
    bool isFloat = false;
    bool direct = false;   // already interleaved int16 with outChannels == inChannels
};

Format describe(WAVEFORMATEX const* f) {
    Format r;
    r.rate = static_cast<int>(f->nSamplesPerSec);
    r.inChannels = std::max<int>(f->nChannels, 1);
    r.outChannels = std::clamp<int>(f->nChannels, 1, 2);
    r.bits = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) r.isFloat = true;
    else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto const* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE const*>(f);
        r.isFloat = IsEqualGUID(ext->SubFormat, kSubtypeFloat) != 0;
    }
    r.direct = !r.isFloat && r.bits == 16 && r.inChannels == r.outChannels;
    return r;
}

std::int16_t toPcm(float v) {
    v = std::clamp(v, -1.f, 1.f);
    return static_cast<std::int16_t>(std::lrint(v * 32767.f));
}

void convert(Format const& f, BYTE const* data, UINT32 frames, std::vector<std::int16_t>& out) {
    out.resize(static_cast<std::size_t>(frames) * f.outChannels);
    if (f.direct) {
        std::memcpy(out.data(), data, out.size() * sizeof(std::int16_t));
        return;
    }
    int bytesPer = std::max(f.bits / 8, 1);
    for (UINT32 i = 0; i < frames; ++i) {
        for (int c = 0; c < f.outChannels; ++c) {
            int sc = std::min(c, f.inChannels - 1);
            BYTE const* s = data + (static_cast<std::size_t>(i) * f.inChannels + sc) * bytesPer;
            float v = 0.f;
            if (f.isFloat) {
                if (f.bits == 32) std::memcpy(&v, s, 4);
                else if (f.bits == 64) {
                    double d = 0.0;
                    std::memcpy(&d, s, 8);
                    v = static_cast<float>(d);
                }
            }
            else if (f.bits == 16) {
                std::int16_t x = 0;
                std::memcpy(&x, s, 2);
                v = x / 32768.f;
            }
            else if (f.bits == 32) {
                std::int32_t x = 0;
                std::memcpy(&x, s, 4);
                v = static_cast<float>(x / 2147483648.0);
            }
            else if (f.bits == 24) {
                std::int32_t x = (s[0] << 8) | (s[1] << 16) | (static_cast<std::int32_t>(s[2]) << 24);
                v = static_cast<float>(x / 2147483648.0);
            }
            else if (f.bits == 8) {
                v = (static_cast<int>(s[0]) - 128) / 128.f;
            }
            out[static_cast<std::size_t>(i) * f.outChannels + c] = toPcm(v);
        }
    }
}

bool overlaps(std::vector<KeepRange> const& keep, double a, double b) {
    for (auto const& r : keep) {
        if (a <= r.to && b >= r.from) return true;
    }
    return false;
}

std::wstring endpointId(IMMDevice* dev) {
    LPWSTR w = nullptr;
    std::wstring id;
    if (dev && SUCCEEDED(dev->GetId(&w)) && w) id = w;
    if (w) CoTaskMemFree(w);
    return id;
}

std::string endpointName(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    if (!dev || FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string name;
    if (SUCCEEDED(props->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) name = narrow(pv.pwszVal);
    PropVariantClear(&pv);
    return name;
}

}  // namespace

MicCapture& MicCapture::get() {
    static MicCapture* capture = new MicCapture();   // never destroyed: its threads live until the game exits
    return *capture;
}

MicCapture& MicCapture::desktop() {
    static MicCapture* capture = new MicCapture(true);
    return *capture;
}

void MicCapture::setEnabled(bool on) {
    bool was = m_enabled.exchange(on);
    if (on && !m_started.exchange(true)) std::thread([this] { managerLoop(); }).detach();
    if (was != on) {
        if (m_loopback) log::info("GPRL clip: desktop sound track {} (the default output device, loopback)", on ? "ON" : "off");
        else log::info("GPRL clip: microphone track {} (separate opt-in; default recording device only)", on ? "ON" : "off");
    }
}

std::string MicCapture::deviceName() const {
    std::lock_guard lock(m_mutex);
    return m_name;
}

std::string MicCapture::problem() const {
    std::lock_guard lock(m_mutex);
    return m_error;
}

// ---- manager thread: follows the default recording device ----

void MicCapture::managerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring currentId;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!m_enabled) {
            if (m_thread.joinable()) stopCapture();
            {
                // opt-out: nothing recorded stays in memory. Also when no capture thread is left to
                // stop (the device had gone away earlier): older chunks must not survive an opt-out
                // and reappear in a clip after a later opt-in.
                std::lock_guard lock(m_mutex);
                m_chunks.clear();
                m_name.clear();
                m_error.clear();
            }
            currentId.clear();
            continue;
        }

        std::wstring defaultId;
        std::string name;
        {
            ComPtr<IMMDeviceEnumerator> enumr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(&enumr));
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(hr) && SUCCEEDED(enumr->GetDefaultAudioEndpoint(m_loopback ? eRender : eCapture, eConsole, &dev))) {
                defaultId = endpointId(dev.p);
                name = endpointName(dev.p);
            }
        }
        if (defaultId.empty()) {
            if (m_thread.joinable()) stopCapture();
            std::lock_guard lock(m_mutex);
            if (m_error != "no recording device") log::info("GPRL clip: no default {} device, clips will have no {} track", m_loopback ? "output" : "recording", label());
            m_error = "no recording device";
            continue;
        }
        if (m_thread.joinable() && (defaultId != currentId || !m_running)) {
            stopCapture();
            if (defaultId != currentId) {
                std::lock_guard lock(m_mutex);
                m_chunks.clear();   // another device: its format may differ
            }
        }
        if (!m_thread.joinable() && now() - m_lastStop.load() >= kRestartSeconds) {
            currentId = defaultId;
            m_stop = false;
            m_running = true;
            m_thread = std::thread([this, defaultId, name] { captureLoop(defaultId, name.empty() ? std::string(label()) : name); });
        }
    }
}

void MicCapture::stopCapture() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

// ---- capture thread ----

void MicCapture::captureLoop(std::wstring deviceId, std::string name) {
    struct Done {
        MicCapture& self;
        ~Done() {
            self.m_lastStop = now();
            self.m_running = false;
        }
    } done{*this};

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        bool init;
        ~ComScope() {
            if (init) CoUninitialize();
        }
    } comScope{SUCCEEDED(hr)};

    auto fail = [&](char const* what, HRESULT code) {
        auto msg = fmt::format("{} (0x{:08x})", what, static_cast<unsigned>(code));
        std::lock_guard lock(m_mutex);
        if (m_error != msg) log::warn("GPRL clip: {} '{}': {}", label(), name, msg);
        m_error = msg;
    };

    ComPtr<IMMDeviceEnumerator> enumr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumr));
    ComPtr<IMMDevice> dev;
    if (SUCCEEDED(hr)) hr = enumr->GetDevice(deviceId.c_str(), &dev);
    if (FAILED(hr)) {
        fail("could not open the device", hr);
        return;
    }

    ComPtr<IAudioClient> client;
    auto activate = [&] {
        client.reset();
        return dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    };
    hr = activate();
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        if (mix) CoTaskMemFree(mix);
        fail("could not read the format", hr);
        return;
    }

    // Ask Windows for 48 kHz 16-bit directly; it converts from whatever the device does.
    WAVEFORMATEXTENSIBLE want{};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = static_cast<WORD>(std::clamp<int>(mix->nChannels, 1, 2));
    want.Format.nSamplesPerSec = kWantRate;
    want.Format.wBitsPerSample = 16;
    want.Format.nBlockAlign = static_cast<WORD>(want.Format.nChannels * 2);
    want.Format.nAvgBytesPerSec = kWantRate * want.Format.nBlockAlign;
    want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want.Samples.wValidBitsPerSample = 16;
    want.dwChannelMask = want.Format.nChannels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    want.SubFormat = kSubtypePcm;

    Format fmtIn;
    // Loopback (v0.9.0, the desktop sound): WASAPI delivers no events on a loopback stream, so it
    // is polled every 20 ms from a one-second buffer instead of waiting on the event.
    DWORD const modeFlags = m_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    REFERENCE_TIME const bufferDuration = m_loopback ? 10'000'000 : 0;   // 100 ns units: 1 s | the default
    DWORD flags = modeFlags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, &want.Format, nullptr);
    if (SUCCEEDED(hr)) {
        fmtIn.rate = kWantRate;
        fmtIn.inChannels = fmtIn.outChannels = want.Format.nChannels;
        fmtIn.bits = 16;
        fmtIn.direct = true;
    }
    else {
        // Older path: take the shared format as is and convert by hand.
        hr = activate();
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, modeFlags, bufferDuration, 0, mix, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(mix);
            fail("could not start recording", hr);
            return;
        }
        fmtIn = describe(mix);
    }
    CoTaskMemFree(mix);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        fail("no event", E_FAIL);
        return;
    }
    if (!m_loopback) hr = client->SetEventHandle(ev);
    ComPtr<IAudioCaptureClient> cap;
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        CloseHandle(ev);
        fail("could not start recording", hr);
        return;
    }
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    {
        std::lock_guard lock(m_mutex);
        if (m_rate != fmtIn.rate || m_channels != fmtIn.outChannels) m_chunks.clear();
        m_rate = fmtIn.rate;
        m_channels = fmtIn.outChannels;
        m_name = name;
        m_error.clear();
    }
    log::info("GPRL clip: recording the {} '{}' at {} Hz {} ({} track, kept in memory only until a clip is cut)", label(), name, fmtIn.rate,
              fmtIn.outChannels == 1 ? "mono" : "stereo", m_loopback ? "desktop" : "opt-in");

    std::vector<std::int16_t> conv;
    bool first = true;
    double nextGuess = 0.0;
    while (!m_stop) {
        if (m_loopback) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        else WaitForSingleObject(ev, 200);
        if (m_stop) break;
        UINT32 packet = 0;
        hr = cap->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0 && !m_stop) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufFlags = 0;
            UINT64 devPos = 0, qpc = 0;
            hr = cap->GetBuffer(&data, &frames, &bufFlags, &devPos, &qpc);
            if (FAILED(hr)) break;
            double nowT = now();
            double dur = static_cast<double>(frames) / fmtIn.rate;
            // The device clock stamps the first frame in QPC time, the same clock as now().
            double t = 0.0;
            bool fromDevice = !(bufFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
            if (fromDevice) {
                t = static_cast<double>(qpc) / 1e7;
                if (std::abs(t - nowT) > 1.0) fromDevice = false;
            }
            if (!fromDevice) {
                double guess = nowT - dur;
                t = std::abs(guess - nextGuess) < 0.05 ? nextGuess : guess;
            }
            nextGuess = t + dur;
            bool disc = first || (bufFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            first = false;
            if (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) conv.assign(static_cast<std::size_t>(frames) * fmtIn.outChannels, 0);
            else convert(fmtIn, data, frames, conv);
            push(conv.data(), frames, t, disc);
            cap->ReleaseBuffer(frames);
            hr = cap->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) {
            fail("recording stopped", hr);
            break;
        }
    }
    client->Stop();
    if (task) AvRevertMmThreadCharacteristics(task);
    CloseHandle(ev);
}

void MicCapture::push(std::int16_t const* frames, std::size_t count, double t, bool discontinuity) {
    // No level is open: what the device delivers is dropped here, never stored (setStoring).
    if (!m_storing.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(m_mutex);
    int ch = m_channels;
    auto const chunkFrames = static_cast<std::size_t>(m_rate);   // one second
    bool fresh = m_chunks.empty() || discontinuity;
    if (!fresh) {
        auto& back = m_chunks.back();
        std::size_t have = back.pcm.size() / ch;
        double expected = back.wallTime + static_cast<double>(have) / m_rate;
        if (have >= chunkFrames || std::abs(t - expected) > 0.02) fresh = true;
    }
    if (fresh) {
        Chunk c;
        c.wallTime = t;
        c.pcm.reserve((chunkFrames + 2048) * ch);
        m_chunks.push_back(std::move(c));
    }
    auto& chunk = m_chunks.back();
    chunk.pcm.insert(chunk.pcm.end(), frames, frames + count * ch);
    // Safety net when nothing prunes (chunks are at most one second).
    while (m_chunks.size() > static_cast<std::size_t>(kMaxStoredSeconds) * 2 + 120) m_chunks.pop_front();
}

// ---- reading ----

bool MicCapture::hasData(double from, double to) const {
    std::lock_guard lock(m_mutex);
    for (auto const& c : m_chunks) {
        double a = c.wallTime, b = a + static_cast<double>(c.pcm.size() / m_channels) / m_rate;
        if (a <= to && b >= from) return true;
    }
    return false;
}

void MicCapture::extract(double t0, std::int64_t count, std::vector<std::int16_t>& out, int& rate, int& channels) const {
    std::lock_guard lock(m_mutex);
    rate = m_rate;
    channels = std::max(m_channels, 1);
    int ch = channels;
    std::size_t base = out.size();
    out.resize(base + static_cast<std::size_t>(std::max<std::int64_t>(count, 0)) * ch, 0);
    if (count <= 0) return;
    for (auto const& c : m_chunks) {
        auto frames = static_cast<std::int64_t>(c.pcm.size() / ch);
        auto off = static_cast<std::int64_t>(std::llround((c.wallTime - t0) * rate));
        if (off >= count || off + frames <= 0) continue;
        std::int64_t s0 = std::max<std::int64_t>(0, -off);
        std::int64_t d0 = std::max<std::int64_t>(0, off);
        std::int64_t n = std::min(frames - s0, count - d0);
        if (n <= 0) continue;
        std::memcpy(&out[base + static_cast<std::size_t>(d0) * ch], &c.pcm[static_cast<std::size_t>(s0) * ch],
                    static_cast<std::size_t>(n) * ch * sizeof(std::int16_t));
    }
}

void MicCapture::prune(std::vector<KeepRange> const& keep) {
    std::lock_guard lock(m_mutex);
    int rate = std::max(m_rate, 1);
    int ch = std::max(m_channels, 1);
    auto seconds = [&](Chunk const& c) { return static_cast<double>(c.pcm.size() / ch) / rate; };
    std::size_t n = m_chunks.size();
    std::deque<Chunk> survivors;
    for (std::size_t i = 0; i < n; ++i) {
        double a = m_chunks[i].wallTime;
        double b = a + seconds(m_chunks[i]);
        // Never drop the newest two chunks (the capture thread is filling the last one).
        if (i + 2 >= n || overlaps(keep, a - 1.0, b + 1.0)) survivors.push_back(std::move(m_chunks[i]));
    }
    // Hard memory cap: drop the oldest first.
    double total = 0.0;
    std::size_t keepFrom = 0;
    for (std::size_t i = survivors.size(); i-- > 0;) {
        total += seconds(survivors[i]);
        if (total > kMaxStoredSeconds) {
            keepFrom = i + 1;
            break;
        }
    }
    if (keepFrom > 0) survivors.erase(survivors.begin(), survivors.begin() + static_cast<std::ptrdiff_t>(keepFrom));
    m_chunks.swap(survivors);
}

}  // namespace gprl::clip
