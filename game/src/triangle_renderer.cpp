#include "triangle_renderer.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"
#include "sol/rhi/pipeline.hpp"

#include <string>

namespace game {

namespace {

// Fails fast: frame-loop Vulkan errors are unrecoverable in Phase 1.
#define GAME_VK_CHECK(expression)                                                                            \
    do {                                                                                                     \
        const VkResult gameVkResult_ = (expression);                                                         \
        if (gameVkResult_ != VK_SUCCESS) {                                                                   \
            SOL_LOG_FATAL("Vulkan call failed (%d): %s", static_cast<int>(gameVkResult_), #expression);      \
        }                                                                                                    \
    } while (0)

constexpr VkClearColorValue kSpaceClearColor = {{0.004f, 0.006f, 0.015f, 1.0f}};

} // namespace

bool TriangleRenderer::initialize(sol::rhi::Context& context,
                                  sol::rhi::Swapchain& swapchain,
                                  const char* shaderDirectory)
{
    m_context = &context;
    m_swapchain = &swapchain;

    const VkDevice device = context.device();

    // Pipeline (shader modules are only needed during creation)
    const std::string vertexPath = std::string(shaderDirectory) + "triangle.vert.spv";
    const std::string fragmentPath = std::string(shaderDirectory) + "triangle.frag.spv";

    VkShaderModule vertexShader = sol::rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = sol::rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    sol::rhi::GraphicsPipelineDesc pipelineDesc = {};
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;
    pipelineDesc.colorFormat = swapchain.imageFormat();
    const bool pipelineCreated =
        sol::rhi::createGraphicsPipeline(device, pipelineDesc, m_pipelineLayout, m_pipeline);

    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (!pipelineCreated) {
        return false;
    }

    // Per-frame resources
    for (FrameResources& frame : m_frames) {
        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = context.graphicsQueueFamily();
        GAME_VK_CHECK(vkCreateCommandPool(device, &poolCreateInfo, nullptr, &frame.commandPool));

        VkCommandBufferAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = frame.commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        GAME_VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &frame.commandBuffer));

        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GAME_VK_CHECK(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &frame.imageAvailable));

        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        GAME_VK_CHECK(vkCreateFence(device, &fenceCreateInfo, nullptr, &frame.inFlight));
    }

    return createPerImageSemaphores();
}

bool TriangleRenderer::createPerImageSemaphores()
{
    m_renderFinished.resize(m_swapchain->imageCount());
    for (VkSemaphore& semaphore : m_renderFinished) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GAME_VK_CHECK(vkCreateSemaphore(m_context->device(), &semaphoreCreateInfo, nullptr, &semaphore));
    }
    return true;
}

void TriangleRenderer::destroyPerImageSemaphores()
{
    for (VkSemaphore semaphore : m_renderFinished) {
        vkDestroySemaphore(m_context->device(), semaphore, nullptr);
    }
    m_renderFinished.clear();
}

bool TriangleRenderer::onSwapchainRecreated()
{
    // Caller has already made the device idle.
    destroyPerImageSemaphores();
    return createPerImageSemaphores();
}

void TriangleRenderer::shutdown()
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
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_context = nullptr;
    m_swapchain = nullptr;
}

void TriangleRenderer::recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex)
{
    const VkImage image = m_swapchain->image(imageIndex);
    const VkExtent2D extent = m_swapchain->extent();

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GAME_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    // Undefined -> color attachment (chains with the imageAvailable semaphore wait stage)
    VkImageMemoryBarrier2 toColorAttachment = {};
    toColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toColorAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColorAttachment.srcAccessMask = VK_ACCESS_2_NONE;
    toColorAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColorAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColorAttachment.image = image;
    toColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toColorAttachment.subresourceRange.levelCount = 1;
    toColorAttachment.subresourceRange.layerCount = 1;

    VkDependencyInfo dependencyInfo = {};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &toColorAttachment;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    VkRenderingAttachmentInfo colorAttachment = {};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_swapchain->imageView(imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = kSpaceClearColor;

    VkRenderingInfo renderingInfo = {};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);

    const VkViewport viewport = {
        0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRendering(commandBuffer);

    // Color attachment -> present
    VkImageMemoryBarrier2 toPresent = {};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    toPresent.dstAccessMask = VK_ACCESS_2_NONE;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = image;
    toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toPresent.subresourceRange.levelCount = 1;
    toPresent.subresourceRange.layerCount = 1;

    dependencyInfo.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    GAME_VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

TriangleRenderer::DrawResult TriangleRenderer::drawFrame()
{
    const VkDevice device = m_context->device();
    FrameResources& frame = m_frames[m_frameIndex];

    GAME_VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    std::uint32_t imageIndex = 0;
    switch (m_swapchain->acquireNextImage(frame.imageAvailable, imageIndex)) {
    case sol::rhi::Swapchain::PresentResult::OutOfDate:
        return DrawResult::NeedSwapchainRecreate; // fence untouched: still signaled for next attempt
    case sol::rhi::Swapchain::PresentResult::Failure:
        return DrawResult::Failure;
    case sol::rhi::Swapchain::PresentResult::Success:
        break;
    }

    GAME_VK_CHECK(vkResetFences(device, 1, &frame.inFlight));
    GAME_VK_CHECK(vkResetCommandPool(device, frame.commandPool, 0));
    recordCommands(frame.commandBuffer, imageIndex);

    VkSemaphoreSubmitInfo waitSemaphoreInfo = {};
    waitSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSemaphoreInfo.semaphore = frame.imageAvailable;
    waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSemaphoreInfo = {};
    signalSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalSemaphoreInfo.semaphore = m_renderFinished[imageIndex];
    signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandBufferInfo = {};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = frame.commandBuffer;

    VkSubmitInfo2 submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSemaphoreInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;

    GAME_VK_CHECK(vkQueueSubmit2(m_context->graphicsQueue(), 1, &submitInfo, frame.inFlight));

    const sol::rhi::Swapchain::PresentResult presentResult =
        m_swapchain->present(m_renderFinished[imageIndex], imageIndex);

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;

    switch (presentResult) {
    case sol::rhi::Swapchain::PresentResult::OutOfDate:
        return DrawResult::NeedSwapchainRecreate;
    case sol::rhi::Swapchain::PresentResult::Failure:
        return DrawResult::Failure;
    case sol::rhi::Swapchain::PresentResult::Success:
        break;
    }
    return DrawResult::Success;
}

} // namespace game
