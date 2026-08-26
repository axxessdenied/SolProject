#pragma once

// FNV-1a hashing (engine plan §2.2). Compile-time capable, so hashed ids can
// be constants; stable across runs and platforms, which is what makes it safe
// for anything persisted or compared between builds.

#include <cstdint>
#include <string_view>

namespace sol::core {

inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ull;

[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view text, std::uint64_t seed = kFnvOffsetBasis)
{
    std::uint64_t hash = seed;
    for (const char character : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= kFnvPrime;
    }
    return hash;
}

// Folds an integer into an existing hash, byte by byte through the same
// mixing step - for ids assembled from a stack of names and indices.
[[nodiscard]] constexpr std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value)
{
    std::uint64_t hash = seed;
    for (int i = 0; i < 8; ++i) {
        hash ^= value & 0xFFull;
        hash *= kFnvPrime;
        value >>= 8;
    }
    return hash;
}

} // namespace sol::core
