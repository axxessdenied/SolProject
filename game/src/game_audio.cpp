#include "game_audio.hpp"

#include "asset_paths.hpp"

#include "sol/core/log.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace sol;

namespace {

const std::string kNoName;

} // namespace

bool GameAudio::initialize(const assets::DefDatabase& defs, std::span<const std::string> cookedSearchPath)
{
    m_cueTable.clear();
    m_bank.clear();

    // The bank is built before the device opens and never mutated while it is
    // running: the mixer thread reads these samples directly.
    for (const assets::SoundDef& def : defs.sounds()) {
        const std::string path = resolveAsset(cookedSearchPath, def.asset + ".saud");
        const audio::SoundId sound = path.empty() ? audio::kNoSound : m_bank.load(path.c_str());
        if (sound == audio::kNoSound) {
            SOL_LOG_WARN("audio: cue '%s' has no cooked asset '%s.saud' in any of: %s",
                         def.id.c_str(),
                         def.asset.c_str(),
                         describeSearchPath(cookedSearchPath).c_str());
            continue;
        }
        m_cueTable.push_back({.id = def.id,
                              .sound = sound,
                              .gain = def.gain,
                              .pitchJitter = def.pitchJitter,
                              .rolloff = def.rolloff,
                              .maxInstances = def.maxInstances});
    }
    resolveCueSet();

    m_mixer.initialize(&m_bank, 48000);
    if (!m_device.open(&m_mixer)) {
        SOL_LOG_WARN("audio: no output device; %zu cue(s) loaded but nothing will be heard",
                     m_cueTable.size());
        return false;
    }
    // The device decides the rate, so the mixer only learns it once the device
    // is up - re-initializing here is what keeps the step ratio honest.
    m_mixer.initialize(&m_bank, m_device.info().sampleRate);
    SOL_LOG_INFO("audio: %zu cue(s) loaded", m_cueTable.size());
    return true;
}

void GameAudio::shutdown()
{
    m_device.close();
    m_cueTable.clear();
    m_bank.clear();
    m_cues = CueSet{};
}

void GameAudio::reloadDefs(const assets::DefDatabase& defs)
{
    // Tuning only. A cue whose asset changed keeps the samples it was cooked
    // with, which is the documented limit of audio hot-reload this phase.
    for (Cue& cue : m_cueTable) {
        const assets::SoundDef* def = defs.findSound(cue.id.c_str());
        if (def == nullptr) {
            continue;
        }
        cue.gain = def->gain;
        cue.pitchJitter = def->pitchJitter;
        cue.rolloff = def->rolloff;
        cue.maxInstances = def->maxInstances;
    }
}

void GameAudio::resolveCueSet()
{
    // Through find() rather than by table index: a cue whose asset failed to
    // load is absent from both the bank and the table, and leaning on the two
    // staying in lockstep is the kind of assumption that breaks silently.
    m_cues.weaponFire = find("sol.weapon_fire");
    m_cues.hitShield = find("sol.hit_shield");
    m_cues.hitHull = find("sol.hit_hull");
    m_cues.explosion = find("sol.explosion");
    m_cues.miningCut = find("sol.mining_cut");
    m_cues.engineLoop = find("sol.engine_loop");
    m_cues.uiClick = find("sol.ui_click");
    m_cues.docking = find("sol.docking");
    m_cues.alarm = find("sol.alarm");
}

audio::SoundId GameAudio::find(const char* defId) const
{
    for (std::size_t i = 0; i < m_cueTable.size(); ++i) {
        if (m_cueTable[i].id == defId) {
            return m_cueTable[i].sound;
        }
    }
    return audio::kNoSound;
}

const std::string& GameAudio::cueName(audio::SoundId sound) const
{
    const Cue* cue = cueFor(sound);
    return cue == nullptr ? kNoName : cue->id;
}

const GameAudio::Cue* GameAudio::cueFor(audio::SoundId sound) const
{
    const auto it = std::find_if(
        m_cueTable.begin(), m_cueTable.end(), [&](const Cue& cue) { return cue.sound == sound; });
    return it == m_cueTable.end() ? nullptr : &*it;
}

float GameAudio::rollPitch(float jitter)
{
    if (jitter <= 0.0f) {
        return 1.0f;
    }
    return 1.0f + m_rng.rangeFloat(-jitter, jitter);
}

void GameAudio::playAt(audio::SoundId cue, const core::DVec3& position, std::uint32_t system)
{
    // Before the cue lookup and before any voice is claimed: a sound from
    // another system has no business spending an instance cap. See the header.
    if (system != m_listenerSystem) {
        ++m_outOfFrame;
        return;
    }
    const Cue* entry = cueFor(cue);
    if (entry == nullptr) {
        return;
    }
    audio::PlayParams params;
    params.sound = entry->sound;
    params.gain = entry->gain;
    params.pitch = rollPitch(entry->pitchJitter);
    params.positional = true;
    params.position = position;
    params.rolloffDistance = entry->rolloff;
    params.maxInstances = entry->maxInstances;
    m_mixer.play(params);
    ++m_played;
}

void GameAudio::play2D(audio::SoundId cue)
{
    const Cue* entry = cueFor(cue);
    if (entry == nullptr) {
        return;
    }
    audio::PlayParams params;
    params.sound = entry->sound;
    params.gain = entry->gain;
    params.pitch = rollPitch(entry->pitchJitter);
    params.positional = false;
    params.maxInstances = entry->maxInstances;
    m_mixer.play(params);
    ++m_played;
}

void GameAudio::setListener(const core::DVec3& position, const core::Quat& orientation)
{
    audio::Listener listener;
    listener.position = position;
    listener.orientation = orientation;
    m_mixer.setListener(listener);
}

void GameAudio::setVolumes(float master, float effects)
{
    // Called every frame from the loop so a slider drag is heard as it moves,
    // but only posted when it actually moved - the command ring is not a place
    // to put 144 identical messages a second.
    if (master == m_masterVolume && effects == m_effectsVolume) {
        return;
    }
    m_masterVolume = master;
    m_effectsVolume = effects;
    m_mixer.setGains(master, effects);
}

void GameAudio::setEngineThrottle(float throttle)
{
    const float clamped = std::clamp(throttle, 0.0f, 1.0f);
    // Only post when it actually moved: the loop is set every frame and the
    // command ring is not a place to put 144 identical messages a second.
    if (std::abs(clamped - m_engineThrottle) < 0.01f) {
        return;
    }
    m_engineThrottle = clamped;

    const Cue* entry = cueFor(m_cues.engineLoop);
    if (entry == nullptr) {
        return;
    }
    // Idle still hums; the throttle rides on top of that, and lifts the pitch
    // a little so a hard burn is heard as well as felt.
    const float gain = entry->gain * (0.35f + (0.65f * clamped));
    const float pitch = 0.9f + (0.25f * clamped);
    m_mixer.setEngineLoop(entry->sound, gain, pitch);
}

void GameAudio::stopAll()
{
    m_mixer.stopAll();
    m_engineThrottle = -1.0f; // the loop must be re-armed for the new system
}

} // namespace game
