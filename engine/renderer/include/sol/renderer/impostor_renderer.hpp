#pragma once

#include "sol/core/math/math.hpp"
#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <string>

namespace sol::renderer {

// Analytic sphere impostors for celestial bodies (engine plan 2.4: planets
// are scenery, rendered scaled-space). A camera-facing quad is ray-traced
// against the sphere in the fragment shader:
//  - Planet: opaque, sun-lit, procedural surface, exact per-pixel depth.
//  - Star: emissive disc + additive glow halo, no depth write.
class ImpostorRenderer
{
public:
    struct Body
    {
        core::Vec3 centerRelative; // camera-relative, meters
        float radius = 1.0f;       // meters
        core::Vec3 colorA;         // planet: low-noise albedo / star: core
        core::Vec3 colorB;         // planet: high-noise albedo / star: glow
        core::Vec3 sunDirection;   // toward the sun (planet shading)
    };

    [[nodiscard]] bool initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                                  const char* shaderDirectory);
    void shutdown();
    [[nodiscard]] bool reloadPipeline();

    void drawPlanet(VkCommandBuffer commandBuffer, const core::Mat4& viewProjection,
                    const Body& body) const;
    void drawStar(VkCommandBuffer commandBuffer, const core::Mat4& viewProjection,
                  const Body& body) const;

private:
    void draw(VkCommandBuffer commandBuffer, VkPipeline pipeline, const core::Mat4& viewProjection,
              const Body& body, float mode, float quadScale) const;

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_planetPipeline = VK_NULL_HANDLE; // opaque, depth write
    VkPipeline m_starPipeline = VK_NULL_HANDLE;   // additive, no depth write
};

} // namespace sol::renderer
