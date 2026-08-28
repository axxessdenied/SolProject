#pragma once

// Auditioning a cue inside the Forge (engine plan Phase 24 stage U1) - the
// third asset kind to become audible-or-visible in this tool, after the mesh
// viewport and the texture preview.
//
// ⚑⚑ IT LINKS `sol::audio` AND DELIBERATELY NOT `GameAudio`, WHICH IS THE WHOLE
// OF THE ARCHITECTURAL DECISION HERE. `GameAudio` is game POLICY: a listener
// that follows a ship, a single looping engine voice, a cue set resolved by
// hard-coded id, volumes read from the player's settings file. A tool wants
// none of it - it wants one clip, played once, at the gain the `[[sound]]` row
// declares. So this reaches for the same two engine pieces `GameAudio` reaches
// for (`audio::Mixer` over `platform::AudioDevice`) and stops there. That is
// AGENTS.md 4's line arriving at the audio stack: the tool reads the game's
// DATA and never its CODE.
//
// ⚑⚑ AND IT DOES NOT APPLY THE PLAYER'S VOLUMES, WHICH IS A DECISION AND NOT AN
// OMISSION. The live settings file on this machine carries `effects_volume =
// 0.209`; a cue auditioned through that and then tuned until it sounded right
// would ship five times too loud for everybody else. The mixer's master and
// effects gains stay at 1, so what an author hears is exactly what the row
// says and nothing about who is sitting in front of it.

#include "mesh_library.hpp"

#include "sol/audio/mixer.hpp"
#include "sol/core/random.hpp"
#include "sol/platform/audio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace forge {

class SoundPreview
{
public:
    ~SoundPreview();

    SoundPreview() = default;
    SoundPreview(const SoundPreview&) = delete;
    SoundPreview& operator=(const SoundPreview&) = delete;

    // Loads every entry into the bank and (re)opens the device. Safe to call
    // again whenever the listing changes - a Rescan, a Cook, a drop imported.
    //
    // ⚑⚑ THE ORDER IS THE CONTRACT AND IT IS `mixer.hpp`'s OWN RULE: the bank is
    // filled BEFORE the device opens and never mutated while it is running,
    // because the mixer thread reads these samples directly and a `push_back`
    // that reallocates would pull the buffer out from under it. So a rebuild
    // CLOSES first - `AudioDevice::close` joins the device thread - and every
    // path in here relies on that having happened.
    void rebuild(const std::vector<AssetEntry>& entries);

    void shutdown();

    [[nodiscard]] bool deviceOpen() const { return m_device.isOpen(); }

    // One line for the panel: the device, or why there is no sound.
    [[nodiscard]] const std::string& status() const { return m_status; }

    // Whether this row of the last `rebuild` listing has samples behind it. A
    // file that would not decode is still LISTED - an author needs to see the
    // thing that is broken - it simply cannot be played.
    [[nodiscard]] bool canPlay(int index) const;

    // The cue as the row declares it: its gain, its jitter, its instance cap.
    //
    // ⚑ THE JITTER IS ROLLED HERE RATHER THAN PASSED IN, so the tool cannot
    // drift from `GameAudio::rollPitch` on the one thing an author is listening
    // FOR: repeated presses have to differ the way repeated shots do, and a
    // preview that played a fixed pitch would make the slider look inert.
    // ⚑ 2D at the listener, so `rolloff` has nothing to act on - the panel says
    // so rather than passing a number that would do nothing.
    void play(int index, float gain, float pitchJitter, std::uint32_t maxInstances);

    void stopAll();

    [[nodiscard]] std::uint32_t activeVoices() const;

    // The samples behind a row, or nullptr when it did not load.
    //
    // ⚑⚑ THE BANK IS ALREADY THE RIGHT PLACE TO ASK, WHICH IS WHY THE
    // WAVEFORM COSTS NO FILE READING. Every listed cue was decoded once by
    // `rebuild` and lives here until the next one; drawing a picture of it
    // needs the samples, not the file. ⚑ Handing out a pointer into the bank
    // is safe for exactly the reason `mixer.hpp` states about the mixer
    // thread: the bank is filled before the device opens and never mutated
    // while it runs, and a rebuild throws the whole thing away rather than
    // editing it. A caller must not hold this across a `rebuild`, and no
    // caller does - the panel asks once a frame.
    [[nodiscard]] const sol::audio::SoundClip* clip(int index) const;

    // What `reportSound` measured while the bank was being filled, so the panel
    // does not re-read the file to print a duration. Zeroed for a row that did
    // not load.
    [[nodiscard]] const SoundReport& report(int index) const;

private:
    // ⚑⚑ BY POINTER SO A REBUILD CAN THROW THE WHOLE MIXER AWAY, AND THAT IS
    // NOT A STYLE CHOICE - COLLAPSING IT BACK INTO A MEMBER REINTRODUCES A BUG.
    // A `Mixer` carries device-thread state that outlives a `close()`: voices
    // mid-clip with a cursor and a `SoundId`, plus whatever is still unread in
    // the command ring. Reopening onto a REBUILT bank would resume those voices
    // against clips that have moved or ceased to exist, and the first buffer
    // out of the new device would be a burst of the wrong sound. A fresh mixer
    // makes that state inexpressible instead of requiring it to be cleared,
    // and a rebuild is a button press rather than a frame cost.
    std::unique_ptr<sol::audio::Mixer> m_mixer;
    sol::audio::SoundBank m_bank;
    sol::platform::AudioDevice m_device;

    // Parallel to the listing `rebuild` was given: `kNoSound` for a row whose
    // file would not decode.
    std::vector<sol::audio::SoundId> m_sounds;
    std::vector<SoundReport> m_reports;
    std::string m_status;
    sol::core::Rng m_rng{0xF012'5EEDull, 11};
};

} // namespace forge
