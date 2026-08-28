#pragma once

// Reducing a cue to something that fits in a panel (engine plan Phase 26
// stage C).
//
// ⚑⚑ THE SOUND PANEL HAS PRINTED NUMBERS *ABOUT* A WAVEFORM SINCE PHASE 24
// STAGE U1 AND HAS NEVER DRAWN ONE. It reports `seconds`, `frames` and `peak`,
// with `mesh_library.hpp` explaining that `peak` is there because it is "the
// number `gain` is a number ABOUT". All three are summaries of a shape nobody
// could see - and a cue that is right at 0.85 peak and a cue that clips for a
// tenth of a second at 0.85 peak print the identical line.
//
// ⚑⚑⚑ MIN/MAX PER COLUMN, NOT DECIMATION, AND THE REASON IS THE EXIT CRITERION
// ITSELF. `ui_click` is 2,205 frames and `explosion` is 57,330; drawn across a
// few hundred columns that is tens to hundreds of samples behind every pixel.
// Sampling one of them - every Nth frame - would miss transients entirely and
// draw a quieter cue than the one that plays, so the `peak` the panel prints
// would sit ABOVE the picture beside it and the panel would be contradicting
// itself. Keeping the extremes of each span makes "the peak is the top of the
// drawing" true by construction rather than usually.
//
// ⚑ It is a free function over raw samples rather than a method on anything,
// so `forge.unit` can check it with no device, no window and no mixer - the
// same reason `soundLayerCoverage` and `materialPipelineSlots` are shaped this
// way. A rule that lives in a panel is a rule with no test.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace forge {

// The vertical extent of one column, as a fraction of full scale in [-1, 1].
// A column covering silence is {0, 0}.
struct WaveformColumn
{
    float low = 0.0f;
    float high = 0.0f;
};

// One column per horizontal pixel the caller intends to draw.
//
// ⚑⚑ CHANNELS ARE FOLDED THE WAY `audio::Mixer::sampleClip` FOLDS THEM - summed
// and divided by the stride - rather than drawn as separate lanes or maxed
// across. This panel's standing promise is that what an author sees is what the
// game receives ("as cooked: imported exactly as the cooker would", and stage
// G's BC1 argument before it), and the mixer has no per-channel path at all: a
// stereo cue is folded one step before the speakers. Drawing two lanes would
// show an author a stereo image the game is going to throw away. ⚑ A `.snd` is
// mono by refusal anyway (`buildSound`), so this only ever bites an imported
// `.wav`, which is exactly the case where being honest is worth something.
//
// ⚑⚑ INTERLEAVED SAMPLES AND A CHANNEL COUNT RATHER THAN AN
// `assets::SoundData`, BECAUSE THE PANEL'S SAMPLES DO NOT LIVE IN ONE. The
// Sound panel already holds every cue as an `audio::SoundClip` inside
// `SoundPreview`'s bank - loaded once, on the rebuild the panel already
// does. Taking `SoundData` would have meant re-reading and re-parsing the
// file EVERY FRAME to draw a picture of something already in memory, or
// copying a clip into the other struct to satisfy a signature. The two
// structs carry the same three fields for the same reason and this function
// needs neither of them by name.
//
// Returns empty for no samples, no columns, or a zero channel count.
[[nodiscard]] std::vector<WaveformColumn>
waveformEnvelope(std::span<const std::int16_t> samples, std::uint32_t channelCount, std::size_t columns);

} // namespace forge
