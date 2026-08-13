#include "Sol/Render/Renderer.h"

#include "Sol/Render/CapabilityRequirement.h"
#include "RenderMath.h"
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

/// Push constant block, matching Reference.vert exactly.
struct PushConstants {
    Mat4f viewProjection;
    float colour[4]{};
    float originAndRadius[4]{};
};
static_assert(sizeof(PushConstants) == 96,
              "Push constant block must match the shader's layout and stay within the 128-byte "
              "minimum every Vulkan implementation guarantees.");

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
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAllocation = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAllocation = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable{};
    std::array<VkFence, kFramesInFlight> inFlight{};

    std::vector<SceneObject> scene;
    std::uint32_t frameSlot = 0;
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
        const CameraState& camera);
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
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
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
    const CameraState& camera)
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

    const std::array<VkClearValue, 2> clearValues{
        VkClearValue{.color = {{0.01F, 0.012F, 0.02F, 1.0F}}},
        VkClearValue{.depthStencil = {kFarDepth, 0}},
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

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT16);

    const double aspect =
        static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Mat4f view = detail::cameraRelativeView(camera.forward, camera.up);
    const Mat4f projection = detail::reversedZInfinitePerspective(
        camera.verticalFovRadians, aspect, camera.nearPlaneMetres);
    const Mat4f viewProjection = detail::multiply(projection, view);

    for (const SceneObject& object : scene) {
        const Vec3f relative = detail::cameraRelative(object.worldPosition, camera.position);

        PushConstants push{};
        push.viewProjection = viewProjection;
        push.colour[0] = object.colour[0];
        push.colour[1] = object.colour[1];
        push.colour[2] = object.colour[2];
        push.colour[3] = 1.0F;
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

    vkCmdEndRenderPass(commandBuffer);

    if (const VkResult result = vkEndCommandBuffer(commandBuffer); result != VK_SUCCESS) {
        return std::unexpected(fail("Ending the command buffer failed", result));
    }
    return {};
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

        VkSurfaceFormatKHR chosen = formats.front();
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB
                && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = candidate;
                break;
            }
        }
        impl->swapchainFormat = chosen.format;
        impl->swapchainColorSpace = chosen.colorSpace;
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
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
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
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
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

    vkDestroyShaderModule(impl->device, *vertexModule, nullptr);
    vkDestroyShaderModule(impl->device, *fragmentModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
        return std::unexpected(fail("Creating the graphics pipeline failed", pipelineResult));
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
    Impl& impl = *m_impl;
    FrameStats stats{.frameIndex = impl.frameIndex, .swapchainRebuilt = false};

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

    // Reset only once the acquire succeeded. Resetting earlier and then returning early on
    // OUT_OF_DATE leaves an unsignalled fence that the next frame waits on forever.
    if (const VkResult result = vkResetFences(impl.device, 1, &impl.inFlight[slot]);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Resetting the frame fence failed", result));
    }

    if (const VkResult result = vkResetCommandBuffer(impl.commandBuffers[slot], 0);
        result != VK_SUCCESS) {
        return std::unexpected(fail("Resetting the command buffer failed", result));
    }

    if (auto recorded = impl.recordFrame(impl.commandBuffers[slot], imageIndex, camera);
        !recorded.has_value()) {
        return std::unexpected(recorded.error());
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

    impl.frameSlot = (slot + 1) % kFramesInFlight;
    ++impl.frameIndex;
    stats.frameIndex = impl.frameIndex;
    return stats;
}

} // namespace sol::render
