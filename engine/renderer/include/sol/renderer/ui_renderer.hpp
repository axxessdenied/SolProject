#pragma once

#include "sol/core/math/vec.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sol::renderer {

// Screen-space quad renderer for the game UI (engine plan §2.9 / Phase 8d).
// Draws a batched stream of textured, tinted, scissor-clipped triangles in
// pixel coordinates with the origin at the top-left. It knows nothing about
// widgets - `sol::ui` builds the geometry, this puts it on screen.
class UiRenderer
{
public:
    // Vertex positions are pixels; color is straight-alpha RGBA8 (0xAABBGGRR
    // in memory order R,G,B,A) and is premultiplied in the shader.
    struct Vertex
    {
        core::Vec2 position;
        core::Vec2 uv;
        std::uint32_t color = 0xFFFFFFFFu;
    };

    // One draw: a run of indices sharing a texture and a clip rectangle.
    struct Batch
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t texture = 0; // index returned by registerTexture
        // Clip in pixels; an empty rect means "no clipping".
        core::Vec2 clipMin;
        core::Vec2 clipMax;
    };

    static constexpr std::uint32_t kMaxVertices = 65536;
    static constexpr std::uint32_t kMaxIndices = 98304;
    static constexpr std::uint32_t kMaxTextures = 16;

    // `depthFormat` must match the pass this draws into even though the UI
    // neither tests nor writes depth: a pipeline that disagrees with its
    // render pass's attachments is invalid.
    [[nodiscard]] bool initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  const char* shaderDirectory,
                                  std::uint32_t framesInFlight);
    void shutdown();
    [[nodiscard]] bool reloadPipeline();

    // Registers a sampled texture and returns the index batches refer to.
    // Texture 0 is always a 1x1 opaque white pixel, so solid fills need no
    // texture of their own.
    [[nodiscard]] std::uint32_t registerTexture(VkImageView view, VkSampler sampler);

    [[nodiscard]] std::uint32_t whiteTexture() const { return 0; }

    // Uploads this frame's geometry and records the batches. Geometry beyond
    // the fixed capacities is dropped with a warning rather than overrunning.
    //
    // `uiSize` is the virtual screen the geometry was laid out against and
    // sets the projection; `framebufferExtent` is the real target. They differ
    // when a UI scale is in effect, and clip rectangles are converted between
    // them - a scissor is always in real pixels.
    void draw(VkCommandBuffer commandBuffer,
              std::uint32_t frameIndex,
              core::Vec2 uiSize,
              VkExtent2D framebufferExtent,
              std::span<const Vertex> vertices,
              std::span<const std::uint16_t> indices,
              std::span<const Batch> batches);

private:
    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    std::vector<rhi::Buffer> m_vertexBuffers; // one per frame in flight
    std::vector<rhi::Buffer> m_indexBuffers;
    std::vector<void*> m_vertexMapped;
    std::vector<void*> m_indexMapped;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_textureSets;

    rhi::Image m_whiteImage;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace sol::renderer
