#include "scene_renderer.hpp"

#include "sol/assets/asset_loader.hpp"
#include "sol/core/log.hpp"
#include "sol/renderer/scene_pass.hpp"

#include <string>

namespace game {

using namespace sol;

namespace {

#define GAME_VK_CHECK(expression)                                                                            \
    do {                                                                                                     \
        const VkResult gameVkResult_ = (expression);                                                         \
        if (gameVkResult_ != VK_SUCCESS) {                                                                   \
            SOL_LOG_FATAL("Vulkan call failed (%d): %s", static_cast<int>(gameVkResult_), #expression);      \
        }                                                                                                    \
    } while (0)

constexpr VkClearColorValue kSpaceClearColor = {{0.004f, 0.006f, 0.015f, 1.0f}};

} // namespace

bool SceneRenderer::initialize(rhi::Context& context, rhi::Swapchain& swapchain,
                               const char* shaderDirectory, const char* cookedDirectory)
{
    m_context = &context;
    m_swapchain = &swapchain;

    if (!m_meshRenderer.initialize(context, swapchain.imageFormat(), VK_FORMAT_D32_SFLOAT,
                                   shaderDirectory)) {
        return false;
    }

    // Assets
    assets::MeshData cubeData;
    assets::TextureData checkerData;
    const std::string cookedBase = cookedDirectory;
    if (!assets::loadMesh((cookedBase + "cube.smesh").c_str(), cubeData) ||
        !assets::loadTexture((cookedBase + "checker.stex").c_str(), checkerData)) {
        return false;
    }
    m_cubeMesh = m_meshRenderer.createMesh(cubeData);
    m_checkerTexture = m_meshRenderer.createTexture(checkerData);

    m_depth = rhi::createDepthImage(context, swapchain.extent());

    // Test scene: ground slab, cube grid, a spinning marker cube, a tall tower.
    m_instances.push_back({{0.0, -1.6, 0.0}, core::Quat::identity(), {24.0f, 0.2f, 24.0f}, false});
    for (int x = -2; x <= 2; ++x) {
        for (int z = -2; z <= 2; ++z) {
            m_instances.push_back({{x * 3.0, 0.0, z * 3.0},
                                   core::fromAxisAngle({0.0f, 1.0f, 0.0f},
                                                 static_cast<float>(x * 5 + z) * 0.35f),
                                   {1.0f, 1.0f, 1.0f},
                                   false});
        }
    }
    m_instances.push_back({{0.0, 2.5, 0.0}, core::Quat::identity(), {0.8f, 0.8f, 0.8f}, true});
    m_instances.push_back({{8.0, 2.0, -8.0}, core::Quat::identity(), {1.0f, 8.0f, 1.0f}, false});

    // Frame resources
    const VkDevice device = context.device();
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

bool SceneRenderer::createPerImageSemaphores()
{
    m_renderFinished.resize(m_swapchain->imageCount());
    for (VkSemaphore& semaphore : m_renderFinished) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GAME_VK_CHECK(vkCreateSemaphore(m_context->device(), &semaphoreCreateInfo, nullptr, &semaphore));
    }
    return true;
}

void SceneRenderer::destroyPerImageSemaphores()
{
    for (VkSemaphore semaphore : m_renderFinished) {
        vkDestroySemaphore(m_context->device(), semaphore, nullptr);
    }
    m_renderFinished.clear();
}

bool SceneRenderer::onSwapchainRecreated()
{
    destroyPerImageSemaphores();
    rhi::destroyImage(*m_context, m_depth);
    m_depth = rhi::createDepthImage(*m_context, m_swapchain->extent());
    return createPerImageSemaphores();
}

void SceneRenderer::shutdown()
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
    m_meshRenderer.destroyTexture(m_checkerTexture);
    m_meshRenderer.destroyMesh(m_cubeMesh);
    m_meshRenderer.shutdown();
    m_context = nullptr;
    m_swapchain = nullptr;
}

void SceneRenderer::recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                                   const FlyCamera& camera, double timeSeconds)
{
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GAME_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    renderer::beginScenePass(commandBuffer, *m_swapchain, imageIndex, m_depth, kSpaceClearColor);

    const VkExtent2D extent = m_swapchain->extent();
    m_meshRenderer.bind(commandBuffer, extent);

    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const core::Mat4 projection =
        core::perspectiveInfiniteReversedZ(core::radians(70.0f), aspect, 0.05f);
    const core::Mat4 viewProjection = projection * camera.viewRotation();

    m_drawCallCount = 0;
    for (const Instance& instance : m_instances) {
        core::Quat rotation = instance.rotation;
        if (instance.spins) {
            rotation = core::fromAxisAngle({0.0f, 1.0f, 0.0f}, static_cast<float>(timeSeconds) * 0.8f) *
                       core::fromAxisAngle({1.0f, 0.0f, 0.0f}, static_cast<float>(timeSeconds) * 0.5f);
        }
        // Camera-relative: demote sim-space positions to float only after
        // subtracting the camera position (the large-world rule).
        const core::Vec3 relative = (instance.position - camera.position()).toVec3();
        const core::Mat4 model =
            core::translation(relative) * toMat4(rotation) * core::scale(instance.scale);
        m_meshRenderer.draw(commandBuffer, m_cubeMesh, m_checkerTexture, viewProjection * model, model);
        ++m_drawCallCount;
    }

    if (m_devUi != nullptr) {
        m_devUi->render(commandBuffer);
    }

    renderer::endScenePass(commandBuffer, *m_swapchain, imageIndex);
    GAME_VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

SceneRenderer::DrawResult SceneRenderer::drawFrame(const FlyCamera& camera, double timeSeconds)
{
    const VkDevice device = m_context->device();
    FrameResources& frame = m_frames[m_frameIndex];

    GAME_VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

    std::uint32_t imageIndex = 0;
    switch (m_swapchain->acquireNextImage(frame.imageAvailable, imageIndex)) {
    case rhi::Swapchain::PresentResult::OutOfDate:
        return DrawResult::NeedSwapchainRecreate; // fence untouched: still signaled for next attempt
    case rhi::Swapchain::PresentResult::Failure:
        return DrawResult::Failure;
    case rhi::Swapchain::PresentResult::Success:
        break;
    }

    GAME_VK_CHECK(vkResetFences(device, 1, &frame.inFlight));
    GAME_VK_CHECK(vkResetCommandPool(device, frame.commandPool, 0));
    recordCommands(frame.commandBuffer, imageIndex, camera, timeSeconds);

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

} // namespace game
