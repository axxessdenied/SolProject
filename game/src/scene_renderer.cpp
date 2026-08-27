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

    // ⚑ Phase 25 stage B: the mesh pipelines are `MaterialRegistry`'s now, and
    // stage C moved the pipeline LAYOUTS there too - so what the mesh renderer
    // still hands over is set 0's descriptor layout and the push block's size.
    // It must come up first either way, and it is the only one of the seven
    // that no longer takes a shader directory at all.
    m_shaderSearchPath.assign(1, std::string(shaderDirectory));
    if (!m_meshRenderer.initialize(context) ||
        !m_materials.initialize(context,
                                kHdrFormat,
                                kDepthFormat,
                                m_meshRenderer.textureSetLayout(),
                                sol::renderer::MeshRenderer::kPushConstantSize,
                                m_shaderSearchPath) ||
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
                               std::span<const assets::MaterialDef> materials,
                               std::span<const std::string> cookedSearchPath)
{
    unloadModels();

    // ⚑⚑ THE PIPELINES COME FIRST (Phase 25 stage B), because whether a model
    // is drawable now depends on whether its material's SHADERS loaded as well
    // as on whether its mesh and texture did. False here is structural - not
    // one pipeline could be built - and is the shader-side twin of an empty
    // base `cooked/`: an install missing its shaders must say so rather than
    // boot into a black galaxy.
    if (!m_materials.build(materials)) {
        return false;
    }
    m_opaqueBuckets.assign(materials.size(), {});
    m_translucentDraws.clear();

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

    // ⚑⚑ A MATERIAL'S OWN TEXTURES, RESOLVED PER MATERIAL RATHER THAN PER
    // MODEL (Phase 25 stage C). The albedo below still goes through the model
    // loop, because it is one texture that every model has had since Phase 3
    // and its error already names both rows. These are different: a declared
    // slot belongs to the material and to nothing else, so its failure is the
    // material's failure and the message says so once instead of once per model
    // wearing it.
    //
    // ⚑ A slot that will not load leaves the material with an unwritten set,
    // and `MaterialRegistry::pipeline` returns null for exactly that - so the
    // models using it go undrawable through the same branch as a missing mesh,
    // keeping their slots, which is Phase 24 stage S's rule again.
    std::vector<sol::rhi::MaterialTextureBinding> slotBindings;
    for (std::uint32_t m = 0; m < materials.size(); ++m) {
        const assets::MaterialDef& material = materials[m];
        if (material.slots.empty()) {
            continue;
        }
        // ⚑ A material the registry already refused - a missing shader, or a
        // declaration its SPIR-V disagreed with - has no set to write, and it
        // has already been named once with the actual reason. Uploading its
        // textures anyway would cost real memory for a surface that will not
        // draw, and reporting the failed write would bury the real error under
        // a second one that describes a consequence.
        if (m_materials.materialSet(m) == VK_NULL_HANDLE) {
            continue;
        }
        slotBindings.clear();
        bool resolved = true;
        for (const assets::MaterialSlot& slot : material.slots) {
            std::uint32_t index = 0;
            if (!textureIndex(slot.texture, index)) {
                SOL_LOG_ERROR("material '%s': cannot load texture '%s' for slot '%s' - every model "
                              "wearing it will draw nothing. Looked in: %s",
                              material.id.c_str(),
                              slot.texture.c_str(),
                              slot.name.c_str(),
                              describeSearchPath(cookedSearchPath).c_str());
                resolved = false;
                break;
            }
            slotBindings.push_back(
                {.view = m_textures[index].image.view, .sampler = m_textures[index].sampler});
        }
        if (resolved && !m_materials.writeMaterialSet(m, slotBindings)) {
            SOL_LOG_ERROR("material '%s': its textures loaded but its descriptor set would not write",
                          material.id.c_str());
        }
    }

    LodReport& report = lodReport();
    report.levelsLoaded = 0;
    report.modelsWithLevels = 0;

    m_models.reserve(models.size());
    for (const assets::ModelDef& def : models) {
        // ⚑⚑ PHASE 25 STAGE A: THE SURFACE IS THE MATERIAL'S, AND THIS IS THE
        // WHOLE OF THE STAGE ON THE RENDERER'S SIDE. `DefDatabase` resolves
        // every row - synthesising a material for one that names none - so the
        // index is in range for a database that was loaded and validated. The
        // guard is for a caller that skipped `validateMaterials`, which is a
        // programming error rather than a data one, and it refuses rather than
        // drawing something nobody authored.
        if (def.materialIndex >= materials.size()) {
            SOL_LOG_ERROR("model '%s': material '%s' did not resolve - the def database was not validated",
                          def.id.c_str(),
                          def.material.c_str());
            unloadModels();
            return false;
        }
        const assets::MaterialDef& material = materials[def.materialIndex];
        CatalogEntry entry = {.radius = def.radius,
                              .emissive = material.emissive,
                              .translucent = material.translucent,
                              .alpha =
                                  material.blend == assets::MaterialBlend::Opaque ? 1.0f : material.alpha,
                              .material = def.materialIndex};

        // ⚑ A material whose shaders would not load leaves its models
        // undrawable, exactly as a missing mesh does and in the same slot-
        // keeping way - `MaterialRegistry::build` already named the material
        // and the directories it searched, once, rather than every frame.
        if (m_materials.pipeline(def.materialIndex) == VK_NULL_HANDLE) {
            SOL_LOG_ERROR("model '%s': material '%s' has no pipeline - it will draw nothing",
                          def.id.c_str(),
                          material.id.c_str());
            entry.drawable = false;
            m_models.push_back(entry);
            continue;
        }
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
        if (!meshIndex(def.mesh, entry.levels[0]) || !textureIndex(material.texture, entry.texture)) {
            // ⚑ The material is NAMED in the error, because with Phase 25 the
            // texture may come from a row in a different file than the model -
            // and "which file do I edit" is the question an error like this
            // exists to answer.
            SOL_LOG_ERROR("model '%s': cannot load mesh '%s' / texture '%s' (material '%s') - it will "
                          "draw nothing. Looked in: %s",
                          def.id.c_str(),
                          def.mesh.c_str(),
                          material.texture.c_str(),
                          material.id.c_str(),
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
    // ⚑ The material count is here because it is stage A's exit criterion in
    // one number: every model drew through a row, and this says how many rows
    // there were. It will stop equalling the model count the moment anybody
    // authors a material two models share.
    SOL_LOG_INFO("models: %zu (%zu meshes, %zu textures, %zu materials over %zu pipeline(s), %u "
                 "level(s) over %u model(s), %zu undrawable)",
                 m_models.size(),
                 m_meshes.size(),
                 m_textures.size(),
                 materials.size(),
                 m_materials.pipelineCount(),
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
    // ⚑ Both hold pointers into the caller's instance span, which is
    // frame-scoped - and the opaque one is also sized to the MATERIAL count,
    // which a reload can change. Emptied here so a bucket for a material that
    // no longer exists cannot outlive it; `loadModels` sizes it again. The
    // translucent list is flat and carries a material INDEX rather than a
    // bucket per material, so it only has the pointer problem, but it is
    // cleared beside its neighbour rather than being the one exception a reader
    // has to notice.
    m_opaqueBuckets.clear();
    m_translucentDraws.clear();
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
    // ⚑ Before the mesh renderer: the registry's pipelines were built against
    // that renderer's layout, and destroying the layout first would leave them
    // referring to a dead handle.
    m_materials.shutdown();
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
    m_drawCallCount = 0;
    for (std::vector<const RenderInstance*>& bucket : m_opaqueBuckets) {
        bucket.clear();
    }
    m_translucentDraws.clear();

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

    // ⚑⚑ ONE PASS TO SORT, ONE PASS TO DRAW (Phase 25 stage B). The old code
    // bound a single pipeline up front and drew straight out of this loop,
    // which is only possible while every mesh in the game shares one pipeline.
    // Now each instance is filed under its model's MATERIAL, so the pipeline
    // is bound once per material rather than once per draw - the bind count is
    // the number of materials on screen, not the number of objects.
    for (const RenderInstance& instance : instances) {
        // A stale or unset model draws nothing rather than crashing: the def
        // layer is reloadable at runtime, so an index can outlive its row.
        const std::uint32_t index = modelIndex(instance.model);
        if (index >= m_models.size()) {
            continue;
        }
        const CatalogEntry& entry = m_models[index];
        // ⚑ Phase 24 stage S: a row whose files were not found in ANY layer,
        // and since stage B also one whose material has no pipeline. Same
        // treatment and the same line of code as a stale index, one reason
        // further along - the entity still exists, still collides and still
        // shows on the map; it just has no picture. `loadModels` logged which
        // model and why, once, rather than every frame.
        if (!entry.drawable) {
            continue;
        }
        // Phase 12: translucent models sit out the opaque block entirely and
        // are replayed after the sky. Deferring the pointer rather than the
        // built matrix keeps this loop doing one thing.
        //
        // ⚑ Phase 25 stage C: the translucent side keeps a FLAT list with the
        // camera distance already in hand, because it is about to be sorted
        // across materials rather than grouped by them. The distance is the
        // same subtraction the draw does, done once.
        if (entry.translucent) {
            const core::Vec3 relative = (instance.position - camera.position).toVec3();
            m_translucentDraws.push_back({.instance = &instance,
                                          .material = entry.material,
                                          .distanceSquared = dot(relative, relative)});
        } else if (entry.material < m_opaqueBuckets.size()) {
            m_opaqueBuckets[entry.material].push_back(&instance);
        }
    }

    // Records ONE instance under an already-bound material. Shared by the
    // opaque block here and the sorted translucent one after the sky, so the
    // two cannot drift on level selection or on the report - which is the
    // mistake Phase 12 explicitly wrote its second loop to avoid, by hand.
    //
    // ⚑ It takes the material because the LAYOUT is the material's since stage
    // C, and a push constant is addressed through a layout.
    const auto drawInstance = [&](std::uint32_t material, const RenderInstance& instance) {
        const CatalogEntry& entry = m_models[modelIndex(instance.model)];
        // Camera-relative: demote sim-space positions to float only after
        // subtracting the camera position (the large-world rule).
        const core::Vec3 relative = (instance.position - camera.position).toVec3();
        const core::Mat4 model =
            core::translation(relative) * toMat4(instance.rotation) * core::scale(instance.scale);

        // ⚑ The instance scale is not uniform in general (a rock takes its
        // size from it), so the silhouette is bounded by the LARGEST axis.
        // Erring large means erring towards detail, which is the safe
        // direction: the cost of being wrong is a few triangles, not a
        // visible pop.
        const float widest = std::max({instance.scale.x, instance.scale.y, instance.scale.z});
        const float screenRadius =
            ui::screenRadiusPixels(static_cast<double>(entry.radius * widest), length(relative), lodFocal);
        const std::uint32_t level = chooseLevel(instance.key, screenRadius, entry.levelCount);

        m_meshRenderer.draw(commandBuffer,
                            m_meshes[entry.levels[level]],
                            m_textures[entry.texture],
                            m_materials.pipelineLayout(material),
                            viewProjection * model,
                            model,
                            entry.emissive,
                            entry.alpha);
        ++m_drawCallCount;
        ++report.drawn[level];
        if (entry.levelCount > 1 && screenRadius > report.largestChainedRadius) {
            report.largestChainedRadius = screenRadius;
            report.largestChainedLevel = level;
        }
    };

    const auto bindMaterial = [&](std::uint32_t material) {
        m_meshRenderer.bindMaterial(commandBuffer,
                                    extent,
                                    m_materials.pipeline(material),
                                    m_materials.pipelineLayout(material),
                                    m_materials.materialSet(material));
    };

    for (std::uint32_t material = 0; material < m_opaqueBuckets.size(); ++material) {
        if (m_opaqueBuckets[material].empty()) {
            continue;
        }
        bindMaterial(material);
        for (const RenderInstance* instance : m_opaqueBuckets[material]) {
            drawInstance(material, *instance);
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
    // ⚑⚑⚑ SORTED BACK TO FRONT, AND THIS IS THE QUESTION PHASE 25 STAGE C WAS
    // HANDED AND DECIDED. Stage B did not change the behaviour but it changed
    // the reasoning: the deferral used to rest on "the only translucent object
    // in the game is one flat disc per gate, and gates are ~100,000 km apart",
    // and once a second translucent MATERIAL became a def row and no C++, that
    // stopped being a fact about the engine and became a fact about today's
    // content. Blending is order-dependent, and the order it USED to run in was
    // material index - the order rows happen to sit in a file. Arbitrary, and
    // wrong in the way nobody files a bug about, because it looks like a
    // lighting problem.
    //
    // ⚑ THE PASS GIVES UP ITS BUCKETING TO GET THIS, DELIBERATELY. Grouping by
    // material exists to hold the bind count down, which matters in the opaque
    // block where the whole bubble draws; here the draws are few and each is
    // expensive per pixel, so a bind per run of one material is a price worth
    // paying for a correct picture. The rebind is on CHANGE, so the shipped
    // case - several gate membranes, one material - is still one bind.
    if (!m_translucentDraws.empty()) {
        const std::uint32_t gpuTranslucentZone = m_gpuProfiler.beginZone(commandBuffer, "gpu.translucent");
        std::sort(m_translucentDraws.begin(),
                  m_translucentDraws.end(),
                  [](const TranslucentDraw& a, const TranslucentDraw& b) {
                      // Farthest first. ⚑ The material index breaks the tie so
                      // the order is TOTAL: two membranes at the same distance
                      // would otherwise sort differently between std::sort
                      // implementations, and a frame that differs by toolchain
                      // is a frame no A/B capture can measure against.
                      if (a.distanceSquared != b.distanceSquared) {
                          return a.distanceSquared > b.distanceSquared;
                      }
                      return a.material < b.material;
                  });
        std::uint32_t bound = 0xFFFFFFFFu;
        for (const TranslucentDraw& draw : m_translucentDraws) {
            if (draw.material != bound) {
                bindMaterial(draw.material);
                bound = draw.material;
            }
            drawInstance(draw.material, *draw.instance);
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
