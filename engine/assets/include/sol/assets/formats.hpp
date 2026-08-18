#pragma once

// Cooked binary asset formats shared by tools/cooker (writer) and the
// runtime loader (reader). Little-endian, tightly packed.

#include <cstdint>

namespace sol::assets {

inline constexpr std::uint32_t kMeshMagic = 0x48534D53u;    // "SMSH"
inline constexpr std::uint32_t kTextureMagic = 0x58455453u; // "STEX"
inline constexpr std::uint32_t kFontMagic = 0x4E4F4653u;    // "SFON"
inline constexpr std::uint32_t kSoundMagic = 0x44554153u;   // "SAUD"
inline constexpr std::uint32_t kFormatVersion = 1;

// .smesh layout: MeshFileHeader, vertexCount * MeshVertex, indexCount * uint32.
struct MeshFileHeader
{
    std::uint32_t magic = kMeshMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
};

struct MeshVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(MeshVertex) == 32);

enum class TextureFormat : std::uint32_t
{
    BC1 = 1, // 8 bytes per 4x4 block, opaque RGB
    R8 = 2,  // uncompressed single channel; glyph coverage, which BC1 would smear
};

// .stex layout: TextureFileHeader, then mip payloads from mip 0 down,
// each ceil(w/4)*ceil(h/4)*8 bytes of BC1 blocks.
struct TextureFileHeader
{
    std::uint32_t magic = kTextureMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    TextureFormat format = TextureFormat::BC1;
};

// Longest style name a cooked font stores, including the terminator.
inline constexpr std::uint32_t kFontStyleNameCapacity = 32;

// .sfont layout: FontFileHeader, styleCount * FontStyleRecord, glyphCount *
// FontGlyphRecord, then atlasWidth * atlasHeight bytes of R8 coverage.
//
// Every style in a font shares one atlas, so the UI draws all of its text -
// any weight, any size - without rebinding a texture. Glyph records are
// grouped by style and sorted by codepoint within a style, so a lookup is a
// binary search over one contiguous run.
struct FontFileHeader
{
    std::uint32_t magic = kFontMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t styleCount = 0;
    std::uint32_t glyphCount = 0;
    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
};

struct FontStyleRecord
{
    char name[kFontStyleNameCapacity] = {}; // "hud", "body", "heading", ...
    float pixelSize = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f; // negative, below the baseline
    float lineHeight = 0.0f;
    std::uint32_t firstGlyph = 0; // index into the glyph array
    std::uint32_t glyphCount = 0;
};
static_assert(sizeof(FontStyleRecord) == 56);

struct FontGlyphRecord
{
    std::uint32_t codepoint = 0;
    std::uint16_t atlasX = 0;
    std::uint16_t atlasY = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t bearingX = 0; // left edge relative to the pen
    std::int16_t bearingY = 0; // top edge relative to the baseline; negative is above
    float advance = 0.0f;      // pen movement, pixels
};
static_assert(sizeof(FontGlyphRecord) == 20);

enum class SoundCodec : std::uint32_t
{
    Pcm16 = 1, // interleaved signed 16-bit frames, native endian
};

// .saud layout: SoundFileHeader, then frameCount * channelCount int16 samples,
// interleaved.
//
// The authored sample rate is stored rather than normalized at cook time: a
// cue carries pitch jitter, so the mixer steps through a source at a
// fractional rate regardless, and device-rate conversion is that same step
// rather than a second mechanism. `codec` exists so a streamed, still-encoded
// payload can be added without a format version bump - see
// docs/decisions/009-audio-decoder.md for why the decoder stops at the cooker.
struct SoundFileHeader
{
    std::uint32_t magic = kSoundMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t sampleRate = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t channelCount = 1; // 1 for anything a 3D voice may play
    SoundCodec codec = SoundCodec::Pcm16;
};

[[nodiscard]] constexpr std::uint32_t bc1MipByteSize(std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    return blocksX * blocksY * 8;
}

[[nodiscard]] constexpr std::uint32_t mipDimension(std::uint32_t base, std::uint32_t mip)
{
    const std::uint32_t d = base >> mip;
    return d > 0 ? d : 1;
}

} // namespace sol::assets
