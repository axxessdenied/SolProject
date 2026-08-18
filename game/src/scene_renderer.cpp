#include "scene_renderer.hpp"

#include "sol/assets/asset_loader.hpp"
#include "sol/core/log.hpp"
#include "sol/core/profiler.hpp"
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
// Cabin light (Phase 8m): unlit albedo added to the cockpit only, so the side
// of the frame the star is not on still reads.
constexpr float kCockpitEmissive = 0.10f;

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
                                       kFramesInFlight) ||
        !m_uiRenderer.initialize(context, swapchain.imageFormat(), kDepthFormat, shaderDirectory,
                                 kFramesInFlight)) {
        return false;
    }
    m_hdrColor = rhi::createColorTarget(context, swapchain.extent(), kHdrFormat);
    m_tonemapRenderer.setSource(m_hdrColor.view);

    // Phase 8o. A device that cannot timestamp degrades to no GPU rows, so
    // this failing is not a reason to fail startup for a dev instrument.
    if (!m_gpuProfiler.initialize(context, kFramesInFlight)) {
        SOL_LOG_WARN("GPU profiler unavailable; frame report will have no gpu.* zones");
    }

    // Assets
    assets::MeshData cubeData;
    assets::MeshData stationData;
    assets::MeshData shipData;
    assets::MeshData asteroidData;
    assets::MeshData cockpitData;
    assets::TextureData checkerData;
    assets::TextureData hullData;
    assets::TextureData cockpitTextureData;
    const std::string cookedBase = cookedDirectory;
    if (!assets::loadMesh((cookedBase + "cube.smesh").c_str(), cubeData) ||
        !assets::loadMesh((cookedBase + "station.smesh").c_str(), stationData) ||
        !assets::loadMesh((cookedBase + "ship.smesh").c_str(), shipData) ||
        !assets::loadMesh((cookedBase + "asteroid.smesh").c_str(), asteroidData) ||
        !assets::loadMesh((cookedBase + "cockpit.smesh").c_str(), cockpitData) ||
        !assets::loadTexture((cookedBase + "checker.stex").c_str(), checkerData) ||
        !assets::loadTexture((cookedBase + "hull.stex").c_str(), hullData) ||
        !assets::loadTexture((cookedBase + "cockpit.stex").c_str(), cockpitTextureData)) {
        return false;
    }
    m_cubeMesh = m_meshRenderer.createMesh(cubeData);
    m_stationMesh = m_meshRenderer.createMesh(stationData);
    m_shipMesh = m_meshRenderer.createMesh(shipData);
    m_asteroidMesh = m_meshRenderer.createMesh(asteroidData);
    m_cockpitMesh = m_meshRenderer.createMesh(cockpitData);
    m_checkerTexture = m_meshRenderer.createTexture(checkerData);
    m_hullTexture = m_meshRenderer.createTexture(hullData);
    m_cockpitTexture = m_meshRenderer.createTexture(cockpitTextureData);

    // Game UI font: one R8 coverage atlas, viewed as white with the coverage
    // in alpha so it shares the UI shader with solid fills.
    if (!m_uiFont.load((cookedBase + "ui.sfont").c_str())) {
        return false;
    }
    {
        const std::uint8_t* atlasData = m_uiFont.atlas().data();
        const std::uint32_t atlasSize = static_cast<std::uint32_t>(m_uiFont.atlas().size());
        rhi::TextureUploadDesc atlasDesc = {};
        atlasDesc.width = m_uiFont.atlasWidth();
        atlasDesc.height = m_uiFont.atlasHeight();
        atlasDesc.format = VK_FORMAT_R8_UNORM;
        atlasDesc.mipCount = 1;
        atlasDesc.mipData = &atlasData;
        atlasDesc.mipSizes = &atlasSize;
        atlasDesc.swizzle = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE,
                             VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R};
        m_uiFontAtlas = rhi::createSampledTexture(context, atlasDesc);
        m_uiFontSampler = rhi::createClampSampler(context);
        m_uiFontTexture = m_uiRenderer.registerTexture(m_uiFontAtlas.view, m_uiFontSampler);
    }

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

    m_gpuProfiler.shutdown();
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
    m_meshRenderer.destroyTexture(m_cockpitTexture);
    m_meshRenderer.destroyMesh(m_cubeMesh);
    m_meshRenderer.destroyMesh(m_stationMesh);
    m_meshRenderer.destroyMesh(m_shipMesh);
    m_meshRenderer.destroyMesh(m_asteroidMesh);
    m_meshRenderer.destroyMesh(m_cockpitMesh);
    m_meshRenderer.shutdown();
    m_skyRenderer.shutdown();
    m_impostorRenderer.shutdown();
    m_tonemapRenderer.shutdown();
    m_debugDraw.shutdown();
    m_particleRenderer.shutdown();
    m_uiRenderer.shutdown();
    if (m_uiFontSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->device(), m_uiFontSampler, nullptr);
        m_uiFontSampler = VK_NULL_HANDLE;
    }
    rhi::destroyImage(*m_context, m_uiFontAtlas);
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

    // Phase 8o. The zones below are read off the structure that is already
    // here - scene_pass.hpp's two brackets - rather than invented, which is
    // the opposite of 8n's sim instrumentation, where SpaceWorld::tick was
    // 568 lines with no seams and every zone had to be placed by hand.
    m_gpuProfiler.beginFrame(commandBuffer, m_frameIndex);
    const std::uint32_t gpuFrameZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.frame");

    const VkExtent2D extent = m_swapchain->extent();
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const core::Mat4 projection =
        core::perspectiveInfiniteReversedZ(kCameraVerticalFov, aspect, 0.05f);
    const core::Mat4 viewProjection = projection * camera.viewRotation();

    // The sun is far enough away to treat as a directional light.
    const core::Vec3 sunDirection = toVec3(normalize(scene.sun.position - camera.position));

    // --- HDR scene pass ---
    const std::uint32_t gpuSceneZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.scene");
    renderer::beginHdrScenePass(commandBuffer, m_hdrColor, m_depth, kSpaceClearColor);

    const std::uint32_t gpuMeshZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.meshes");
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
        float emissive = 0.0f;
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
        case ModelId::Asteroid:
            mesh = &m_asteroidMesh;
            texture = &m_hullTexture;
            break;
        case ModelId::Cockpit:
            mesh = &m_cockpitMesh;
            texture = &m_cockpitTexture;
            // Interior lighting. kAmbient is 1.2%, which is right for a hull
            // in vacuum and pitch black for a room the player is sitting in -
            // the sun still sweeps across the dash on top of this, which is
            // the whole reason the cockpit is geometry rather than paint.
            emissive = kCockpitEmissive;
            break;
        }
        m_meshRenderer.draw(commandBuffer, *mesh, *texture, viewProjection * model, model, emissive);
        ++m_drawCallCount;
    }

    m_gpuProfiler.endZone(commandBuffer, gpuMeshZone);

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
    const std::uint32_t gpuImpostorZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.impostors");
    for (const CelestialDraw& body : scene.planets) {
        renderer::ImpostorRenderer::Body planet = {};
        planet.centerRelative = (body.position - camera.position).toVec3();
        planet.radius = static_cast<float>(body.radius);
        planet.colorA = kPlanetPalette[body.palette % kPaletteCount][0];
        planet.colorB = kPlanetPalette[body.palette % kPaletteCount][1];
        planet.sunDirection = sunDirection;
        m_impostorRenderer.drawPlanet(commandBuffer, viewProjection, planet);
    }
    m_gpuProfiler.endZone(commandBuffer, gpuImpostorZone);

    // Sky after opaques (passes only at the far clear), star glow over the sky.
    // Full-screen and ray-marched, which makes it the most plausible fill-rate
    // cost in the frame and the one zone worth naming in advance (Phase 8o).
    const std::uint32_t gpuSkyZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.sky");
    m_skyRenderer.draw(commandBuffer, extent, camera.orientation, kCameraVerticalFov, aspect,
                       kSkyIntensity * scene.skyScale, scene.travelDirection, scene.skyWarp);
    m_gpuProfiler.endZone(commandBuffer, gpuSkyZone);

    // The star reopens gpu.impostors rather than getting a zone of its own:
    // it is the same pass, drawn after the sky because it glows over it. The
    // report shows two calls against that zone, which is the honest reading.
    const std::uint32_t gpuStarZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.impostors");
    renderer::ImpostorRenderer::Body star = {};
    star.centerRelative = (scene.sun.position - camera.position).toVec3();
    star.radius = static_cast<float>(scene.sun.radius);
    star.colorA = {14.0f, 12.5f, 10.5f}; // disc, HDR
    star.colorB = {5.0f, 3.6f, 2.2f};    // glow, HDR
    m_impostorRenderer.drawStar(commandBuffer, viewProjection, star);
    m_gpuProfiler.endZone(commandBuffer, gpuStarZone);

    const std::uint32_t gpuParticleZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.particles");
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
    m_gpuProfiler.endZone(commandBuffer, gpuParticleZone);

    renderer::endHdrScenePass(commandBuffer, m_hdrColor);
    m_gpuProfiler.endZone(commandBuffer, gpuSceneZone);

    // --- Present pass: tonemap, game UI, dev UI ---
    const std::uint32_t gpuPresentZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.present");
    renderer::beginPresentPass(commandBuffer, *m_swapchain, imageIndex, m_depth);
    m_tonemapRenderer.draw(commandBuffer, extent, scene.exposure);
    if (m_uiDrawList != nullptr && !m_uiDrawList->batches().empty()) {
        // The virtual screen the UI was laid out against; dividing here is
        // what turns the scale setting into visibly larger widgets.
        const core::Vec2 uiSize = {static_cast<float>(extent.width) / m_uiScale,
                                   static_cast<float>(extent.height) / m_uiScale};
        m_uiRenderer.draw(commandBuffer, m_frameIndex, uiSize, extent, m_uiDrawList->vertices(),
                          m_uiDrawList->indices(), m_uiDrawList->batches());
        ++m_drawCallCount;
    }
    if (m_devUi != nullptr) {
        m_devUi->render(commandBuffer);
    }
    renderer::endPresentPass(commandBuffer, *m_swapchain, imageIndex);
    m_gpuProfiler.endZone(commandBuffer, gpuPresentZone);
    m_gpuProfiler.endZone(commandBuffer, gpuFrameZone);

    GAME_VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

SceneRenderer::DrawResult SceneRenderer::drawFrame(const CameraFrame& camera,
                                                   std::span<const RenderInstance> instances,
                                                   std::span<const ParticleInstance> particles,
                                                   const SceneInfo& scene)
{
    const VkDevice device = m_context->device();
    FrameResources& frame = m_frames[m_frameIndex];

    // Phase 8o: this function is four different things and three of them can
    // block, so one zone over all of it cannot say which wait it measured.
    // 8n proved the ~6.2 ms here is a wait rather than work; the split below
    // is what says WHAT it waits for. A fence block means the CPU is gated
    // behind the GPU or behind present pacing two frames back; an acquire
    // block means the presentation engine is holding images. Those are
    // different findings, and the enclosing render.submit zone still sums
    // them so the number stays comparable to 8n's.
    {
        SOL_PROFILE_ZONE("render.waitFence");
        GAME_VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    }

    std::uint32_t imageIndex = 0;
    {
        SOL_PROFILE_ZONE("render.acquire");
        switch (m_swapchain->acquireNextImage(frame.imageAvailable, imageIndex)) {
        case rhi::Swapchain::PresentResult::OutOfDate:
            return DrawResult::NeedSwapchainRecreate; // fence untouched: still signaled for next attempt
        case rhi::Swapchain::PresentResult::Failure:
            return DrawResult::Failure;
        case rhi::Swapchain::PresentResult::Success:
            break;
        }
    }

    {
        SOL_PROFILE_ZONE("render.record");
        GAME_VK_CHECK(vkResetFences(device, 1, &frame.inFlight));
        GAME_VK_CHECK(vkResetCommandPool(device, frame.commandPool, 0));
        recordCommands(frame.commandBuffer, imageIndex, camera, instances, particles, scene);
    }

    rhi::Swapchain::PresentResult presentResult = rhi::Swapchain::PresentResult::Success;
    {
        SOL_PROFILE_ZONE("render.present");

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

        presentResult = m_swapchain->present(m_renderFinished[imageIndex], imageIndex);
    }

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
