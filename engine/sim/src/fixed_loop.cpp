#include "sol/sim/fixed_loop.hpp"

#include "sol/core/assert.hpp"

#include <algorithm>

namespace sol::sim {

FixedLoop::FixedLoop(double ticksPerSecond, std::uint32_t maxTicksPerFrame)
    : m_tickDelta(1.0 / ticksPerSecond)
    , m_maxTicksPerFrame(maxTicksPerFrame)
{
    SOL_ASSERT(ticksPerSecond > 0.0);
    SOL_ASSERT(maxTicksPerFrame > 0);
}

void FixedLoop::beginFrame(double frameDeltaSeconds)
{
    m_accumulator += std::max(frameDeltaSeconds, 0.0);
    const double cap = static_cast<double>(m_maxTicksPerFrame) * m_tickDelta;
    m_accumulator = std::min(m_accumulator, cap);
}

bool FixedLoop::shouldTick()
{
    if (m_accumulator < m_tickDelta) {
        return false;
    }
    m_accumulator -= m_tickDelta;
    ++m_tickCount;
    return true;
}

float FixedLoop::alpha() const
{
    return static_cast<float>(m_accumulator / m_tickDelta);
}

} // namespace sol::sim
