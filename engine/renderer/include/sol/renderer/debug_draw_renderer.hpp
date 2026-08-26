#pragma once

#include "sol/core/math/math.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::renderer {

// Immediate-mode debug lines (engine plan 2.4: debug draw from day one -
// space is empty and dark). Positions are camera-relative floats; callers
// rebase from sim space. Accumulate with line()/arrow()/axes() each frame,
// then draw() inside the HDR pass.
class DebugDrawRenderer
{
public:
    static constexpr std::uint32_t kMaxVertices = 8192;

    [[nodiscard]] bool initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  const char* shaderDirectory,
                                  std::uint32_t framesInFlight);
    void shutdown();
    [[nodiscard]] bool reloadPipeline();

    void clear() { m_vertices.clear(); }

    void line(core::Vec3 a, core::Vec3 b, core::Vec4 color);
    void arrow(core::Vec3 from, core::Vec3 to, core::Vec4 color);
    // RGB basis lines for an orientation at a position.
    void axes(core::Vec3 origin, const core::Quat& orientation, float length);

    // Uploads this frame's lines and records the draw.
    void draw(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, const core::Mat4& viewProjection);

private:
    struct Vertex
    {
        core::Vec3 position;
        core::Vec4 color;
    };

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    std::vector<Vertex> m_vertices;
    std::vector<rhi::Buffer> m_vertexBuffers; // one per frame in flight
    std::vector<void*> m_mappedPointers;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace sol::renderer
