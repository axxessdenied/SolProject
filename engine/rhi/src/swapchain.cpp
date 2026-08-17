#include "sol/rhi/swapchain.hpp"

#include "vk_check.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"

#include <algorithm>
#include <vector>

namespace sol::rhi {

Swapchain::~Swapchain()
{
    destroy();
}

bool Swapchain::create(Context& context, std::uint32_t width, std::uint32_t height, bool vsync)
{
    SOL_ASSERT(m_swapchain == VK_NULL_HANDLE);
    m_context = &context;
    return createInternal(width, height, vsync);
}

bool Swapchain::recreate(std::uint32_t width, std::uint32_t height, bool vsync)
{
    SOL_ASSERT(m_context != nullptr);
    destroyImageViews();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context->device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    return createInternal(width, height, vsync);
}

// FIFO is the only mode the spec guarantees, so it is both the vsync-on answer
// and the floor when vsync is off: MAILBOX first (tears nothing, drops frames),
// then IMMEDIATE (tears, uncapped), then give up and stay capped rather than
// pass a mode the surface never offered.
VkPresentModeKHR Swapchain::choosePresentMode(bool vsync) const
{
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    std::uint32_t modeCount = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_context->physicalDevice(), m_context->surface(),
                                                  &modeCount, nullptr) != VK_SUCCESS ||
        modeCount == 0) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_context->physicalDevice(), m_context->surface(),
                                                  &modeCount, modes.data()) != VK_SUCCESS) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    for (const VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR,
                                             VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
            return preferred;
        }
    }
    SOL_LOG_WARN("Surface offers no unsynchronized present mode; V-Sync stays on");
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool Swapchain::createInternal(std::uint32_t width, std::uint32_t height, bool vsync)
{
    const VkPhysicalDevice physicalDevice = m_context->physicalDevice();
    const VkSurfaceKHR surface = m_context->surface();
    const VkDevice device = m_context->device();

    VkSurfaceCapabilitiesKHR capabilities = {};
    SOL_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities));

    // Surface format: prefer sRGB BGRA8, else take the first offered.
    std::uint32_t formatCount = 0;
    SOL_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    SOL_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data()));
    SOL_VERIFY(!formats.empty());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = format;
            break;
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width =
            std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height =
            std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        SOL_LOG_WARN("Swapchain extent is zero (minimized?); creation skipped");
        return false;
    }

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = choosePresentMode(vsync);
    createInfo.clipped = VK_TRUE;

    SOL_VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_swapchain));

    m_imageFormat = surfaceFormat.format;
    m_extent = extent;

    std::uint32_t actualImageCount = 0;
    SOL_VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &actualImageCount, nullptr));
    m_images.resize(actualImageCount);
    SOL_VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &actualImageCount, m_images.data()));

    m_imageViews.resize(actualImageCount);
    for (std::uint32_t i = 0; i < actualImageCount; ++i) {
        VkImageViewCreateInfo viewCreateInfo = {};
        viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCreateInfo.image = m_images[i];
        viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCreateInfo.format = m_imageFormat;
        viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCreateInfo.subresourceRange.levelCount = 1;
        viewCreateInfo.subresourceRange.layerCount = 1;
        SOL_VK_CHECK(vkCreateImageView(device, &viewCreateInfo, nullptr, &m_imageViews[i]));
    }

    return true;
}

void Swapchain::destroyImageViews()
{
    for (VkImageView view : m_imageViews) {
        vkDestroyImageView(m_context->device(), view, nullptr);
    }
    m_imageViews.clear();
    m_images.clear();
}

void Swapchain::destroy()
{
    if (m_context == nullptr) {
        return;
    }
    destroyImageViews();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context->device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_context = nullptr;
}

Swapchain::PresentResult Swapchain::acquireNextImage(VkSemaphore signalSemaphore,
                                                     std::uint32_t& outImageIndex)
{
    const VkResult result = vkAcquireNextImageKHR(
        m_context->device(), m_swapchain, UINT64_MAX, signalSemaphore, VK_NULL_HANDLE, &outImageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return PresentResult::OutOfDate;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        SOL_LOG_ERROR("vkAcquireNextImageKHR failed (%d)", static_cast<int>(result));
        return PresentResult::Failure;
    }
    return PresentResult::Success;
}

Swapchain::PresentResult Swapchain::present(VkSemaphore waitSemaphore, std::uint32_t imageIndex)
{
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult result = vkQueuePresentKHR(m_context->graphicsQueue(), &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return PresentResult::OutOfDate;
    }
    if (result != VK_SUCCESS) {
        SOL_LOG_ERROR("vkQueuePresentKHR failed (%d)", static_cast<int>(result));
        return PresentResult::Failure;
    }
    return PresentResult::Success;
}

} // namespace sol::rhi
