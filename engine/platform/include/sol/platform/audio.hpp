#pragma once

// Audio output device behind a portable interface (engine plan 2.10, Phase
// 8t). The platform layer owns the device and the thread that feeds it; what
// goes into the buffer is sol::audio's business, exactly as the window owns
// the surface and the renderer owns what is drawn on it.
//
// The device pulls. It calls back on its OWN thread whenever it wants more
// frames - never on the frame loop - because a mix that depends on frame
// pacing turns any hitch into an audible click, and the buffer has to be fed
// whether or not the game is drawing.

#include <cstdint>
#include <memory>

namespace sol::platform {

// The one layout the mixer produces. Devices with more channels get silence
// in the rest; devices with fewer are refused.
inline constexpr std::uint32_t kAudioChannels = 2;

class AudioRenderer
{
public:
    virtual ~AudioRenderer() = default;

    // Write frameCount * kAudioChannels interleaved floats in [-1, 1] into
    // out. Called on the device thread: never block, never allocate, never
    // touch game state that the frame loop can be writing.
    virtual void renderAudio(float* out, std::uint32_t frameCount) = 0;
};

struct AudioDeviceInfo
{
    std::uint32_t sampleRate = 0;   // whatever the endpoint mixes at
    std::uint32_t bufferFrames = 0; // one buffer's worth, for logging
};

class AudioDevice
{
public:
    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Starts the device thread. Returns false when there is no usable output
    // endpoint, which is NOT an error the game should die of: the caller logs
    // it and runs silent. renderer must outlive the device.
    [[nodiscard]] bool open(AudioRenderer* renderer);
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] AudioDeviceInfo info() const;

    // Buffers the device asked for that the renderer could not fill in time.
    // Nonzero means the mix is too slow or the thread is being starved.
    [[nodiscard]] std::uint64_t underrunCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::platform
