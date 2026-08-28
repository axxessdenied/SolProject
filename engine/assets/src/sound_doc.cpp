#include "sol/assets/sound_doc.hpp"

#include "doc_trivia.hpp"

#include "sol/core/random.hpp"
#include "sol/core/toml.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

namespace sol::assets {
namespace {

using core::TomlValue;

constexpr double kPi = 3.14159265358979323846;

struct OpName
{
    SoundOp op;
    const char* name;
};

constexpr std::array<OpName, 5> kOpNames = {{
    {SoundOp::Tone, "tone"},
    {SoundOp::NoiseBurst, "noise_burst"},
    {SoundOp::LoopNoise, "loop_noise"},
    {SoundOp::Fade, "fade"},
    {SoundOp::Peak, "peak"},
}};

[[nodiscard]] SoundValue numberValue(double v)
{
    SoundValue out;
    out.number = v;
    return out;
}

[[nodiscard]] SoundValue seedValue(std::int64_t v)
{
    SoundValue out;
    out.integer = v;
    return out;
}

// ⚑⚑⚑ ROUND-HALF-TO-EVEN, AND EVERY SECONDS-TO-SAMPLES CONVERSION IN THIS FILE
// GOES THROUGH IT. The PowerShell wrote `[int]($start * $audioRate)`, and
// PowerShell's `[int]` cast is `Convert.ToInt32`, which ROUNDS - measured
// rather than assumed: `[int]2.7` is 3 and `[int]3.5` is 4. A C-style cast
// truncates, and the difference is not theoretical here. Of the 36 conversions
// the nine committed cues perform, TWO disagree: `weapon_hit_shield`'s length
// (0.35 * 44100 is 15434.999999999998, so truncating loses a sample off the
// whole cue) and `ui_click`'s fade-out (352.8, which changes the ramp's
// DIVISOR and therefore every sample in it). ⚑ Neither is in the two cues the
// stage's exit criterion proves, so this comment is doing work the proof
// cannot: it was found by checking all 36, not by the test passing.
[[nodiscard]] std::int64_t toSamples(double seconds, double rate)
{
    return static_cast<std::int64_t>(std::nearbyint(seconds * rate));
}

// The spelling a person would have written, chosen to read back as the same
// double. FIXED notation first, then scientific only when fixed would be
// absurd; always carrying a decimal point so a float key cannot be demoted to
// an integer key by a save.
//
// ⚑⚑ THE SHORTEST SPELLING IS NOT THE RIGHT ONE, AND `ui_click` IS WHAT
// SAID SO. `std::to_chars` in its default shortest mode writes 0.0005 as
// "5e-04" - fewer characters, identical value, and a fade time no author would
// recognise in a file whose entire purpose is being edited by hand. The round
// trip was never in danger; the readability was. A document format optimising
// for bytes over the person reading it has forgotten what it is for.
[[nodiscard]] std::string formatNumber(double v)
{
    std::array<char, 64> buffer{};
    std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), v, std::chars_format::fixed);
    // ⚑ A number whose fixed form runs away - 1e300, or a value needing many
    // significant digits - falls back rather than writing a wall of zeroes.
    if (result.ec != std::errc{} || (result.ptr - buffer.data()) > 24) {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
        if (result.ec != std::errc{}) {
            return "0.0";
        }
    }
    std::string text(buffer.data(), result.ptr);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
        text += ".0";
    }
    return text;
}

// Collects an error with the source name and, once a layer is being read, which
// one - "explosion.snd: op 2: ..." says more than "explosion.snd: ..." about a
// file whose ops all look alike.
struct Reader
{
    const char* sourceName = "";
    std::string* error = nullptr;
    std::string context;

    void fail(const std::string& message) const
    {
        if (error != nullptr) {
            *error = (context.empty() ? std::string(sourceName) : context) + ": " + message;
        }
    }
};

// --- evaluation --------------------------------------------------------------

// One-pole lowpass coefficient for a cutoff in hertz, the same expression the
// PowerShell used.
[[nodiscard]] double onePoleCoefficient(double cutoff, double rate)
{
    return std::exp(-2.0 * kPi * cutoff / rate);
}

void applyTone(const SoundLayer& layer, std::vector<double>& buffer, double rate)
{
    const double start = layer.value("start").number;
    const double duration = layer.value("duration").number;
    const double f0 = layer.value("f0").number;
    const double f1 = layer.value("f1").number;
    const double decay = layer.value("decay").number;
    const double gain = layer.value("gain").number;
    const double harmonic = layer.value("harmonic").number;

    const std::int64_t first = toSamples(start, rate);
    const std::int64_t count = toSamples(duration, rate);
    if (count <= 0) {
        return;
    }

    double phase = 0.0;
    for (std::int64_t i = 0; i < count; ++i) {
        const std::int64_t index = first + i;
        // ⚑ `break`, not `continue`: the phase must not advance past the end of
        // the buffer, because a later op has no way to resume it. The
        // PowerShell broke here too and the sweep depends on it.
        if (index >= static_cast<std::int64_t>(buffer.size())) {
            break;
        }
        const double t = static_cast<double>(i) / rate;
        const double u = static_cast<double>(i) / static_cast<double>(count);
        const double f = f0 + ((f1 - f0) * u);
        phase += (2.0 * kPi * f / rate);
        const double env = std::exp(-t * decay);
        const double s = std::sin(phase) + (harmonic * std::sin(3.0 * phase));
        if (index >= 0) {
            buffer[static_cast<std::size_t>(index)] += (gain * env * s);
        }
    }
}

void applyNoiseBurst(const SoundLayer& layer, std::vector<double>& buffer, double rate)
{
    const double start = layer.value("start").number;
    const double duration = layer.value("duration").number;
    const double decay = layer.value("decay").number;
    const double gain = layer.value("gain").number;
    const double cutoff0 = layer.value("cutoff0").number;
    const double cutoff1 = layer.value("cutoff1").number;
    const std::int64_t seed = layer.value("seed").integer;

    const std::int64_t first = toSamples(start, rate);
    const std::int64_t count = toSamples(duration, rate);
    if (count <= 0) {
        return;
    }

    core::Rng rng(static_cast<std::uint64_t>(seed));
    double state = 0.0;
    for (std::int64_t i = 0; i < count; ++i) {
        const std::int64_t index = first + i;
        if (index >= static_cast<std::int64_t>(buffer.size())) {
            break;
        }
        const double t = static_cast<double>(i) / rate;
        const double u = static_cast<double>(i) / static_cast<double>(count);
        const double cutoff = cutoff0 + ((cutoff1 - cutoff0) * u);
        const double a = onePoleCoefficient(cutoff, rate);
        const double white = (rng.nextDouble01() * 2.0) - 1.0;
        state = (a * state) + ((1.0 - a) * white);
        const double env = std::exp(-t * decay);
        if (index >= 0) {
            buffer[static_cast<std::size_t>(index)] += (gain * env * state);
        }
    }
}

// ⚑⚑ THE CROSSFADE IS WHAT MAKES A LOOP A LOOP, AND IT IS WHY THIS IS AN OP
// RATHER THAN `noise_burst` WITH A ZERO DECAY. A tail is generated past the end
// of the buffer and mixed back over the head with a rising weight, so sample 0
// continues from the last sample instead of restarting the filter. Without it
// `engine_loop` ticks once per revolution of the buffer.
void applyLoopNoise(const SoundLayer& layer, std::vector<double>& buffer, double rate)
{
    const double gain = layer.value("gain").number;
    const double cutoff = layer.value("cutoff").number;
    const std::int64_t seed = layer.value("seed").integer;

    const std::size_t n = buffer.size();
    if (n == 0) {
        return;
    }
    const std::size_t fade = static_cast<std::size_t>(std::max<std::int64_t>(toSamples(0.25, rate), 0));

    core::Rng rng(static_cast<std::uint64_t>(seed));
    std::vector<double> scratch(n + fade, 0.0);
    const double a = onePoleCoefficient(cutoff, rate);
    double state = 0.0;
    for (std::size_t i = 0; i < scratch.size(); ++i) {
        const double white = (rng.nextDouble01() * 2.0) - 1.0;
        state = (a * state) + ((1.0 - a) * white);
        scratch[i] = state;
    }
    for (std::size_t i = 0; i < n; ++i) {
        double v = scratch[i];
        if (i < fade) {
            const double w = static_cast<double>(i) / static_cast<double>(fade);
            v = (scratch[i] * w) + (scratch[n + i] * (1.0 - w));
        }
        buffer[i] += (gain * v);
    }
}

void applyFade(const SoundLayer& layer, std::vector<double>& buffer, double rate)
{
    const std::int64_t inCount = toSamples(layer.value("in").number, rate);
    const std::int64_t outCount = toSamples(layer.value("out").number, rate);
    const auto size = static_cast<std::int64_t>(buffer.size());

    for (std::int64_t i = 0; i < inCount && i < size; ++i) {
        buffer[static_cast<std::size_t>(i)] *= (static_cast<double>(i) / static_cast<double>(inCount));
    }
    for (std::int64_t i = 0; i < outCount && i < size; ++i) {
        const std::int64_t index = size - 1 - i;
        buffer[static_cast<std::size_t>(index)] *= (static_cast<double>(i) / static_cast<double>(outCount));
    }
}

void applyPeak(const SoundLayer& layer, std::vector<double>& buffer)
{
    const double level = layer.value("level").number;
    double max = 0.0;
    for (const double v : buffer) {
        max = std::max(max, std::abs(v));
    }
    if (max <= 0.0) {
        return;
    }
    const double scale = level / max;
    for (double& v : buffer) {
        v *= scale;
    }
}

} // namespace

const char* soundOpName(SoundOp op)
{
    for (const OpName& row : kOpNames) {
        if (row.op == op) {
            return row.name;
        }
    }
    return "tone";
}

bool soundOpFromName(const char* name, SoundOp& out)
{
    if (name == nullptr) {
        return false;
    }
    for (const OpName& row : kOpNames) {
        if (std::string_view(name) == row.name) {
            out = row.op;
            return true;
        }
    }
    return false;
}

std::span<const SoundOp> soundOps()
{
    static const std::array<SoundOp, 5> kAll = {
        SoundOp::Tone, SoundOp::NoiseBurst, SoundOp::LoopNoise, SoundOp::Fade, SoundOp::Peak};
    return kAll;
}

std::span<const SoundParamSpec> soundParams(SoundOp op)
{
    static const std::vector<SoundParamSpec> kTone = {
        {"start", SoundParamKind::Number, numberValue(0.0)},
        {"duration", SoundParamKind::Number, numberValue(0.0)},
        {"f0", SoundParamKind::Number, numberValue(440.0)},
        {"f1", SoundParamKind::Number, numberValue(440.0)},
        {"decay", SoundParamKind::Number, numberValue(0.0)},
        {"gain", SoundParamKind::Number, numberValue(1.0)},
        // ⚑ Defaults to silence rather than to a value, because every cue that
        // does not say `harmonic` means a pure sine. `alarm` is the only file
        // that names it.
        {"harmonic", SoundParamKind::Number, numberValue(0.0)},
    };
    static const std::vector<SoundParamSpec> kNoiseBurst = {
        {"start", SoundParamKind::Number, numberValue(0.0)},
        {"duration", SoundParamKind::Number, numberValue(0.0)},
        {"decay", SoundParamKind::Number, numberValue(0.0)},
        {"gain", SoundParamKind::Number, numberValue(1.0)},
        {"cutoff0", SoundParamKind::Number, numberValue(8000.0)},
        {"cutoff1", SoundParamKind::Number, numberValue(8000.0)},
        {"seed", SoundParamKind::Seed, seedValue(0)},
    };
    static const std::vector<SoundParamSpec> kLoopNoise = {
        {"gain", SoundParamKind::Number, numberValue(1.0)},
        {"cutoff", SoundParamKind::Number, numberValue(8000.0)},
        {"seed", SoundParamKind::Seed, seedValue(0)},
    };
    static const std::vector<SoundParamSpec> kFade = {
        {"in", SoundParamKind::Number, numberValue(0.0)},
        {"out", SoundParamKind::Number, numberValue(0.0)},
    };
    static const std::vector<SoundParamSpec> kPeak = {
        {"level", SoundParamKind::Number, numberValue(1.0)},
    };

    switch (op) {
    case SoundOp::Tone:
        return kTone;
    case SoundOp::NoiseBurst:
        return kNoiseBurst;
    case SoundOp::LoopNoise:
        return kLoopNoise;
    case SoundOp::Fade:
        return kFade;
    case SoundOp::Peak:
        return kPeak;
    }
    return kTone;
}

const SoundValue* SoundLayer::find(const char* name) const
{
    for (const std::pair<std::string, SoundValue>& entry : params) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

SoundValue SoundLayer::value(const char* name) const
{
    if (const SoundValue* authored = find(name); authored != nullptr) {
        return *authored;
    }
    for (const SoundParamSpec& spec : soundParams(op)) {
        if (std::string_view(spec.name) == name) {
            return spec.defaultValue;
        }
    }
    return {};
}

void SoundLayer::set(const char* name, const SoundValue& value)
{
    for (std::pair<std::string, SoundValue>& entry : params) {
        if (entry.first == name) {
            entry.second = value;
            return;
        }
    }
    params.emplace_back(name, value);
}

bool parseSound(
    const char* text, std::size_t length, const char* sourceName, SoundDoc& out, std::string* error)
{
    TomlValue root;
    std::string tomlError;
    if (!TomlValue::parse(text, length, root, &tomlError)) {
        if (error != nullptr) {
            *error = std::string(sourceName) + ": " + tomlError;
        }
        return false;
    }

    Reader reader{sourceName, error, {}};
    SoundDoc doc;

    if (const TomlValue* name = root.find("name"); name != nullptr) {
        if (!name->isString()) {
            reader.fail("'name' must be a string");
            return false;
        }
        doc.name = name->asString();
    }

    const TomlValue* seconds = root.find("seconds");
    if (seconds == nullptr) {
        reader.fail("missing key 'seconds'");
        return false;
    }
    if (!seconds->isFloat() && !seconds->isInteger()) {
        reader.fail("'seconds' must be a number");
        return false;
    }
    doc.seconds = seconds->asFloat();
    // 120 s is far past anything this game fires and short of a typo that would
    // allocate gigabytes. The lower bound is a real limit rather than a
    // formality: a zero-length cue is a file with no product.
    if (!(doc.seconds > 0.0) || doc.seconds > 120.0) {
        reader.fail("'seconds' must be greater than 0 and at most 120");
        return false;
    }

    if (const TomlValue* rate = root.find("sample_rate"); rate != nullptr) {
        if (!rate->isInteger()) {
            reader.fail("'sample_rate' must be an integer");
            return false;
        }
        const std::int64_t value = rate->asInteger();
        if (value < 8000 || value > 192000) {
            reader.fail("'sample_rate' must be between 8000 and 192000");
            return false;
        }
        doc.sampleRate = static_cast<std::uint32_t>(value);
    }

    if (const TomlValue* channels = root.find("channels"); channels != nullptr) {
        if (!channels->isInteger()) {
            reader.fail("'channels' must be an integer");
            return false;
        }
        const std::int64_t value = channels->asInteger();
        if (value < 1 || value > 2) {
            reader.fail("'channels' must be 1 or 2");
            return false;
        }
        doc.channels = static_cast<std::uint32_t>(value);
    }

    const TomlValue* ops = root.find("op");
    if (ops != nullptr) {
        if (!ops->isArray()) {
            reader.fail("'op' must be an [[op]] array");
            return false;
        }
        for (std::size_t i = 0; i < ops->size(); ++i) {
            const TomlValue& table = (*ops)[i];
            reader.context = std::string(sourceName) + ": op " + std::to_string(i);
            if (!table.isTable()) {
                reader.fail("'op' must be an [[op]] array");
                return false;
            }

            const TomlValue* kind = table.find("kind");
            if (kind == nullptr || !kind->isString()) {
                reader.fail("missing key 'kind'");
                return false;
            }
            SoundLayer layer;
            if (!soundOpFromName(kind->asString().c_str(), layer.op)) {
                reader.fail("unknown op kind '" + kind->asString() + "'");
                return false;
            }

            const std::span<const SoundParamSpec> specs = soundParams(layer.op);
            for (const std::pair<std::string, TomlValue>& member : table.members()) {
                if (member.first == "kind") {
                    continue;
                }
                const SoundParamSpec* spec = nullptr;
                for (const SoundParamSpec& candidate : specs) {
                    if (member.first == candidate.name) {
                        spec = &candidate;
                        break;
                    }
                }
                if (spec == nullptr) {
                    reader.fail("'" + std::string(soundOpName(layer.op)) + "' has no parameter '" +
                                member.first + "'");
                    return false;
                }

                const TomlValue& value = member.second;
                SoundValue parsed;
                switch (spec->kind) {
                case SoundParamKind::Number:
                    // ⚑ An integer is accepted where a number is wanted, because
                    // `f0 = 988` is what an author writes for a frequency and
                    // refusing it would teach a rule the format does not need.
                    // The WRITER still emits `988.0`, so the spelling settles on
                    // the first save rather than drifting per key.
                    if (!value.isFloat() && !value.isInteger()) {
                        reader.fail("'" + member.first + "' must be a number");
                        return false;
                    }
                    parsed.number = value.asFloat();
                    if (!std::isfinite(parsed.number)) {
                        reader.fail("'" + member.first + "' must be finite");
                        return false;
                    }
                    break;
                case SoundParamKind::Seed:
                    if (!value.isInteger()) {
                        reader.fail("'" + member.first + "' must be an integer");
                        return false;
                    }
                    parsed.integer = value.asInteger();
                    break;
                }
                layer.set(member.first.c_str(), parsed);
            }

            doc.layers.push_back(std::move(layer));
        }
        reader.context.clear();
    }

    const doc::SourceTrivia trivia = doc::scanTrivia(text, length, "[[op]]");
    doc.header = trivia.header;
    doc.trailer = trivia.trailer;
    doc.hasUnplaceableComments = trivia.unplaceable;
    for (std::size_t i = 0; i < doc.layers.size() && i < trivia.elementLeading.size(); ++i) {
        doc.layers[i].leading = trivia.elementLeading[i];
    }

    out = std::move(doc);
    return true;
}

std::string writeSound(const SoundDoc& doc)
{
    // Faithful or nothing, exactly as `writeTexture` and `writeForge`: no
    // invented header, no invented key.
    std::string out;
    out += doc.header;
    if (!doc.name.empty()) {
        out += "name = \"" + doc.name + "\"\n";
    }
    out += "seconds = " + formatNumber(doc.seconds) + "\n";
    out += "sample_rate = " + std::to_string(doc.sampleRate) + "\n";
    out += "channels = " + std::to_string(doc.channels) + "\n";

    for (const SoundLayer& layer : doc.layers) {
        out += doc::separatorFor(layer.leading, out);
        out += "[[op]]\n";
        out += std::string("kind = \"") + soundOpName(layer.op) + "\"\n";

        for (const SoundParamSpec& spec : soundParams(layer.op)) {
            const SoundValue* authored = layer.find(spec.name);
            if (authored == nullptr) {
                continue;
            }
            out += std::string(spec.name) + " = ";
            switch (spec.kind) {
            case SoundParamKind::Number:
                out += formatNumber(authored->number);
                break;
            case SoundParamKind::Seed:
                out += std::to_string(authored->integer);
                break;
            }
            out += "\n";
        }
    }

    out += doc.trailer;
    return out;
}

bool buildSound(const SoundDoc& doc, SoundData& out, std::string* error)
{
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (doc.sampleRate == 0) {
        return fail("sound has no sample rate");
    }
    // ⚑⚑ REFUSED BY NAME RATHER THAN DOWNMIXED SILENTLY, WHICH IS THE WHOLE
    // POINT OF HAVING THE KEY (Phase 26 decision 6). `audio::Mixer::sampleClip`
    // sums a clip's channels and divides by the stride before a `Voice` ever
    // sees it, so a stereo cue is authored, cooked, loaded and then thrown away
    // one step from the speakers. Naming the component is what makes this a
    // limitation somebody can lift instead of a result somebody has to explain.
    if (doc.channels != 1) {
        return fail("channels = " + std::to_string(doc.channels) +
                    ": only mono is supported, because audio::Mixer folds every clip to one channel "
                    "in sampleClip() - a stereo cue would be silently downmixed");
    }

    const double rate = static_cast<double>(doc.sampleRate);
    const std::int64_t frames = toSamples(doc.seconds, rate);
    if (frames <= 0) {
        return fail("sound has no length");
    }

    std::vector<double> buffer(static_cast<std::size_t>(frames), 0.0);
    for (const SoundLayer& layer : doc.layers) {
        switch (layer.op) {
        case SoundOp::Tone:
            applyTone(layer, buffer, rate);
            break;
        case SoundOp::NoiseBurst:
            applyNoiseBurst(layer, buffer, rate);
            break;
        case SoundOp::LoopNoise:
            applyLoopNoise(layer, buffer, rate);
            break;
        case SoundOp::Fade:
            applyFade(layer, buffer, rate);
            break;
        case SoundOp::Peak:
            applyPeak(layer, buffer);
            break;
        }
    }

    out.sampleRate = doc.sampleRate;
    out.channelCount = doc.channels;
    out.samples.resize(buffer.size());
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        // `[math]::Round` was banker's rounding; `std::nearbyint` under the
        // default FE_TONEAREST is the same rule, and `std::round` is not.
        const double scaled = std::nearbyint(buffer[i] * 32767.0);
        const double clamped = std::clamp(scaled, -32768.0, 32767.0);
        out.samples[i] = static_cast<std::int16_t>(clamped);
    }
    return true;
}

std::size_t soundLayerCoverage(const SoundDoc& doc, std::size_t layerIndex)
{
    if (layerIndex >= doc.layers.size() || doc.sampleRate == 0) {
        return 0;
    }
    const double rate = static_cast<double>(doc.sampleRate);
    const std::int64_t frames = toSamples(doc.seconds, rate);
    if (frames <= 0) {
        return 0;
    }
    const SoundLayer& layer = doc.layers[layerIndex];

    // A span clipped to the buffer, which is what "actually writes" means.
    const auto clippedSpan = [frames](std::int64_t first, std::int64_t count) -> std::size_t {
        if (count <= 0) {
            return 0;
        }
        const std::int64_t begin = std::max<std::int64_t>(first, 0);
        const std::int64_t end = std::min<std::int64_t>(first + count, frames);
        return end > begin ? static_cast<std::size_t>(end - begin) : 0;
    };

    switch (layer.op) {
    case SoundOp::Tone:
    case SoundOp::NoiseBurst:
        return clippedSpan(toSamples(layer.value("start").number, rate),
                           toSamples(layer.value("duration").number, rate));
    case SoundOp::LoopNoise:
    case SoundOp::Peak:
        // Both touch the whole buffer by construction.
        return static_cast<std::size_t>(frames);
    case SoundOp::Fade: {
        const std::size_t head = clippedSpan(0, toSamples(layer.value("in").number, rate));
        const std::size_t tail = clippedSpan(0, toSamples(layer.value("out").number, rate));
        // ⚑ Summed rather than unioned: the two ramps are at opposite ends, and
        // a `fade` long enough for them to meet is a broken cue rather than one
        // whose coverage wants a set operation. Capped so it can never exceed
        // the buffer, which is what a caller printing it will assume.
        return std::min<std::size_t>(head + tail, static_cast<std::size_t>(frames));
    }
    }
    return 0;
}

} // namespace sol::assets
