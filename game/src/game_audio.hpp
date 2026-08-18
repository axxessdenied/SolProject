#pragma once

// The game's side of the audio stack (Phase 8t): owns the device, the bank
// and the mixer, resolves [[sound]] defs into voices, and is the only thing
// the rest of the game talks to.
//
// Shaped like CombatEffects deliberately - cosmetic, absent from the save,
// cleared on a system change - because a sound and a spark are the same kind
// of thing: feedback the world does not depend on.

#include "sol/assets/data_defs.hpp"
#include "sol/audio/mixer.hpp"
#include "sol/core/math/math.hpp"
#include "sol/core/random.hpp"
#include "sol/platform/audio.hpp"

#include <string>
#include <vector>

namespace game {

// Cues the engine itself fires. Resolved once at load so a hot path never
// looks a cue up by string; anything Lua fires goes through find() instead.
struct CueSet
{
    sol::audio::SoundId weaponFire = sol::audio::kNoSound;
    sol::audio::SoundId hitShield = sol::audio::kNoSound;
    sol::audio::SoundId hitHull = sol::audio::kNoSound;
    sol::audio::SoundId explosion = sol::audio::kNoSound;
    sol::audio::SoundId miningCut = sol::audio::kNoSound;
    sol::audio::SoundId engineLoop = sol::audio::kNoSound;
    sol::audio::SoundId uiClick = sol::audio::kNoSound;
    sol::audio::SoundId docking = sol::audio::kNoSound;
    sol::audio::SoundId alarm = sol::audio::kNoSound;
};

class GameAudio
{
public:
    // Loads every [[sound]] def's cooked asset and opens the device. Returns
    // false when there is no output endpoint, which the caller logs and
    // carries on from: a machine with no sound card still plays the game.
    bool initialize(const sol::assets::DefDatabase& defs, const std::string& cookedDirectory);
    void shutdown();

    // Re-reads gains, jitter, rolloff and caps from the defs after a script
    // reload. The cooked samples are NOT reloaded - retuning a cue is cheap,
    // recooking one is a build.
    void reloadDefs(const sol::assets::DefDatabase& defs);

    [[nodiscard]] bool deviceOpen() const { return m_device.isOpen(); }
    [[nodiscard]] const CueSet& cues() const { return m_cues; }
    // Def id -> cue, for sol.play_sound and the console.
    [[nodiscard]] sol::audio::SoundId find(const char* defId) const;
    [[nodiscard]] std::size_t cueCount() const { return m_cueTable.size(); }
    [[nodiscard]] const std::string& cueName(sol::audio::SoundId sound) const;

    // A cue at a place in the world: attenuated and panned against the ear.
    void playAt(sol::audio::SoundId cue, const sol::core::DVec3& position);
    // A cue that belongs to the player: their own gun, the UI, an alarm.
    void play2D(sol::audio::SoundId cue);

    void setListener(const sol::core::DVec3& position, const sol::core::Quat& orientation);
    void setVolumes(float master, float effects);
    // Throttle 0..1 drives the one looping voice; 0 leaves it running silent
    // rather than restarting it every time the player coasts.
    void setEngineThrottle(float throttle);
    void stopAll();

    // Console readout (sol.audio).
    [[nodiscard]] std::uint32_t activeVoices() const { return m_mixer.activeVoices(); }
    [[nodiscard]] std::uint64_t underruns() const { return m_device.underrunCount(); }
    [[nodiscard]] std::uint64_t droppedCommands() const { return m_mixer.droppedCommands(); }
    [[nodiscard]] std::uint64_t stolenVoices() const { return m_mixer.stolenVoices(); }
    [[nodiscard]] sol::platform::AudioDeviceInfo deviceInfo() const { return m_device.info(); }
    [[nodiscard]] std::uint64_t playedCues() const { return m_played; }

private:
    struct Cue
    {
        std::string id;
        sol::audio::SoundId sound = sol::audio::kNoSound;
        float gain = 1.0f;
        float pitchJitter = 0.0f;
        float rolloff = 500.0f;
        std::uint32_t maxInstances = 0;
    };

    [[nodiscard]] const Cue* cueFor(sol::audio::SoundId sound) const;
    [[nodiscard]] float rollPitch(float jitter);
    void resolveCueSet();

    sol::audio::SoundBank m_bank;
    sol::audio::Mixer m_mixer;
    sol::platform::AudioDevice m_device;

    std::vector<Cue> m_cueTable;
    CueSet m_cues;
    std::uint64_t m_played = 0;
    float m_engineThrottle = -1.0f; // forces the first update through
    float m_masterVolume = -1.0f;   // ditto: the first setVolumes always posts
    float m_effectsVolume = -1.0f;
    sol::core::Rng m_rng{0x50FA'11EDull, 7};
};

} // namespace game
