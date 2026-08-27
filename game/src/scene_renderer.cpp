#include "scene_renderer.hpp"

#include "asset_paths.hpp"

#include "sol/assets/asset_loader.hpp"
#include "sol/assets/mesh_lod.hpp"
#include "sol/core/log.hpp"
#include "sol/core/profiler.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/renderer/scene_pass.hpp"
#include "sol/ui/pick.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace game {

using namespace sol;

LodReport& lodReport()
{
    static LodReport report;
    return report;
}

std::int32_t& lodPin()
{
    static std::int32_t pin = kLodPinAutomatic;
    return pin;
}

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
// The cabin light Phase 8m added here as kCockpitEmissive is now the cockpit
// model's own `emissive` in models.toml (Phase 9) - per-model lighting is a
// property of the asset, not of the pass that draws it.

} // namespace

std::uint32_t
SceneRenderer::chooseLevel(RenderInstanceKey key, float screenRadiusPixels, std::uint32_t levelCount)
{
    const std::int32_t pin = lodPin();
    if (pin > kLodPinAutomatic) {
        // Clamped to what exists exactly as `selectMeshLevel` clamps, so a pin
        // past the end of a short chain draws that chain's last level instead
        // of indexing off it. A model with no chain stays at 0, which is what
        // makes it safe to pin globally while four of seven models are refused
        // a chain at all.
        const std::uint32_t pinned =
            levelCount == 0 ? 0u : std::min(static_cast<std::uint32_t>(pin), levelCount - 1u);
        // ⚑ Recorded rather than skipped, so `sol.lod_pin(-1)` resumes from
        // the level actually on screen instead of from one nobody chose.
        if (key != kNoInstanceKey) {
            m_lodThis[key] = pinned;
        }
        return pinned;
    }

    // ⚑ An instance with no identity gets no memory and answers statelessly.
    // The only one is the cockpit, which has no chain, so this costs nothing
    // real - but it must be a supported case rather than an assumption.
    if (key == kNoInstanceKey) {
        return assets::selectMeshLevel(screenRadiusPixels, levelCount);
    }
    const auto it = m_lodLast.find(key);
    const std::uint32_t previous = it == m_lodLast.end() ? assets::kNoPreviousLevel : it->second;
    const std::uint32_t level = assets::selectMeshLevel(screenRadiusPixels, levelCount, previous);
    m_lodThis[key] = level;
    return level;
}

bool SceneRenderer::initialize(rhi::Context& context,
                               rhi::Swapchain& swapchain,
                               const char* shaderDirectory,
                               std::span<const std::string> cookedSearchPath)
{
    m_context = &context;
    m_swapchain = &swapchain;

    if (!m_meshRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory) ||
        !m_skyRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory, kStarfieldSeed) ||
        !m_impostorRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory) ||
        !m_tonemapRenderer.initialize(context, swapchain.imageFormat(), kDepthFormat, shaderDirectory) ||
        !m_debugDraw.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory, kFramesInFlight) ||
        !m_particleRenderer.initialize(context, kHdrFormat, kDepthFormat, shaderDirectory, kFramesInFlight) ||
        !m_uiRenderer.initialize(
            context, swapchain.imageFormat(), kDepthFormat, shaderDirectory, kFramesInFlight)) {
        return false;
    }
    m_hdrColor = rhi::createColorTarget(context, swapchain.extent(), kHdrFormat);
    m_tonemapRenderer.setSource(m_hdrColor.view);

    // Phase 8o. A device that cannot timestamp degrades to no GPU rows, so
    // this failing is not a reason to fail startup for a dev instrument.
    if (!m_gpuProfiler.initialize(context, kFramesInFlight)) {
        SOL_LOG_WARN("GPU profiler unavailable; frame report will have no gpu.* zones");
    }

    // Game UI font: one R8 coverage atlas, viewed as white with the coverage
    // in alpha so it shares the UI shader with solid fills.
    //
    // ⚑ Resolved through the layer search since Phase 24 stage S, so a mod CAN
    // replace it - but the style names `hud`, `body`, `body_strong` and
    // `heading` are read by name all over `game_ui`, so a replacement that
    // drops one leaves that text unrenderable. Nothing enforces that here;
    // it is a documented constraint in `mods/README.md`, not a schema.
    // ⚑ Still a HARD failure when it is missing, and correctly so: this runs
    // before any def is read, so there is no mod to blame and no degraded
    // state worth entering - a game with no font can draw no menu to say so.
    const std::string fontPath = resolveAsset(cookedSearchPath, "ui.sfont");
    if (fontPath.empty() || !m_uiFont.load(fontPath.c_str())) {
        SOL_LOG_ERROR("ui font: no ui.sfont in any of: %s", describeSearchPath(cookedSearchPath).c_str());
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
        atlasDesc.swizzle = {VK_COMPONENT_SWIZZLE_ONE,
                             VK_COMPONENT_SWIZZLE_ONE,
                             VK_COMPONENT_SWIZZLE_ONE,
                             VK_COMPONENT_SWIZZLE_R};
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

bool SceneRenderer::loadModels(std::span<const assets::ModelDef> models,
                               std::span<const std::string> cookedSearchPath)
{
    unloadModels();

    // Uploads a cooked asset once per stem and hands back its pool index; the
    // shipped catalog has six models sharing three meshes and three textures,
    // and an authored one will share far more than that.
    const auto meshIndex = [&](const std::string& stem, std::uint32_t& out) {
        for (std::size_t i = 0; i < m_meshStems.size(); ++i) {
            if (m_meshStems[i] == stem) {
                out = static_cast<std::uint32_t>(i);
                return true;
            }
        }
        const std::string path = resolveAsset(cookedSearchPath, stem + ".smesh");
        assets::MeshData data;
        if (path.empty() || !assets::loadMesh(path.c_str(), data)) {
            return false;
        }
        out = static_cast<std::uint32_t>(m_meshes.size());
        m_meshes.push_back(m_meshRenderer.createMesh(data));
        m_meshStems.push_back(stem);
        return true;
    };
    const auto textureIndex = [&](const std::string& stem, std::uint32_t& out) {
        for (std::size_t i = 0; i < m_textureStems.size(); ++i) {
            if (m_textureStems[i] == stem) {
                out = static_cast<std::uint32_t>(i);
                return true;
            }
        }
        const std::string path = resolveAsset(cookedSearchPath, stem + ".stex");
        assets::TextureData data;
        if (path.empty() || !assets::loadTexture(path.c_str(), data)) {
            return false;
        }
        out = static_cast<std::uint32_t>(m_textures.size());
        m_textures.push_back(m_meshRenderer.createTexture(data));
        m_textureStems.push_back(stem);
        return true;
    };

    LodReport& report = lodReport();
    report.levelsLoaded = 0;
    report.modelsWithLevels = 0;

    m_models.reserve(models.size());
    for (const assets::ModelDef& def : models) {
        CatalogEntry entry = {.radius = def.radius,
                              .emissive = def.emissive,
                              .translucent = def.translucent,
                              .alpha = def.alpha};
        // ⚑⚑⚑ NOT A HARD FAILURE ANY MORE, AND THAT IS PHASE 24 STAGE S's ONE
        // BEHAVIOURAL CHANGE. Until a mod could carry an asset, every
        // `[[model]]` row was ours, so a row naming a missing mesh was our bug
        // and killing startup was right. Now the row may be a mod's, and
        // "one broken mod and the game will not boot" is not a thing a player
        // can diagnose or a modder can survive. The def layer has said the same
        // thing since Phase 5 - a layer that fails to parse leaves the previous
        // state intact rather than half-applying - and assets simply never had
        // the equivalent rule.
        //
        // ⚑⚑ THE ROW KEEPS ITS SLOT. `ModelId` IS the index into
        // `defs.models()` and `m_models` is built parallel to it, so skipping
        // an entry would silently re-point every model after it - a far worse
        // failure than the one being handled, and a silent one. It is marked
        // undrawable instead and the draw loop passes over it, which is the
        // rule that loop already applies to a stale index.
        //
        // ⚑ The error names every directory searched, because when a load
        // fails WHERE it looked is most of the answer (Phase 22's lesson about
        // the data directory, one level down).
        if (!meshIndex(def.mesh, entry.levels[0]) || !textureIndex(def.texture, entry.texture)) {
            SOL_LOG_ERROR("model '%s': cannot load mesh '%s' / texture '%s' - it will draw "
                          "nothing. Looked in: %s",
                          def.id.c_str(),
                          def.mesh.c_str(),
                          def.texture.c_str(),
                          describeSearchPath(cookedSearchPath).c_str());
            entry.drawable = false;
            m_models.push_back(entry);
            continue;
        }

        // ⚑ The chain is whatever the cook left on disk, and its ABSENCE is the
        // normal case rather than an error - four of the seven committed meshes
        // are under the triangle floor. So the existence check is a timestamp
        // probe and not a failed load: `assets::loadMesh` logs when it cannot
        // read a file, which is right when someone asked for that file by name
        // and wrong for an optional sibling nobody promised.
        //
        // ⚑⚑ IT GOES THROUGH THE SAME SEARCH AS LEVEL 0, AND MUST. A probe of
        // the base directory alone would leave a mod's `.lod1.smesh` invisible
        // while its level 0 loaded fine - a mod whose LOD chain silently never
        // engages, with nothing logged and nothing to see but a distant model
        // that costs too much.
        for (std::uint32_t level = 1; level < kMaxDrawLevels; ++level) {
            const std::string stem = def.mesh + ".lod" + std::to_string(level);
            if (resolveAsset(cookedSearchPath, stem + ".smesh").empty()) {
                break; // the chain ends where the files do
            }
            if (!meshIndex(stem, entry.levels[level])) {
                SOL_LOG_ERROR("model '%s': level %u exists but will not load", def.id.c_str(), level);
                unloadModels();
                return false;
            }
            entry.levelCount = level + 1;
        }
        if (entry.levelCount > 1) {
            report.levelsLoaded += entry.levelCount - 1;
            ++report.modelsWithLevels;
        }
        m_models.push_back(entry);
    }
    // ⚑ The undrawable count is in the SUMMARY as well as in the per-model
    // errors above, because the errors scroll and this line is the one anybody
    // reads. A zero here is the only thing that says "every model in this
    // install, mods included, found its files".
    std::size_t undrawable = 0;
    for (const CatalogEntry& entry : m_models) {
        undrawable += entry.drawable ? 0u : 1u;
    }
    SOL_LOG_INFO("models: %zu (%zu meshes, %zu textures, %u level(s) over %u model(s), %zu undrawable)",
                 m_models.size(),
                 m_meshes.size(),
                 m_textures.size(),
                 report.levelsLoaded,
                 report.modelsWithLevels,
                 undrawable);
    return true;
}

void SceneRenderer::unloadModels()
{
    for (renderer::GpuTexture& texture : m_textures) {
        m_meshRenderer.destroyTexture(texture);
    }
    for (renderer::GpuMesh& mesh : m_meshes) {
        m_meshRenderer.destroyMesh(mesh);
    }
    m_models.clear();
    m_meshes.clear();
    m_textures.clear();
    m_meshStems.clear();
    m_textureStems.clear();
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
    unloadModels();
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

void SceneRenderer::recordCommands(VkCommandBuffer commandBuffer,
                                   std::uint32_t imageIndex,
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
    const core::Mat4 projection = core::perspectiveInfiniteReversedZ(kCameraVerticalFov, aspect, 0.05f);
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
    m_translucentScratch.clear();

    // Stage F: how big a thing is on screen decides which level it draws, and
    // the focal length is the same one the target pick projects through - one
    // expression of "how big is this", shared, so a level and a hit box cannot
    // disagree about how far away something looks.
    const float lodFocal =
        ui::focalLength(static_cast<float>(extent.height), std::tan(kCameraVerticalFov * 0.5f));
    LodReport& report = lodReport();
    for (std::uint32_t& count : report.drawn) {
        count = 0;
    }
    report.largestChainedRadius = 0.0f;
    report.largestChainedLevel = 0;
    report.viewportHeight = static_cast<float>(extent.height);

    // ⚑ Phase 18's frame boundary, and the only one there is: what the last
    // frame decided becomes what this frame reads, and this frame starts
    // empty. Both the opaque loop below and the translucent pass after the sky
    // run inside this call, so they share one swap by construction rather than
    // by anyone remembering to keep them in step. Anything not drawn this
    // frame is dropped here, which is the whole eviction policy.
    m_lodLast.swap(m_lodThis);
    m_lodThis.clear();

    for (const RenderInstance& instance : instances) {
        // A stale or unset model draws nothing rather than crashing: the def
        // layer is reloadable at runtime, so an index can outlive its row.
        const std::uint32_t index = modelIndex(instance.model);
        if (index >= m_models.size()) {
            continue;
        }
        const CatalogEntry& entry = m_models[index];
        // ⚑ Phase 24 stage S: a row whose files were not found in ANY layer.
        // Same treatment and the same line of code as a stale index, one
        // reason further along - the entity still exists, still collides and
        // still shows on the map; it just has no picture. `loadModels` logged
        // which model and where it looked, once, rather than every frame.
        if (!entry.drawable) {
            continue;
        }
        // Phase 12: translucent models sit out the opaque block entirely and
        // are replayed after the sky. Deferring the pointer rather than the
        // built matrix keeps this loop doing one thing.
        if (entry.translucent) {
            m_translucentScratch.push_back(&instance);
            continue;
        }

        // Camera-relative: demote sim-space positions to float only after
        // subtracting the camera position (the large-world rule).
        const core::Vec3 relative = (instance.position - camera.position).toVec3();
        const core::Mat4 model =
            core::translation(relative) * toMat4(instance.rotation) * core::scale(instance.scale);

        // ⚑ The instance scale is not uniform in general (a rock takes its size
        // from it), so the silhouette is bounded by the LARGEST axis. Erring
        // large means erring towards detail, which is the safe direction: the
        // cost of being wrong is a few triangles, not a visible pop.
        const float widest = std::max({instance.scale.x, instance.scale.y, instance.scale.z});
        const float screenRadius =
            ui::screenRadiusPixels(static_cast<double>(entry.radius * widest), length(relative), lodFocal);
        const std::uint32_t level = chooseLevel(instance.key, screenRadius, entry.levelCount);

        m_meshRenderer.draw(commandBuffer,
                            m_meshes[entry.levels[level]],
                            m_textures[entry.texture],
                            viewProjection * model,
                            model,
                            entry.emissive);
        ++m_drawCallCount;
        ++report.drawn[level];
        if (entry.levelCount > 1 && screenRadius > report.largestChainedRadius) {
            report.largestChainedRadius = screenRadius;
            report.largestChainedLevel = level;
        }
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
    constexpr std::uint32_t kPaletteCount = static_cast<std::uint32_t>(std::size(kPlanetPalette));
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
    m_skyRenderer.draw(commandBuffer,
                       extent,
                       camera.orientation,
                       kCameraVerticalFov,
                       aspect,
                       kSkyIntensity * scene.skyScale,
                       scene.travelDirection,
                       scene.skyWarp);
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

    // --- Translucent meshes (Phase 12) ---
    //
    // ⚑⚑ THE POSITION OF THIS BLOCK IS THE WHOLE TRICK, AND IT IS NOT A STYLE
    // CHOICE. The sky above is a full-screen pass that survives wherever depth
    // is still at the reversed-Z clear, and a translucent draw writes no depth
    // by design - so a membrane recorded in the opaque block would be painted
    // over by the sky and would appear ONLY where a hull happened to sit
    // behind it. That reads as broken blending and is in fact a misplaced
    // pass. After the sky, before the particles: exhaust glow then reads as
    // being in front of the film rather than trapped behind it.
    //
    // No back-to-front sort. The only translucent object in the game is one
    // flat disc per gate and gates are ~100,000 km apart, so two cannot
    // meaningfully overlap on screen; sorting is the right answer the day a
    // second translucent KIND exists, and is machinery guarding nothing today.
    if (!m_translucentScratch.empty()) {
        const std::uint32_t gpuTranslucentZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.translucent");
        m_meshRenderer.bindTranslucent(commandBuffer, extent);
        for (const RenderInstance* instance : m_translucentScratch) {
            const core::Vec3 relative = (instance->position - camera.position).toVec3();
            const core::Mat4 model =
                core::translation(relative) * toMat4(instance->rotation) * core::scale(instance->scale);
            const CatalogEntry& entry = m_models[modelIndex(instance->model)];
            // Selected the same way as an opaque draw, so translucency does not
            // quietly become a second rule about which mesh to use. Nothing
            // translucent has a chain today, which is exactly why it must be
            // written the same rather than left as an assumption.
            const float widest = std::max({instance->scale.x, instance->scale.y, instance->scale.z});
            const float screenRadius = ui::screenRadiusPixels(
                static_cast<double>(entry.radius * widest), length(relative), lodFocal);
            const std::uint32_t level = chooseLevel(instance->key, screenRadius, entry.levelCount);
            m_meshRenderer.draw(commandBuffer,
                                m_meshes[entry.levels[level]],
                                m_textures[entry.texture],
                                viewProjection * model,
                                model,
                                entry.emissive,
                                entry.alpha);
            ++m_drawCallCount;
            ++report.drawn[level];
        }
        m_gpuProfiler.endZone(commandBuffer, gpuTranslucentZone);
    }

    const std::uint32_t gpuParticleZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.particles");
    m_particleScratch.clear();
    for (const ParticleInstance& particle : particles) {
        m_particleScratch.push_back({
            .position = (particle.position - camera.position).toVec3(),
            .size = particle.size,
            .color = particle.color,
        });
    }
    m_particleRenderer.draw(
        commandBuffer, m_frameIndex, viewProjection, camera.orientation, m_particleScratch);

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
        m_uiRenderer.draw(commandBuffer,
                          m_frameIndex,
                          uiSize,
                          extent,
                          m_uiDrawList->vertices(),
                          m_uiDrawList->indices(),
                          m_uiDrawList->batches());
        ++m_drawCallCount;
    }
    if (m_imguiHost != nullptr) {
        m_imguiHost->render(commandBuffer);
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
