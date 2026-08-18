#pragma once

// Software mixer (engine plan 2.10, Phase 8t). sol::audio is the renderer's
// acoustic sibling: it links sol::assets the way sol::renderer links
// sol::rhi + sol::assets - cooked data in, a device out - and the device
// itself belongs to the platform layer.
//
// Two threads meet here and only here. The game thread posts commands into a
// single-producer ring; the device thread drains that ring and mixes, inside
// renderAudio. Nothing else crosses, and the mixer never allocates, locks, or
// touches game state once it is running.
//
// A one-shot is positioned where it was fired and does NOT track its emitter.
// That is the simplification the phase's scope buys: tracking would give every
// voice an entity lifetime, and emitters outliving their entity is where audio
// systems stop being simple. Exactly one looping voice exists - the player's
// own engine - and it is addressed by a dedicated command rather than a
// handle, because there is only ever one of it.

#include "sol/assets/asset_loader.hpp"
#include "sol/core/math/math.hpp"
#include "sol/platform/audio.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace sol::audio {

using SoundId = std::uint32_t;
inline constexpr SoundId kNoSound = 0xffff'ffffu;

// Voices mixed at once. A bound is what stops one fireball's worth of debris
// from swallowing everything else.
inline constexpr std::size_t kMaxVoices = 32;

// Commands the game thread can post before the ring drops them. Sized for a
// frame's worth of the worst case, not for a queue.
inline constexpr std::size_t kCommandCapacity = 256;

// A voice below this gain is not worth a slot.
inline constexpr float kAudibleGain = 0.0005f;

struct SoundClip
{
    std::vector<std::int16_t> samples;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 1;

    [[nodiscard]] std::uint32_t frameCount() const
    {
        return channelCount == 0 ? 0
                                 : static_cast<std::uint32_t>(samples.size()) / channelCount;
    }
};

// Loaded before the device opens and not mutated while it is running: the
// mixer thread reads these samples directly, and hot-reloading the payload is
// out of scope for this phase (the defs that point at them do reload).
class SoundBank
{
public:
    // Loads a cooked .saud; returns kNoSound if it cannot be read.
    [[nodiscard]] SoundId load(const char* path);
    // Adds a clip already in memory - what the tests build waveforms with,
    // and the seam a generated cue would arrive through.
    [[nodiscard]] SoundId add(SoundClip clip);
    [[nodiscard]] const SoundClip* clip(SoundId id) const;
    [[nodiscard]] std::size_t size() const { return m_clips.size(); }
    void clear() { m_clips.clear(); }

private:
    std::vector<SoundClip> m_clips;
};

struct Listener
{
    core::DVec3 position;
    core::Quat orientation; // the ear's frame; +X is right, which is what pans
};

struct PlayParams
{
    SoundId sound = kNoSound;
    float gain = 1.0f;
    float pitch = 1.0f; // playback rate multiplier, after the rate conversion
    bool positional = false;
    core::DVec3 position;              // sim space, meters; ignored when 2D
    float rolloffDistance = 500.0f;    // meters at which gain has halved
    std::uint32_t maxInstances = 0;    // 0 = unlimited
};

class Mixer final : public platform::AudioRenderer
{
public:
    // outputSampleRate is the device's; sources are stepped through at
    // whatever ratio their own rate implies, which is the same mechanism that
    // applies pitch.
    void initialize(const SoundBank* bank, std::uint32_t outputSampleRate);

    // --- game thread ---
    void play(const PlayParams& params);
    void setListener(const Listener& listener);
    void setGains(float master, float effects);
    // The single looping voice. gain 0 leaves it running and silent, so the
    // loop never restarts mid-flight; sound == kNoSound stops it outright.
    void setEngineLoop(SoundId sound, float gain, float pitch);
    // System change: everything in flight belonged to the old system.
    void stopAll();

    // Commands posted that the ring had no room for. Nonzero means the game
    // thread is outrunning the device, which is a bug worth seeing.
    [[nodiscard]] std::uint64_t droppedCommands() const;
    [[nodiscard]] std::uint32_t activeVoices() const;
    [[nodiscard]] std::uint64_t stolenVoices() const;

    // --- device thread ---
    void renderAudio(float* out, std::uint32_t frameCount) override;

private:
    enum class CommandKind : std::uint32_t
    {
        Play = 0,
        Listener,
        Gains,
        EngineLoop,
        StopAll,
    };

    struct Command
    {
        CommandKind kind = CommandKind::StopAll;
        PlayParams play;
        Listener listener;
        float master = 1.0f;
        float effects = 1.0f;
        SoundId loopSound = kNoSound;
        float loopGain = 0.0f;
        float loopPitch = 1.0f;
    };

    struct Voice
    {
        SoundId sound = kNoSound;
        double cursor = 0.0; // fractional frame index into the clip
        double step = 1.0;   // source frames advanced per output frame
        float gainLeft = 0.0f;
        float gainRight = 0.0f;
        float priority = 0.0f; // pre-pan gain, what stealing compares
        bool loop = false;
        bool active = false;
        std::uint64_t order = 0; // start sequence, for age comparisons
    };

    void post(const Command& command);
    void drainCommands();
    void applyPlay(const PlayParams& params);
    void mixVoice(Voice& voice, float* out, std::uint32_t frameCount);
    // Slot for a new voice, stealing the quietest if every slot is busy.
    // Returns kMaxVoices when the incoming sound loses that comparison.
    [[nodiscard]] std::size_t claimVoice(float priority, SoundId sound,
                                         std::uint32_t maxInstances);

    const SoundBank* m_bank = nullptr;
    std::uint32_t m_outputSampleRate = 48000;

    Command m_commands[kCommandCapacity];
    std::atomic<std::size_t> m_writeIndex{0};
    std::atomic<std::size_t> m_readIndex{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint32_t> m_activeVoices{0};
    std::atomic<std::uint64_t> m_stolen{0};

    // Device-thread state; nothing outside renderAudio touches these.
    Voice m_voices[kMaxVoices];
    Voice m_engineVoice;
    Listener m_listener;
    float m_masterGain = 1.0f;
    float m_effectsGain = 1.0f;
    std::uint64_t m_nextOrder = 1;
};

// Distance attenuation: 1 at the listener, 0.5 at rolloff, never quite zero.
// Inverse-distance rather than a linear ramp, because a linear ramp makes
// everything the same loudness until it abruptly is not.
[[nodiscard]] float attenuationAt(double distance, float rolloffDistance);

// Equal-power pan from -1 (hard left) to 1 (hard right): both channels sit at
// ~0.707 in the middle, so a source sweeping past does not get louder as it
// crosses the centre.
void equalPowerPan(float pan, float& leftGain, float& rightGain);

// How hard to pan a source at this distance. A sound a few meters away is
// effectively all around the listener, so panning collapses toward centre as
// it approaches - otherwise an explosion at arm's length is 100% one ear.
[[nodiscard]] float panScaleAt(double distance);

} // namespace sol::audio
