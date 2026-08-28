#pragma once

// The authored source of a sound: an ordered list of synthesis ops in TOML,
// evaluated to the int16 samples a `.saud` holds (engine plan Phase 26 stage A).
//
// ⚑ This exists for the third time and the reason has not changed once. The
// meshes in this repo were procedural, drawn by `tools/scripts/gen_assets.ps1`
// and committed as base64 glTF, until Phase 9 stage D made them `.forge`. The
// textures were procedural, drawn by the same script with System.Drawing and
// committed as PNG, until stage G made them `.tex`. The nine sounds are
// procedural, synthesised by the same script and committed as `.wav` - and the
// script's own header has named itself the last holdout ever since: "the
// System.Drawing dependency left with them; only audio needs this script."
// What that arrangement costs is the same list every time: the source is opaque
// binary, an edit is undiffable, the Forge cannot open it, and generating one
// needs Windows with .NET present.
//
// ⚑⚑ FIVE OPS, WHICH ARE THE FIVE THAT SCRIPT ACTUALLY USED AND NO MORE. The
// same rule `texture_doc.hpp` states for its own five: the vocabulary grows when
// an asset needs it, not when an op seems useful. `LoopNoise` is the sharp case
// - exactly one cue uses it - and it is in because `engine_loop` is the game's
// only looping voice and cannot be built without it.
//
// ⚑⚑⚑ WHAT THIS FORMAT DOES *NOT* PROMISE, SAID HERE BECAUSE IT IS THE ONE
// PLACE SOMEBODY WILL LOOK. A `.snd` is not a re-implementation of the
// PowerShell: `Add-NoiseBurst` and `Add-LoopNoise` drew from .NET's
// `System.Random`, whose "subtractive generator has no C++ twin" - the sentence
// `texture_doc.hpp` already wrote when it faced the same wall and escaped it by
// storing row lists as DATA. A sound cannot escape that way, because 1.3 seconds
// of noise is 57,330 samples, which is not a document. So the noise here comes
// from `core::Rng` (PCG32, integer arithmetic, identical on every compiler) and
// the seven noise-bearing cues are DIFFERENT NOISE, deliberately, with the
// user's ruling recorded at Phase 26 decision 2. The three analytic ops - Tone,
// Fade, Peak - are transcribed exactly, and the two cues built from those alone
// are what proves it.

#include "sol/assets/asset_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sol::assets {

enum class SoundOp
{
    // Exponentially decaying sine sweeping f0 -> f1, with an optional third
    // harmonic to square it off. Every pitched layer in the game is one of
    // these; `alarm` is six of them end to end.
    Tone,
    // Decaying white noise through a one-pole lowpass whose cutoff may sweep.
    // The percussive half of every impact, and the whole of `explosion`.
    NoiseBurst,
    // Noise that survives being looped: a tail is generated past the end and
    // crossfaded back over the head, so sample 0 continues from the last one.
    //
    // ⚑ Used by exactly ONE asset (`engine_loop`, the only looping voice in the
    // game) and in the vocabulary for that reason rather than for symmetry with
    // NoiseBurst. Phase 26 decision 7: an op names its consumer.
    LoopNoise,
    // Linear ramps at the head and tail, in seconds. What stops a cue clicking
    // when it starts and stops.
    Fade,
    // Scales the whole buffer so its loudest sample sits at `level`.
    //
    // ⚑ NOT the same thing as the `[[sound]]` row's `gain`, and the difference
    // is worth keeping straight because the Forge's Sound panel prints both.
    // This is the shape of the recording; `gain` is how loudly the game fires
    // it. `mesh_library.hpp` already calls `peak` "the number `gain` is a
    // number ABOUT" - this op is what sets it.
    Peak,
};

[[nodiscard]] const char* soundOpName(SoundOp op);
[[nodiscard]] bool soundOpFromName(const char* name, SoundOp& out);
// Every op, in the order an editor should offer them.
[[nodiscard]] std::span<const SoundOp> soundOps();

enum class SoundParamKind
{
    // Seconds, hertz, gains, decay rates. Always written with a decimal point.
    Number,
    // A noise seed. An integer, and written as one.
    //
    // ⚑⚑ TWO KINDS RATHER THAN ONE, AND THE REASON IS A GOTCHA THIS PROJECT HAS
    // ALREADY PAID FOR ONCE: "a 'write a number' helper that always emits a
    // float cannot write an integer key". The inverse is equally true here - a
    // `decay = 22.0` written back as `22` is a float key silently demoted to an
    // integer, which reads as a reformat in the diff of a file that promised
    // not to reformat. The schema decides which, so neither is expressible.
    Seed,
};

// One parameter value; the schema decides which member is live.
struct SoundValue
{
    double number = 0.0;
    std::int64_t integer = 0;
};

struct SoundParamSpec
{
    const char* name = "";
    SoundParamKind kind = SoundParamKind::Number;
    SoundValue defaultValue;
};

// ⚑ The single source of truth for what an op takes: the parser validates
// against it, the writer omits anything still at its default, and the tool's
// panel is generated from it - so adding a parameter is one table entry rather
// than three edits that can disagree. Same arrangement as `textureParams` and
// `forgeParams`, and for the same reason.
[[nodiscard]] std::span<const SoundParamSpec> soundParams(SoundOp op);

struct SoundLayer
{
    SoundOp op = SoundOp::Tone;
    std::vector<std::pair<std::string, SoundValue>> params;

    // The comments and blank lines that stood above this op's `[[op]]` line,
    // kept verbatim and written back in front of it. Same trivia model as
    // `TextureLayer::leading` and `ForgePart::leading`, load-bearing for the
    // same reason: a tool that saves on every edit must not reformat a
    // committed file.
    std::string leading;

    [[nodiscard]] const SoundValue* find(const char* name) const;
    // The authored value if present, the schema default otherwise.
    [[nodiscard]] SoundValue value(const char* name) const;
    void set(const char* name, const SoundValue& value);
};

struct SoundDoc
{
    // Optional and purely a label. The identity of a sound is its file stem,
    // which is what a `[[sound]]` row's `asset` names.
    std::string name;
    // The buffer's length. Every op's own `start` and `duration` are clipped to
    // it, so this is the cue's length and not a hint.
    double seconds = 0.0;
    std::uint32_t sampleRate = 44100;
    // ⚑⚑ A DOCUMENT KEY THAT CAN CURRENTLY SAY ONLY ONE THING, AND IT IS STILL
    // WORTH HAVING (Phase 26 decision 6, the user's ruling plus the check that
    // followed it). `audio::Mixer` folds every clip to mono in `sampleClip` -
    // it sums the channels, divides by the stride, and gives a `Voice` two gain
    // scalars over that one sample. There is no per-channel path anywhere, not
    // even for the 2D cues. So a stereo `.snd` would be authored, cooked,
    // loaded and then silently downmixed, and `buildSound` REFUSES it by name
    // instead. A key that says "1" out loud is what lets the limitation be
    // stated; without it there is nowhere to write this down.
    std::uint32_t channels = 1;
    std::vector<SoundLayer> layers;

    std::string header;
    std::string trailer;
    // A comment this model cannot place: after a value on the same line, or
    // inside a multi-line array. The tool says so rather than dropping it.
    bool hasUnplaceableComments = false;
};

// Parses a `.snd` document. On failure returns false and sets `error`.
[[nodiscard]] bool parseSound(const char* text,
                              std::size_t length,
                              const char* sourceName,
                              SoundDoc& out,
                              std::string* error = nullptr);

// Serialises back to TOML.
//
// ⚑⚑ THE ROUND-TRIP GUARANTEE IS WEAKER THAN `writeTexture`'s BY EXACTLY ONE
// STEP, AND PRETENDING OTHERWISE WOULD BE THE LIE. A `.tex` comes back byte for
// byte unconditionally because every value in it is an integer, and an integer
// has one spelling. A `.snd` carries doubles, and `2.20` and `2.2` are the same
// number written two ways - so what is promised here is: **every file this
// writer produced comes back byte for byte**, because numbers are emitted in
// the shortest form that round-trips and re-reading one yields the identical
// spelling. A hand-written `0.220` becomes `0.22` on the first save. Since this
// phase writes all nine committed cues, the corpus has the same practical
// guarantee `.tex` has, and `assets.unit` asserts it over the real files.
[[nodiscard]] std::string writeSound(const SoundDoc& doc);

// Evaluates the ops in file order into the exact struct a `.saud` serialises -
// `cooker::encodeSound` writes this and `assets::loadSound` reads it back, so a
// `.snd` reaching the game passes through no other representation.
//
// ⚑⚑⚑ FILE ORDER IS THE SEMANTICS AND NOT A DETAIL. Every committed cue runs
// its layers, then `peak`, then `fade` - normalise the mix, then shape the ends
// - and reversing those last two silently changes the result, because `peak`
// scales by the loudest sample it can see and a faded buffer has quieter ends.
// The PowerShell had the order baked into the call sequence; here it is the
// order of the `[[op]]` blocks, which is what makes it editable.
//
// ⚑⚑ AND THE CONVERSION AT THE END IS ROUND-HALF-TO-EVEN, WHICH IS NOT WHAT
// `std::round` DOES. `Write-Wav` used `[math]::Round`, and .NET's default is
// `MidpointRounding.ToEven`; `std::round` is half-away-from-zero. `std::nearbyint`
// under the default FE_TONEAREST is the match. The same applies to every
// seconds-to-samples conversion in here, because PowerShell's `[int]` cast is
// `Convert.ToInt32` and therefore ALSO rounds rather than truncating -
// measured, not assumed: `[int]2.7` is 3 and `[int]3.5` is 4.
[[nodiscard]] bool buildSound(const SoundDoc& doc, SoundData& out, std::string* error = nullptr);

// How many samples a layer actually writes, given the document's length. Zero
// means the op contributes nothing - a duration of zero, or a start entirely
// past the end of the buffer - which is the one thing about a sound source that
// is broken rather than merely unusual. The analogue of
// `textureLayerCoverage`, and the Forge's report line is its consumer.
[[nodiscard]] std::size_t soundLayerCoverage(const SoundDoc& doc, std::size_t layerIndex);

} // namespace sol::assets
