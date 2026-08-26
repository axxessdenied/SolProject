#include "sol/audio/mixer.hpp"

#include "sol/core/log.hpp"

#include <algorithm>
#include <cmath>

namespace sol::audio {

namespace {

// Below this the pan collapses to centre; a source closer than a few meters
// has no meaningful direction from a listener with two ears.
constexpr double kPanNearDistance = 25.0;

float sampleAt(const SoundClip& clip, double cursor)
{
    // Linear interpolation between neighbouring frames. This is the one place
    // rate conversion happens, and it serves both the device rate and pitch.
    const std::uint32_t frames = clip.frameCount();
    if (frames == 0) {
        return 0.0f;
    }
    const double clamped = std::clamp(cursor, 0.0, static_cast<double>(frames - 1));
    const auto index = static_cast<std::uint32_t>(clamped);
    const auto next = static_cast<std::uint32_t>(std::min<std::uint64_t>(index + 1, frames - 1));
    const auto fraction = static_cast<float>(clamped - static_cast<double>(index));

    const std::uint32_t stride = clip.channelCount;
    // A stereo clip is folded to one channel here; only mono is pannable, and
    // a stereo cue is played 2D, where the fold is the correct downmix.
    float a = 0.0f;
    float b = 0.0f;
    for (std::uint32_t channel = 0; channel < stride; ++channel) {
        a += static_cast<float>(clip.samples[index * stride + channel]);
        b += static_cast<float>(clip.samples[next * stride + channel]);
    }
    a /= static_cast<float>(stride);
    b /= static_cast<float>(stride);

    return ((a * (1.0f - fraction)) + (b * fraction)) / 32768.0f;
}

} // namespace

float attenuationAt(double distance, float rolloffDistance)
{
    const double rolloff = std::max(1.0, static_cast<double>(rolloffDistance));
    const double clamped = std::max(0.0, distance);
    return static_cast<float>(rolloff / (rolloff + clamped));
}

void equalPowerPan(float pan, float& leftGain, float& rightGain)
{
    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (clamped + 1.0f) * 0.25f * 3.14159265358979323846f;
    leftGain = std::cos(angle);
    rightGain = std::sin(angle);
}

float panScaleAt(double distance)
{
    const double clamped = std::max(0.0, distance);
    return static_cast<float>(clamped / (clamped + kPanNearDistance));
}

SoundId SoundBank::load(const char* path)
{
    assets::SoundData data;
    if (!assets::loadSound(path, data) || data.samples.empty()) {
        return kNoSound;
    }

    SoundClip clip;
    clip.samples = std::move(data.samples);
    clip.sampleRate = data.sampleRate;
    clip.channelCount = data.channelCount;

    return add(std::move(clip));
}

SoundId SoundBank::add(SoundClip clip)
{
    if (clip.samples.empty() || clip.channelCount == 0 || clip.sampleRate == 0) {
        return kNoSound;
    }
    m_clips.push_back(std::move(clip));
    return static_cast<SoundId>(m_clips.size() - 1);
}

const SoundClip* SoundBank::clip(SoundId id) const
{
    return id < m_clips.size() ? &m_clips[id] : nullptr;
}

void Mixer::initialize(const SoundBank* bank, std::uint32_t outputSampleRate)
{
    m_bank = bank;
    m_outputSampleRate = outputSampleRate == 0 ? 48000 : outputSampleRate;
}

void Mixer::post(const Command& command)
{
    const std::size_t write = m_writeIndex.load(std::memory_order_relaxed);
    const std::size_t next = (write + 1) % kCommandCapacity;
    if (next == m_readIndex.load(std::memory_order_acquire)) {
        // Full. Dropping is right for audio: the alternative is stalling the
        // frame loop on the device thread, which trades a lost cue for a hitch.
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_commands[write] = command;
    m_writeIndex.store(next, std::memory_order_release);
}

void Mixer::play(const PlayParams& params)
{
    if (params.sound == kNoSound) {
        return;
    }
    Command command;
    command.kind = CommandKind::Play;
    command.play = params;
    post(command);
}

void Mixer::setListener(const Listener& listener)
{
    Command command;
    command.kind = CommandKind::Listener;
    command.listener = listener;
    post(command);
}

void Mixer::setGains(float master, float effects)
{
    Command command;
    command.kind = CommandKind::Gains;
    command.master = std::clamp(master, 0.0f, 1.0f);
    command.effects = std::clamp(effects, 0.0f, 1.0f);
    post(command);
}

void Mixer::setEngineLoop(SoundId sound, float gain, float pitch)
{
    Command command;
    command.kind = CommandKind::EngineLoop;
    command.loopSound = sound;
    command.loopGain = std::max(0.0f, gain);
    command.loopPitch = std::clamp(pitch, 0.25f, 4.0f);
    post(command);
}

void Mixer::stopAll()
{
    Command command;
    command.kind = CommandKind::StopAll;
    post(command);
}

std::uint64_t Mixer::droppedCommands() const
{
    return m_dropped.load(std::memory_order_relaxed);
}

std::uint32_t Mixer::activeVoices() const
{
    return m_activeVoices.load(std::memory_order_relaxed);
}

std::uint64_t Mixer::stolenVoices() const
{
    return m_stolen.load(std::memory_order_relaxed);
}

std::size_t Mixer::claimVoice(float priority, SoundId sound, std::uint32_t maxInstances)
{
    // A cue that caps its own instances steals from itself first, so one
    // rattling gun cannot crowd out everything else in the mix.
    if (maxInstances > 0) {
        std::uint32_t instances = 0;
        std::size_t oldest = kMaxVoices;
        std::uint64_t oldestOrder = UINT64_MAX;
        for (std::size_t i = 0; i < kMaxVoices; ++i) {
            if (!m_voices[i].active || m_voices[i].sound != sound) {
                continue;
            }
            ++instances;
            if (m_voices[i].order < oldestOrder) {
                oldestOrder = m_voices[i].order;
                oldest = i;
            }
        }
        if (instances >= maxInstances && oldest < kMaxVoices) {
            m_stolen.fetch_add(1, std::memory_order_relaxed);
            return oldest;
        }
    }

    for (std::size_t i = 0; i < kMaxVoices; ++i) {
        if (!m_voices[i].active) {
            return i;
        }
    }

    // Everything is busy: the quietest voice loses, oldest breaking a tie.
    // Quietest rather than oldest outright, because the loudest voices are the
    // near ones, and dropping a near sound to keep a distant one is backwards.
    std::size_t quietest = kMaxVoices;
    float quietestPriority = priority;
    std::uint64_t quietestOrder = UINT64_MAX;
    for (std::size_t i = 0; i < kMaxVoices; ++i) {
        const Voice& voice = m_voices[i];
        if (voice.priority > quietestPriority) {
            continue;
        }
        if (voice.priority < quietestPriority ||
            (voice.priority == quietestPriority && voice.order < quietestOrder)) {
            quietestPriority = voice.priority;
            quietestOrder = voice.order;
            quietest = i;
        }
    }
    if (quietest < kMaxVoices) {
        m_stolen.fetch_add(1, std::memory_order_relaxed);
    }
    return quietest; // kMaxVoices means the incoming sound is the quietest
}

void Mixer::applyPlay(const PlayParams& params)
{
    if (m_bank == nullptr) {
        return;
    }
    const SoundClip* clip = m_bank->clip(params.sound);
    if (clip == nullptr || clip->frameCount() == 0) {
        return;
    }

    float gain = std::max(0.0f, params.gain);
    float left = 0.70710678f;
    float right = 0.70710678f;

    if (params.positional) {
        const core::DVec3 offset = params.position - m_listener.position;
        const double distance = core::length(offset);
        gain *= attenuationAt(distance, params.rolloffDistance);
        if (distance > 1e-6) {
            const core::DVec3 direction = offset * (1.0 / distance);
            const core::Vec3 rightAxis = core::rotate(m_listener.orientation, core::Vec3{1, 0, 0});
            const auto pan = static_cast<float>(direction.x * rightAxis.x + direction.y * rightAxis.y +
                                                direction.z * rightAxis.z);
            equalPowerPan(pan * panScaleAt(distance), left, right);
        }
    }

    if (gain < kAudibleGain) {
        return; // too far or too quiet to be worth a slot
    }

    const std::size_t slot = claimVoice(gain, params.sound, params.maxInstances);
    if (slot >= kMaxVoices) {
        return;
    }

    Voice& voice = m_voices[slot];
    voice.sound = params.sound;
    voice.cursor = 0.0;
    voice.step =
        (static_cast<double>(clip->sampleRate) / m_outputSampleRate) * std::clamp(params.pitch, 0.25f, 4.0f);
    voice.gainLeft = gain * left;
    voice.gainRight = gain * right;
    voice.priority = gain;
    voice.loop = false;
    voice.active = true;
    voice.order = m_nextOrder++;
}

void Mixer::drainCommands()
{
    std::size_t read = m_readIndex.load(std::memory_order_relaxed);
    while (read != m_writeIndex.load(std::memory_order_acquire)) {
        const Command& command = m_commands[read];
        switch (command.kind) {
        case CommandKind::Play:
            applyPlay(command.play);
            break;
        case CommandKind::Listener:
            m_listener = command.listener;
            break;
        case CommandKind::Gains:
            m_masterGain = command.master;
            m_effectsGain = command.effects;
            break;
        case CommandKind::EngineLoop:
            if (command.loopSound == kNoSound) {
                m_engineVoice.active = false;
                m_engineVoice.sound = kNoSound;
            } else {
                if (!m_engineVoice.active || m_engineVoice.sound != command.loopSound) {
                    m_engineVoice.sound = command.loopSound;
                    m_engineVoice.cursor = 0.0;
                    m_engineVoice.active = true;
                    m_engineVoice.loop = true;
                    m_engineVoice.order = m_nextOrder++;
                }
                const SoundClip* clip = m_bank == nullptr ? nullptr : m_bank->clip(command.loopSound);
                const double rate = clip == nullptr ? m_outputSampleRate : clip->sampleRate;
                m_engineVoice.step = (rate / m_outputSampleRate) * command.loopPitch;
                m_engineVoice.gainLeft = command.loopGain * 0.70710678f;
                m_engineVoice.gainRight = command.loopGain * 0.70710678f;
                m_engineVoice.priority = command.loopGain;
            }
            break;
        case CommandKind::StopAll:
            for (Voice& voice : m_voices) {
                voice.active = false;
            }
            m_engineVoice.active = false;
            m_engineVoice.sound = kNoSound;
            break;
        }
        read = (read + 1) % kCommandCapacity;
    }
    m_readIndex.store(read, std::memory_order_release);
}

void Mixer::mixVoice(Voice& voice, float* out, std::uint32_t frameCount)
{
    const SoundClip* clip = m_bank->clip(voice.sound);
    if (clip == nullptr || clip->frameCount() == 0) {
        voice.active = false;
        return;
    }
    const auto frames = static_cast<double>(clip->frameCount());

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        if (voice.cursor >= frames) {
            if (!voice.loop) {
                voice.active = false;
                return;
            }
            // Wrapping rather than resetting keeps the fractional remainder,
            // so a loop played at a non-integer step does not drift.
            voice.cursor -= frames;
        }
        const float sample = sampleAt(*clip, voice.cursor);
        out[frame * 2 + 0] += sample * voice.gainLeft;
        out[frame * 2 + 1] += sample * voice.gainRight;
        voice.cursor += voice.step;
    }
}

void Mixer::renderAudio(float* out, std::uint32_t frameCount)
{
    const std::size_t sampleCount = static_cast<std::size_t>(frameCount) * platform::kAudioChannels;
    std::fill(out, out + sampleCount, 0.0f);

    drainCommands();

    if (m_bank == nullptr) {
        return;
    }

    std::uint32_t active = 0;
    for (Voice& voice : m_voices) {
        if (voice.active) {
            mixVoice(voice, out, frameCount);
            active += voice.active ? 1u : 0u;
        }
    }
    if (m_engineVoice.active) {
        mixVoice(m_engineVoice, out, frameCount);
        active += 1;
    }
    m_activeVoices.store(active, std::memory_order_relaxed);

    const float gain = m_masterGain * m_effectsGain;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        out[i] = std::clamp(out[i] * gain, -1.0f, 1.0f);
    }
}

} // namespace sol::audio
