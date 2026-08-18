#pragma once

#include "sol/core/math/vec.hpp"
#include "sol/core/profiler.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace sol::ui {

struct OverlayStats
{
    float fps = 0.0f;
    float frameMilliseconds = 0.0f;
    core::DVec3 cameraPosition;
    float cameraSpeed = 0.0f;
    std::uint32_t drawCalls = 0;
    std::uint64_t simTicks = 0;
    std::uint32_t simEntities = 0;
    float simAlpha = 0.0f;
    // Where the frame actually went (Phase 8n). Non-owning; null draws the
    // overlay exactly as it was before the profiler existed. Passed rather
    // than read off the global so the overlay can be driven with any
    // instance, and so this layer states its dependency instead of hiding it.
    const core::Profiler* profiler = nullptr;
};

// Dear ImGui dev/debug overlay (never player-facing UI - see engine plan 2.9).
class DevUi
{
public:
    [[nodiscard]] bool initialize(platform::Window& window, rhi::Context& context,
                                  VkFormat colorFormat, VkFormat depthFormat,
                                  std::uint32_t swapchainImageCount);
    void shutdown();

    // Once per frame, before recording; builds the overlay + console windows.
    // Player-facing screens are not this class's business: they live on the
    // custom UI stack (`sol::ui::UiContext`, `sol/ui/screens.hpp`).
    void beginFrame(const OverlayStats& stats);

    // Records draw data; must be called inside the scene's dynamic rendering pass.
    void render(VkCommandBuffer commandBuffer);

    // Call instead of render() when the frame is abandoned (swapchain out of date).
    void discardFrame();

    // Console command line: submitted text goes to the handler (e.g. a Lua
    // VM); without one the input line is hidden and the console is log-only.
    using CommandHandler = void (*)(const char* command, void* userData);
    void setCommandHandler(CommandHandler handler, void* userData);

    // True while ImGui wants the mouse (e.g. clicking the console) - game
    // actions like the weapon trigger should stand down.
    [[nodiscard]] bool wantsMouseCapture() const;

private:
    void buildWindows(const OverlayStats& stats);
    void buildConsoleInput();
    static int consoleTextCallback(ImGuiInputTextCallbackData* data);

    bool m_initialized = false;
    bool m_frameOpen = false;
    bool m_showConsole = true;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    CommandHandler m_commandHandler = nullptr;
    void* m_commandUserData = nullptr;
    char m_commandBuffer[512] = {};
    std::vector<std::string> m_commandHistory;
    int m_historyIndex = -1; // -1 = editing a fresh line
};

} // namespace sol::ui
