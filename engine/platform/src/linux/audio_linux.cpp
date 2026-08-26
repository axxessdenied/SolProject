// Linux audio output (Phase 21) — DELIBERATELY NOT IMPLEMENTED YET.
//
// Phase 21 goes as far as the first rendered frame and stops; ALSA is its own
// stage. This is not a placeholder that pretends: `AudioDevice::open` is
// documented to return false when there is no usable endpoint, and to have the
// caller log it and run silent. That is a state the game already handles on
// Windows (no default endpoint, exclusive-mode device, no sound card), so
// reporting it here exercises a real path rather than inventing a fake one.
//
// ⚑ When ALSA lands, note that WSLg publishes a PULSEAUDIO server at
// /mnt/wslg/PulseServer and NOT an ALSA device, so ALSA reaches it only through
// libasound's pulse plugin (libasound2-plugins). A first run that finds no
// device under WSL means that package is missing, not that the code is wrong.

#include "sol/platform/audio.hpp"

namespace sol::platform {

struct AudioDevice::Impl
{
};

AudioDevice::AudioDevice() = default;
AudioDevice::~AudioDevice() = default;

bool AudioDevice::open(AudioRenderer* renderer)
{
    (void)renderer;
    return false; // no endpoint: the caller logs it and runs silent
}

void AudioDevice::close()
{
}

bool AudioDevice::isOpen() const
{
    return false;
}

AudioDeviceInfo AudioDevice::info() const
{
    return {};
}

std::uint64_t AudioDevice::underrunCount() const
{
    // Zero rather than "unknown": nothing was ever asked for, so nothing was
    // ever missed. A nonzero value here would read as a starved mixer.
    return 0;
}

} // namespace sol::platform
