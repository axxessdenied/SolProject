#pragma once

#include "sol/core/math/math.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sol::renderer {

// Camera-facing additive billboards for CPU-simulated particles (thrusters,
// impacts; engine plan 2.4 - GPU sim comes later). Positions are
// camera-relative floats; the caller rebases from sim space.
class ParticleRenderer
{
public:
    static constexpr std::uint32_t kMaxParticles = 4096;

    struct Particle
    {
        core::Vec3 position; // camera-relative, meters
        float size = 1.0f;   // world-space half size
        core::Vec4 color;    // linear HDR, additive
    };

    [[nodiscard]] bool initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                                  const char* shaderDirectory, std::uint32_t framesInFlight);
    void shutdown();
    [[nodiscard]] bool reloadPipeline();

    void draw(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
              const core::Mat4& viewProjection, const core::Quat& cameraOrientation,
              std::span<const Particle> particles);

private:
    struct Vertex
    {
        core::Vec3 position;
        core::Vec2 corner;
        core::Vec4 color;
        float size;
    };

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    std::vector<rhi::Buffer> m_vertexBuffers; // one per frame in flight
    std::vector<void*> m_mappedPointers;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace sol::renderer
