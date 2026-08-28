#include "sol/assets/sound_doc.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sol;
using assets::SoundData;
using assets::SoundDoc;
using assets::SoundOp;
using assets::SoundValue;

namespace {

[[nodiscard]] bool parses(const std::string& text, SoundDoc& out)
{
    std::string error;
    if (assets::parseSound(text.c_str(), text.size(), "test.snd", out, &error)) {
        return true;
    }
    std::printf("  unexpected parse failure: %s\n", error.c_str());
    return false;
}

[[nodiscard]] std::string rejects(const std::string& text)
{
    SoundDoc doc;
    std::string error;
    if (assets::parseSound(text.c_str(), text.size(), "test.snd", doc, &error)) {
        return {};
    }
    return error.empty() ? "rejected" : error;
}

[[nodiscard]] bool builds(const SoundDoc& doc, SoundData& out)
{
    std::string error;
    if (assets::buildSound(doc, out, &error)) {
        return true;
    }
    std::printf("  unexpected build failure: %s\n", error.c_str());
    return false;
}

// A one-op document, spelled the way the writer spells one, so a round trip is
// a fair test rather than a test of this string's formatting.
[[nodiscard]] std::string toneDoc(const char* extra = "")
{
    return std::string("seconds = 0.1\nsample_rate = 44100\nchannels = 1\n\n[[op]]\nkind = \"tone\"\n"
                       "start = 0.0\nduration = 0.1\nf0 = 440.0\nf1 = 440.0\ngain = 0.5\n") +
           extra;
}

[[nodiscard]] float peakOf(const SoundData& data)
{
    int worst = 0;
    for (const std::int16_t sample : data.samples) {
        worst = std::max(worst, std::abs(static_cast<int>(sample)));
    }
    return static_cast<float>(worst) / 32767.0f;
}

} // namespace

SOL_TEST(soundParsesAnOpListInFileOrder)
{
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.5\n\n[[op]]\nkind = \"tone\"\nf0 = 100.0\n\n"
                       "[[op]]\nkind = \"peak\"\nlevel = 0.8\n\n[[op]]\nkind = \"fade\"\nout = 0.05\n",
                       doc));
    SOL_REQUIRE(doc.layers.size() == 3);
    SOL_CHECK(doc.layers[0].op == SoundOp::Tone);
    SOL_CHECK(doc.layers[1].op == SoundOp::Peak);
    SOL_CHECK(doc.layers[2].op == SoundOp::Fade);
    // The defaults the schema supplies, not zero.
    SOL_CHECK(doc.sampleRate == 44100);
    SOL_CHECK(doc.channels == 1);
    SOL_CHECK(doc.layers[0].value("f0").number == 100.0);
    SOL_CHECK(doc.layers[0].value("gain").number == 1.0);
}

SOL_TEST(soundAcceptsAnIntegerWhereANumberIsWanted)
{
    // `f0 = 988` is what an author writes for a frequency; refusing it would
    // teach a rule the format does not need.
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 1\n\n[[op]]\nkind = \"tone\"\nf0 = 988\nduration = 1\n", doc));
    SOL_CHECK(doc.seconds == 1.0);
    SOL_CHECK(doc.layers[0].value("f0").number == 988.0);
    // ...and the writer settles the spelling on the first save rather than
    // leaving two forms in the corpus.
    SOL_CHECK(assets::writeSound(doc).find("f0 = 988.0") != std::string::npos);
}

SOL_TEST(soundRejectsMalformedDocuments)
{
    SOL_CHECK(!rejects("[[op]]\nkind = \"tone\"\n").empty());                            // no seconds
    SOL_CHECK(!rejects("seconds = 0.0\n").empty());                                      // zero length
    SOL_CHECK(!rejects("seconds = 500.0\n").empty());                                    // absurd length
    SOL_CHECK(!rejects("seconds = 0.1\n\n[[op]]\nkind = \"warble\"\n").empty());         // unknown op
    SOL_CHECK(!rejects("seconds = 0.1\n\n[[op]]\nkind = \"fade\"\nf0 = 1.0\n").empty()); // wrong param
    SOL_CHECK(!rejects("seconds = 0.1\nsample_rate = 3\n").empty());                     // absurd rate
    SOL_CHECK(!rejects("seconds = 0.1\n\n[[op]]\nkind = \"noise_burst\"\nseed = 1.5\n").empty());
    // A seed is an integer and a frequency is not: the schema decides, so
    // neither spelling can be smuggled into the other.
    SOL_CHECK(!rejects("seconds = 0.1\n\n[[op]]\nkind = \"tone\"\nf0 = \"low\"\n").empty());
}

SOL_TEST(soundRoundTripsThroughItsOwnWriter)
{
    SoundDoc doc;
    SOL_REQUIRE(parses(toneDoc(), doc));
    const std::string written = assets::writeSound(doc);
    SOL_CHECK(written == toneDoc());

    SoundDoc again;
    SOL_REQUIRE(parses(written, again));
    SOL_CHECK(assets::writeSound(again) == written);
}

SOL_TEST(soundKeepsCommentsWhereTheAuthorPutThem)
{
    // The property the whole trivia model exists for, and the reason a tool may
    // save on every edit: a committed file comes back the way it was found.
    const std::string source = "# explosion.snd - broadband collapsing to a rumble.\n"
                               "seconds = 1.3\nsample_rate = 44100\nchannels = 1\n"
                               "\n# the body\n[[op]]\nkind = \"tone\"\nf0 = 70.0\nf1 = 28.0\n"
                               "\n# then normalise\n[[op]]\nkind = \"peak\"\nlevel = 0.95\n";
    SoundDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(!doc.hasUnplaceableComments);
    SOL_CHECK(assets::writeSound(doc) == source);
}

SOL_TEST(soundReportsACommentItCannotPlace)
{
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.1 # trailing\n\n[[op]]\nkind = \"tone\"\n", doc));
    SOL_CHECK(doc.hasUnplaceableComments);
}

SOL_TEST(soundWritesAFloatKeyAsAFloatAndASeedAsAnInteger)
{
    // ⚑ The gotcha this project has already paid for once, from both sides: a
    // helper that always emits a float cannot write `seed = 104`, and one that
    // emits the shortest form would write `decay = 22.0` as `22` - demoting a
    // float key to an integer key in the diff of a file that promised not to
    // reformat.
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.1\n\n[[op]]\nkind = \"noise_burst\"\n"
                       "decay = 22.0\nseed = 104\n",
                       doc));
    const std::string written = assets::writeSound(doc);
    SOL_CHECK(written.find("decay = 22.0") != std::string::npos);
    SOL_CHECK(written.find("seed = 104\n") != std::string::npos);
    SOL_CHECK(written.find("seed = 104.0") == std::string::npos);
}

SOL_TEST(soundBuildsSamplesAtTheDeclaredRateAndLength)
{
    SoundDoc doc;
    SOL_REQUIRE(parses(toneDoc(), doc));
    SoundData data;
    SOL_REQUIRE(builds(doc, data));
    SOL_CHECK(data.sampleRate == 44100);
    SOL_CHECK(data.channelCount == 1);
    SOL_CHECK(data.frameCount() == 4410);
    SOL_CHECK(peakOf(data) > 0.4f);
}

SOL_TEST(soundRefusesStereoByNameBecauseTheMixerWouldFoldIt)
{
    // ⚑⚑ The key exists so the limitation can be STATED. A stereo cue is
    // representable through every other stage - importWav, SoundData,
    // encodeSound, loadSound all carry channelCount - and then discarded by
    // `Mixer::sampleClip`, one step from the speakers. Refused here rather than
    // silently downmixed there.
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.1\nchannels = 2\n\n[[op]]\nkind = \"tone\"\nduration = 0.1\n", doc));
    SOL_CHECK(doc.channels == 2);
    SoundData data;
    std::string error;
    SOL_CHECK(!assets::buildSound(doc, data, &error));
    SOL_CHECK(error.find("mono") != std::string::npos);
    SOL_CHECK(error.find("sampleClip") != std::string::npos);
    // ...and three channels is refused by the parser, before it can reach here.
    SOL_CHECK(!rejects("seconds = 0.1\nchannels = 3\n").empty());
}

SOL_TEST(soundPeakNormalisesToItsLevel)
{
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.1\n\n[[op]]\nkind = \"tone\"\nduration = 0.1\nf0 = 440.0\n"
                       "f1 = 440.0\ngain = 0.1\n\n[[op]]\nkind = \"peak\"\nlevel = 0.8\n",
                       doc));
    SoundData data;
    SOL_REQUIRE(builds(doc, data));
    // Quantisation to int16 is the only slack here.
    SOL_CHECK(std::abs(peakOf(data) - 0.8f) < 0.001f);
}

SOL_TEST(soundPeakBeforeFadeIsNotTheSameAsFadeBeforePeak)
{
    // ⚑⚑ FILE ORDER IS THE SEMANTICS. Every committed cue normalises and THEN
    // shapes its ends; reversing the two makes `peak` scale against a buffer
    // whose ends are already quiet, so the result is louder. Asserted because
    // it is the one thing about this format an editor could reorder by
    // accident, and nothing else would complain.
    const char* body = "seconds = 0.1\n\n[[op]]\nkind = \"tone\"\nduration = 0.1\nf0 = 440.0\n"
                       "f1 = 440.0\ngain = 0.1\n\n";
    SoundDoc peakThenFade;
    SOL_REQUIRE(parses(std::string(body) + "[[op]]\nkind = \"peak\"\nlevel = 0.8\n\n"
                                           "[[op]]\nkind = \"fade\"\nin = 0.05\nout = 0.05\n",
                       peakThenFade));
    SoundDoc fadeThenPeak;
    SOL_REQUIRE(parses(std::string(body) + "[[op]]\nkind = \"fade\"\nin = 0.05\nout = 0.05\n\n"
                                           "[[op]]\nkind = \"peak\"\nlevel = 0.8\n",
                       fadeThenPeak));

    SoundData a;
    SoundData b;
    SOL_REQUIRE(builds(peakThenFade, a));
    SOL_REQUIRE(builds(fadeThenPeak, b));
    SOL_CHECK(a.samples != b.samples);
    // Normalising last always reaches the level; normalising first cannot,
    // because the fade runs afterwards.
    SOL_CHECK(std::abs(peakOf(b) - 0.8f) < 0.001f);
    SOL_CHECK(peakOf(a) < 0.8f);
}

SOL_TEST(soundFadeSilencesTheFirstAndLastSample)
{
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.1\n\n[[op]]\nkind = \"tone\"\nduration = 0.1\nf0 = 440.0\n"
                       "f1 = 440.0\ngain = 0.9\n\n[[op]]\nkind = \"fade\"\nin = 0.01\nout = 0.01\n",
                       doc));
    SoundData data;
    SOL_REQUIRE(builds(doc, data));
    SOL_REQUIRE(data.samples.size() > 2);
    SOL_CHECK(data.samples.front() == 0);
    SOL_CHECK(data.samples.back() == 0);
}

SOL_TEST(soundNoiseIsDeterministicAndSeedSensitive)
{
    // The whole reason the noise moved to PCG32: the same document must give
    // the same bytes on every compiler and platform, which `System.Random`
    // could not promise anywhere but .NET.
    const char* shape = "seconds = 0.1\n\n[[op]]\nkind = \"noise_burst\"\nduration = 0.1\n"
                        "gain = 0.8\ncutoff0 = 4000.0\ncutoff1 = 1000.0\nseed = ";
    SoundDoc first;
    SoundDoc same;
    SoundDoc other;
    SOL_REQUIRE(parses(std::string(shape) + "101\n", first));
    SOL_REQUIRE(parses(std::string(shape) + "101\n", same));
    SOL_REQUIRE(parses(std::string(shape) + "102\n", other));

    SoundData a;
    SoundData b;
    SoundData c;
    SOL_REQUIRE(builds(first, a));
    SOL_REQUIRE(builds(same, b));
    SOL_REQUIRE(builds(other, c));
    SOL_CHECK(a.samples == b.samples);
    SOL_CHECK(a.samples != c.samples);
    SOL_CHECK(peakOf(a) > 0.0f);
}

SOL_TEST(soundLoopNoiseJoinsItsOwnEnd)
{
    // ⚑ The property that makes this a separate op rather than `noise_burst`
    // with no decay: sample 0 must continue from the last sample, or
    // `engine_loop` ticks once per revolution. Measured as "the seam is no
    // worse than a typical step inside the buffer".
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 1.0\n\n[[op]]\nkind = \"loop_noise\"\ngain = 0.6\n"
                       "cutoff = 900.0\nseed = 107\n\n[[op]]\nkind = \"peak\"\nlevel = 0.7\n",
                       doc));
    SoundData data;
    SOL_REQUIRE(builds(doc, data));
    SOL_REQUIRE(data.samples.size() > 1000);

    const int seam = std::abs(static_cast<int>(data.samples.front()) - static_cast<int>(data.samples.back()));
    int worst = 0;
    for (std::size_t i = 1; i < data.samples.size(); ++i) {
        worst = std::max(worst,
                         std::abs(static_cast<int>(data.samples[i]) - static_cast<int>(data.samples[i - 1])));
    }
    SOL_CHECK(seam <= worst);
}

SOL_TEST(soundLayerCoverageFindsAnOpThatWritesNothing)
{
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.5\n\n[[op]]\nkind = \"tone\"\nstart = 0.0\nduration = 0.25\n\n"
                       "[[op]]\nkind = \"tone\"\nstart = 9.0\nduration = 0.25\n\n"
                       "[[op]]\nkind = \"peak\"\nlevel = 0.5\n",
                       doc));
    SOL_CHECK(assets::soundLayerCoverage(doc, 0) == 11025);
    // Entirely past the end of the buffer: the one thing about a sound source
    // that is broken rather than merely unusual.
    SOL_CHECK(assets::soundLayerCoverage(doc, 1) == 0);
    // `peak` touches everything by construction.
    SOL_CHECK(assets::soundLayerCoverage(doc, 2) == 22050);
}

SOL_TEST(soundEveryOpNamesItselfBothWays)
{
    // The schema table is the single source of truth for the parser, the writer
    // and the tool's panel; a name that does not survive the round trip would
    // desynchronise all three.
    SOL_CHECK(assets::soundOps().size() == 5);
    for (const SoundOp op : assets::soundOps()) {
        SoundOp back = SoundOp::Tone;
        SOL_CHECK(assets::soundOpFromName(assets::soundOpName(op), back));
        SOL_CHECK(back == op);
        SOL_CHECK(!assets::soundParams(op).empty());
    }
    SoundOp ignored = SoundOp::Tone;
    SOL_CHECK(!assets::soundOpFromName("fill", ignored));
    SOL_CHECK(!assets::soundOpFromName(nullptr, ignored));
}

// --- the committed corpus (Phase 26 stage B) ---------------------------------

namespace {

[[nodiscard]] std::string readWholeFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  cannot open %s\n", path.c_str());
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string soundPath(const char* stem)
{
    return std::string(SOL_SOUND_SOURCE_DIR) + "/" + stem + ".snd";
}

constexpr const char* kCommittedCues[] = {"weapon_fire",
                                          "weapon_hit_shield",
                                          "weapon_hit_hull",
                                          "explosion",
                                          "mining_cut",
                                          "engine_loop",
                                          "ui_click",
                                          "docking_chime",
                                          "alarm"};

} // namespace

SOL_TEST(everyCommittedSoundRoundTripsByteForByte)
{
    // ⚑⚑⚑ THE GUARANTEE `writeSound` PROMISES, ASSERTED WHERE IT MATTERS. The
    // header is explicit that a `.snd` round trip is weaker than a `.tex` one by
    // one step - a double has more than one spelling, so only files this writer
    // produced come back byte for byte. These nine ARE such files, which is what
    // makes the promise real rather than theoretical, and it is the property
    // that lets the Forge save on every edit without reformatting a comment
    // somebody wrote.
    for (const char* stem : kCommittedCues) {
        const std::string source = readWholeFile(soundPath(stem));
        SOL_REQUIRE(!source.empty());
        SoundDoc doc;
        std::string error;
        SOL_REQUIRE(assets::parseSound(source.c_str(), source.size(), stem, doc, &error));
        SOL_CHECK(!doc.hasUnplaceableComments);
        const std::string written = assets::writeSound(doc);
        if (written != source) {
            std::printf("  %s.snd does not round trip\n", stem);
        }
        SOL_CHECK(written == source);
    }
}

SOL_TEST(everyCommittedSoundKeepsItsAuthoredCommentary)
{
    // ⚑⚑ THE DOCUMENTS CARRY THE ARGUMENT THE DELETED SCRIPT USED TO CARRY, and
    // that is load-bearing rather than decorative: `gen_assets.ps1` held the
    // only written explanation of why `engine_loop` uses integer frequencies
    // over a two-second buffer, and it does not exist any more. A transcription
    // that dropped the prose would have moved the format forward and the
    // knowledge backward.
    for (const char* stem : kCommittedCues) {
        const std::string source = readWholeFile(soundPath(stem));
        SOL_REQUIRE(!source.empty());
        SoundDoc doc;
        SOL_REQUIRE(assets::parseSound(source.c_str(), source.size(), stem, doc, nullptr));
        SOL_CHECK(doc.header.find('#') != std::string::npos);
    }
}

SOL_TEST(noCommittedSoundHasAnOpThatWritesNothing)
{
    // The one thing about a sound source that is broken rather than unusual: an
    // op whose span falls entirely outside the buffer contributes silence and
    // reads, in a diff, exactly like one that works.
    for (const char* stem : kCommittedCues) {
        const std::string source = readWholeFile(soundPath(stem));
        SOL_REQUIRE(!source.empty());
        SoundDoc doc;
        SOL_REQUIRE(assets::parseSound(source.c_str(), source.size(), stem, doc, nullptr));
        SOL_REQUIRE(!doc.layers.empty());
        for (std::size_t i = 0; i < doc.layers.size(); ++i) {
            if (assets::soundLayerCoverage(doc, i) == 0) {
                std::printf("  %s.snd op %zu writes nothing\n", stem, i);
            }
            SOL_CHECK(assets::soundLayerCoverage(doc, i) > 0);
        }
    }
}

SOL_TEST(theOnlyLoopingCueCarriesNoFade)
{
    // ⚑⚑ A RULE THE FORMAT CANNOT ENFORCE AND THE ASSET MUST OBEY, so it is
    // asserted here rather than trusted. `engine_loop` is the game's one looping
    // voice; a fade at either end is precisely what a loop must not have, and
    // adding one in the Forge would be an easy, plausible edit whose symptom is
    // a pulse under thrust rather than an error.
    const std::string source = readWholeFile(soundPath("engine_loop"));
    SOL_REQUIRE(!source.empty());
    SoundDoc doc;
    SOL_REQUIRE(assets::parseSound(source.c_str(), source.size(), "engine_loop", doc, nullptr));
    for (const auto& layer : doc.layers) {
        SOL_CHECK(layer.op != SoundOp::Fade);
    }
    // ...and it is the only cue using the op that exists for it.
    int loopNoiseUsers = 0;
    for (const char* stem : kCommittedCues) {
        const std::string text = readWholeFile(soundPath(stem));
        SoundDoc other;
        SOL_REQUIRE(assets::parseSound(text.c_str(), text.size(), stem, other, nullptr));
        for (const auto& layer : other.layers) {
            if (layer.op == SoundOp::LoopNoise) {
                ++loopNoiseUsers;
                break;
            }
        }
    }
    SOL_CHECK(loopNoiseUsers == 1);
}

SOL_TEST(soundWritesNumbersTheWayAPersonWouldReadThem)
{
    // ⚑⚑ FOUND BY `ui_click.snd` FAILING THE CORPUS ROUND TRIP, and worth an
    // assertion of its own because the failure was invisible in every other
    // cue. `std::to_chars` in its default shortest mode spells 0.0005 as
    // "5e-04" - the same value in fewer bytes, and a fade time no author would
    // recognise in a format whose whole purpose is being edited by hand. The
    // round trip was never in danger; the readability was.
    SoundDoc doc;
    SOL_REQUIRE(parses("seconds = 0.05\n\n[[op]]\nkind = \"fade\"\nin = 0.0005\nout = 0.008\n", doc));
    const std::string written = assets::writeSound(doc);
    SOL_CHECK(written.find("in = 0.0005") != std::string::npos);
    SOL_CHECK(written.find("out = 0.008") != std::string::npos);
    SOL_CHECK(written.find("e-0") == std::string::npos);
    // ...and a large round value still keeps the decimal point that marks it a
    // float key, which is the same rule pulling the other way.
    SoundDoc big;
    SOL_REQUIRE(parses("seconds = 0.1\n\n[[op]]\nkind = \"tone\"\nf0 = 1800.0\n", big));
    SOL_CHECK(assets::writeSound(big).find("f0 = 1800.0") != std::string::npos);
}
