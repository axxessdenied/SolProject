#include "sol/renderer/sky_renderer.hpp"

#include "sol/core/log.hpp"
#include "sol/core/random.hpp"
#include "sol/rhi/descriptors.hpp"
#include "sol/rhi/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sol::renderer {

namespace {

constexpr std::uint32_t kFaceSize = 1024;

struct PushConstants
{
    core::Vec4 right;   // camera right * tan(fovX/2)
    core::Vec4 up;      // camera up * tan(fovY/2)
    core::Vec4 forward; // camera forward, .w = intensity
    core::Vec4 warp;    // Phase 8v: .xyz = travel axis, .w = strength
};

// Face texel -> direction, Vulkan cubemap layer order (+X,-X,+Y,-Y,+Z,-Z).
core::Vec3 faceDirection(int face, float u, float v)
{
    switch (face) {
    case 0:
        return {1.0f, -v, -u};
    case 1:
        return {-1.0f, -v, u};
    case 2:
        return {u, 1.0f, v};
    case 3:
        return {u, -1.0f, -v};
    case 4:
        return {u, -v, 1.0f};
    default:
        return {-u, -v, -1.0f};
    }
}

// Direction -> face texel (inverse of faceDirection).
void directionToFace(core::Vec3 d, int& face, float& u, float& v)
{
    const float ax = std::abs(d.x);
    const float ay = std::abs(d.y);
    const float az = std::abs(d.z);
    if (ax >= ay && ax >= az) {
        if (d.x > 0.0f) {
            face = 0;
            u = -d.z / ax;
            v = -d.y / ax;
        } else {
            face = 1;
            u = d.z / ax;
            v = -d.y / ax;
        }
    } else if (ay >= az) {
        if (d.y > 0.0f) {
            face = 2;
            u = d.x / ay;
            v = d.z / ay;
        } else {
            face = 3;
            u = d.x / ay;
            v = -d.z / ay;
        }
    } else {
        if (d.z > 0.0f) {
            face = 4;
            u = d.x / az;
            v = -d.y / az;
        } else {
            face = 5;
            u = -d.x / az;
            v = -d.y / az;
        }
    }
}

[[nodiscard]] float latticeHash(int x, int y, int z, std::uint64_t seed)
{
    std::uint64_t h = seed;
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0xBF58476D1CE4E5B9ull;
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 0x94D049BB133111EBull;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

[[nodiscard]] float valueNoise(core::Vec3 p, std::uint64_t seed)
{
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const int x = static_cast<int>(fx);
    const int y = static_cast<int>(fy);
    const int z = static_cast<int>(fz);
    const float tx = p.x - fx;
    const float ty = p.y - fy;
    const float tz = p.z - fz;

    auto corner = [&](int dx, int dy, int dz) { return latticeHash(x + dx, y + dy, z + dz, seed); };
    const float c00 = core::lerp(corner(0, 0, 0), corner(1, 0, 0), tx);
    const float c10 = core::lerp(corner(0, 1, 0), corner(1, 1, 0), tx);
    const float c01 = core::lerp(corner(0, 0, 1), corner(1, 0, 1), tx);
    const float c11 = core::lerp(corner(0, 1, 1), corner(1, 1, 1), tx);
    return core::lerp(core::lerp(c00, c10, ty), core::lerp(c01, c11, ty), tz);
}

// One RGB float accumulation buffer per face.
using FaceBuffer = std::vector<float>;

void splatStar(FaceBuffer faces[6], core::Vec3 direction, core::Vec3 color)
{
    int face = 0;
    float u = 0.0f;
    float v = 0.0f;
    directionToFace(direction, face, u, v);
    const float px = (u * 0.5f + 0.5f) * kFaceSize - 0.5f;
    const float py = (v * 0.5f + 0.5f) * kFaceSize - 0.5f;
    const int ix = static_cast<int>(std::floor(px));
    const int iy = static_cast<int>(std::floor(py));

    // 2x2 bilinear footprint; stars near a face edge just lose the texels
    // that fall off (invisible at these brightness levels).
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            const int tx = ix + dx;
            const int ty = iy + dy;
            if (tx < 0 || ty < 0 || tx >= static_cast<int>(kFaceSize) || ty >= static_cast<int>(kFaceSize)) {
                continue;
            }
            const float w = (1.0f - std::abs(px - static_cast<float>(tx))) *
                            (1.0f - std::abs(py - static_cast<float>(ty)));
            float* texel = &faces[face][(static_cast<std::size_t>(ty) * kFaceSize + tx) * 3];
            texel[0] += color.x * w;
            texel[1] += color.y * w;
            texel[2] += color.z * w;
        }
    }
}

[[nodiscard]] core::Vec3 randomDirection(core::Rng& rng)
{
    // Uniform on the sphere via z + azimuth.
    const float z = rng.rangeFloat(-1.0f, 1.0f);
    const float azimuth = rng.rangeFloat(0.0f, core::kTwoPi);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(azimuth), z, r * std::sin(azimuth)};
}

} // namespace

bool SkyRenderer::initialize(rhi::Context& context,
                             VkFormat colorFormat,
                             VkFormat depthFormat,
                             const char* shaderDirectory,
                             std::uint64_t seed)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_shaderDirectory = shaderDirectory;

    // --- Generate the starfield ---
    FaceBuffer faces[6];
    for (FaceBuffer& face : faces) {
        face.assign(static_cast<std::size_t>(kFaceSize) * kFaceSize * 3, 0.0f);
    }

    // Milky-way band: patchy glow around a tilted great circle.
    const core::Vec3 bandNormal = normalize(core::Vec3{0.28f, 1.0f, 0.2f});
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < kFaceSize; ++y) {
            for (std::uint32_t x = 0; x < kFaceSize; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) * (2.0f / kFaceSize) - 1.0f;
                const float v = (static_cast<float>(y) + 0.5f) * (2.0f / kFaceSize) - 1.0f;
                const core::Vec3 dir = normalize(faceDirection(face, u, v));

                const float planeDistance = dot(dir, bandNormal);
                const float band = std::exp(-planeDistance * planeDistance * 42.0f);
                if (band < 0.02f) {
                    continue;
                }
                const float patchy =
                    0.3f + 0.7f * valueNoise(dir * 9.0f, seed) * valueNoise(dir * 23.0f, seed ^ 0x5555u);
                const float glow = band * patchy * 0.028f;

                float* texel = &faces[face][(static_cast<std::size_t>(y) * kFaceSize + x) * 3];
                texel[0] += glow * 0.85f;
                texel[1] += glow * 0.9f;
                texel[2] += glow;
            }
        }
    }

    // Stars: an isotropic field plus a population flattened into the band.
    core::Rng rng(seed, 7);
    auto starColor = [&](float brightness) {
        // Blue-white to orange-ish tint spread.
        const float warmth = rng.rangeFloat(0.0f, 1.0f);
        const core::Vec3 warm = {1.0f, 0.82f, 0.62f};
        const core::Vec3 cool = {0.72f, 0.82f, 1.0f};
        const core::Vec3 tint = {core::lerp(cool.x, warm.x, warmth),
                                 core::lerp(cool.y, warm.y, warmth),
                                 core::lerp(cool.z, warm.z, warmth)};
        return tint * brightness;
    };
    for (int i = 0; i < 9'000; ++i) {
        const float magnitude = rng.rangeFloat(0.0f, 1.0f);
        const float m4 = magnitude * magnitude * magnitude * magnitude;
        const float brightness = 0.02f + 0.98f * m4 * m4;
        splatStar(faces, randomDirection(rng), starColor(brightness));
    }
    for (int i = 0; i < 7'000; ++i) {
        core::Vec3 dir = randomDirection(rng);
        dir = normalize(dir - bandNormal * (dot(dir, bandNormal) * 0.85f));
        const float magnitude = rng.rangeFloat(0.0f, 1.0f);
        const float m3 = magnitude * magnitude * magnitude;
        splatStar(faces, dir, starColor(0.015f + 0.35f * m3 * m3));
    }

    // Pack to RGBA8.
    std::vector<std::uint8_t> packed[6];
    const std::uint8_t* packedPointers[6];
    for (int face = 0; face < 6; ++face) {
        packed[face].resize(static_cast<std::size_t>(kFaceSize) * kFaceSize * 4);
        for (std::size_t t = 0; t < static_cast<std::size_t>(kFaceSize) * kFaceSize; ++t) {
            for (int c = 0; c < 3; ++c) {
                const float value = std::min(faces[face][t * 3 + c], 1.0f);
                packed[face][t * 4 + c] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
            }
            packed[face][t * 4 + 3] = 255;
        }
        packedPointers[face] = packed[face].data();
    }

    m_cubemap = rhi::createSampledCubemap(
        context, kFaceSize, VK_FORMAT_R8G8B8A8_UNORM, packedPointers, kFaceSize * kFaceSize * 4);
    m_sampler = rhi::createClampSampler(context);
    m_setLayout = rhi::createTextureSetLayout(context.device());
    m_descriptorPool = rhi::createTextureDescriptorPool(context.device(), 1);
    m_descriptorSet = rhi::allocateDescriptorSet(context.device(), m_descriptorPool, m_setLayout);
    rhi::writeTextureDescriptor(context.device(), m_descriptorSet, m_cubemap.view, m_sampler);
    m_pipelineLayout = rhi::createPipelineLayout(context.device(), &m_setLayout, 1, sizeof(PushConstants));

    SOL_LOG_INFO("Starfield cubemap generated (%ux%u x6, seed %llu)",
                 kFaceSize,
                 kFaceSize,
                 static_cast<unsigned long long>(seed));
    return reloadPipeline();
}

bool SkyRenderer::reloadPipeline()
{
    const VkDevice device = m_context->device();
    const std::string vertexPath = m_shaderDirectory + "sky.vert.spv";
    const std::string fragmentPath = m_shaderDirectory + "sky.frag.spv";

    VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    rhi::GraphicsPipelineDesc desc = {};
    desc.vertexShader = vertexShader;
    desc.fragmentShader = fragmentShader;
    desc.colorFormat = m_colorFormat;
    desc.depthFormat = m_depthFormat;
    desc.depthTest = true; // passes only at the reversed-Z far clear (0 >= 0)
    desc.depthWrite = false;
    desc.layout = m_pipelineLayout;

    VkPipeline newPipeline = VK_NULL_HANDLE;
    const bool created = rhi::createGraphicsPipeline(device, desc, newPipeline);
    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (!created) {
        return false;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    m_pipeline = newPipeline;
    return true;
}

void SkyRenderer::draw(VkCommandBuffer commandBuffer,
                       VkExtent2D extent,
                       const core::Quat& cameraOrientation,
                       float verticalFovRadians,
                       float aspect,
                       float intensity,
                       const core::Vec3& warpAxis,
                       float warp) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const float tanHalfFov = std::tan(verticalFovRadians * 0.5f);
    const core::Vec3 right = rotate(cameraOrientation, {1.0f, 0.0f, 0.0f}) * (tanHalfFov * aspect);
    const core::Vec3 up = rotate(cameraOrientation, {0.0f, 1.0f, 0.0f}) * tanHalfFov;
    const core::Vec3 forward = rotate(cameraOrientation, {0.0f, 0.0f, -1.0f});

    PushConstants push = {};
    push.right = {right.x, right.y, right.z, 0.0f};
    push.up = {up.x, up.y, up.z, 0.0f};
    push.forward = {forward.x, forward.y, forward.z, intensity};
    push.warp = {warpAxis.x, warpAxis.y, warpAxis.z, warp};
    vkCmdPushConstants(commandBuffer,
                       m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(push),
                       &push);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Viewport/scissor inherited from the mesh pass (same dynamic state).
    (void)extent;
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void SkyRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    vkDestroySampler(device, m_sampler, nullptr);
    rhi::destroyImage(*m_context, m_cubemap);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_context = nullptr;
}

} // namespace sol::renderer
