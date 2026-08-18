#pragma once

// GPU timestamp zones (engine plan Phase 8o). Phase 8n measured the CPU frame
// and found the game vsync-bound: ~0.7 ms of work against a 6.94 ms budget,
// with ~6.2 ms sitting inside render.submit as a WAIT. What it could not say
// is what the GPU was doing underneath that wait, because a CPU profiler
// cannot see across the submit boundary. This is that instrument.
//
// It lives in the RHI layer because a query pool is a device resource, the
// same category as descriptors.hpp and resources.hpp. It knows nothing about
// scene passes: the caller places the zones, because the caller is what knows
// where its passes are.
//
// Two things about it are not incidental:
//
//   A GPU number is always about a frame that has already ended. Reading
//   frame N's queries during frame N means vkGetQueryPoolResults with WAIT,
//   which blocks the CPU on the GPU - manufacturing exactly the stall this
//   exists to measure, and folding the instrument into its own reading.
//   Results are therefore read `framesInFlight` frames later, once the fence
//   for that slot has already been waited on by the ordinary frame path, so
//   the read is guaranteed non-blocking and costs nothing.
//
//   GPU passes overlap, so children do NOT sum to their parent. That is the
//   same rule core::Profiler already states for inclusive time - report the
//   tree, do not subtract - and it is more true here, not less.
//
// Results are published into core::Profiler via endZoneMeasured, so sol.perf
// prints one tree and a drive asserts a GPU budget with the binding it
// already has. Those rows are marked external, because their `last` column is
// stale by `framesInFlight` frames by construction.

#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace sol::rhi {

inline constexpr std::uint32_t kInvalidGpuZone = 0xFFFFFFFFu;

class GpuProfiler
{
public:
    // Sixteen is deliberately smaller than the CPU profiler's 64: a zone here
    // costs two device queries and a readback slot, and the frame has two
    // passes with a handful of stages inside them.
    static constexpr std::uint32_t kMaxZones = 16;

    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;
    GpuProfiler() = default;

    // Returns true even on a device that cannot timestamp - that is a
    // degraded instrument, not a failed startup. Check available().
    [[nodiscard]] bool initialize(Context& context, std::uint32_t framesInFlight);
    void shutdown();

    [[nodiscard]] bool available() const { return m_pool != VK_NULL_HANDLE; }

    // Publishes every slot whose device results have all landed into the CPU
    // profiler, and leaves the rest for a later frame. Reads without WAIT, so
    // it cannot block - an unavailable query simply stays pending.
    //
    // MUST be called at the top of the frame with no CPU zone open. The CPU
    // profiler derives a zone's depth and parent from the stack at the moment
    // the zone is first seen, and it keeps that parent forever; calling this
    // from inside the render zones would permanently graft the whole GPU tree
    // under render.record, which is both wrong and unfixable afterwards.
    void publishPending();

    // Once per frame, at the top of recording, with the frame slot about to
    // be recorded. Records the reset for this slot's queries and starts a new
    // event list. Any results this slot still had pending are dropped: the
    // reset below is about to invalidate them, and the ordinary frame path
    // waits on this slot's fence before recording, so a slot that comes round
    // still pending would mean that contract was already broken.
    void beginFrame(VkCommandBuffer commandBuffer, std::uint32_t frameIndex);

    // `name` must outlive the profiler - a string literal - and is published
    // into the CPU profiler's zone table unchanged, so it carries the `gpu.`
    // prefix the report is read by. Returns kInvalidGpuZone when the frame's
    // zone budget is spent, which endZone tolerates.
    [[nodiscard]] std::uint32_t beginZone(VkCommandBuffer commandBuffer, const char* name);
    void endZone(VkCommandBuffer commandBuffer, std::uint32_t zone);

private:
    static constexpr std::uint32_t kQueriesPerFrame = kMaxZones * 2;

    // One recorded open or close, kept in the order it was recorded so the
    // resolve can replay the nesting into the CPU profiler. Storing events
    // rather than pairs is what preserves the tree: a pair list would have
    // the durations and no idea which zone contained which.
    struct Event
    {
        const char* name = nullptr; // set on an open; null on a close
        std::uint32_t query = 0;
        bool open = false;
    };

    struct Slot
    {
        std::vector<Event> events;
        std::uint32_t queryCount = 0;
        bool pending = false; // has results waiting to be read
    };

    // Reads one slot's results and publishes them if every query landed.
    // Returns false - changing nothing - when they have not.
    [[nodiscard]] bool resolve(Slot& slot, std::uint32_t frameIndex);

    Context* m_context = nullptr;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    std::vector<Slot> m_slots;
    std::uint32_t m_frameIndex = 0;
    bool m_recording = false;
    std::vector<std::uint64_t> m_readback; // sized once; the frame path never allocates
};

} // namespace sol::rhi
