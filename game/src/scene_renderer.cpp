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

constexpr VkClearColorValue kSpaceClearColor = {{0.0f, 0.0f, 0.0f, 1.0f}};
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint64_t kStarfieldSeed = 1337;

// Provisional lighting/tuning until materials are data-driven.
constexpr float kSunIntensity = 3.2f;
constexpr float kAmbient = 0.012f;
constexpr float kSkyIntensity = 1.0f;

} // namespace

bool SceneRenderer::initialize(rhi::Context& context, rhi::Swapchain& swapchain,
                               const char* shaderDirectory, const char* cookedDirectory)
{
    m_context = &context;
    m_swapchain = &swapchain;

    if (!m_meshRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory) ||
        !m_skyRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory,
                                  kStarfieldSeed) ||
        !m_impostorRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory) ||
        !m_tonemapRenderer.initialize(context, swapchain.imageFormat(), kDepthFormat,
                                      shaderDirectory) ||
        !m_debugDraw.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory,
                                kFramesInFlight) ||
        !m_particleRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory,
                                       kFramesInFlight)) {
        return false;
    }
    m_hdrColor = rhi::createColorTarget(context, swapchain.extent(), kHdrFormat);
    m_tonemapRenderer.setSource(m_hdrColor.view);

    // Assets
    assets::MeshData cubeData;
    assets::MeshData stationData;
    assets::MeshData shipData;
    assets::TextureData checkerData;
    assets::TextureData hullData;
    const std::string cookedBase = cookedDirectory;
    if (!assets::loadMesh((cookedBase + "cube.smesh").c_str(), cubeData) ||
        !assets::loadMesh((cookedBase + "station.smesh").c_str(), stationData) ||
        !assets::loadMesh((cookedBase + "ship.smesh").c_str(), shipData) ||
        !assets::loadTexture((cookedBase + "checker.stex").c_str(), checkerData) ||
        !assets::loadTexture((cookedBase + "hull.stex").c_str(), hullData)) {
        return false;
    }
    m_cubeMesh = m_meshRenderer.createMesh(cubeData);
    m_stationMesh = m_meshRenderer.createMesh(stationData);
    m_shipMesh = m_meshRenderer.createMesh(shipData);
    m_checkerTexture = m_meshRenderer.createTexture(checkerData);
    m_hullTexture = m_meshRenderer.createTexture(hullData);

    m_depth = rhi::createDepthImage(context, swapchain.extent());

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
    rhi::destroyImage(*m_context, m_hdrColor);
    m_hdrColor = rhi::createColorTarget(*m_context, m_swapchain->extent(), kHdrFormat);
    m_tonemapRenderer.setSource(m_hdrColor.view);
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
    rhi::destroyImage(*m_context, m_hdrColor);
    m_meshRenderer.destroyTexture(m_checkerTexture);
    m_meshRenderer.destroyTexture(m_hullTexture);
    m_meshRenderer.destroyMesh(m_cubeMesh);
    m_meshRenderer.destroyMesh(m_stationMesh);
    m_meshRenderer.destroyMesh(m_shipMesh);
    m_meshRenderer.shutdown();
    m_skyRenderer.shutdown();
    m_impostorRenderer.shutdown();
    m_tonemapRenderer.shutdown();
    m_debugDraw.shutdown();
    m_particleRenderer.shutdown();
    m_context = nullptr;
    m_swapchain = nullptr;
}

void SceneRenderer::recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                                   const CameraFrame& camera,
                                   std::span<const RenderInstance> instances,
                                   std::span<const ParticleInstance> particles,
                                   const SceneInfo& scene)
{
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GAME_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    const VkExtent2D extent = m_swapchain->extent();
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const core::Mat4 projection =
        core::perspectiveInfiniteReversedZ(kCameraVerticalFov, aspect, 0.05f);
    const core::Mat4 viewProjection = projection * camera.viewRotation();

    // The sun is far enough away to treat as a directional light.
    const core::Vec3 sunDirection = toVec3(normalize(scene.sun.position - camera.position));

    // --- HDR scene pass ---
    renderer::beginHdrScenePass(commandBuffer, m_hdrColor, m_depth, kSpaceClearColor);

    m_meshRenderer.setSunlight(sunDirection, kSunIntensity, kAmbient);
    m_meshRenderer.bind(commandBuffer, extent);
    m_drawCallCount = 0;
    for (const RenderInstance& instance : instances) {
        // Camera-relative: demote sim-space positions to float only after
        // subtracting the camera position (the large-world rule).
        const core::Vec3 relative = (instance.position - camera.position).toVec3();
        const core::Mat4 model =
            core::translation(relative) * toMat4(instance.rotation) * core::scale(instance.scale);

        const renderer::GpuMesh* mesh = &m_cubeMesh;
        const renderer::GpuTexture* texture = &m_checkerTexture;
        switch (instance.model) {
        case ModelId::Cube: break;
        case ModelId::Station:
            mesh = &m_stationMesh;
            texture = &m_hullTexture;
            break;
        case ModelId::Ship:
            mesh = &m_shipMesh;
            texture = &m_hullTexture;
            break;
        }
        m_meshRenderer.draw(commandBuffer, *mesh, *texture, viewProjection * model, model);
        ++m_drawCallCount;
    }

    // Small fixed palette; CelestialDraw::palette indexes it (mod count).
    static constexpr core::Vec3 kPlanetPalette[][2] = {
        {{0.06f, 0.11f, 0.18f}, {0.30f, 0.26f, 0.18f}}, // ocean world
        {{0.20f, 0.12f, 0.07f}, {0.42f, 0.30f, 0.18f}}, // desert
        {{0.10f, 0.16f, 0.10f}, {0.26f, 0.32f, 0.20f}}, // jungle
        {{0.16f, 0.17f, 0.20f}, {0.32f, 0.33f, 0.36f}}, // barren rock
        {{0.22f, 0.16f, 0.09f}, {0.38f, 0.33f, 0.24f}}, // gas banding
    };
    constexpr std::uint32_t kPaletteCount =
        static_cast<std::uint32_t>(std::size(kPlanetPalette));
    for (const CelestialDraw& body : scene.planets) {
        renderer::ImpostorRenderer::Body planet = {};
        planet.centerRelative = (body.position - camera.position).toVec3();
        planet.radius = static_cast<float>(body.radius);
        planet.colorA = kPlanetPalette[body.palette % kPaletteCount][0];
        planet.colorB = kPlanetPalette[body.palette % kPaletteCount][1];
        planet.sunDirection = sunDirection;
        m_impostorRenderer.drawPlanet(commandBuffer, viewProjection, planet);
    }

    // Sky after opaques (passes only at the far clear), star glow over the sky.
    m_skyRenderer.draw(commandBuffer, extent, camera.orientation, kCameraVerticalFov, aspect,
                       kSkyIntensity);

    renderer::ImpostorRenderer::Body star = {};
    star.centerRelative = (scene.sun.position - camera.position).toVec3();
    star.radius = static_cast<float>(scene.sun.radius);
    star.colorA = {14.0f, 12.5f, 10.5f}; // disc, HDR
    star.colorB = {5.0f, 3.6f, 2.2f};    // glow, HDR
    m_impostorRenderer.drawStar(commandBuffer, viewProjection, star);

    m_particleScratch.clear();
    for (const ParticleInstance& particle : particles) {
        m_particleScratch.push_back({
            .position = (particle.position - camera.position).toVec3(),
            .size = particle.size,
            .color = particle.color,
        });
    }
    m_particleRenderer.draw(commandBuffer, m_frameIndex, viewProjection, camera.orientation,
                            m_particleScratch);

    m_debugDraw.draw(commandBuffer, m_frameIndex, viewProjection);
    m_debugDraw.clear();

    renderer::endHdrScenePass(commandBuffer, m_hdrColor);

    // --- Present pass: tonemap + dev UI ---
    renderer::beginPresentPass(commandBuffer, *m_swapchain, imageIndex, m_depth);
    m_tonemapRenderer.draw(commandBuffer, extent, scene.exposure);
    if (m_devUi != nullptr) {
        m_devUi->render(commandBuffer);
    }
    renderer::endPresentPass(commandBuffer, *m_swapchain, imageIndex);

    GAME_VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

SceneRenderer::DrawResult SceneRenderer::drawFrame(const CameraFrame& camera,
                                                   std::span<const RenderInstance> instances,
                                                   std::span<const ParticleInstance> particles,
                                                   const SceneInfo& scene)
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
    recordCommands(frame.commandBuffer, imageIndex, camera, instances, particles, scene);

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
