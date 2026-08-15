#include "sol/ui/dev_ui.hpp"

#include "dev_ui_platform.hpp"

#include "sol/core/log.hpp"
#include "sol/rhi/descriptors.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <cstring>
#include <string>
#include <vector>

namespace sol::ui {

namespace {

platform::Window* g_hookedWindow = nullptr;

// Ring buffer console fed by the core log sink.
struct ConsoleBuffer
{
    static constexpr std::size_t kMaxLines = 256;
    std::vector<std::pair<core::LogLevel, std::string>> lines;
    bool scrollToBottom = false;

    void add(core::LogLevel level, const char* message)
    {
        if (lines.size() >= kMaxLines) {
            lines.erase(lines.begin());
        }
        lines.emplace_back(level, message);
        scrollToBottom = true;
    }
};

ConsoleBuffer g_console;

void consoleLogSink(core::LogLevel level, const char* message, void* /*userData*/)
{
    g_console.add(level, message);
}

bool messageHookTrampoline(void* windowHandle, std::uint32_t message, std::uint64_t wParam,
                           std::int64_t lParam)
{
    return devUiPlatformMessageHook(windowHandle, message, wParam, lParam);
}

void checkVkResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        SOL_LOG_ERROR("[imgui] Vulkan call failed (%d)", static_cast<int>(result));
    }
}

ImVec4 levelColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::Warn: return {1.0f, 0.8f, 0.3f, 1.0f};
    case core::LogLevel::Error:
    case core::LogLevel::Fatal: return {1.0f, 0.35f, 0.35f, 1.0f};
    default: return {0.75f, 0.78f, 0.82f, 1.0f};
    }
}

} // namespace

bool DevUi::initialize(platform::Window& window, rhi::Context& context, VkFormat colorFormat,
                       VkFormat depthFormat, std::uint32_t swapchainImageCount)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini litter

    if (!devUiPlatformInit(window.nativeHandle())) {
        SOL_LOG_ERROR("[imgui] platform backend init failed");
        return false;
    }

    m_descriptorPool = rhi::createTextureDescriptorPool(context.device(), 8, /*allowFree=*/true);
    m_device = context.device();

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = context.instance();
    initInfo.PhysicalDevice = context.physicalDevice();
    initInfo.Device = context.device();
    initInfo.QueueFamily = context.graphicsQueueFamily();
    initInfo.Queue = context.graphicsQueue();
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = swapchainImageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo = {};
    initInfo.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    static VkFormat s_colorFormat; // must outlive init (backend keeps the pointer)
    s_colorFormat = colorFormat;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_colorFormat;
    initInfo.PipelineRenderingCreateInfo.depthAttachmentFormat = depthFormat;
    initInfo.CheckVkResultFn = &checkVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        SOL_LOG_ERROR("[imgui] Vulkan backend init failed");
        devUiPlatformShutdown();
        return false;
    }

    window.setMessageHook(&messageHookTrampoline);
    g_hookedWindow = &window;
    core::setLogSink(&consoleLogSink, nullptr);

    m_initialized = true;
    return true;
}

void DevUi::shutdown()
{
    if (!m_initialized) {
        return;
    }
    core::setLogSink(nullptr, nullptr);
    if (g_hookedWindow != nullptr) {
        g_hookedWindow->setMessageHook(nullptr);
        g_hookedWindow = nullptr;
    }
    ImGui_ImplVulkan_Shutdown();
    devUiPlatformShutdown();
    ImGui::DestroyContext();
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_initialized = false;
}

void DevUi::beginFrame(const OverlayStats& stats)
{
    ImGui_ImplVulkan_NewFrame();
    devUiPlatformNewFrame();
    ImGui::NewFrame();
    m_frameOpen = true;

    buildWindows(stats);
}

void DevUi::buildWindows(const OverlayStats& stats)
{
    // Perf overlay: fixed top-left, transparent, no interaction.
    ImGui::SetNextWindowPos({12.0f, 12.0f});
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("perf##overlay", nullptr, overlayFlags)) {
        ImGui::Text("%.1f fps  (%.2f ms)", stats.fps, stats.frameMilliseconds);
        ImGui::Text("cam  %.2f  %.2f  %.2f", stats.cameraPosition.x, stats.cameraPosition.y,
                    stats.cameraPosition.z);
        ImGui::Text("speed %.1f m/s   draws %u", stats.cameraSpeed, stats.drawCalls);
        ImGui::Text("sim  tick %llu   entities %u   alpha %.2f",
                    static_cast<unsigned long long>(stats.simTicks), stats.simEntities,
                    stats.simAlpha);
        ImGui::TextDisabled("RMB look, WASD move, F1 console, F5 reload shaders");
    }
    ImGui::End();

    if (m_showConsole) {
        ImGui::SetNextWindowSize({560.0f, 220.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({12.0f, 120.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Console", &m_showConsole)) {
            if (ImGui::BeginChild("##log", {0, 0}, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const auto& [level, text] : g_console.lines) {
                    ImGui::PushStyleColor(ImGuiCol_Text, levelColor(level));
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopStyleColor();
                }
                if (g_console.scrollToBottom &&
                    ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 24.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
                g_console.scrollToBottom = false;
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    // F1 toggles the console.
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        m_showConsole = !m_showConsole;
    }
}

void DevUi::render(VkCommandBuffer commandBuffer)
{
    if (!m_frameOpen) {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    m_frameOpen = false;
}

void DevUi::discardFrame()
{
    if (m_frameOpen) {
        ImGui::EndFrame();
        m_frameOpen = false;
    }
}

} // namespace sol::ui
