#include "sound_preview.hpp"

#include "sol/assets/asset_loader.hpp"
#include "sol/core/log.hpp"

#include <utility>

namespace forge {

using namespace sol;

namespace {

const SoundReport kNoReport;

} // namespace

SoundPreview::~SoundPreview()
{
    shutdown();
}

void SoundPreview::rebuild(const std::vector<AssetEntry>& entries)
{
    // ⚑ First, and before anything touches the bank: this joins the device
    // thread, which is what makes every line below single-threaded again.
    m_device.close();
    m_mixer = std::make_unique<audio::Mixer>();
    m_bank.clear();
    m_sounds.clear();
    m_reports.clear();
    m_sounds.reserve(entries.size());
    m_reports.reserve(entries.size());

    std::size_t loaded = 0;
    for (const AssetEntry& entry : entries) {
        assets::SoundData data;
        std::string error;
        if (!loadSound(entry, data, &error)) {
            SOL_LOG_WARN("forge: %s", error.c_str());
            m_sounds.push_back(audio::kNoSound);
            m_reports.push_back(SoundReport{});
            continue;
        }
        m_reports.push_back(reportSound(data));
        // ⚑ Measured BEFORE the move: the samples belong to the bank
        // afterwards, and the panel's numbers must not depend on that.
        m_sounds.push_back(m_bank.add(audio::SoundClip{.samples = std::move(data.samples),
                                                       .sampleRate = data.sampleRate,
                                                       .channelCount = data.channelCount}));
        if (m_sounds.back() != audio::kNoSound) {
            ++loaded;
        }
    }

    // The device decides the rate, so the mixer is initialised twice: once to
    // have a bank at all before the thread starts pulling, and again with the
    // rate the endpoint actually mixes at. Exactly what `GameAudio` does, for
    // exactly the same reason.
    m_mixer->initialize(&m_bank, 48000);
    if (!m_device.open(m_mixer.get())) {
        // ⚑ NOT AN ERROR, AND THE TOOL MUST NOT DIE OF IT. A machine with no
        // output endpoint still authors meshes, textures and def rows; the
        // panel says the cue cannot be heard and every other surface in here
        // keeps working. Same rule the game has followed since Phase 8t.
        m_status = std::to_string(loaded) + " sound(s) loaded, no output device";
        SOL_LOG_WARN("forge: no audio output device; %zu sound(s) loaded but nothing will be heard", loaded);
        return;
    }
    m_mixer->initialize(&m_bank, m_device.info().sampleRate);
    const platform::AudioDeviceInfo info = m_device.info();
    m_status = std::to_string(loaded) + " sound(s), device at " + std::to_string(info.sampleRate) + " Hz";
    SOL_LOG_INFO("forge: audio %s", m_status.c_str());
}

void SoundPreview::shutdown()
{
    m_device.close();
    m_mixer.reset();
    m_bank.clear();
    m_sounds.clear();
    m_reports.clear();
    m_status.clear();
}

bool SoundPreview::canPlay(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_sounds.size()) {
        return false;
    }
    return m_sounds[static_cast<std::size_t>(index)] != audio::kNoSound;
}

void SoundPreview::play(int index, float gain, float pitchJitter, std::uint32_t maxInstances)
{
    if (!canPlay(index) || m_mixer == nullptr) {
        return;
    }
    // `GameAudio::rollPitch`, one tool over: no jitter is exactly 1, never a
    // roll that happens to land there.
    const float pitch = pitchJitter <= 0.0f ? 1.0f : 1.0f + m_rng.rangeFloat(-pitchJitter, pitchJitter);
    m_mixer->play(audio::PlayParams{.sound = m_sounds[static_cast<std::size_t>(index)],
                                    .gain = gain,
                                    .pitch = pitch,
                                    .positional = false,
                                    .maxInstances = maxInstances});
}

void SoundPreview::stopAll()
{
    if (m_mixer != nullptr) {
        m_mixer->stopAll();
    }
}

std::uint32_t SoundPreview::activeVoices() const
{
    return m_mixer == nullptr ? 0 : m_mixer->activeVoices();
}

const sol::audio::SoundClip* SoundPreview::clip(int index) const
{
    if (!canPlay(index)) {
        return nullptr;
    }
    return m_bank.clip(m_sounds[static_cast<std::size_t>(index)]);
}

const SoundReport& SoundPreview::report(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_reports.size()) {
        return kNoReport;
    }
    return m_reports[static_cast<std::size_t>(index)];
}

} // namespace forge
