#include "sound.hpp"

#include "sol/assets/formats.hpp"
#include "sol/core/log.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

// Declarations only; the implementation is its own target so the vendored
// source is never compiled under sol_options (third_party/CMakeLists.txt).
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace sol::cooker {

namespace {

// The mixer pans a mono source and plays a stereo one flat; more channels
// than that would need a downmix policy nothing in the game asks for.
constexpr std::uint32_t kMaxChannels = 2;

std::uint16_t readLittleEndian16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(p[0]) |
                                      (static_cast<std::uint32_t>(p[1]) << 8));
}

std::uint32_t readLittleEndian32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int16_t clampToInt16(float value)
{
    const float scaled = value * 32767.0f;
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}

// Every supported storage width collapses to int16 here, so the rest of the
// pipeline has exactly one sample type to reason about.
bool convertSamples(const std::uint8_t* data,
                    std::size_t size,
                    std::uint16_t format,
                    std::uint16_t bitsPerSample,
                    std::vector<std::int16_t>& out)
{
    const std::size_t bytesPerSample = bitsPerSample / 8u;
    if (bytesPerSample == 0 || size % bytesPerSample != 0) {
        SOL_LOG_ERROR("wav: data chunk is not a whole number of %u-bit samples", bitsPerSample);
        return false;
    }
    const std::size_t count = size / bytesPerSample;
    out.resize(count);

    if (format == 3) { // IEEE float
        if (bitsPerSample != 32) {
            SOL_LOG_ERROR("wav: float samples must be 32-bit, got %u", bitsPerSample);
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            float sample = 0.0f;
            std::memcpy(&sample, data + i * 4, 4);
            out[i] = clampToInt16(sample);
        }
        return true;
    }

    switch (bitsPerSample) {
    case 8: // unsigned, midpoint 128
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<std::int16_t>((static_cast<int>(data[i]) - 128) << 8);
        }
        return true;
    case 16:
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<std::int16_t>(readLittleEndian16(data + i * 2));
        }
        return true;
    case 24: // keep the top 16 bits
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* p = data + i * 3;
            out[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(p[1]) | (static_cast<std::uint32_t>(p[2]) << 8)));
        }
        return true;
    case 32:
        for (std::size_t i = 0; i < count; ++i) {
            out[i] =
                static_cast<std::int16_t>(static_cast<std::uint16_t>(readLittleEndian32(data + i * 4) >> 16));
        }
        return true;
    default:
        SOL_LOG_ERROR("wav: unsupported sample width %u", bitsPerSample);
        return false;
    }
}

} // namespace

bool importWav(const std::uint8_t* data, std::size_t size, assets::SoundData& out)
{
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        SOL_LOG_ERROR("wav: not a RIFF/WAVE file");
        return false;
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    bool haveFormat = false;

    // Chunks are 8-byte headers with word-aligned payloads; walk them rather
    // than assuming fmt-then-data, because writers interleave LIST and fact.
    std::size_t offset = 12;
    while (offset + 8 <= size) {
        const std::uint8_t* header = data + offset;
        const std::uint32_t chunkSize = readLittleEndian32(header + 4);
        const std::size_t payload = offset + 8;
        if (chunkSize > size - payload) {
            SOL_LOG_ERROR("wav: chunk runs past end of file");
            return false;
        }

        if (std::memcmp(header, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                SOL_LOG_ERROR("wav: fmt chunk is %u bytes, need 16", chunkSize);
                return false;
            }
            format = readLittleEndian16(data + payload);
            channels = readLittleEndian16(data + payload + 2);
            sampleRate = readLittleEndian32(data + payload + 4);
            bitsPerSample = readLittleEndian16(data + payload + 14);
            // WAVE_FORMAT_EXTENSIBLE carries the real format in the first two
            // bytes of its subformat GUID.
            if (format == 0xFFFE && chunkSize >= 40) {
                format = readLittleEndian16(data + payload + 24);
            }
            haveFormat = true;
        } else if (std::memcmp(header, "data", 4) == 0) {
            if (!haveFormat) {
                SOL_LOG_ERROR("wav: data chunk precedes fmt chunk");
                return false;
            }
            if (format != 1 && format != 3) {
                SOL_LOG_ERROR("wav: compressed formats are not supported (format %u)", format);
                return false;
            }
            if (channels == 0 || channels > kMaxChannels) {
                SOL_LOG_ERROR("wav: %u channels; mono or stereo only", channels);
                return false;
            }
            if (sampleRate == 0) {
                SOL_LOG_ERROR("wav: zero sample rate");
                return false;
            }
            if (!convertSamples(data + payload, chunkSize, format, bitsPerSample, out.samples)) {
                return false;
            }
            if (out.samples.size() % channels != 0) {
                SOL_LOG_ERROR("wav: data is not a whole number of frames");
                return false;
            }
            out.sampleRate = sampleRate;
            out.channelCount = channels;
            return true;
        }

        offset = payload + chunkSize + (chunkSize & 1u); // chunks pad to even
    }

    SOL_LOG_ERROR("wav: no data chunk");
    return false;
}

bool importOgg(const std::uint8_t* data, std::size_t size, assets::SoundData& out)
{
    if (size > static_cast<std::size_t>(INT_MAX)) {
        SOL_LOG_ERROR("ogg: file is too large to decode in one call");
        return false;
    }

    int channels = 0;
    int sampleRate = 0;
    short* decoded = nullptr;
    const int frames =
        stb_vorbis_decode_memory(data, static_cast<int>(size), &channels, &sampleRate, &decoded);
    if (frames < 0 || decoded == nullptr) {
        SOL_LOG_ERROR("ogg: decode failed");
        std::free(decoded);
        return false;
    }
    if (channels <= 0 || static_cast<std::uint32_t>(channels) > kMaxChannels || sampleRate <= 0) {
        SOL_LOG_ERROR("ogg: %d channels at %d Hz; mono or stereo only", channels, sampleRate);
        std::free(decoded);
        return false;
    }

    const std::size_t count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
    out.sampleRate = static_cast<std::uint32_t>(sampleRate);
    out.channelCount = static_cast<std::uint32_t>(channels);
    out.samples.assign(decoded, decoded + count);
    std::free(decoded);
    return true;
}

std::vector<std::uint8_t> encodeSound(const assets::SoundData& sound)
{
    assets::SoundFileHeader header = {};
    header.sampleRate = sound.sampleRate;
    header.channelCount = sound.channelCount;
    header.frameCount = sound.frameCount();

    const std::size_t sampleBytes = sound.samples.size() * sizeof(std::int16_t);
    std::vector<std::uint8_t> bytes(sizeof(header) + sampleBytes);
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (sampleBytes > 0) {
        std::memcpy(bytes.data() + sizeof(header), sound.samples.data(), sampleBytes);
    }
    return bytes;
}

} // namespace sol::cooker
