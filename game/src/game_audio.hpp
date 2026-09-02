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

#include <cstdint>
#include <span>
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
    // ⚑ `cookedSearchPath` is the layer-ordered list from `asset_paths.hpp`
    // (Phase 24 stage S), so a mod can replace a cue by shipping the same
    // stem. A cue whose asset is missing has warned and carried on since
    // Phase 8t, which is already the rule the model catalog only just learned.
    bool initialize(const sol::assets::DefDatabase& defs, std::span<const std::string> cookedSearchPath);
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

    // ⚑⚑⚑⚑ A CUE AT A PLACE IN THE WORLD, AND `system` IS WHICH WORLD (Phase
    // 38 stage D). The mixer has ONE listener and a `DVec3` is metres in one
    // system's barycentre frame, so a position without a frame beside it is
    // only meaningful while exactly one system exists - which stopped being
    // true when stage C left a system running behind the player. A cue from
    // any system but the ear's is dropped here.
    //
    // ⚑⚑⚑ AND WHAT IT COSTS TO GET WRONG IS NOT A NOISE, IT IS A SILENCE.
    // `sounds.toml` caps `sol.explosion` at 4 concurrent voices, the two hit
    // cues at 6 and `sol.weapon_fire` at 4, and `Mixer::claimVoice` STEALS the
    // oldest when a cue is at its cap. Those numbers were tuned in Phase 8t
    // against the only fight there could be. The mixer's `kAudibleGain` does
    // not save it either: at 0.0005 an explosion stays worth a voice out to
    // ~6,000 km of coordinate separation and a hull hit to ~1,280 km, while
    // two ships in DIFFERENT systems sit ~160 km apart in coordinates, because
    // both systems lay their contents around a barycentre origin. That is the
    // spec's own `pilotEngageEnemy` argument arriving in the mixer: the shots
    // of a fight the player cannot see would take the slots their own fight is
    // heard through.
    void playAt(sol::audio::SoundId cue, const sol::core::DVec3& position, std::uint32_t system);
    // A cue that belongs to the player: their own gun, the UI, an alarm. No
    // frame, because there is no position to be in one - it is played AT the
    // listener, so it is in the listener's by construction.
    void play2D(sol::audio::SoundId cue);

    void setListener(const sol::core::DVec3& position, const sol::core::Quat& orientation);

    // ⚑⚑ THE EAR'S FRAME, AND IT IS SET APART FROM THE EAR'S POSE ON PURPOSE.
    // The pose comes from the render loop, which runs AFTER `world.tick`; the
    // frame changes INSIDE `tick`, in the middle of a jump. Taking both from
    // the render loop would leave the ear a frame behind across exactly the
    // crossing the cooling bubble is about - the arrival's first tick silenced
    // and the departed system's played. So SpaceWorld sets this where it sets
    // its own frame (see `SpaceWorld::enterFrame`).
    void setListenerSystem(std::uint32_t system) { m_listenerSystem = system; }

    [[nodiscard]] std::uint32_t listenerSystem() const { return m_listenerSystem; }

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

    // Positional cues refused because they happened in a system the ear is not
    // in (Phase 38 stage D). Reported by `sol.audio`, and it is the only way
    // to see this working from outside: what the fix produces is a sound that
    // does not happen.
    [[nodiscard]] std::uint64_t outOfFrameCues() const { return m_outOfFrame; }

    // The gains last posted to the mixer. Reading them back is what proves a
    // slider reached the audio system rather than only the settings file; that
    // the mixer then applies them is what gainsScaleTheMix asserts.
    [[nodiscard]] float masterVolume() const { return m_masterVolume; }

    [[nodiscard]] float effectsVolume() const { return m_effectsVolume; }

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
    std::uint64_t m_outOfFrame = 0;
    // The system the ear is in. Starts where `SpaceWorld::spawn` opens its
    // first bubble, which is 0 until a galaxy exists.
    std::uint32_t m_listenerSystem = 0;
    float m_engineThrottle = -1.0f; // forces the first update through
    float m_masterVolume = -1.0f;   // ditto: the first setVolumes always posts
    float m_effectsVolume = -1.0f;
    sol::core::Rng m_rng{0x50FA'11EDull, 7};
};

} // namespace game
