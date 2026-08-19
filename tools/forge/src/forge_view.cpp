#include "forge_view.hpp"

#include "sol/core/log.hpp"
#include "sol/renderer/scene_pass.hpp"

namespace forge {

using namespace sol;

namespace {

#define FORGE_VK_CHECK(expression)                                                                   \
    do {                                                                                             \
        const VkResult forgeVkResult_ = (expression);                                                \
        if (forgeVkResult_ != VK_SUCCESS) {                                                          \
            SOL_LOG_FATAL("Vulkan call failed (%d): %s", static_cast<int>(forgeVkResult_),           \
                          #expression);                                                              \
        }                                                                                            \
    } while (0)

// Near-black rather than pure black: an unlit face against a pure black
// clear is invisible, and "I cannot see that face" is exactly the question a
// viewer exists to answer.
constexpr VkClearColorValue kViewportClearColor = {{0.012f, 0.014f, 0.018f, 1.0f}};
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

} // namespace

bool ForgeView::initialize(rhi::Context& context, rhi::Swapchain& swapchain,
                           const char* shaderDirectory)
{
    m_context = &context;
    m_swapchain = &swapchain;

    if (!m_meshRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory) ||
        !m_debugDraw.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory,
                                kFramesInFlight) ||
        !m_tonemapRenderer.initialize(context, swapchain.imageFormat(), kDepthFormat,
                                      shaderDirectory)) {
        return false;
    }

    m_hdrColor = rhi::createColorTarget(context, swapchain.extent(), kHdrFormat);
    m_tonemapRenderer.setSource(m_hdrColor.view);
    m_depth = rhi::createDepthImage(context, swapchain.extent());

    const VkDevice device = context.device();
    for (FrameResources& frame : m_frames) {
        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = context.graphicsQueueFamily();
        FORGE_VK_CHECK(vkCreateCommandPool(device, &poolCreateInfo, nullptr, &frame.commandPool));

        VkCommandBufferAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = frame.commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        FORGE_VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &frame.commandBuffer));

        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        FORGE_VK_CHECK(
            vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &frame.imageAvailable));

        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        FORGE_VK_CHECK(vkCreateFence(device, &fenceCreateInfo, nullptr, &frame.inFlight));
    }

    return createPerImageSemaphores();
}

bool ForgeView::createPerImageSemaphores()
{
    m_renderFinished.resize(m_swapchain->imageCount());
    for (VkSemaphore& semaphore : m_renderFinished) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        FORGE_VK_CHECK(
            vkCreateSemaphore(m_context->device(), &semaphoreCreateInfo, nullptr, &semaphore));
    }
    return true;
}

void ForgeView::destroyPerImageSemaphores()
{
    for (VkSemaphore semaphore : m_renderFinished) {
        vkDestroySemaphore(m_context->device(), semaphore, nullptr);
    }
    m_renderFinished.clear();
}

bool ForgeView::onSwapchainRecreated()
{
    destroyPerImageSemaphores();
    rhi::destroyImage(*m_context, m_depth);
    m_depth = rhi::createDepthImage(*m_context, m_swapchain->extent());
    rhi::destroyImage(*m_context, m_hdrColor);
    m_hdrColor = rhi::createColorTarget(*m_context, m_swapchain->extent(), kHdrFormat);
    m_tonemapRenderer.setSource(m_hdrColor.view);
    return createPerImageSemaphores();
}

void ForgeView::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();

    destroyPerImageSemaphores();
    for (FrameResources& frame : m_frames) {
        vkDestroyFence(device, frame.inFlight, nullptr);
        vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        vkDestroyCommandPool(device, frame.commandPool, nullptr);
        frame = {};
    }

    rhi::destroyImage(*m_context, m_depth);
    rhi::destroyImage(*m_context, m_hdrColor);
    m_meshRenderer.shutdown();
    m_debugDraw.shutdown();
    m_tonemapRenderer.shutdown();
    m_context = nullptr;
    m_swapchain = nullptr;
}

void ForgeView::recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                               const FrameDesc& frame)
{
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    FORGE_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    const VkExtent2D extent = m_swapchain->extent();
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    // The near plane has to reach an arm's length as well as a station: 5 cm,
    // which is the game's own, and reversed-Z infinite far carries the rest.
    const core::Mat4 projection = core::perspectiveInfiniteReversedZ(kCameraVerticalFov, aspect, 0.05f);
    const core::Mat4 viewProjection = projection * frame.view;

    renderer::beginHdrScenePass(commandBuffer, m_hdrColor, m_depth, kViewportClearColor);

    m_meshRenderer.bind(commandBuffer, extent);
    for (const DrawItem& item : frame.items) {
        if (item.mesh == nullptr || item.texture == nullptr) {
            continue;
        }
        m_meshRenderer.draw(commandBuffer, *item.mesh, *item.texture, viewProjection * item.model,
                            item.model, item.emissive);
    }

    m_debugDraw.draw(commandBuffer, m_frameIndex, viewProjection);
    m_debugDraw.clear();

    renderer::endHdrScenePass(commandBuffer, m_hdrColor);

    renderer::beginPresentPass(commandBuffer, *m_swapchain, imageIndex, m_depth);
    m_tonemapRenderer.draw(commandBuffer, extent, frame.exposure);
    if (m_imguiHost != nullptr) {
        m_imguiHost->render(commandBuffer);
    }
    renderer::endPresentPass(commandBuffer, *m_swapchain, imageIndex);

    FORGE_VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

ForgeView::DrawResult ForgeView::drawFrame(const FrameDesc& frame)
{
    const VkDevice device = m_context->device();
    FrameResources& resources = m_frames[m_frameIndex];

    FORGE_VK_CHECK(vkWaitForFences(device, 1, &resources.inFlight, VK_TRUE, UINT64_MAX));

    std::uint32_t imageIndex = 0;
    switch (m_swapchain->acquireNextImage(resources.imageAvailable, imageIndex)) {
    case rhi::Swapchain::PresentResult::OutOfDate:
        return DrawResult::NeedSwapchainRecreate; // fence untouched: still signaled
    case rhi::Swapchain::PresentResult::Failure:
        return DrawResult::Failure;
    case rhi::Swapchain::PresentResult::Success:
        break;
    }

    m_meshRenderer.setSunlight(frame.sunDirection, kSunIntensity, kAmbient);

    FORGE_VK_CHECK(vkResetFences(device, 1, &resources.inFlight));
    FORGE_VK_CHECK(vkResetCommandPool(device, resources.commandPool, 0));
    recordCommands(resources.commandBuffer, imageIndex, frame);

    VkSemaphoreSubmitInfo waitSemaphoreInfo = {};
    waitSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSemaphoreInfo.semaphore = resources.imageAvailable;
    waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSemaphoreInfo = {};
    signalSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalSemaphoreInfo.semaphore = m_renderFinished[imageIndex];
    signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandBufferInfo = {};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = resources.commandBuffer;

    VkSubmitInfo2 submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSemaphoreInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;

    FORGE_VK_CHECK(
        vkQueueSubmit2(m_context->graphicsQueue(), 1, &submitInfo, resources.inFlight));

    const rhi::Swapchain::PresentResult presentResult =
        m_swapchain->present(m_renderFinished[imageIndex], imageIndex);

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;

    switch (presentResult) {
    case rhi::Swapchain::PresentResult::OutOfDate:
        return DrawResult::NeedSwapchainRecreate;
    case rhi::Swapchain::PresentResult::Failure:
        return DrawResult::Failure;
    case rhi::Swapchain::PresentResult::Success:
        break;
    }
    return DrawResult::Success;
}

} // namespace forge
