#pragma once

#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace sol::rhi {

class Swapchain
{
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    [[nodiscard]] bool create(Context& context, std::uint32_t width, std::uint32_t height);
    void destroy();

    // Caller must ensure the device is idle first.
    [[nodiscard]] bool recreate(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] VkFormat imageFormat() const { return m_imageFormat; }
    [[nodiscard]] VkExtent2D extent() const { return m_extent; }
    [[nodiscard]] std::uint32_t imageCount() const { return static_cast<std::uint32_t>(m_images.size()); }
    [[nodiscard]] VkImage image(std::uint32_t index) const { return m_images[index]; }
    [[nodiscard]] VkImageView imageView(std::uint32_t index) const { return m_imageViews[index]; }

    enum class PresentResult
    {
        Success,
        OutOfDate, // recreate the swapchain and try again next frame
        Failure,
    };

    [[nodiscard]] PresentResult acquireNextImage(VkSemaphore signalSemaphore, std::uint32_t& outImageIndex);
    [[nodiscard]] PresentResult present(VkSemaphore waitSemaphore, std::uint32_t imageIndex);

private:
    [[nodiscard]] bool createInternal(std::uint32_t width, std::uint32_t height);
    void destroyImageViews();

    Context* m_context = nullptr;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
    VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent = {};
};

} // namespace sol::rhi
