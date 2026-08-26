#pragma once

#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace sol::ui {

// What a host is allowed to differ about. Both of these are OFF by default
// because the GAME is the default case and neither belongs in it.
//
// ⚑⚑ THESE TWO MUST TRAVEL TOGETHER, WHICH IS THE WHOLE REASON THIS STRUCT
// EXISTS RATHER THAN A LOOSE BOOL. Dockable windows without a persisted layout
// is a WORSE tool than fixed panels, not a better one: the author rebuilds the
// arrangement on every launch. That is exactly the defect Forge stage J was
// built to remove - "the climb was paid on every launch, forever" - coming back
// wearing different clothes, because a layout you must reassemble by hand costs
// more than one you must scroll. Turning on `docking` while leaving `iniPath`
// null is therefore a mistake, not a configuration.
struct HostOptions
{
    // Dockable windows. Off for the game: its dev UI is a fixed overlay and a
    // console, neither of which anybody arranges.
    bool docking = false;

    // Where ImGui persists window layout and dock state. Empty means "nowhere",
    // which is RIGHT for the game - a shipped binary should not drop an
    // imgui.ini beside itself - and wrong for a tool whose layout is the
    // author's own work.
    //
    // ⚑ The host COPIES this, because ImGui stores the pointer it is given and
    // never the string: handing it a temporary's c_str() is a dangling read on
    // the first save, and the first save happens long after initialize()
    // returns.
    std::string iniPath;
};

// The Dear ImGui host: context, platform backend, Vulkan backend and the
// per-frame bracket. It draws nothing of its own - a frame contains whatever
// its clients build between beginFrame() and render().
//
// Split out of DevUi in Phase 9 stage C. DevUi *was* the host: one call
// brought ImGui up and built the game's overlay and console together, which
// left a second application (tools/forge) no way to have ImGui without also
// having the game's windows. DevUi is the first client of this class and the
// Forge is the second.
//
// ⚑ ImGui's context and its platform hook are process-global, so exactly one
// host may be live at a time; a second initialize() fails loudly rather than
// quietly stamping on the first.
class ImGuiHost
{
public:
    [[nodiscard]] bool initialize(platform::Window& window,
                                  rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  std::uint32_t swapchainImageCount,
                                  const HostOptions& options = {});
    void shutdown();

    // Opens the frame. Clients build their windows after this returns.
    void beginFrame();

    // Records draw data; must be called inside a dynamic rendering pass whose
    // attachment formats match the ones initialize() was given.
    void render(VkCommandBuffer commandBuffer);

    // Call instead of render() when the frame is abandoned (swapchain out of
    // date): ImGui insists every NewFrame is closed, one way or the other.
    void discardFrame();

    [[nodiscard]] bool initialized() const { return m_initialized; }

    [[nodiscard]] bool frameOpen() const { return m_frameOpen; }

    // True while ImGui wants the input - a game action like the weapon trigger
    // stands down rather than firing through a console click.
    [[nodiscard]] bool wantsMouseCapture() const;
    [[nodiscard]] bool wantsKeyboardCapture() const;

private:
    bool m_initialized = false;
    bool m_frameOpen = false;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // Owns the string ImGui only borrows; see HostOptions::iniPath.
    std::string m_iniPath;
};

} // namespace sol::ui
