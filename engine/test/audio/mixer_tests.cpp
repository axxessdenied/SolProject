#include "sol/audio/mixer.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <vector>

using namespace sol;

namespace {

// A clip of one constant value, so every assertion about gain is arithmetic
// rather than a guess about a waveform.
audio::SoundClip constantClip(std::int16_t value, std::uint32_t frames, std::uint32_t sampleRate = 48000)
{
    audio::SoundClip clip;
    clip.sampleRate = sampleRate;
    clip.channelCount = 1;
    clip.samples.assign(frames, value);
    return clip;
}

// A ramp, for asserting that rate conversion actually walks the source.
audio::SoundClip rampClip(std::uint32_t frames, std::uint32_t sampleRate)
{
    audio::SoundClip clip;
    clip.sampleRate = sampleRate;
    clip.channelCount = 1;
    clip.samples.resize(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        clip.samples[i] = static_cast<std::int16_t>(i);
    }
    return clip;
}

float peakOf(const std::vector<float>& buffer, std::uint32_t channel)
{
    float peak = 0.0f;
    for (std::size_t i = channel; i < buffer.size(); i += 2) {
        peak = std::max(peak, std::abs(buffer[i]));
    }
    return peak;
}

audio::Listener originListener()
{
    audio::Listener listener;
    listener.position = core::DVec3{0.0, 0.0, 0.0};
    listener.orientation = core::Quat{0.0f, 0.0f, 0.0f, 1.0f};
    return listener;
}

bool nearly(float a, float b, float epsilon = 1e-4f)
{
    return std::abs(a - b) <= epsilon;
}

} // namespace

SOL_TEST(attenuationHalvesAtTheRolloffDistance)
{
    SOL_CHECK(nearly(audio::attenuationAt(0.0, 500.0f), 1.0f));
    SOL_CHECK(nearly(audio::attenuationAt(500.0, 500.0f), 0.5f));
    SOL_CHECK(nearly(audio::attenuationAt(1500.0, 500.0f), 0.25f));

    // Monotonic, and never actually reaches zero - a hard cutoff is what
    // makes distant sounds pop in and out.
    float previous = 2.0f;
    for (double distance = 0.0; distance < 100000.0; distance += 250.0) {
        const float gain = audio::attenuationAt(distance, 500.0f);
        SOL_CHECK(gain < previous);
        SOL_CHECK(gain > 0.0f);
        previous = gain;
    }
}

SOL_TEST(panIsEqualPowerAcrossTheSweep)
{
    float left = 0.0f;
    float right = 0.0f;

    audio::equalPowerPan(0.0f, left, right);
    SOL_CHECK(nearly(left, 0.70710678f));
    SOL_CHECK(nearly(right, 0.70710678f));

    audio::equalPowerPan(-1.0f, left, right);
    SOL_CHECK(nearly(left, 1.0f));
    SOL_CHECK(nearly(right, 0.0f));

    audio::equalPowerPan(1.0f, left, right);
    SOL_CHECK(nearly(left, 0.0f));
    SOL_CHECK(nearly(right, 1.0f));

    // Constant power is the whole point: a source sweeping past the listener
    // must not get louder as it crosses the centre.
    for (float pan = -1.0f; pan <= 1.0f; pan += 0.05f) {
        audio::equalPowerPan(pan, left, right);
        SOL_CHECK(nearly((left * left) + (right * right), 1.0f, 1e-3f));
    }

    // Out of range clamps rather than wrapping.
    audio::equalPowerPan(-9.0f, left, right);
    SOL_CHECK(nearly(left, 1.0f));
}

SOL_TEST(panCollapsesTowardCentreUpClose)
{
    SOL_CHECK(nearly(audio::panScaleAt(0.0), 0.0f));
    SOL_CHECK(audio::panScaleAt(25.0) > 0.4f && audio::panScaleAt(25.0) < 0.6f);
    SOL_CHECK(audio::panScaleAt(10000.0) > 0.99f);

    float previous = -1.0f;
    for (double distance = 0.0; distance < 1000.0; distance += 10.0) {
        const float scale = audio::panScaleAt(distance);
        SOL_CHECK(scale >= previous);
        previous = scale;
    }
}

SOL_TEST(nearSourceIsLouderThanFarSource)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 256));
    SOL_REQUIRE(cue != audio::kNoSound);

    std::vector<float> buffer(128 * 2, 0.0f);

    auto peakAt = [&](double x) {
        audio::Mixer mixer;
        mixer.initialize(&bank, 48000);
        mixer.setListener(originListener());
        mixer.setGains(1.0f, 1.0f);

        audio::PlayParams params;
        params.sound = cue;
        params.positional = true;
        params.rolloffDistance = 500.0f;
        params.position = core::DVec3{x, 0.0, 0.0};
        mixer.play(params);

        mixer.renderAudio(buffer.data(), 128);
        return std::max(peakOf(buffer, 0), peakOf(buffer, 1));
    };

    const float near = peakAt(100.0);
    const float far = peakAt(5000.0);
    SOL_CHECK(near > 0.0f);
    SOL_CHECK(far > 0.0f);
    SOL_CHECK(near > far * 4.0f); // 500/600 vs 500/5500
}

SOL_TEST(positionalVoicePansToTheSideItIsOn)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 256));
    SOL_REQUIRE(cue != audio::kNoSound);

    std::vector<float> buffer(128 * 2, 0.0f);

    auto render = [&](double x, float& leftPeak, float& rightPeak) {
        audio::Mixer mixer;
        mixer.initialize(&bank, 48000);
        mixer.setListener(originListener());
        mixer.setGains(1.0f, 1.0f);

        audio::PlayParams params;
        params.sound = cue;
        params.positional = true;
        params.rolloffDistance = 5000.0f;
        params.position = core::DVec3{x, 0.0, 0.0};
        mixer.play(params);

        mixer.renderAudio(buffer.data(), 128);
        leftPeak = peakOf(buffer, 0);
        rightPeak = peakOf(buffer, 1);
    };

    // Identity orientation puts +X to the listener's right.
    float left = 0.0f;
    float right = 0.0f;
    render(2000.0, left, right);
    SOL_CHECK(right > left * 2.0f);

    render(-2000.0, left, right);
    SOL_CHECK(left > right * 2.0f);

    // Dead ahead (+Z with nothing on X) is centred.
    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setListener(originListener());
    mixer.setGains(1.0f, 1.0f);
    audio::PlayParams params;
    params.sound = cue;
    params.positional = true;
    params.rolloffDistance = 5000.0f;
    params.position = core::DVec3{0.0, 0.0, 2000.0};
    mixer.play(params);
    mixer.renderAudio(buffer.data(), 128);
    SOL_CHECK(nearly(peakOf(buffer, 0), peakOf(buffer, 1), 1e-3f));
}

SOL_TEST(twoDeeVoiceIgnoresPositionAndDistance)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 256));
    SOL_REQUIRE(cue != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setListener(originListener());
    mixer.setGains(1.0f, 1.0f);

    audio::PlayParams params;
    params.sound = cue;
    params.positional = false;
    params.position = core::DVec3{1.0e9, 0.0, 0.0}; // a UI click is not out there
    mixer.play(params);

    std::vector<float> buffer(64 * 2, 0.0f);
    mixer.renderAudio(buffer.data(), 64);
    SOL_CHECK(nearly(peakOf(buffer, 0), peakOf(buffer, 1), 1e-3f));
    SOL_CHECK(peakOf(buffer, 0) > 0.3f);
}

SOL_TEST(gainsScaleTheMix)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 256));
    SOL_REQUIRE(cue != audio::kNoSound);

    auto peakWithGains = [&](float master, float effects) {
        audio::Mixer mixer;
        mixer.initialize(&bank, 48000);
        mixer.setGains(master, effects);
        audio::PlayParams params;
        params.sound = cue;
        mixer.play(params);
        std::vector<float> buffer(64 * 2, 0.0f);
        mixer.renderAudio(buffer.data(), 64);
        return peakOf(buffer, 0);
    };

    const float full = peakWithGains(1.0f, 1.0f);
    SOL_CHECK(nearly(peakWithGains(0.5f, 1.0f), full * 0.5f, 1e-3f));
    SOL_CHECK(nearly(peakWithGains(1.0f, 0.5f), full * 0.5f, 1e-3f));
    SOL_CHECK(nearly(peakWithGains(0.0f, 1.0f), 0.0f));
}

SOL_TEST(oneShotEndsAndTheLoopDoesNot)
{
    audio::SoundBank bank;
    const audio::SoundId shot = bank.add(constantClip(16384, 8));
    const audio::SoundId loop = bank.add(constantClip(16384, 8));
    SOL_REQUIRE(shot != audio::kNoSound && loop != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setGains(1.0f, 1.0f);

    audio::PlayParams params;
    params.sound = shot;
    mixer.play(params);

    std::vector<float> buffer(8 * 2, 0.0f);
    mixer.renderAudio(buffer.data(), 8);
    SOL_CHECK(peakOf(buffer, 0) > 0.3f);

    // The clip is 8 frames long, so the next block is silence and the voice
    // has released its slot.
    mixer.renderAudio(buffer.data(), 8);
    SOL_CHECK(nearly(peakOf(buffer, 0), 0.0f));
    SOL_CHECK(mixer.activeVoices() == 0);

    // A 2D voice sits at the centre, so both channels carry the equal-power
    // 0.707 of a 0.5-amplitude clip at the requested gain.
    const float expectedLoopPeak = 0.5f * 0.70710678f * 0.8f;
    mixer.setEngineLoop(loop, 0.8f, 1.0f);
    for (int block = 0; block < 20; ++block) {
        mixer.renderAudio(buffer.data(), 8);
        // Still going twenty clip-lengths later, and at full amplitude - a
        // loop that wrapped badly would dip at the seam.
        SOL_CHECK(nearly(peakOf(buffer, 0), expectedLoopPeak, 1e-3f));
    }
    SOL_CHECK(mixer.activeVoices() == 1);

    // Gain 0 keeps the voice running silently rather than restarting it.
    mixer.setEngineLoop(loop, 0.0f, 1.0f);
    mixer.renderAudio(buffer.data(), 8);
    SOL_CHECK(nearly(peakOf(buffer, 0), 0.0f));
    SOL_CHECK(mixer.activeVoices() == 1);

    mixer.setEngineLoop(audio::kNoSound, 0.0f, 1.0f);
    mixer.renderAudio(buffer.data(), 8);
    SOL_CHECK(mixer.activeVoices() == 0);
}

SOL_TEST(stealingKeepsTheLoudVoicesAndDropsTheQuietIncoming)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 4096));
    SOL_REQUIRE(cue != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setGains(1.0f, 1.0f);
    std::vector<float> buffer(16 * 2, 0.0f);

    // Fill every slot with something quiet.
    for (std::size_t i = 0; i < audio::kMaxVoices; ++i) {
        audio::PlayParams params;
        params.sound = cue;
        params.gain = 0.01f;
        mixer.play(params);
    }
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() == audio::kMaxVoices);
    SOL_CHECK(mixer.stolenVoices() == 0);

    // A loud one arrives: it takes a slot from the quietest.
    audio::PlayParams loud;
    loud.sound = cue;
    loud.gain = 1.0f;
    mixer.play(loud);
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.stolenVoices() == 1);
    SOL_CHECK(mixer.activeVoices() == audio::kMaxVoices);
    SOL_CHECK(peakOf(buffer, 0) > 0.3f); // the loud one is what we hear

    // Now every slot is loud and a quiet one arrives: the incoming loses.
    for (std::size_t i = 0; i < audio::kMaxVoices; ++i) {
        audio::PlayParams params;
        params.sound = cue;
        params.gain = 1.0f;
        mixer.play(params);
    }
    mixer.renderAudio(buffer.data(), 16);
    const std::uint64_t stolenBefore = mixer.stolenVoices();

    audio::PlayParams quiet;
    quiet.sound = cue;
    quiet.gain = 0.001f;
    mixer.play(quiet);
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.stolenVoices() == stolenBefore); // nothing was displaced
}

SOL_TEST(maxInstancesCapsOneCue)
{
    audio::SoundBank bank;
    const audio::SoundId capped = bank.add(constantClip(16384, 4096));
    const audio::SoundId free = bank.add(constantClip(16384, 4096));
    SOL_REQUIRE(capped != audio::kNoSound && free != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setGains(1.0f, 1.0f);

    for (int i = 0; i < 10; ++i) {
        audio::PlayParams params;
        params.sound = capped;
        params.maxInstances = 2;
        mixer.play(params);
    }
    std::vector<float> buffer(16 * 2, 0.0f);
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() == 2);

    // The cap is per cue, not global.
    for (int i = 0; i < 3; ++i) {
        audio::PlayParams params;
        params.sound = free;
        mixer.play(params);
    }
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() == 5);
}

SOL_TEST(inaudibleVoiceNeverTakesASlot)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 4096));
    SOL_REQUIRE(cue != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setListener(originListener());
    mixer.setGains(1.0f, 1.0f);

    audio::PlayParams params;
    params.sound = cue;
    params.positional = true;
    params.rolloffDistance = 100.0f;
    params.position = core::DVec3{1.0e9, 0.0, 0.0}; // effectively another system
    mixer.play(params);

    std::vector<float> buffer(16 * 2, 0.0f);
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() == 0);
}

SOL_TEST(stopAllSilencesEverything)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 4096));
    SOL_REQUIRE(cue != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setGains(1.0f, 1.0f);

    for (int i = 0; i < 5; ++i) {
        audio::PlayParams params;
        params.sound = cue;
        mixer.play(params);
    }
    mixer.setEngineLoop(cue, 0.5f, 1.0f);

    std::vector<float> buffer(16 * 2, 0.0f);
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() == 6);

    mixer.stopAll();
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() == 0);
    SOL_CHECK(nearly(peakOf(buffer, 0), 0.0f));
}

SOL_TEST(sourceRateAndPitchShareOneStep)
{
    audio::SoundBank bank;
    // 96 frames at half the device rate: it should take 192 output frames.
    const audio::SoundId halfRate = bank.add(rampClip(96, 24000));
    SOL_REQUIRE(halfRate != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);
    mixer.setGains(1.0f, 1.0f);

    std::vector<float> buffer(16 * 2, 0.0f);

    // Count blocks that actually carry signal. A voice releases its slot on
    // the block AFTER its last sample - it only learns it is finished when
    // asked for one more frame - so counting slots would measure that
    // one-block latency rather than the clip's length.
    auto audibleBlocks = [&](float pitch) {
        audio::Mixer mixer;
        mixer.initialize(&bank, 48000);
        mixer.setGains(1.0f, 1.0f);

        audio::PlayParams params;
        params.sound = halfRate;
        params.pitch = pitch;
        mixer.play(params);

        int blocks = 0;
        for (int i = 0; i < 64; ++i) {
            mixer.renderAudio(buffer.data(), 16);
            if (peakOf(buffer, 0) > 0.0f) {
                ++blocks;
            }
        }
        return blocks;
    };

    // 96 source frames at half the device rate step at 0.5, so they stretch
    // to 192 output frames: twelve 16-frame blocks.
    SOL_CHECK(audibleBlocks(1.0f) == 12);
    // Pitch multiplies that same step - one mechanism, not two - so double
    // pitch halves the duration.
    SOL_CHECK(audibleBlocks(2.0f) == 6);
    SOL_CHECK(audibleBlocks(0.5f) == 24);
}

SOL_TEST(commandRingDropsRatherThanBlocking)
{
    audio::SoundBank bank;
    const audio::SoundId cue = bank.add(constantClip(16384, 64));
    SOL_REQUIRE(cue != audio::kNoSound);

    audio::Mixer mixer;
    mixer.initialize(&bank, 48000);

    // Post far more than the ring holds without ever rendering, which is the
    // frame loop outrunning a stalled device.
    for (std::size_t i = 0; i < audio::kCommandCapacity * 3; ++i) {
        audio::PlayParams params;
        params.sound = cue;
        mixer.play(params);
    }
    SOL_CHECK(mixer.droppedCommands() > 0);

    // And it still works afterwards: the ring drained, not corrupted.
    std::vector<float> buffer(16 * 2, 0.0f);
    mixer.renderAudio(buffer.data(), 16);
    SOL_CHECK(mixer.activeVoices() > 0);
}

SOL_TEST(mixerWithoutABankIsSilentNotBroken)
{
    audio::Mixer mixer;
    mixer.initialize(nullptr, 48000);

    audio::PlayParams params;
    params.sound = 0;
    mixer.play(params);
    mixer.setEngineLoop(0, 1.0f, 1.0f);

    std::vector<float> buffer(32 * 2, 1.0f);
    mixer.renderAudio(buffer.data(), 32);
    SOL_CHECK(nearly(peakOf(buffer, 0), 0.0f));
    SOL_CHECK(nearly(peakOf(buffer, 1), 0.0f));
}
