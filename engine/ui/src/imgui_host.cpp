#include "sol/ui/imgui_host.hpp"

#include "dev_ui_platform.hpp"

#include "sol/core/log.hpp"
#include "sol/rhi/descriptors.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace sol::ui {

namespace {

// The platform hook is a free function, and ImGui's own backends keep global
// state, so the window a host has hooked lives here beside them rather than
// pretending to be per-instance.
platform::Window* g_hookedWindow = nullptr;
bool g_hostLive = false;

bool messageHookTrampoline(void* windowHandle,
                           std::uint32_t message,
                           std::uint64_t wParam,
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

} // namespace

bool ImGuiHost::initialize(platform::Window& window,
                           rhi::Context& context,
                           VkFormat colorFormat,
                           VkFormat depthFormat,
                           std::uint32_t swapchainImageCount,
                           const HostOptions& options)
{
    if (g_hostLive) {
        SOL_LOG_ERROR("[imgui] a host is already live; ImGui's context is process-global");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // ⚑⚑ THE DEFAULT IS STILL "NO imgui.ini LITTER" AND THAT IS STILL RIGHT FOR
    // THE GAME. What changed is that it stopped being right for EVERY client: a
    // tool with dockable windows keeps the author's arrangement in this file,
    // and without it the arrangement is rebuilt by hand on every launch.
    //
    // ⚑ ImGui KEEPS THE POINTER AND NEVER THE STRING, and it writes the file
    // long after this function has returned - so the storage has to outlive the
    // call, which is why the host owns a copy rather than forwarding what the
    // caller passed.
    if (options.iniPath.empty()) {
        ImGui::GetIO().IniFilename = nullptr;
    } else {
        m_iniPath = options.iniPath;
        ImGui::GetIO().IniFilename = m_iniPath.c_str();
    }

    // ⚑ Docking only - deliberately NOT ImGuiConfigFlags_ViewportsEnable, which
    // would let panels leave the window entirely. That needs
    // ImGui::UpdatePlatformWindows()/RenderPlatformWindowsDefault() after
    // Render() and viewport support live in both backends, none of which this
    // host does; enabling the flag without them draws nothing and looks like a
    // corrupted panel.
    if (options.docking) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    if (!devUiPlatformInit(window.nativeHandle())) {
        SOL_LOG_ERROR("[imgui] platform backend init failed");
        ImGui::DestroyContext();
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
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        ImGui::DestroyContext();
        return false;
    }

    window.setMessageHook(&messageHookTrampoline);
    g_hookedWindow = &window;
    g_hostLive = true;

    m_initialized = true;
    return true;
}

void ImGuiHost::shutdown()
{
    if (!m_initialized) {
        return;
    }
    if (g_hookedWindow != nullptr) {
        g_hookedWindow->setMessageHook(nullptr);
        g_hookedWindow = nullptr;
    }
    ImGui_ImplVulkan_Shutdown();
    devUiPlatformShutdown();
    // ⚑ DestroyContext is what FLUSHES a dirty layout to disk, so `m_iniPath`
    // has to still be alive here - clear it only afterwards.
    ImGui::DestroyContext();
    m_iniPath.clear();
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    g_hostLive = false;
    m_initialized = false;
}

void ImGuiHost::beginFrame()
{
    if (!m_initialized) {
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    devUiPlatformNewFrame();
    ImGui::NewFrame();
    m_frameOpen = true;
}

void ImGuiHost::render(VkCommandBuffer commandBuffer)
{
    if (!m_frameOpen) {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    m_frameOpen = false;
}

void ImGuiHost::discardFrame()
{
    if (m_frameOpen) {
        ImGui::EndFrame();
        m_frameOpen = false;
    }
}

bool ImGuiHost::wantsMouseCapture() const
{
    return m_initialized && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiHost::wantsKeyboardCapture() const
{
    return m_initialized && ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace sol::ui
