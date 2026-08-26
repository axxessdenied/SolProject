#include "sol/core/log.hpp"
#include "sol/platform/audio.hpp"

// clang-format off
// ⚑ THIS ORDER IS LOAD-BEARING AND THE GUARD IS THE ONLY THING HOLDING IT.
// <audioclient.h> and <mmdeviceapi.h> both build on types <windows.h> declares,
// so they do not compile ahead of it. `IncludeBlocks: Regroup` ignores the blank
// line that used to separate them and sorts the three together, which puts
// `windows.h` last on its own name.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
// clang-format on

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace sol::platform {

namespace {

// The device thread waits this long for a buffer before deciding the endpoint
// has stopped answering; long enough that a scheduling hiccup is not mistaken
// for a dead device.
constexpr DWORD kBufferWaitMs = 2000;

// How long open() waits for the thread to report whether it found a device.
constexpr DWORD kOpenWaitMs = 5000;

template <typename T>
void release(T*& ptr)
{
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

// Shared mode hands back whatever the endpoint mixes at, and on every modern
// Windows that is 32-bit float. Anything else would need a conversion path
// this item does not have a use for, so it degrades to silence instead.
bool isFloat32(const WAVEFORMATEX& format)
{
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return format.wBitsPerSample == 32;
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        // KSDATAFORMAT_SUBTYPE_* are the format tag in Data1 against a fixed
        // template, so this is the subtype comparison without dragging in
        // ksmedia.h for one constant.
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        return format.wBitsPerSample == 32 && extensible.SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

} // namespace

struct AudioDevice::Impl
{
    AudioRenderer* renderer = nullptr;

    HANDLE stopEvent = nullptr;
    HANDLE readyEvent = nullptr;
    std::thread thread;

    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> underruns{0};
    std::atomic<std::uint32_t> sampleRate{0};
    std::atomic<std::uint32_t> bufferFrames{0};

    // Scratch the renderer fills, owned by the device thread. Sized once at
    // startup so the callback never allocates.
    std::vector<float> mixBuffer;

    void run();
    bool startDevice(IAudioClient*& client,
                     IAudioRenderClient*& render,
                     HANDLE bufferEvent,
                     std::uint32_t& channels);
};

// Everything COM touches happens on this thread, so the apartment lives
// exactly as long as the device does and open() never has to care.
void AudioDevice::Impl::run()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comResult);
    if (!comOwned && comResult != RPC_E_CHANGED_MODE) {
        SOL_LOG_WARN("audio: CoInitializeEx failed (0x%08lx); running silent",
                     static_cast<unsigned long>(comResult));
        SetEvent(readyEvent);
        return;
    }

    HANDLE bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    std::uint32_t channels = 0;

    if (bufferEvent == nullptr || !startDevice(client, render, bufferEvent, channels)) {
        release(render);
        release(client);
        if (bufferEvent != nullptr) {
            CloseHandle(bufferEvent);
        }
        if (comOwned) {
            CoUninitialize();
        }
        SetEvent(readyEvent);
        return;
    }

    running.store(true, std::memory_order_release);
    SetEvent(readyEvent);

    const std::uint32_t totalFrames = bufferFrames.load(std::memory_order_relaxed);
    HANDLE waits[2] = {bufferEvent, stopEvent};

    while (true) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, kBufferWaitMs);
        if (wait != WAIT_OBJECT_0) {
            // WAIT_OBJECT_0 + 1 is the stop request; anything else means the
            // endpoint stopped answering and there is nothing to recover to.
            break;
        }

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding)) || padding > totalFrames) {
            break;
        }
        const std::uint32_t available = totalFrames - padding;
        if (available == 0) {
            continue;
        }

        BYTE* buffer = nullptr;
        if (FAILED(render->GetBuffer(available, &buffer)) || buffer == nullptr) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        renderer->renderAudio(mixBuffer.data(), available);

        // The mix is stereo; an endpoint with more channels gets silence in
        // the rest rather than a fold-out this item has no policy for.
        float* out = reinterpret_cast<float*>(buffer);
        if (channels == kAudioChannels) {
            std::memcpy(
                out, mixBuffer.data(), static_cast<std::size_t>(available) * kAudioChannels * sizeof(float));
        } else {
            std::memset(out, 0, static_cast<std::size_t>(available) * channels * sizeof(float));
            for (std::uint32_t frame = 0; frame < available; ++frame) {
                out[frame * channels + 0] = mixBuffer[frame * kAudioChannels + 0];
                out[frame * channels + 1] = mixBuffer[frame * kAudioChannels + 1];
            }
        }

        if (FAILED(render->ReleaseBuffer(available, 0))) {
            break;
        }
    }

    running.store(false, std::memory_order_release);
    client->Stop();
    release(render);
    release(client);
    CloseHandle(bufferEvent);
    if (comOwned) {
        CoUninitialize();
    }
}

bool AudioDevice::Impl::startDevice(IAudioClient*& client,
                                    IAudioRenderClient*& render,
                                    HANDLE bufferEvent,
                                    std::uint32_t& channels)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        SOL_LOG_WARN("audio: no device enumerator (0x%08lx); running silent", static_cast<unsigned long>(hr));
        return false;
    }

    IMMDevice* endpoint = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    release(enumerator);
    if (FAILED(hr)) {
        // A machine with no speakers is a machine that plays the game silently.
        SOL_LOG_WARN("audio: no default output endpoint (0x%08lx); running silent",
                     static_cast<unsigned long>(hr));
        return false;
    }

    hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    release(endpoint);
    if (FAILED(hr)) {
        SOL_LOG_WARN("audio: cannot activate endpoint (0x%08lx); running silent",
                     static_cast<unsigned long>(hr));
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(client->GetMixFormat(&mixFormat)) || mixFormat == nullptr) {
        return false;
    }
    const bool usable = isFloat32(*mixFormat) && mixFormat->nChannels >= kAudioChannels;
    if (!usable) {
        SOL_LOG_WARN("audio: endpoint mixes %u-bit x%u, not float32 stereo+; running silent",
                     mixFormat->wBitsPerSample,
                     mixFormat->nChannels);
        CoTaskMemFree(mixFormat);
        return false;
    }
    channels = mixFormat->nChannels;
    sampleRate.store(mixFormat->nSamplesPerSec, std::memory_order_relaxed);

    // Zero duration asks for the engine's own period, which is what an
    // event-driven shared-mode client wants.
    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    if (FAILED(hr)) {
        SOL_LOG_WARN("audio: client init failed (0x%08lx); running silent", static_cast<unsigned long>(hr));
        return false;
    }

    UINT32 frames = 0;
    if (FAILED(client->SetEventHandle(bufferEvent)) || FAILED(client->GetBufferSize(&frames)) ||
        frames == 0) {
        return false;
    }
    bufferFrames.store(frames, std::memory_order_relaxed);
    mixBuffer.assign(static_cast<std::size_t>(frames) * kAudioChannels, 0.0f);

    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)))) {
        return false;
    }

    // Hand the device a buffer of silence before starting, so the first real
    // callback is not racing the endpoint.
    BYTE* prime = nullptr;
    if (SUCCEEDED(render->GetBuffer(frames, &prime)) && prime != nullptr) {
        std::memset(prime, 0, static_cast<std::size_t>(frames) * channels * sizeof(float));
        (void)render->ReleaseBuffer(frames, 0);
    }

    if (FAILED(client->Start())) {
        return false;
    }

    SOL_LOG_INFO("audio: WASAPI shared, %u Hz, %u channels, %u frame buffer (%.1f ms)",
                 sampleRate.load(std::memory_order_relaxed),
                 channels,
                 frames,
                 1000.0 * frames / static_cast<double>(sampleRate.load(std::memory_order_relaxed)));
    return true;
}

AudioDevice::AudioDevice() = default;

AudioDevice::~AudioDevice()
{
    close();
}

bool AudioDevice::open(AudioRenderer* renderer)
{
    if (renderer == nullptr || m_impl != nullptr) {
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->renderer = renderer;
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl->stopEvent == nullptr || impl->readyEvent == nullptr) {
        if (impl->stopEvent != nullptr) {
            CloseHandle(impl->stopEvent);
        }
        if (impl->readyEvent != nullptr) {
            CloseHandle(impl->readyEvent);
        }
        return false;
    }

    Impl* raw = impl.get();
    impl->thread = std::thread([raw]() { raw->run(); });

    // The thread signals ready whether it found a device or not, so a machine
    // with no endpoint costs one startup wait rather than the full timeout.
    (void)WaitForSingleObject(impl->readyEvent, kOpenWaitMs);
    if (!impl->running.load(std::memory_order_acquire)) {
        SetEvent(impl->stopEvent);
        if (impl->thread.joinable()) {
            impl->thread.join();
        }
        CloseHandle(impl->stopEvent);
        CloseHandle(impl->readyEvent);
        return false;
    }

    m_impl = std::move(impl);
    return true;
}

void AudioDevice::close()
{
    if (m_impl == nullptr) {
        return;
    }
    SetEvent(m_impl->stopEvent);
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    CloseHandle(m_impl->stopEvent);
    CloseHandle(m_impl->readyEvent);
    m_impl.reset();
}

bool AudioDevice::isOpen() const
{
    return m_impl != nullptr && m_impl->running.load(std::memory_order_acquire);
}

AudioDeviceInfo AudioDevice::info() const
{
    AudioDeviceInfo out = {};
    if (m_impl != nullptr) {
        out.sampleRate = m_impl->sampleRate.load(std::memory_order_relaxed);
        out.bufferFrames = m_impl->bufferFrames.load(std::memory_order_relaxed);
    }
    return out;
}

std::uint64_t AudioDevice::underrunCount() const
{
    return m_impl == nullptr ? 0 : m_impl->underruns.load(std::memory_order_relaxed);
}

} // namespace sol::platform
