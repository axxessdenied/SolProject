#pragma once

#include "sol/core/math/math.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"

#include <cstdint>
#include <string>

namespace sol::renderer {

// Procedural starfield background: a per-seed cubemap (stars + milky-way
// band) generated on the CPU at initialize time, drawn as a fullscreen
// triangle after the opaque scene (reversed-Z: passes only where depth is
// still at the far clear).
class SkyRenderer
{
public:
    [[nodiscard]] bool initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                                  const char* shaderDirectory, std::uint64_t seed);
    void shutdown();
    [[nodiscard]] bool reloadPipeline();

    // Camera basis in world space; intensity scales the stored radiance into
    // HDR units.
    // `warpAxis` is the world-space direction the jump tunnel's streaks
    // converge on and `warp` their strength (0 at rest); see shaders/sky.frag.
    void draw(VkCommandBuffer commandBuffer, VkExtent2D extent, const core::Quat& cameraOrientation,
              float verticalFovRadians, float aspect, float intensity,
              const core::Vec3& warpAxis = {0.0f, 0.0f, -1.0f}, float warp = 0.0f) const;

private:
    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    rhi::Image m_cubemap;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace sol::renderer
