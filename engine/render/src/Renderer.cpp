#include "Sol/Render/Renderer.h"

#include "Sol/Render/CapabilityRequirement.h"
#include "RenderMath.h"
#include "TerrainLod.h"
#include "VulkanInstanceImpl.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace sol::render {
namespace {

using detail::Mat4f;
using detail::Vec3f;

/// Two frames in flight: enough to overlap CPU recording with GPU execution, few enough that
/// input-to-photon latency stays short. A third buys throughput the frame loop does not need.
constexpr std::uint32_t kFramesInFlight = 2;

/// Reversed-Z clears depth to zero — the far value. See RenderMath.h.
constexpr float kFarDepth = 0.0F;

constexpr std::array kReferenceVertices{
    Vec3f{-1.0F, -1.0F, -1.0F}, Vec3f{1.0F, -1.0F, -1.0F},
    Vec3f{1.0F, 1.0F, -1.0F},   Vec3f{-1.0F, 1.0F, -1.0F},
    Vec3f{-1.0F, -1.0F, 1.0F},  Vec3f{1.0F, -1.0F, 1.0F},
    Vec3f{1.0F, 1.0F, 1.0F},    Vec3f{-1.0F, 1.0F, 1.0F},
};

constexpr std::array<std::uint16_t, 36> kReferenceIndices{
    0, 1, 2, 2, 3, 0, // -Z
    5, 4, 7, 7, 6, 5, // +Z
    4, 0, 3, 3, 7, 4, // -X
    1, 5, 6, 6, 2, 1, // +X
    4, 5, 1, 1, 0, 4, // -Y
    3, 2, 6, 6, 7, 3, // +Y
};

constexpr std::uint32_t kReferenceVertexSpv[] =
#include "shaders/Reference.vert.inc"
    ;

constexpr std::uint32_t kReferenceFragmentSpv[] =
#include "shaders/Reference.frag.inc"
    ;

constexpr std::uint32_t kTerrainVertexSpv[] =
#include "shaders/Terrain.vert.inc"
    ;

constexpr std::uint32_t kTerrainFragmentSpv[] =
#include "shaders/Terrain.frag.inc"
    ;

/// Terrain buffer capacity, expressed as a patch budget so the two sizes cannot drift apart.
///
/// Terrain geometry is regenerated every frame as the camera moves, so its buffers are sized
/// once for the worst case rather than reallocated per frame. Reallocation would both cost time
/// and make the LOD gate's memory measurement meaningless — a working set that churns cannot be
/// shown to be bounded.
///
/// Previously the vertex and index capacities were independent round numbers in a 2:1 ratio,
/// while a patch emits 81 vertices and 384 indices — 4.74:1. The index buffer therefore always
/// exhausted first, at 15 625 patches, and roughly 55 MB of the vertex allocation could never
/// be addressed at any camera position.
///
/// 4 096 patches is 4.06x the peak of 1 008 measured on the LOD gate's descent, which is margin
/// without being an order of magnitude of dead allocation. At 32 bytes per vertex and 4 bytes per
/// index that is ~10.1 MiB and ~6.0 MiB, and there is one set **per frame in flight**.
///
/// The peak is a function of the quality setting, not a constant, so it has to be re-measured
/// when that setting moves. The 874 this replaces was recorded in the same commit that raised the
/// gate's `subdivisionFactor` from 0.6 to 3.0 and does not correspond to any measurement at 3.0 —
/// a stale number carried past the change that invalidated it, which is the failure this note
/// exists to stop repeating.
constexpr VkDeviceSize kMaxTerrainPatches = 4096;
constexpr VkDeviceSize kTerrainVerticesPerPatch = 81;  // (grid + 1)^2 for grid = 8
constexpr VkDeviceSize kTerrainIndicesPerPatch = 384;  // grid^2 * 6
constexpr VkDeviceSize kTerrainVertexCapacity = kMaxTerrainPatches * kTerrainVerticesPerPatch;
constexpr VkDeviceSize kTerrainIndexCapacity = kMaxTerrainPatches * kTerrainIndicesPerPatch;

/// Push constant block, matching Reference.vert exactly.
struct PushConstants {
    Mat4f viewProjection;
    float colour[4]{};
    float originAndRadius[4]{};
};
static_assert(sizeof(PushConstants) == 96,
              "Push constant block must match the shader's layout and stay within the 128-byte "
              "minimum every Vulkan implementation guarantees.");

/// Surface colour formats this renderer will present through, in preference order.
///
/// A closed list rather than a preference, because two separate things depend on it and both
/// fail silently rather than loudly:
///
/// - **sRGB.** An sRGB surface makes the display hardware apply the transfer function, so shader
///   output stays linear. A UNORM surface double-applies gamma, and every pixel gate — the
///   jitter centroid especially — would then be measured through a tone response nothing in the
///   harness models. The measurement would still produce a number.
/// - **32 bits, 8 per channel.** @ref Renderer::Impl::readCapture sizes its staging buffer at
///   4 bytes per pixel and reads back byte triples. A wider surface such as
///   `R16G16B16A16_SFLOAT` would make the buffer-image copy exceed the allocation.
///
/// Widening this list means revisiting the readback path with it. A later milestone may want a
/// UNORM entry with an explicit shader-side transfer function, once presentation is a product
/// concern rather than a measurement one; today it is a measurement one.
constexpr std::array kAcceptedSurfaceFormats{
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_R8G8B8A8_SRGB,
};

/// Names the formats this renderer reasons about; anything else prints as its numeric value.
///
/// Deliberately not a general `VkFormat` table. This exists for one diagnostic, and a table
/// covering every format would be a maintenance burden that the one caller does not need.
std::string describeSurfaceFormat(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "VK_FORMAT_R16G16B16A16_SFLOAT";
    default: return std::format("VkFormat {}", static_cast<int>(format));
    }
}

/// The matrix this frame draws with.
///
/// Computed in one place and handed to both the terrain selection and the draw, rather than
/// derived twice. The frustum the selection culls against has to be the frustum the draw
/// rasterises with; recomputing it from the same inputs would *usually* agree, and a cull that
/// disagrees with its own draw removes visible geometry.
Mat4f viewProjectionFor(const CameraState& camera, VkExtent2D extent)
{
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Mat4f view = detail::cameraRelativeView(camera.forward, camera.up);
    const Mat4f projection =
        camera.projection == ProjectionMode::ReversedZInfinite
            ? detail::reversedZInfinitePerspective(
                  camera.verticalFovRadians, aspect, camera.nearPlaneMetres)
            : detail::conventionalPerspective(camera.verticalFovRadians,
                                              aspect,
                                              camera.nearPlaneMetres,
                                              camera.farPlaneMetres);
    return detail::multiply(projection, view);
}

std::string fail(std::string_view what, VkResult result)
{
    return std::format("{}: {}.", what, detail::describeResult(result));
}

} // namespace

struct Renderer::Impl {
    // Borrowed. The instance outlives the renderer by contract.
    VkInstance instance = VK_NULL_HANDLE;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    DeviceCapabilities capabilities;
    std::uint32_t queueFamilyIndex = 0;

    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    /// Heaps on the selected device. Read once, because the per-frame allocation query writes
    /// one budget per heap and summing past the count would add uninitialised entries.
    std::uint32_t memoryHeapCount = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    /// Chosen once, before the render pass is created, and never re-chosen. The render pass
    /// bakes in the colour format, so a swapchain rebuild that picked a different one would
    /// silently become incompatible with every framebuffer.
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkFramebuffer> framebuffers;
    /// One per swapchain image, not per frame in flight. A per-frame signal semaphore can be
    /// reused while a previous present still owns it when the driver returns images out of
    /// order, which validation reports; keying on the image removes the possibility.
    std::vector<VkSemaphore> renderFinished;

    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    /// Reversed-Z: depth compares GREATER and clears to 0.
    VkPipeline pipeline = VK_NULL_HANDLE;
    /// Conventional: depth compares LESS and clears to 1. Negative control only. Vulkan 1.2
    /// has no dynamic depth-compare state, so the control needs its own pipeline rather than
    /// a state change — which is a fair price for a test that can fail.
    VkPipeline conventionalPipeline = VK_NULL_HANDLE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;

    /// Readback staging, sized to the swapchain and rebuilt with it.
    VkBuffer captureBuffer = VK_NULL_HANDLE;
    VmaAllocation captureAllocation = VK_NULL_HANDLE;
    void* captureMapped = nullptr;
    VkDeviceSize captureSize = 0;

    VkBuffer depthCaptureBuffer = VK_NULL_HANDLE;
    VmaAllocation depthCaptureAllocation = VK_NULL_HANDLE;
    void* depthCaptureMapped = nullptr;
    VkDeviceSize depthCaptureSize = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable{};
    std::array<VkFence, kFramesInFlight> inFlight{};

    VkPipeline terrainPipeline = VK_NULL_HANDLE;
    /// One set per frame in flight.
    ///
    /// A single shared set was a data race. `submitFrame` waits on `inFlight[slot]`, which is
    /// the fence for the frame that used that slot *two* submissions ago; the frame submitted
    /// immediately before is still executing and may still be fetching vertices and indices
    /// when the CPU overwrites them. The result would be geometry drawn from a mixture of two
    /// frames, with index and vertex-offset pairs that no longer describe the patch layout
    /// they were recorded against.
    ///
    /// It was invisible to every existing gate, which is the uncomfortable part: all three
    /// pixel-measuring harnesses go through `renderFrameCaptured`, which waits on the
    /// just-submitted fence and so accidentally serialises to one frame in flight.
    std::array<VkBuffer, kFramesInFlight> terrainVertexBuffers{};
    std::array<VmaAllocation, kFramesInFlight> terrainVertexAllocations{};
    std::array<void*, kFramesInFlight> terrainVertexMapped{};
    std::array<VkBuffer, kFramesInFlight> terrainIndexBuffers{};
    std::array<VmaAllocation, kFramesInFlight> terrainIndexAllocations{};
    std::array<void*, kFramesInFlight> terrainIndexMapped{};

    std::optional<TerrainSettings> terrain;
    detail::TerrainFrame terrainFrame;

    std::vector<SceneObject> scene;
    std::uint32_t frameSlot = 0;
    /// Which slot's fence the last submit signals. The capture path waits on this one rather
    /// than on the next slot, which has not been submitted.
    std::uint32_t lastSubmittedSlot = 0;
    std::uint64_t frameIndex = 0;
    std::uint32_t pendingWidth = 0;
    std::uint32_t pendingHeight = 0;
    bool needsRebuild = false;

    ~Impl() { destroy(); }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void destroySwapchainResources()
    {
        for (VkFramebuffer framebuffer : framebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        framebuffers.clear();

        for (VkSemaphore semaphore : renderFinished) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        renderFinished.clear();

        for (VkImageView view : swapchainViews) {
            vkDestroyImageView(device, view, nullptr);
        }
        swapchainViews.clear();
        swapchainImages.clear();

        if (depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, depthView, nullptr);
            depthView = VK_NULL_HANDLE;
        }
        if (depthImage != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, depthImage, depthAllocation);
            depthImage = VK_NULL_HANDLE;
            depthAllocation = VK_NULL_HANDLE;
        }

        if (captureBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, captureBuffer, captureAllocation);
            captureBuffer = VK_NULL_HANDLE;
            captureAllocation = VK_NULL_HANDLE;
            captureMapped = nullptr;
            captureSize = 0;
        }
        if (depthCaptureBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, depthCaptureBuffer, depthCaptureAllocation);
            depthCaptureBuffer = VK_NULL_HANDLE;
            depthCaptureAllocation = VK_NULL_HANDLE;
            depthCaptureMapped = nullptr;
            depthCaptureSize = 0;
        }
    }

    void destroy()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            destroySwapchainResources();

            if (swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device, swapchain, nullptr);
                swapchain = VK_NULL_HANDLE;
            }
            for (VkSemaphore semaphore : imageAvailable) {
                vkDestroySemaphore(device, semaphore, nullptr);
            }
            for (VkFence fence : inFlight) {
                vkDestroyFence(device, fence, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
            }
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (conventionalPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, conventionalPipeline, nullptr);
            }
            if (terrainPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, terrainPipeline, nullptr);
            }
            for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
                if (terrainVertexBuffers[i] != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(
                        allocator, terrainVertexBuffers[i], terrainVertexAllocations[i]);
                }
                if (terrainIndexBuffers[i] != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(
                        allocator, terrainIndexBuffers[i], terrainIndexAllocations[i]);
                }
            }
            if (pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            }
            if (renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device, renderPass, nullptr);
            }
            if (vertexBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, vertexBuffer, vertexAllocation);
            }
            if (indexBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, indexBuffer, indexAllocation);
            }
            if (allocator != VK_NULL_HANDLE) {
                vmaDestroyAllocator(allocator);
                allocator = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }

        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] std::expected<void, std::string> buildSwapchain();
    [[nodiscard]] std::expected<void, std::string> recordFrame(
        VkCommandBuffer commandBuffer,
        std::uint32_t imageIndex,
        const CameraState& camera,
        const Mat4f& viewProjection,
        bool capture);
    [[nodiscard]] std::expected<FrameStats, std::string> submitFrame(
        const CameraState& camera,
        bool capture);
    [[nodiscard]] CapturedFrame readCapture() const;
};

namespace {

std::expected<VkShaderModule, std::string> createShaderModule(
    VkDevice device,
    const std::uint32_t* code,
    std::size_t byteSize)
{
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = byteSize,
        .pCode = code,
    };

    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateShaderModule(device, &info, nullptr, &module);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating a shader module failed", result));
    }
    return module;
}

} // namespace

std::expected<void, std::string> Renderer::Impl::buildSwapchain()
{
    vkDeviceWaitIdle(device);
    destroySwapchainResources();

    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    if (const VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, surface, &surfaceCapabilities);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Querying surface capabilities failed", result));
    }

    // 0xFFFFFFFF means "the surface defers to the swapchain", so the window's own size is
    // used; otherwise the surface dictates and the window's size must be clamped into range.
    if (surfaceCapabilities.currentExtent.width != UINT32_MAX) {
        extent = surfaceCapabilities.currentExtent;
    } else {
        extent.width = std::clamp(pendingWidth,
                                  surfaceCapabilities.minImageExtent.width,
                                  surfaceCapabilities.maxImageExtent.width);
        extent.height = std::clamp(pendingHeight,
                                   surfaceCapabilities.minImageExtent.height,
                                   surfaceCapabilities.maxImageExtent.height);
    }

    if (extent.width == 0 || extent.height == 0) {
        // Minimised. Not an error; the frame loop skips rendering until it is restored.
        return {};
    }

    std::uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, surfaceCapabilities.maxImageCount);
    }

    const VkSwapchainKHR oldSwapchain = swapchain;
    const VkSwapchainCreateInfoKHR swapchainInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = surface,
        .minImageCount = imageCount,
        .imageFormat = swapchainFormat,
        .imageColorSpace = swapchainColorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = surfaceCapabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        // FIFO, not mailbox. It is the only present mode Vulkan guarantees, and more
        // importantly it presents every frame exactly once at a fixed cadence — the jitter
        // harness measures per-frame centroids, and a mode that drops frames would put
        // presentation variance into a measurement that is supposed to isolate the renderer.
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain,
    };

    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult swapchainResult =
        vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &created);

    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    }
    swapchain = VK_NULL_HANDLE;

    if (swapchainResult != VK_SUCCESS) {
        return std::unexpected(fail("Creating the swapchain failed", swapchainResult));
    }
    swapchain = created;

    if (const VkResult result = detail::enumerateInto(
            swapchainImages,
            [this](std::uint32_t* count, VkImage* data) {
                return vkGetSwapchainImagesKHR(device, swapchain, count, data);
            });
        result != VK_SUCCESS) {
        return std::unexpected(fail("Querying swapchain images failed", result));
    }

    swapchainViews.reserve(swapchainImages.size());
    for (VkImage image : swapchainImages) {
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchainFormat,
            .components = {},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        if (const VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &view);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Creating a swapchain image view failed", result));
        }
        swapchainViews.push_back(view);
    }

    // Depth buffer. D32_SFLOAT is required by the capability set precisely so this cannot
    // fall back to a narrower format behind the depth gate's back.
    const VkImageCreateInfo depthInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // TRANSFER_SRC so the depth gate can read the buffer back and measure it. A depth
        // value under the reversed-Z projection is an analytic function of distance, so
        // reading it turns "does depth behave" from an inspection into a measurement.
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo depthAllocInfo{
        .usage = VMA_MEMORY_USAGE_AUTO,
        .priority = 1.0F,
    };
    if (const VkResult result = vmaCreateImage(
            allocator, &depthInfo, &depthAllocInfo, &depthImage, &depthAllocation, nullptr);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Allocating the depth buffer failed", result));
    }

    const VkImageViewCreateInfo depthViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .components = {},
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    if (const VkResult result = vkCreateImageView(device, &depthViewInfo, nullptr, &depthView);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating the depth image view failed", result));
    }

    framebuffers.reserve(swapchainViews.size());
    for (VkImageView view : swapchainViews) {
        const std::array attachments{view, depthView};
        const VkFramebufferCreateInfo framebufferInfo{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = renderPass,
            .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
            .pAttachments = attachments.data(),
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (const VkResult result =
                vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Creating a framebuffer failed", result));
        }
        framebuffers.push_back(framebuffer);
    }

    // Readback staging, sized to the swapchain. Persistently mapped: the jitter harness reads
    // one of these per frame for hundreds of frames, and re-mapping each time would add cost
    // to the measurement path for no benefit.
    captureSize = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    const VkBufferCreateInfo captureInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = captureSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    const VmaAllocationCreateInfo captureAllocInfo{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo captureAllocationInfo{};
    if (const VkResult result = vmaCreateBuffer(allocator,
                                                &captureInfo,
                                                &captureAllocInfo,
                                                &captureBuffer,
                                                &captureAllocation,
                                                &captureAllocationInfo);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Allocating the readback buffer failed", result));
    }
    captureMapped = captureAllocationInfo.pMappedData;

    depthCaptureSize = static_cast<VkDeviceSize>(extent.width) * extent.height * sizeof(float);
    const VkBufferCreateInfo depthCaptureInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = depthCaptureSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VmaAllocationInfo depthCaptureAllocationInfo{};
    if (const VkResult result = vmaCreateBuffer(allocator,
                                                &depthCaptureInfo,
                                                &captureAllocInfo,
                                                &depthCaptureBuffer,
                                                &depthCaptureAllocation,
                                                &depthCaptureAllocationInfo);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Allocating the depth readback buffer failed", result));
    }
    depthCaptureMapped = depthCaptureAllocationInfo.pMappedData;

    renderFinished.reserve(swapchainImages.size());
    for (std::size_t i = 0; i < swapchainImages.size(); ++i) {
        const VkSemaphoreCreateInfo semaphoreInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (const VkResult result =
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Creating a present semaphore failed", result));
        }
        renderFinished.push_back(semaphore);
    }

    needsRebuild = false;
    return {};
}

std::expected<void, std::string> Renderer::Impl::recordFrame(
    VkCommandBuffer commandBuffer,
    std::uint32_t imageIndex,
    const CameraState& camera,
    const Mat4f& viewProjection,
    bool capture)
{
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (const VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Beginning the command buffer failed", result));
    }

    // Reversed Z clears depth to 0 (far); the conventional control clears to 1.
    const bool reversedZ = camera.projection == ProjectionMode::ReversedZInfinite;
    const std::array<VkClearValue, 2> clearValues{
        VkClearValue{.color = {{0.01F, 0.012F, 0.02F, 1.0F}}},
        VkClearValue{.depthStencil = {reversedZ ? kFarDepth : 1.0F, 0}},
    };

    const VkRenderPassBeginInfo renderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = renderPass,
        .framebuffer = framebuffers[imageIndex],
        .renderArea = {{0, 0}, extent},
        .clearValueCount = static_cast<std::uint32_t>(clearValues.size()),
        .pClearValues = clearValues.data(),
    };
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer,
                      VK_PIPELINE_BIND_POINT_GRAPHICS,
                      reversedZ ? pipeline : conventionalPipeline);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT16);

    for (const SceneObject& object : scene) {
        const Vec3f relative = detail::cameraRelative(object.worldPosition, camera.position);

        PushConstants push{};
        push.viewProjection = viewProjection;
        push.colour[0] = object.colour[0];
        push.colour[1] = object.colour[1];
        push.colour[2] = object.colour[2];
        // Alpha carries the shading mode rather than opacity: blending is disabled, so the
        // channel is otherwise unused, and this keeps the push-constant block at 96 bytes.
        push.colour[3] = object.smoothMarker ? 1.0F : 0.0F;
        push.originAndRadius[0] = relative.x;
        push.originAndRadius[1] = relative.y;
        push.originAndRadius[2] = relative.z;
        push.originAndRadius[3] = static_cast<float>(object.radiusMetres);

        vkCmdPushConstants(commandBuffer,
                           pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0,
                           sizeof(PushConstants),
                           &push);
        vkCmdDrawIndexed(
            commandBuffer, static_cast<std::uint32_t>(kReferenceIndices.size()), 1, 0, 0, 0);
    }

    // Terrain has only a reversed-Z pipeline. Drawing it under the depth gate's conventional
    // projection would depth-test GREATER against a buffer cleared to 1.0 and produce
    // nonsense, so it is skipped rather than drawn wrongly. Latent today — the depth gate uses
    // no terrain — but the control's whole value is that it differs in exactly one variable,
    // and a silently broken second variable would destroy that.
    if (terrain.has_value() && !terrainFrame.patches.empty() && reversedZ) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipeline);
        const VkDeviceSize terrainOffset = 0;
        // This frame's own buffers, not a shared pair — see the note on terrainVertexBuffers.
        vkCmdBindVertexBuffers(
            commandBuffer, 0, 1, &terrainVertexBuffers[frameSlot], &terrainOffset);
        vkCmdBindIndexBuffer(
            commandBuffer, terrainIndexBuffers[frameSlot], 0, VK_INDEX_TYPE_UINT32);

        for (const detail::TerrainPatch& patch : terrainFrame.patches) {
            PushConstants push{};
            push.viewProjection = viewProjection;
            push.colour[0] = patch.morphStartDistance;
            push.colour[1] = patch.morphEndDistance;
            push.colour[2] = patch.morphEnabled;
            push.colour[3] = static_cast<float>(patch.level);

            vkCmdPushConstants(commandBuffer,
                               pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0,
                               sizeof(PushConstants),
                               &push);
            vkCmdDrawIndexed(
                commandBuffer, patch.indexCount, 1, patch.firstIndex, patch.vertexOffset, 0);
        }
    }

    vkCmdEndRenderPass(commandBuffer);

    if (capture && captureBuffer != VK_NULL_HANDLE) {
        // The render pass left the image in PRESENT_SRC. Move it to TRANSFER_SRC, copy, and
        // move it back, so the presented image and the captured image are the same pixels.
        // Rendering the measurement offscreen instead would measure a different image from
        // the one the screen-space gate is about.
        const auto barrier = [&](VkImageLayout from,
                                 VkImageLayout to,
                                 VkAccessFlags srcAccess,
                                 VkAccessFlags dstAccess,
                                 VkPipelineStageFlags srcStage,
                                 VkPipelineStageFlags dstStage) {
            const VkImageMemoryBarrier imageBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = srcAccess,
                .dstAccessMask = dstAccess,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchainImages[imageIndex],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            vkCmdPipelineBarrier(
                commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
        };

        barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);

        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.width, extent.height, 1},
        };
        vkCmdCopyImageToBuffer(commandBuffer,
                               swapchainImages[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               captureBuffer,
                               1,
                               &region);

        barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT,
                0,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // Depth, the same way. The render pass stored it and left it in the attachment
        // layout, so it only has to move to TRANSFER_SRC and back.
        const auto depthBarrier = [&](VkImageLayout from,
                                      VkImageLayout to,
                                      VkAccessFlags srcAccess,
                                      VkAccessFlags dstAccess,
                                      VkPipelineStageFlags srcStage,
                                      VkPipelineStageFlags dstStage) {
            const VkImageMemoryBarrier imageBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = srcAccess,
                .dstAccessMask = dstAccess,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = depthImage,
                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
            };
            vkCmdPipelineBarrier(
                commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
        };

        depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);

        const VkBufferImageCopy depthRegion{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.width, extent.height, 1},
        };
        vkCmdCopyImageToBuffer(commandBuffer,
                               depthImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               depthCaptureBuffer,
                               1,
                               &depthRegion);

        depthBarrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    }

    if (const VkResult result = vkEndCommandBuffer(commandBuffer); result != VK_SUCCESS) {
        return std::unexpected(fail("Ending the command buffer failed", result));
    }
    return {};
}

CapturedFrame Renderer::Impl::readCapture() const
{
    CapturedFrame frame;
    if (captureMapped == nullptr || captureSize == 0) {
        return frame;
    }

    frame.width = extent.width;
    frame.height = extent.height;
    frame.rgba.resize(static_cast<std::size_t>(captureSize));

    const auto* source = static_cast<const std::uint8_t*>(captureMapped);

    // The surface is very likely B8G8R8A8, so the bytes arrive as B, G, R, A. Normalising to
    // RGBA here means the harness never has to know the surface's channel order — and a
    // harness that guessed wrong would compute a centroid from the wrong channel.
    //
    // Only the two formats in kAcceptedSurfaceFormats can reach here, and device creation fails
    // otherwise — which is what makes the 4-bytes-per-pixel stride above a fact rather than an
    // assumption. The UNORM case this used to test for is unreachable by construction now, and
    // testing for it would suggest the stride had been thought about for a format that never
    // arrives.
    const bool swapRedAndBlue = swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;

    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
        if (swapRedAndBlue) {
            frame.rgba[i + 0] = source[i + 2];
            frame.rgba[i + 1] = source[i + 1];
            frame.rgba[i + 2] = source[i + 0];
        } else {
            frame.rgba[i + 0] = source[i + 0];
            frame.rgba[i + 1] = source[i + 1];
            frame.rgba[i + 2] = source[i + 2];
        }
        frame.rgba[i + 3] = source[i + 3];
    }

    if (depthCaptureMapped != nullptr && depthCaptureSize > 0) {
        frame.depth.resize(static_cast<std::size_t>(extent.width) * extent.height);
        std::memcpy(frame.depth.data(), depthCaptureMapped, static_cast<std::size_t>(depthCaptureSize));
    }

    return frame;
}

std::expected<Renderer, std::string> Renderer::create(
    const VulkanInstance& instance,
    const SurfaceTarget& target)
{
    auto impl = std::make_unique<Impl>();
    impl->instance = instance.m_impl->instance;
    impl->pendingWidth = target.width;
    impl->pendingHeight = target.height;

    // --- Surface -----------------------------------------------------------------------
    const VkWin32SurfaceCreateInfoKHR surfaceInfo{
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .hinstance = static_cast<HINSTANCE>(target.nativeInstance),
        .hwnd = static_cast<HWND>(target.nativeWindow),
    };
    if (const VkResult result =
            vkCreateWin32SurfaceKHR(impl->instance, &surfaceInfo, nullptr, &impl->surface);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating the window surface failed", result));
    }

    // --- Device selection --------------------------------------------------------------
    std::vector<VkPhysicalDevice> handles;
    if (const VkResult result = detail::enumerateInto(
            handles,
            [&impl](std::uint32_t* count, VkPhysicalDevice* data) {
                return vkEnumeratePhysicalDevices(impl->instance, count, data);
            });
        result != VK_SUCCESS) {
        return std::unexpected(fail("Enumerating physical devices failed", result));
    }

    const CapabilityRequirement requirement = baselineRequirement();
    std::string rejectionReport;
    bool selected = false;

    for (VkPhysicalDevice handle : handles) {
        auto described = detail::describeDevice(handle);
        if (!described.has_value()) {
            return std::unexpected(described.error());
        }

        const auto rejections = checkCapabilities(requirement, *described);
        if (!rejections.empty()) {
            rejectionReport += formatRejections(described->deviceName, rejections);
            continue;
        }

        // Presentation support was answered generically during enumeration; this asks about
        // *this* surface, which is the question that actually matters and can differ.
        std::uint32_t chosenFamily = 0;
        bool foundFamily = false;
        for (const QueueFamily& family : described->queueFamilies) {
            if (!family.graphics || family.count == 0) {
                continue;
            }
            VkBool32 presentSupported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                handle, family.index, impl->surface, &presentSupported);
            if (presentSupported == VK_TRUE) {
                chosenFamily = family.index;
                foundFamily = true;
                break;
            }
        }
        if (!foundFamily) {
            rejectionReport += std::format(
                "{}: no queue family can both draw and present to this window.\n",
                described->deviceName);
            continue;
        }

        // Prefer a discrete GPU, but accept the first acceptable device otherwise. On a hybrid
        // laptop this choice is recorded rather than assumed, because which device serves a
        // surface is a driver policy and every evidence report must name the one used.
        const bool better = !selected || described->kind == DeviceKind::DiscreteGpu;
        if (better) {
            impl->physicalDevice = handle;
            impl->capabilities = std::move(*described);
            impl->queueFamilyIndex = chosenFamily;
            selected = true;
            if (impl->capabilities.kind == DeviceKind::DiscreteGpu) {
                break;
            }
        }
    }

    if (!selected) {
        return std::unexpected(std::format(
            "No Vulkan device on this system can run the renderer.\n\n{}", rejectionReport));
    }

    // --- Logical device ----------------------------------------------------------------
    const float queuePriority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = impl->queueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };
    const std::array deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkPhysicalDeviceFeatures enabledFeatures{};
    const VkDeviceCreateInfo deviceInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
        .pEnabledFeatures = &enabledFeatures,
    };
    if (const VkResult result =
            vkCreateDevice(impl->physicalDevice, &deviceInfo, nullptr, &impl->device);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating the logical device failed", result));
    }

    // Device-level entry points now dispatch directly rather than through the loader's
    // trampoline, which removes a per-call indirection from every draw.
    volkLoadDevice(impl->device);
    vkGetDeviceQueue(impl->device, impl->queueFamilyIndex, 0, &impl->queue);

    // --- Allocator ---------------------------------------------------------------------
    const VmaVulkanFunctions vulkanFunctions{
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo allocatorInfo{
        .physicalDevice = impl->physicalDevice,
        .device = impl->device,
        .pVulkanFunctions = &vulkanFunctions,
        .instance = impl->instance,
        .vulkanApiVersion = VK_API_VERSION_1_2,
    };
    if (const VkResult result = vmaCreateAllocator(&allocatorInfo, &impl->allocator);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating the memory allocator failed", result));
    }

    // Heap count, read once here rather than per frame — see the allocation query in submitFrame.
    {
        const VkPhysicalDeviceMemoryProperties* memoryProperties = nullptr;
        vmaGetMemoryProperties(impl->allocator, &memoryProperties);
        impl->memoryHeapCount = memoryProperties->memoryHeapCount;
    }

    // --- Surface format ----------------------------------------------------------------
    // Chosen before the render pass, because the render pass bakes the colour format in.
    // An sRGB surface makes the display hardware apply the transfer function, so shader output
    // stays linear; a UNORM surface would silently double-apply gamma, and the jitter harness
    // would then measure centroids through a wrong tone response.
    {
        std::vector<VkSurfaceFormatKHR> formats;
        if (const VkResult result = detail::enumerateInto(
                formats,
                [&impl](std::uint32_t* count, VkSurfaceFormatKHR* data) {
                    return vkGetPhysicalDeviceSurfaceFormatsKHR(
                        impl->physicalDevice, impl->surface, count, data);
                });
            result != VK_SUCCESS) {
            return std::unexpected(fail("Querying surface formats failed", result));
        }
        if (formats.empty()) {
            return std::unexpected(
                "The window surface reports no supported formats, which indicates a driver "
                "or compositor fault rather than an unsupported device.");
        }

        const VkSurfaceFormatKHR* chosen = nullptr;
        for (const VkFormat accepted : kAcceptedSurfaceFormats) {
            for (const VkSurfaceFormatKHR& candidate : formats) {
                if (candidate.format == accepted
                    && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    chosen = &candidate;
                    break;
                }
            }
            if (chosen != nullptr) {
                break;
            }
        }

        // Rejected rather than accepted-and-quietly-wrong. The previous version fell back to
        // `formats.front()`, so a surface offering neither accepted format was taken anyway —
        // and the two consequences are exactly the ones the comment above and the capture path
        // depend on not happening. A silent wrong tone response is worse than a refusal to
        // start, because the refusal is visible and the tone response is not.
        //
        // How close that was: on the RTX 4060 this surface lists, in order,
        // B8G8R8A8_UNORM, B8G8R8A8_SRGB, R8G8B8A8_UNORM, R8G8B8A8_SRGB, A2B10G10R10_UNORM_PACK32.
        // `formats.front()` is the **UNORM** entry. The old code reached the correct format only
        // because the sRGB one happened to be offered and its search found it first; the
        // fallback it would otherwise have taken is precisely the gamma-doubling case, and the
        // packed 10-bit entry would have kept the 4-byte stride while breaking the byte triples.
        if (chosen == nullptr) {
            std::string offered;
            for (const VkSurfaceFormatKHR& candidate : formats) {
                offered += std::format("  {} in colour space {}\n",
                                       describeSurfaceFormat(candidate.format),
                                       static_cast<int>(candidate.colorSpace));
            }
            return std::unexpected(std::format(
                "This window surface offers no colour format the renderer can present through.\n"
                "  Required: VK_FORMAT_B8G8R8A8_SRGB or VK_FORMAT_R8G8B8A8_SRGB, in "
                "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR.\n"
                "  An sRGB surface is required so the display hardware applies the transfer "
                "function and shader output stays linear; a UNORM surface would double-apply "
                "gamma and every pixel measurement would be taken through a tone response "
                "nothing models.\n"
                "  A 32-bit, 8-bit-per-channel format is required because the capture path "
                "sizes its staging buffer at 4 bytes per pixel.\n"
                "  This surface offered:\n{}",
                offered));
        }

        impl->swapchainFormat = chosen->format;
        impl->swapchainColorSpace = chosen->colorSpace;
    }

    // --- Render pass -------------------------------------------------------------------
    const std::array<VkAttachmentDescription, 2> attachments{
        VkAttachmentDescription{
            .flags = 0,
            .format = impl->swapchainFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        VkAttachmentDescription{
            .flags = 0,
            .format = VK_FORMAT_D32_SFLOAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            // STORE, not DONT_CARE, so the depth gate can read the buffer after the pass. A
            // production renderer with no readback would discard it; this costs write
            // bandwidth every frame and is therefore included in any frame-time figure this
            // renderer produces, which is recorded rather than quietly absorbed.
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    const VkAttachmentReference colourRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colourRef,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = &depthRef,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    const VkSubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        // LATE_FRAGMENT_TESTS as well as EARLY, and a real srcAccessMask.
        //
        // One depth image is shared by both frames in flight, and the previous frame writes
        // depth in both the early and late fragment-test stages. Waiting only on the earlier
        // of the two, with srcAccessMask = 0, gives an execution dependency without making
        // those writes available — so the write-after-write hazard against this frame's
        // LOAD_OP_CLEAR was not correctly scoped.
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dependencyFlags = 0,
    };
    const VkRenderPassCreateInfo renderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = static_cast<std::uint32_t>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    if (const VkResult result =
            vkCreateRenderPass(impl->device, &renderPassInfo, nullptr, &impl->renderPass);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating the render pass failed", result));
    }

    // --- Pipeline ----------------------------------------------------------------------
    auto vertexModule = createShaderModule(
        impl->device, kReferenceVertexSpv, sizeof(kReferenceVertexSpv));
    if (!vertexModule.has_value()) {
        return std::unexpected(vertexModule.error());
    }
    auto fragmentModule = createShaderModule(
        impl->device, kReferenceFragmentSpv, sizeof(kReferenceFragmentSpv));
    if (!fragmentModule.has_value()) {
        vkDestroyShaderModule(impl->device, *vertexModule, nullptr);
        return std::unexpected(fragmentModule.error());
    }

    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
    };
    if (const VkResult result = vkCreatePipelineLayout(
            impl->device, &layoutInfo, nullptr, &impl->pipelineLayout);
        result != VK_SUCCESS) {
        vkDestroyShaderModule(impl->device, *vertexModule, nullptr);
        vkDestroyShaderModule(impl->device, *fragmentModule, nullptr);
        return std::unexpected(fail("Creating the pipeline layout failed", result));
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = *vertexModule,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = *fragmentModule,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    };

    const VkVertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(Vec3f),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const VkVertexInputAttributeDescription attribute{
        .location = 0,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = 0,
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 1,
        .pVertexAttributeDescriptions = &attribute,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        // No culling. The reference cube is a depth-behaviour probe, and back-face culling
        // would make a winding mistake in the index list present as missing geometry —
        // an avoidable way to misread a depth result. Culling is a content-pipeline concern,
        // not something the depth gate measures.
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0F,
        .depthBiasClamp = 0.0F,
        .depthBiasSlopeFactor = 0.0F,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0F,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    // GREATER, not LESS: reversed Z means a nearer fragment has the larger depth value.
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0.0F,
        .maxDepthBounds = 1.0F,
    };
    const VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo colourBlend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
        .blendConstants = {0.0F, 0.0F, 0.0F, 0.0F},
    };
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamicState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };

    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colourBlend,
        .pDynamicState = &dynamicState,
        .layout = impl->pipelineLayout,
        .renderPass = impl->renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    const VkResult pipelineResult = vkCreateGraphicsPipelines(
        impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl->pipeline);

    // The negative control's pipeline differs in exactly one field, which is the point: it
    // isolates the depth arrangement and changes nothing else about the render.
    VkPipelineDepthStencilStateCreateInfo conventionalDepth = depthStencil;
    conventionalDepth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkGraphicsPipelineCreateInfo conventionalInfo = pipelineInfo;
    conventionalInfo.pDepthStencilState = &conventionalDepth;

    const VkResult conventionalResult = vkCreateGraphicsPipelines(
        impl->device, VK_NULL_HANDLE, 1, &conventionalInfo, nullptr, &impl->conventionalPipeline);

    vkDestroyShaderModule(impl->device, *vertexModule, nullptr);
    vkDestroyShaderModule(impl->device, *fragmentModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
        return std::unexpected(fail("Creating the graphics pipeline failed", pipelineResult));
    }
    if (conventionalResult != VK_SUCCESS) {
        return std::unexpected(
            fail("Creating the depth negative-control pipeline failed", conventionalResult));
    }

    // --- Terrain pipeline ---------------------------------------------------------------
    {
        auto terrainVertex =
            createShaderModule(impl->device, kTerrainVertexSpv, sizeof(kTerrainVertexSpv));
        if (!terrainVertex.has_value()) {
            return std::unexpected(terrainVertex.error());
        }
        auto terrainFragment =
            createShaderModule(impl->device, kTerrainFragmentSpv, sizeof(kTerrainFragmentSpv));
        if (!terrainFragment.has_value()) {
            vkDestroyShaderModule(impl->device, *terrainVertex, nullptr);
            return std::unexpected(terrainFragment.error());
        }

        const std::array<VkPipelineShaderStageCreateInfo, 2> terrainStages{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = *terrainVertex,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = *terrainFragment,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
        };

        const VkVertexInputBindingDescription terrainBinding{
            .binding = 0,
            .stride = sizeof(detail::TerrainVertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        const std::array<VkVertexInputAttributeDescription, 4> terrainAttributes{
            VkVertexInputAttributeDescription{
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(detail::TerrainVertex, position)},
            VkVertexInputAttributeDescription{
                .location = 1,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(detail::TerrainVertex, coarsePosition)},
            VkVertexInputAttributeDescription{
                .location = 2,
                .binding = 0,
                .format = VK_FORMAT_R32_SFLOAT,
                .offset = offsetof(detail::TerrainVertex, heightUnit)},
            VkVertexInputAttributeDescription{
                .location = 3,
                .binding = 0,
                .format = VK_FORMAT_R32_SFLOAT,
                .offset = offsetof(detail::TerrainVertex, coarseHeightUnit)},
        };
        const VkPipelineVertexInputStateCreateInfo terrainVertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &terrainBinding,
            .vertexAttributeDescriptionCount =
                static_cast<std::uint32_t>(terrainAttributes.size()),
            .pVertexAttributeDescriptions = terrainAttributes.data(),
        };

        VkGraphicsPipelineCreateInfo terrainInfo = pipelineInfo;
        terrainInfo.pStages = terrainStages.data();
        terrainInfo.pVertexInputState = &terrainVertexInput;


        const VkResult terrainResult = vkCreateGraphicsPipelines(
            impl->device, VK_NULL_HANDLE, 1, &terrainInfo, nullptr, &impl->terrainPipeline);

        vkDestroyShaderModule(impl->device, *terrainVertex, nullptr);
        vkDestroyShaderModule(impl->device, *terrainFragment, nullptr);

        if (terrainResult != VK_SUCCESS) {
            return std::unexpected(fail("Creating the terrain pipeline failed", terrainResult));
        }
    }

    // --- Geometry ----------------------------------------------------------------------
    const auto uploadBuffer = [&impl](const void* data,
                                      VkDeviceSize size,
                                      VkBufferUsageFlags usage,
                                      VkBuffer& buffer,
                                      VmaAllocation& allocation) -> std::expected<void, std::string> {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        // Host-writable device memory. The reference geometry is 128 bytes and never changes,
        // so a staging buffer and a transfer submit would be machinery without a payoff. Real
        // geometry, and the upload-volume measurement P1b requires, will need staging.
        const VmaAllocationCreateInfo allocInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo allocationInfo{};
        if (const VkResult result = vmaCreateBuffer(
                impl->allocator, &bufferInfo, &allocInfo, &buffer, &allocation, &allocationInfo);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Allocating a geometry buffer failed", result));
        }
        std::memcpy(allocationInfo.pMappedData, data, static_cast<std::size_t>(size));
        if (const VkResult result = vmaFlushAllocation(impl->allocator, allocation, 0, size);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Flushing a geometry buffer failed", result));
        }
        return {};
    };

    if (auto uploaded = uploadBuffer(kReferenceVertices.data(),
                                     sizeof(kReferenceVertices),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     impl->vertexBuffer,
                                     impl->vertexAllocation);
        !uploaded.has_value()) {
        return std::unexpected(uploaded.error());
    }
    if (auto uploaded = uploadBuffer(kReferenceIndices.data(),
                                     sizeof(kReferenceIndices),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                     impl->indexBuffer,
                                     impl->indexAllocation);
        !uploaded.has_value()) {
        return std::unexpected(uploaded.error());
    }

    // Terrain buffers: allocated once at worst-case capacity and written in place each frame.
    // Growing them on demand would make the LOD gate's memory half untestable, because a
    // working set that reallocates cannot be shown to be bounded.
    {
        const auto createPersistent = [&impl](VkDeviceSize size,
                                              VkBufferUsageFlags usage,
                                              VkBuffer& buffer,
                                              VmaAllocation& allocation,
                                              void*& mapped) -> std::expected<void, std::string> {
            const VkBufferCreateInfo bufferInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = size,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            };
            const VmaAllocationCreateInfo allocInfo{
                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
            };
            VmaAllocationInfo allocationInfo{};
            if (const VkResult result = vmaCreateBuffer(
                    impl->allocator, &bufferInfo, &allocInfo, &buffer, &allocation, &allocationInfo);
                result != VK_SUCCESS) {
                return std::unexpected(fail("Allocating a terrain buffer failed", result));
            }
            mapped = allocationInfo.pMappedData;
            return {};
        };

        for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (auto created =
                    createPersistent(kTerrainVertexCapacity * sizeof(detail::TerrainVertex),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     impl->terrainVertexBuffers[i],
                                     impl->terrainVertexAllocations[i],
                                     impl->terrainVertexMapped[i]);
                !created.has_value()) {
                return std::unexpected(created.error());
            }
            if (auto created = createPersistent(kTerrainIndexCapacity * sizeof(std::uint32_t),
                                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                impl->terrainIndexBuffers[i],
                                                impl->terrainIndexAllocations[i],
                                                impl->terrainIndexMapped[i]);
                !created.has_value()) {
                return std::unexpected(created.error());
            }
        }
    }

    // --- Commands and synchronisation ---------------------------------------------------
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = impl->queueFamilyIndex,
    };
    if (const VkResult result =
            vkCreateCommandPool(impl->device, &poolInfo, nullptr, &impl->commandPool);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Creating the command pool failed", result));
    }

    const VkCommandBufferAllocateInfo commandInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = impl->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = kFramesInFlight,
    };
    if (const VkResult result =
            vkAllocateCommandBuffers(impl->device, &commandInfo, impl->commandBuffers.data());
        result != VK_SUCCESS) {
        return std::unexpected(fail("Allocating command buffers failed", result));
    }

    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        const VkSemaphoreCreateInfo semaphoreInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        if (const VkResult result = vkCreateSemaphore(
                impl->device, &semaphoreInfo, nullptr, &impl->imageAvailable[i]);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Creating an acquire semaphore failed", result));
        }
        // Created signalled so the first frame does not wait on a fence nothing will submit.
        const VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        if (const VkResult result =
                vkCreateFence(impl->device, &fenceInfo, nullptr, &impl->inFlight[i]);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Creating a frame fence failed", result));
        }
    }

    if (auto built = impl->buildSwapchain(); !built.has_value()) {
        return std::unexpected(built.error());
    }

    return Renderer{std::move(impl)};
}

Renderer::Renderer(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

const DeviceCapabilities& Renderer::selectedDevice() const
{
    return m_impl->capabilities;
}

void Renderer::setScene(std::vector<SceneObject> objects)
{
    m_impl->scene = std::move(objects);
}

void Renderer::setTerrain(std::optional<TerrainSettings> terrain)
{
    m_impl->terrain = std::move(terrain);
}

void Renderer::notifyResized(std::uint32_t width, std::uint32_t height)
{
    m_impl->pendingWidth = width;
    m_impl->pendingHeight = height;
    m_impl->needsRebuild = true;
}

void Renderer::waitIdle()
{
    if (m_impl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_impl->device);
    }
}

std::expected<FrameStats, std::string> Renderer::renderFrame(const CameraState& camera)
{
    return m_impl->submitFrame(camera, false);
}

std::expected<CapturedFrame, std::string> Renderer::renderFrameCaptured(
    const CameraState& camera)
{
    auto stats = m_impl->submitFrame(camera, true);
    if (!stats.has_value()) {
        return std::unexpected(stats.error());
    }
    if (stats->swapchainRebuilt || !stats->presented) {
        // The frame was skipped or the swapchain changed under it; there is nothing valid to
        // read. Returning an empty capture lets the caller discard the sample rather than
        // measure a stale or half-sized buffer.
        //
        // `presented` is the half that was missing. A minimised window returns early from
        // `submitFrame` without rebuilding anything, so `swapchainRebuilt` stays false, and the
        // capture buffers still hold the *previous* frame — which this path would then wait on
        // an already-signalled fence for, invalidate, and hand back as a full, non-empty frame.
        // The jitter gate would count the duplicate as a sample with zero centroid deviation;
        // the LOD gate would score a zero difference and pull down the median that its outlier
        // floor and every ratio are measured against.
        return CapturedFrame{};
    }

    // Waiting here is what makes the capture readable, and it is also why this path must never
    // produce frame-time evidence: it removes the CPU/GPU overlap entirely.
    if (const VkResult result = vkWaitForFences(
            m_impl->device, 1, &m_impl->inFlight[m_impl->lastSubmittedSlot], VK_TRUE, UINT64_MAX);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Waiting for the captured frame failed", result));
    }
    if (const VkResult result = vmaInvalidateAllocation(
            m_impl->allocator, m_impl->captureAllocation, 0, m_impl->captureSize);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Invalidating the readback buffer failed", result));
    }
    if (const VkResult result = vmaInvalidateAllocation(
            m_impl->allocator, m_impl->depthCaptureAllocation, 0, m_impl->depthCaptureSize);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Invalidating the depth readback buffer failed", result));
    }

    return m_impl->readCapture();
}

std::expected<FrameStats, std::string> Renderer::Impl::submitFrame(
    const CameraState& camera,
    bool capture)
{
    Impl& impl = *this;
    FrameStats stats{.frameIndex = impl.frameIndex, .swapchainRebuilt = false};

    // Checked before anything else, including the minimised early return, so a malformed camera
    // is reported the first time it is passed rather than the first time the window happens to
    // have area. See isViewBasisWellFormed for why this is refused rather than fixed up.
    if (!detail::isViewBasisWellFormed(camera.forward, camera.up)) {
        return std::unexpected(std::format(
            "The camera's forward and up vectors do not define a view basis.\n"
            "  forward = ({}, {}, {}), up = ({}, {}, {})\n"
            "  They are parallel, or one of them is zero, so the camera's roll about its own "
            "view direction is undefined rather than merely awkward.\n"
            "  This is refused rather than resolved with a substitute up axis, because a "
            "substitute would render a plausible image at an arbitrary roll — and a measurement "
            "taken in a rotated frame is still a number.",
            camera.forward.x,
            camera.forward.y,
            camera.forward.z,
            camera.up.x,
            camera.up.y,
            camera.up.z));
    }

    if (impl.pendingWidth == 0 || impl.pendingHeight == 0) {
        return stats; // Minimised: a normal state, not an error.
    }

    if (impl.needsRebuild || impl.swapchain == VK_NULL_HANDLE) {
        if (auto built = impl.buildSwapchain(); !built.has_value()) {
            return std::unexpected(built.error());
        }
        stats.swapchainRebuilt = true;
        if (impl.swapchain == VK_NULL_HANDLE || impl.extent.width == 0) {
            return stats;
        }
    }

    const std::uint32_t slot = impl.frameSlot;

    if (const VkResult result =
            vkWaitForFences(impl.device, 1, &impl.inFlight[slot], VK_TRUE, UINT64_MAX);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Waiting for the frame fence failed", result));
    }

    std::uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(
        impl.device, impl.swapchain, UINT64_MAX, impl.imageAvailable[slot], VK_NULL_HANDLE,
        &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        // The surface changed under us. Rebuild and let the next call render; presenting to a
        // stale swapchain is the failure this check exists to prevent.
        impl.needsRebuild = true;
        stats.swapchainRebuilt = true;
        return stats;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(fail("Acquiring a swapchain image failed", acquireResult));
    }

    if (const VkResult result = vkResetCommandBuffer(impl.commandBuffers[slot], 0);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Resetting the command buffer failed", result));
    }

    // One matrix for the whole frame: the frustum terrain is culled against below, and the
    // matrix the draw rasterises with, are the same object rather than two derivations.
    const Mat4f viewProjection = viewProjectionFor(camera, impl.extent);

    // Terrain is selected and generated on the CPU before recording, because the camera
    // position determines which patches exist at all. The buffers are written in place; the
    // fence waited on above guarantees the GPU is done with the previous frame's contents.
    if (impl.terrain.has_value()) {
        detail::TerrainConfig config;
        config.radiusMetres = impl.terrain->radiusMetres;
        config.reliefMetres = impl.terrain->reliefMetres;
        config.maxLevel = impl.terrain->maxLevel;
        config.subdivisionFactor = impl.terrain->subdivisionFactor;
        config.morphEnabled = impl.terrain->morphEnabled;

        const detail::Vec3d cameraPlanetRelative{
            camera.position.x - impl.terrain->centre.x,
            camera.position.y - impl.terrain->centre.y,
            camera.position.z - impl.terrain->centre.z,
        };

        detail::buildTerrain(config,
                             cameraPlanetRelative,
                             detail::extractFrustum(viewProjection),
                             impl.terrainFrame);

        if (impl.terrainFrame.vertices.size() > kTerrainVertexCapacity
            || impl.terrainFrame.indices.size() > kTerrainIndexCapacity) {
            return std::unexpected(std::format(
                "Terrain selection produced {} vertices and {} indices, over the {} / {} "
                "capacity.\n"
                "  Silently dropping patches would corrupt the LOD gate's measurement, so this "
                "is reported instead.",
                impl.terrainFrame.vertices.size(),
                impl.terrainFrame.indices.size(),
                kTerrainVertexCapacity,
                kTerrainIndexCapacity));
        }

        const VkDeviceSize vertexBytes =
            impl.terrainFrame.vertices.size() * sizeof(detail::TerrainVertex);
        const VkDeviceSize indexBytes =
            impl.terrainFrame.indices.size() * sizeof(std::uint32_t);

        std::memcpy(impl.terrainVertexMapped[slot], impl.terrainFrame.vertices.data(),
                    static_cast<std::size_t>(vertexBytes));
        std::memcpy(impl.terrainIndexMapped[slot], impl.terrainFrame.indices.data(),
                    static_cast<std::size_t>(indexBytes));

        // Flush. VMA may place these on host-visible memory that is not host-coherent, which
        // is legal and does happen; without the flush the writes are not guaranteed visible to
        // the device. The one-shot reference-geometry upload already did this and the
        // per-frame path did not.
        if (const VkResult result = vmaFlushAllocation(
                impl.allocator, impl.terrainVertexAllocations[slot], 0, vertexBytes);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Flushing the terrain vertex buffer failed", result));
        }
        if (const VkResult result = vmaFlushAllocation(
                impl.allocator, impl.terrainIndexAllocations[slot], 0, indexBytes);
            result != VK_SUCCESS) {
            return std::unexpected(fail("Flushing the terrain index buffer failed", result));
        }

        stats.terrainPatches = static_cast<std::uint32_t>(impl.terrainFrame.patches.size());
        stats.terrainNodesVisited = impl.terrainFrame.nodesVisited;
        stats.terrainVertices = static_cast<std::uint32_t>(impl.terrainFrame.vertices.size());
    }

    if (auto recorded = impl.recordFrame(
            impl.commandBuffers[slot], imageIndex, camera, viewProjection, capture);
        !recorded.has_value()) {
        return std::unexpected(recorded.error());
    }

    // Reset the fence last, immediately before the submit that will signal it again.
    //
    // Every step between a reset and its submit is a place where an error return leaves the
    // fence unsignalled with nothing left to signal it — and the next frame landing on this
    // slot then blocks in `vkWaitForFences(..., UINT64_MAX)` for ever. The terrain-capacity
    // check is a reachable example: it reports an error the caller is invited to log and carry
    // on from, and carrying on would deadlock. Narrowing the window to the submit call itself
    // removes every one of those paths.
    if (const VkResult result = vkResetFences(impl.device, 1, &impl.inFlight[slot]);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Resetting the frame fence failed", result));
    }

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &impl.imageAvailable[slot],
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &impl.commandBuffers[slot],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &impl.renderFinished[imageIndex],
    };
    if (const VkResult result =
            vkQueueSubmit(impl.queue, 1, &submitInfo, impl.inFlight[slot]);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Submitting the frame failed", result));
    }

    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &impl.renderFinished[imageIndex],
        .swapchainCount = 1,
        .pSwapchains = &impl.swapchain,
        .pImageIndices = &imageIndex,
        .pResults = nullptr,
    };
    const VkResult presentResult = vkQueuePresentKHR(impl.queue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        impl.needsRebuild = true;
    } else if (presentResult != VK_SUCCESS) {
        return std::unexpected(fail("Presenting the frame failed", presentResult));
    }

    // Device memory actually held, from the allocator rather than from process memory. The
    // LOD gate's memory half needs a figure that reflects the renderer's own working set;
    // process memory is too noisy to show boundedness over a long traverse.
    //
    // `vmaGetHeapBudgets`, not `vmaCalculateStatistics`, and the distinction is VMA's own: the
    // one named "calculate" has to walk every block and allocation under the allocator's mutexes
    // and is documented for debugging use, while the one named "get" is documented as fast
    // enough to call every frame. This runs in `renderFrame`, which is the function the header
    // designates as the *only* valid source of frame-time evidence — so the expensive form put a
    // full allocator traversal inside the measurement it was going to be measured by. Nothing
    // times frames yet, which is the only reason this never showed up as a number.
    //
    // The figure is the same one either way: summing `allocationBytes` across heaps is what the
    // total in `VmaTotalStatistics` is. Verified unchanged at 43.03 MiB against the LOD gate.
    {
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(impl.allocator, budgets.data());

        for (std::uint32_t heap = 0; heap < impl.memoryHeapCount; ++heap) {
            const VmaStatistics& heapStatistics = budgets[heap].statistics;
            stats.deviceAllocatedBytes += heapStatistics.allocationBytes;
            stats.deviceBlockBytes += heapStatistics.blockBytes;
            stats.deviceAllocationCount += heapStatistics.allocationCount;
            stats.deviceBlockCount += heapStatistics.blockCount;
        }
    }

    impl.lastSubmittedSlot = slot;
    impl.frameSlot = (slot + 1) % kFramesInFlight;
    ++impl.frameIndex;
    stats.frameIndex = impl.frameIndex;
    // Set here and nowhere else: this is the only point reached with an image submitted and
    // presented, so every early return above leaves it false by construction rather than by
    // each path remembering to clear it.
    stats.presented = true;
    return stats;
}

} // namespace sol::render
