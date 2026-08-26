#pragma once

// Deterministic PRNG streams (PCG32, O'Neill 2014). Each engine system that
// consumes randomness gets its own Rng seeded with the world seed plus a
// distinct stream id, so consumption in one system never perturbs another -
// a hard requirement for procgen determinism (same seed => same galaxy).

#include "sol/core/assert.hpp"

#include <cstdint>

namespace sol::core {

class Rng
{
public:
    Rng() { seed(0x853c'49e6'748f'ea9bull, 0); }

    explicit Rng(std::uint64_t seedValue, std::uint64_t stream = 0) { seed(seedValue, stream); }

    void seed(std::uint64_t seedValue, std::uint64_t stream = 0)
    {
        m_state = 0;
        m_inc = (stream << 1u) | 1u;
        (void)nextU32();
        m_state += seedValue;
        (void)nextU32();
    }

    [[nodiscard]] std::uint32_t nextU32()
    {
        const std::uint64_t old = m_state;
        m_state = old * 6364136223846793005ull + m_inc;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    [[nodiscard]] std::uint64_t nextU64()
    {
        const std::uint64_t high = nextU32();
        return (high << 32u) | nextU32();
    }

    // Uniform in [0, bound), bound > 0. Debiased via Lemire's method.
    [[nodiscard]] std::uint32_t range(std::uint32_t bound)
    {
        SOL_ASSERT(bound > 0);
        std::uint64_t product = static_cast<std::uint64_t>(nextU32()) * bound;
        std::uint32_t low = static_cast<std::uint32_t>(product);
        if (low < bound) {
            const std::uint32_t threshold = (0u - bound) % bound;
            while (low < threshold) {
                product = static_cast<std::uint64_t>(nextU32()) * bound;
                low = static_cast<std::uint32_t>(product);
            }
        }
        return static_cast<std::uint32_t>(product >> 32u);
    }

    // Uniform in [0, 1).
    [[nodiscard]] float nextFloat01() { return static_cast<float>(nextU32() >> 8u) * 0x1.0p-24f; }

    // Uniform in [0, 1).
    [[nodiscard]] double nextDouble01() { return static_cast<double>(nextU64() >> 11u) * 0x1.0p-53; }

    // Uniform in [min, max].
    [[nodiscard]] float rangeFloat(float min, float max) { return min + (max - min) * nextFloat01(); }

    // Raw generator state, for serialization (determinism across save/load).
    struct RawState
    {
        std::uint64_t state = 0;
        std::uint64_t inc = 1;
    };

    [[nodiscard]] RawState rawState() const { return {m_state, m_inc}; }

    void setRawState(RawState raw)
    {
        m_state = raw.state;
        m_inc = raw.inc | 1u; // inc must stay odd for PCG correctness
    }

private:
    std::uint64_t m_state = 0;
    std::uint64_t m_inc = 1;
};

} // namespace sol::core
