#include "sol/rhi/gpu_profiler.hpp"

#include "sol/core/gpu_timestamp.hpp"
#include "sol/core/log.hpp"
#include "sol/core/profiler.hpp"

#include "vk_check.hpp"

namespace sol::rhi {

namespace {

// Two words per query: the counter value, then whether the device actually
// wrote it. Availability has to be asked for separately because 0 is a legal
// timestamp - the value alone can never tell you it is missing.
constexpr std::uint32_t kWordsPerQuery = 2;

// Both ends of a zone are written at ALL_COMMANDS rather than the cheaper
// TOP_OF_PIPE/BOTTOM_OF_PIPE pairing. TOP_OF_PIPE at the open is written as
// soon as prior commands REACH the top of the pipe, which on a pipelined GPU
// can be long before they have finished - so a zone opened that way silently
// includes the tail of the pass before it. ALL_COMMANDS at both ends brackets
// the zone's own work.
//
// The cost, stated because it is a real one: vkCmdWriteTimestamp2 inserts no
// barrier, but asking for ALL_COMMANDS means the value is captured only once
// prior work has completed, which can cost some of the overlap between
// adjacent passes. This instrument therefore perturbs what it measures a
// little. It is the honest direction to be wrong in - it over-reports rather
// than under-reports - and it is recorded as a known gap.
constexpr VkPipelineStageFlags2 kTimestampStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

} // namespace

GpuProfiler::~GpuProfiler()
{
    shutdown();
}

bool GpuProfiler::initialize(Context& context, std::uint32_t framesInFlight)
{
    shutdown();
    m_context = &context;

    if (framesInFlight == 0) {
        return false;
    }

    // A device that cannot timestamp is a degraded instrument, not a failed
    // startup: the game runs and the GPU rows read as absent. Context has
    // already logged which case this is.
    if (!context.supportsTimestamps()) {
        return true;
    }

    VkQueryPoolCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    createInfo.queryCount = kQueriesPerFrame * framesInFlight;
    SOL_VK_CHECK(vkCreateQueryPool(context.device(), &createInfo, nullptr, &m_pool));

    m_slots.resize(framesInFlight);
    for (Slot& slot : m_slots) {
        slot.events.reserve(kQueriesPerFrame);
    }
    m_readback.assign(static_cast<std::size_t>(kQueriesPerFrame) * kWordsPerQuery, 0);
    return true;
}

void GpuProfiler::shutdown()
{
    if (m_pool != VK_NULL_HANDLE && m_context != nullptr) {
        vkDestroyQueryPool(m_context->device(), m_pool, nullptr);
    }
    m_pool = VK_NULL_HANDLE;
    m_slots.clear();
    m_readback.clear();
    m_frameIndex = 0;
    m_recording = false;
    m_context = nullptr;
}

void GpuProfiler::publishPending()
{
    if (!available()) {
        return;
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(m_slots.size()); ++i) {
        Slot& slot = m_slots[i];
        if (slot.pending && resolve(slot, i)) {
            slot.pending = false;
        }
    }
}

void GpuProfiler::beginFrame(VkCommandBuffer commandBuffer, std::uint32_t frameIndex)
{
    if (!available() || frameIndex >= m_slots.size()) {
        m_recording = false;
        return;
    }

    m_frameIndex = frameIndex;
    Slot& slot = m_slots[frameIndex];

    slot.events.clear();
    slot.queryCount = 0;
    slot.pending = false;

    // Recorded on the device timeline rather than through vkResetQueryPool,
    // which would need the hostQueryReset feature the context deliberately
    // does not enable - changing which GPUs the game runs on, for a dev tool.
    vkCmdResetQueryPool(commandBuffer, m_pool, frameIndex * kQueriesPerFrame, kQueriesPerFrame);
    m_recording = true;
}

std::uint32_t GpuProfiler::beginZone(VkCommandBuffer commandBuffer, const char* name)
{
    if (!m_recording || name == nullptr) {
        return kInvalidGpuZone;
    }
    Slot& slot = m_slots[m_frameIndex];
    if (slot.queryCount >= kQueriesPerFrame) {
        return kInvalidGpuZone; // out of budget: costs a measurement, not correctness
    }

    const std::uint32_t query = slot.queryCount++;
    const std::uint32_t index = static_cast<std::uint32_t>(slot.events.size());
    slot.events.push_back(Event{.name = name, .query = query, .open = true});
    vkCmdWriteTimestamp2(commandBuffer, kTimestampStage, m_pool,
                         m_frameIndex * kQueriesPerFrame + query);
    return index;
}

void GpuProfiler::endZone(VkCommandBuffer commandBuffer, std::uint32_t zone)
{
    if (!m_recording || zone == kInvalidGpuZone) {
        return;
    }
    Slot& slot = m_slots[m_frameIndex];
    if (zone >= slot.events.size() || slot.queryCount >= kQueriesPerFrame) {
        return;
    }

    const std::uint32_t query = slot.queryCount++;
    slot.events.push_back(Event{.name = nullptr, .query = query, .open = false});
    vkCmdWriteTimestamp2(commandBuffer, kTimestampStage, m_pool,
                         m_frameIndex * kQueriesPerFrame + query);
    slot.pending = true;
}

bool GpuProfiler::resolve(Slot& slot, std::uint32_t frameIndex)
{
    if (slot.queryCount == 0) {
        return true; // nothing recorded: nothing to wait for
    }

    // No WAIT flag: the whole point of publishing a frame late is that this
    // read never blocks the CPU on the GPU. VK_NOT_READY is an ordinary
    // answer here, not an error - it means "ask again next frame".
    const std::size_t words = static_cast<std::size_t>(slot.queryCount) * kWordsPerQuery;
    const VkResult result = vkGetQueryPoolResults(
        m_context->device(), m_pool, frameIndex * kQueriesPerFrame, slot.queryCount,
        words * sizeof(std::uint64_t), m_readback.data(), kWordsPerQuery * sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        return true; // a real failure: drop it rather than retry forever
    }

    // All or nothing. Publishing a half-landed frame would put a real
    // duration on some zones and a zero on their siblings, which reads as a
    // pass that suddenly became free rather than as a missing measurement.
    for (std::uint32_t i = 0; i < slot.queryCount; ++i) {
        if (m_readback[static_cast<std::size_t>(i) * kWordsPerQuery + 1] == 0) {
            return false;
        }
    }

    core::Profiler& profiler = core::frameProfiler();
    const std::uint32_t validBits = m_context->timestampValidBits();
    const double period = static_cast<double>(m_context->timestampPeriod());

    // Replaying the recorded events is what rebuilds the tree: the CPU
    // profiler derives depth and parent from the order zones are opened in,
    // so opening them here in the same order gives the GPU rows the same
    // nesting they were recorded with.
    struct Open
    {
        std::uint32_t zone = core::kInvalidZone;
        std::uint32_t query = 0;
    };
    Open stack[kMaxZones];
    std::uint32_t depth = 0;

    for (const Event& event : slot.events) {
        if (event.open) {
            if (depth >= kMaxZones) {
                continue;
            }
            stack[depth] = Open{.zone = profiler.beginZone(event.name), .query = event.query};
            ++depth;
            continue;
        }
        if (depth == 0) {
            continue;
        }
        --depth;

        const std::size_t beginWord = static_cast<std::size_t>(stack[depth].query) * kWordsPerQuery;
        const std::size_t endWord = static_cast<std::size_t>(event.query) * kWordsPerQuery;
        const core::TimestampPair pair = {
            .begin = m_readback[beginWord],
            .end = m_readback[endWord],
            .beginAvailable = m_readback[beginWord + 1],
            .endAvailable = m_readback[endWord + 1],
        };

        double milliseconds = 0.0;
        if (core::resolveTimestampPair(pair, validBits, period, milliseconds)) {
            profiler.endZoneMeasured(stack[depth].zone, milliseconds);
        } else {
            // Unmeasured is not zero. Closing it with endZone would publish
            // this frame's CPU clock delta - a number from the wrong clock
            // entirely - so the zone is closed with an explicit nothing.
            profiler.endZoneMeasured(stack[depth].zone, 0.0);
        }
    }
    return true;
}

} // namespace sol::rhi
