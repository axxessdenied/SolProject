// Wayland window and input (Phase 21 stage B).
//
// The Win32 twin is a message pump: one procedure, one switch, and the OS
// hands it a stream of numbered messages. Wayland is the other shape - a set
// of interfaces, each with its own listener struct of callbacks, bound by name
// out of a registry the compositor advertises. Nothing here is optional
// politeness: a surface with no xdg_toplevel is never mapped, a keyboard with
// no xkb keymap produces keycodes with no meaning, and a client that ignores
// xdg_wm_base.ping is killed as unresponsive.
//
// ⚑⚑ TWO CONTRACTS THIS FILE MUST HONOUR THAT ITS SIGNATURES DO NOT SHOW.
//
// 1. ⚑⚑⚑ KEY STATE IS ALWAYS RECORDED. `window_win32.cpp:152` writes keyDown[]
//    BEFORE the dev-UI hook gets a say, on purpose: whether a key is physically
//    down does not depend on who wants the keystroke, and a key ImGui takes the
//    "up" for must not latch down forever. So `wl_keyboard.key` below writes
//    keyDown[] unconditionally, and nothing may ever gate it.
//
// 2. ⚑⚑⚑ TEXT IS PROTECTED, AND STAGE B MOVED WHERE - READ THIS BEFORE
//    "FIXING" IT. The stage-A skeleton predicted this file would suppress text
//    while ImGui wants the keyboard. It cannot: `sol_platform` does not know
//    ImGui exists, Wayland has no message hook to ask through, and Phase 21
//    decision 2 forbids inventing a platform interface for it. So the gate
//    lives at the consumer instead, as `KeyboardRouting::text` in
//    `game/src/input_actions.cpp` - which is a BETTER home than the Win32 one,
//    because a rule in a library is a rule a test can reach. `textInput()` on
//    Wayland therefore carries everything typed, and the game asks before
//    reading it. Getting this backwards reintroduces Phase 20's defect on
//    Linux only, where no test in this repo would catch it.
//
// ⚑ THE PLACEHOLDER BUFFER IS NOT DECORATION. A Wayland surface with no buffer
// attached is never mapped, and an unmapped surface receives no keyboard focus
// and no pointer events at all. Stage B has no swapchain to attach, so it
// attaches one shm buffer of flat colour - without it there is no window to
// look at and no input to check, and the stage could not be verified on its
// own. Stage C's first `vkQueuePresentKHR` attaches over the top of it.

#include "xkb_keys.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/window.hpp"

#include <cstring>
#include <string>

// clang-format off
// ⚑ Order IS load-bearing here and `IncludeBlocks: Regroup` sorts across blank
// lines, so the guard is doing real work (the same trap that broke three files
// in the repo-wide format pass). The generated protocol headers are emitted by
// wayland-scanner and assume <wayland-client.h> has already been seen.
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <xkbcommon/xkbcommon.h>
// clang-format on

namespace sol::platform {

namespace {

// The compositor may advertise a newer wl_seat than this file understands.
// Eight is where `wl_pointer.axis_value120` arrives, which is the only wheel
// event that reports notches the way WHEEL_DELTA does; nothing above it is
// used, so binding higher would only widen the set of events we must ignore.
constexpr std::uint32_t kMaxSeatVersion = 8;

// A surface needs no more than this, and xdg-shell's first version already
// carries everything a plain toplevel does.
constexpr std::uint32_t kCompositorVersion = 4;
constexpr std::uint32_t kWmBaseVersion = 1;

} // namespace

struct WaylandWindow
{
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    std::uint32_t seatVersion = 1;

    zwp_relative_pointer_manager_v1* relativePointerManager = nullptr;
    zwp_relative_pointer_v1* relativePointer = nullptr;
    zwp_pointer_constraints_v1* pointerConstraints = nullptr;
    zwp_locked_pointer_v1* lockedPointer = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;

    wl_cursor_theme* cursorTheme = nullptr;
    wl_cursor* defaultCursor = nullptr;
    wl_surface* cursorSurface = nullptr;

    wl_buffer* placeholderBuffer = nullptr;
    bool placeholderAttached = false;

    xkb_context* xkbContext = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* xkbState = nullptr;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The size the last xdg_toplevel.configure asked for. Zero means "you
    // choose", which is what a compositor sends for the very first configure
    // of an ordinary window - the client's own preference then stands.
    std::uint32_t pendingWidth = 0;
    std::uint32_t pendingHeight = 0;
    bool closeRequested = false;
    bool resized = false;
    bool configured = false;

    bool keyDown[static_cast<int>(Key::Count)] = {};
    bool mouseDown[static_cast<int>(MouseButton::Count)] = {};
    // ⚑⚑⚑ TWO DELTA ACCUMULATORS, AND THE REASON IS A MEASUREMENT, NOT A
    // PRECAUTION. WSLg advertises `zwp_relative_pointer_manager_v1`, hands out a
    // `zwp_relative_pointer_v1` without complaint, and then NEVER SENDS
    // `relative_motion` - confirmed on the wire with WAYLAND_DEBUG=1: the object
    // is created and not one event follows it, while `wl_pointer.motion` keeps
    // arriving fifty times a drag. A backend that reads "the manager is bound"
    // as "raw deltas are coming" therefore has no mouse-look at all here, and
    // nothing says so: every method returns, the window is fine, the ship simply
    // does not turn. So the two sources are accumulated SEPARATELY and
    // mouseDelta() picks between them on evidence - the first relative_motion to
    // actually arrive, not the protocol being offered. Keeping them apart also
    // means no frame can ever count one physical movement twice.
    float relativeDeltaX = 0.0f;
    float relativeDeltaY = 0.0f;
    float motionDeltaX = 0.0f;
    float motionDeltaY = 0.0f;
    bool sawRelativeMotion = false;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool haveMousePosition = false;
    float wheel = 0.0f;
    std::string textInput;
    bool cursorLocked = false;
    std::uint32_t pointerEnterSerial = 0;
    bool havePointerEnterSerial = false;
    // ⚑ Set when the compositor delivered a v8 `axis_value120` or a v5
    // `axis_discrete` for the scroll currently being assembled. A v8 seat sends
    // BOTH the high-resolution event and the legacy `axis` for the same notch,
    // so taking whichever arrives would double-count every scroll.
    bool wheelFromDiscrete = false;

    MessageHook messageHook = nullptr;

    void clearInput()
    {
        for (bool& down : keyDown) {
            down = false;
        }
        for (bool& down : mouseDown) {
            down = false;
        }
    }

    // The level-0 keysym for a keycode: what the physical key produces with no
    // modifiers applied. This is the Win32 parity that matters - a virtual key
    // does not change when Shift is held, so Shift+W must still read as W held
    // rather than as nothing held.
    [[nodiscard]] Key keyForKeycode(xkb_keycode_t keycode) const
    {
        if (keymap == nullptr || xkbState == nullptr) {
            return Key::Unknown;
        }
        const xkb_layout_index_t layout = xkb_state_key_get_layout(xkbState, keycode);
        const xkb_keysym_t* syms = nullptr;
        const int count = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &syms);
        if (count <= 0 || syms == nullptr) {
            return Key::Unknown;
        }
        return keyFromKeysym(static_cast<std::uint32_t>(syms[0]));
    }

    void applyCursorLock();
    void applyCursorImage();
    void ensureRelativePointer();
    bool createPlaceholderBuffer(std::uint32_t bufferWidth, std::uint32_t bufferHeight);
    void attachPlaceholderBuffer();
};

namespace {

WaylandWindow& implOf(void* data)
{
    return *static_cast<WaylandWindow*>(data);
}

// ---------------------------------------------------------------- keyboard

void keyboardKeymap(
    void* data, wl_keyboard* keyboard, std::uint32_t format, std::int32_t fd, std::uint32_t size)
{
    (void)keyboard;
    WaylandWindow& impl = implOf(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        // ⚑ Closed even on the path that refuses it. The compositor hands the
        // keymap over as a file descriptor and it is ours from that moment; a
        // client that returns early without closing leaks one per keymap
        // change, which is once per layout switch rather than once ever.
        close(fd);
        SOL_LOG_ERROR("Wayland keymap arrived in format %u, not xkb v1", format);
        return;
    }

    char* mapped = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped == MAP_FAILED) {
        close(fd);
        SOL_LOG_ERROR("mmap of the Wayland keymap failed");
        return;
    }

    xkb_keymap* newKeymap = xkb_keymap_new_from_string(
        impl.xkbContext, mapped, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mapped, size);
    close(fd);
    if (newKeymap == nullptr) {
        SOL_LOG_ERROR("xkb refused the compositor's keymap");
        return;
    }

    xkb_state* newState = xkb_state_new(newKeymap);
    if (newState == nullptr) {
        xkb_keymap_unref(newKeymap);
        SOL_LOG_ERROR("xkb_state_new failed");
        return;
    }

    // Replaced rather than added to: a keymap event means the layout changed,
    // and the old state describes a keyboard that no longer exists.
    xkb_state_unref(impl.xkbState);
    xkb_keymap_unref(impl.keymap);
    impl.keymap = newKeymap;
    impl.xkbState = newState;
}

void keyboardEnter(
    void* data, wl_keyboard* keyboard, std::uint32_t serial, wl_surface* surface, wl_array* keys)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    WaylandWindow& impl = implOf(data);
    // Focus arrives carrying the set of keys already held, which fixes a gap
    // Win32 has: alt-tabbing back in with a key down would otherwise leave the
    // game believing nothing is pressed until it is released, and the release
    // would then read as an "up" with no matching "down".
    impl.clearInput();
    const auto* codes = static_cast<const std::uint32_t*>(keys->data);
    const std::size_t count = keys->size / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
        const Key key = impl.keyForKeycode(codes[i] + 8);
        if (key != Key::Unknown) {
            impl.keyDown[static_cast<int>(key)] = true;
        }
    }
}

void keyboardLeave(void* data, wl_keyboard* keyboard, std::uint32_t serial, wl_surface* surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    // Same reason as WM_KILLFOCUS on Win32: a key held while focus goes away
    // never sends its "up" here, so it would stay down forever.
    implOf(data).clearInput();
}

void keyboardKey(void* data,
                 wl_keyboard* keyboard,
                 std::uint32_t serial,
                 std::uint32_t time,
                 std::uint32_t key,
                 std::uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;
    WaylandWindow& impl = implOf(data);
    // Wayland reports evdev keycodes; xkb numbers the same keys eight higher.
    const xkb_keycode_t keycode = key + 8;
    const bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    // ⚑ Contract 1 at the top of this file. Unconditional, and it stays that
    // way: this is the physical fact, and who is allowed to ACT on it is a
    // question for the consumer.
    const Key translated = impl.keyForKeycode(keycode);
    if (translated != Key::Unknown) {
        impl.keyDown[static_cast<int>(translated)] = pressed;
    }

    if (!pressed || impl.xkbState == nullptr) {
        return;
    }

    // Text, by contrast, is modifier-aware: this is the layer that knows Shift
    // makes a capital and that a dead key makes nothing at all yet.
    char utf8[16] = {};
    const int written = xkb_state_key_get_utf8(impl.xkbState, keycode, utf8, sizeof(utf8));
    if (written <= 0) {
        return;
    }
    // Control codes arrive here exactly as they do through WM_CHAR - Backspace
    // is 0x08 and Enter 0x0D - and they are keys, handled as keys, not text.
    const auto first = static_cast<unsigned char>(utf8[0]);
    if (written == 1 && (first < 0x20 || first == 0x7F)) {
        return;
    }
    impl.textInput.append(utf8, static_cast<std::size_t>(written));
}

void keyboardModifiers(void* data,
                       wl_keyboard* keyboard,
                       std::uint32_t serial,
                       std::uint32_t depressed,
                       std::uint32_t latched,
                       std::uint32_t locked,
                       std::uint32_t group)
{
    (void)keyboard;
    (void)serial;
    WaylandWindow& impl = implOf(data);
    if (impl.xkbState != nullptr) {
        // update_mask, never update_key: the compositor owns modifier state and
        // tells us the answer, so recomputing it from key events as well would
        // apply every Shift twice.
        xkb_state_update_mask(impl.xkbState, depressed, latched, locked, 0, 0, group);
    }
}

void keyboardRepeatInfo(void* data, wl_keyboard* keyboard, std::int32_t rate, std::int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
    // Deliberately ignored, and it is parity rather than laziness: every key
    // the game reads is either level-triggered (held) or an edge computed by
    // BindingTable, and neither notices a repeat. Win32's auto-repeat WM_CHARs
    // reach no text field the game owns either, because its one text field
    // takes Backspace as an edge.
}

constexpr wl_keyboard_listener kKeyboardListener = {
    .keymap = &keyboardKeymap,
    .enter = &keyboardEnter,
    .leave = &keyboardLeave,
    .key = &keyboardKey,
    .modifiers = &keyboardModifiers,
    .repeat_info = &keyboardRepeatInfo,
};

// ----------------------------------------------------------------- pointer

void pointerEnter(void* data,
                  wl_pointer* pointer,
                  std::uint32_t serial,
                  wl_surface* surface,
                  wl_fixed_t surfaceX,
                  wl_fixed_t surfaceY)
{
    (void)pointer;
    (void)surface;
    WaylandWindow& impl = implOf(data);
    // The serial is kept because wl_pointer.set_cursor needs the one from the
    // most recent enter - a cursor set with a stale serial is silently ignored,
    // which is what "the cursor will not hide" looks like from the outside.
    impl.pointerEnterSerial = serial;
    impl.havePointerEnterSerial = true;
    impl.mouseX = static_cast<float>(wl_fixed_to_double(surfaceX));
    impl.mouseY = static_cast<float>(wl_fixed_to_double(surfaceY));
    impl.haveMousePosition = true;
    impl.applyCursorImage();
}

void pointerLeave(void* data, wl_pointer* pointer, std::uint32_t serial, wl_surface* surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    // Buttons are deliberately NOT cleared. Wayland holds an implicit grab for
    // as long as a button is down, so a leave cannot arrive mid-drag; clearing
    // here could only ever break a drag that was still legal.
    implOf(data).haveMousePosition = false;
}

void pointerMotion(
    void* data, wl_pointer* pointer, std::uint32_t time, wl_fixed_t surfaceX, wl_fixed_t surfaceY)
{
    (void)pointer;
    (void)time;
    WaylandWindow& impl = implOf(data);
    const auto x = static_cast<float>(wl_fixed_to_double(surfaceX));
    const auto y = static_cast<float>(wl_fixed_to_double(surfaceY));
    // The fallback look delta, accumulated ALWAYS and used only if no raw
    // delta ever shows up. It is accelerated where raw input is not, which is
    // why it is the fallback - but on WSLg it is the only thing there is, and
    // a fallback armed by the protocol's absence would never have fired there
    // (see the accumulators' comment).
    if (impl.haveMousePosition) {
        impl.motionDeltaX += x - impl.mouseX;
        impl.motionDeltaY += y - impl.mouseY;
    }
    impl.mouseX = x;
    impl.mouseY = y;
    impl.haveMousePosition = true;
}

void pointerButton(void* data,
                   wl_pointer* pointer,
                   std::uint32_t serial,
                   std::uint32_t time,
                   std::uint32_t button,
                   std::uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    WaylandWindow& impl = implOf(data);
    MouseButton mapped = MouseButton::Count;
    switch (button) {
    case BTN_LEFT:
        mapped = MouseButton::Left;
        break;
    case BTN_RIGHT:
        mapped = MouseButton::Right;
        break;
    case BTN_MIDDLE:
        mapped = MouseButton::Middle;
        break;
    default:
        return; // side buttons: nothing in this game binds one
    }
    impl.mouseDown[static_cast<int>(mapped)] = (state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void pointerAxis(void* data, wl_pointer* pointer, std::uint32_t time, std::uint32_t axis, wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    WaylandWindow& impl = implOf(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || impl.wheelFromDiscrete) {
        return;
    }
    // The legacy event measures scroll in surface units, where one notch is
    // conventionally ten. The sign is inverted because Wayland counts downward
    // scroll as positive and WHEEL_DELTA counts it as negative.
    impl.wheel -= static_cast<float>(wl_fixed_to_double(value)) / 10.0f;
}

void pointerFrame(void* data, wl_pointer* pointer)
{
    (void)pointer;
    // Ends one logical pointer event. The high-resolution flag is cleared here
    // rather than in pumpEvents because it is per SCROLL, not per frame of the
    // game: two notches inside one pump are two frames on the wire.
    implOf(data).wheelFromDiscrete = false;
}

void pointerAxisSource(void* data, wl_pointer* pointer, std::uint32_t axisSource)
{
    (void)data;
    (void)pointer;
    (void)axisSource;
}

void pointerAxisStop(void* data, wl_pointer* pointer, std::uint32_t time, std::uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

void pointerAxisDiscrete(void* data, wl_pointer* pointer, std::uint32_t axis, std::int32_t discrete)
{
    (void)pointer;
    WaylandWindow& impl = implOf(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || impl.seatVersion >= 8) {
        return; // a v8 seat answers this with axis_value120 instead
    }
    impl.wheel -= static_cast<float>(discrete);
    impl.wheelFromDiscrete = true;
}

void pointerAxisValue120(void* data, wl_pointer* pointer, std::uint32_t axis, std::int32_t value120)
{
    (void)pointer;
    WaylandWindow& impl = implOf(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    // 120 per notch, the same quantum WHEEL_DELTA uses and for the same reason:
    // a free-spinning wheel reports fractions of one.
    impl.wheel -= static_cast<float>(value120) / 120.0f;
    impl.wheelFromDiscrete = true;
}

void pointerAxisRelativeDirection(void* data,
                                  wl_pointer* pointer,
                                  std::uint32_t axis,
                                  std::uint32_t direction)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)direction;
}

constexpr wl_pointer_listener kPointerListener = {
    .enter = &pointerEnter,
    .leave = &pointerLeave,
    .motion = &pointerMotion,
    .button = &pointerButton,
    .axis = &pointerAxis,
    .frame = &pointerFrame,
    .axis_source = &pointerAxisSource,
    .axis_stop = &pointerAxisStop,
    .axis_discrete = &pointerAxisDiscrete,
    .axis_value120 = &pointerAxisValue120,
    .axis_relative_direction = &pointerAxisRelativeDirection,
};

void relativeMotion(void* data,
                    zwp_relative_pointer_v1* relativePointer,
                    std::uint32_t utimeHi,
                    std::uint32_t utimeLo,
                    wl_fixed_t dx,
                    wl_fixed_t dy,
                    wl_fixed_t dxUnaccelerated,
                    wl_fixed_t dyUnaccelerated)
{
    (void)relativePointer;
    (void)utimeHi;
    (void)utimeLo;
    (void)dx;
    (void)dy;
    WaylandWindow& impl = implOf(data);
    // Unaccelerated, matching what Win32 raw input delivers. The accelerated
    // pair is what the CURSOR moved; the game is steering a ship, and pointer
    // acceleration applied to a look axis reads as a sticky centre.
    impl.relativeDeltaX += static_cast<float>(wl_fixed_to_double(dxUnaccelerated));
    impl.relativeDeltaY += static_cast<float>(wl_fixed_to_double(dyUnaccelerated));
    // ⚑ Latched on the first event that actually arrives, which is the whole
    // point: this is the only trustworthy evidence that the compositor
    // implements the protocol it advertised.
    impl.sawRelativeMotion = true;
}

constexpr zwp_relative_pointer_v1_listener kRelativePointerListener = {
    .relative_motion = &relativeMotion,
};

// -------------------------------------------------------------------- seat

void seatCapabilities(void* data, wl_seat* seat, std::uint32_t capabilities)
{
    WaylandWindow& impl = implOf(data);
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;

    if (hasKeyboard && impl.keyboard == nullptr) {
        impl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(impl.keyboard, &kKeyboardListener, &impl);
    } else if (!hasKeyboard && impl.keyboard != nullptr) {
        wl_keyboard_release(impl.keyboard);
        impl.keyboard = nullptr;
        impl.clearInput();
    }

    if (hasPointer && impl.pointer == nullptr) {
        impl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(impl.pointer, &kPointerListener, &impl);
        impl.ensureRelativePointer();
    } else if (!hasPointer && impl.pointer != nullptr) {
        if (impl.relativePointer != nullptr) {
            zwp_relative_pointer_v1_destroy(impl.relativePointer);
            impl.relativePointer = nullptr;
        }
        if (impl.lockedPointer != nullptr) {
            zwp_locked_pointer_v1_destroy(impl.lockedPointer);
            impl.lockedPointer = nullptr;
        }
        wl_pointer_release(impl.pointer);
        impl.pointer = nullptr;
        impl.havePointerEnterSerial = false;
    }
}

void seatName(void* data, wl_seat* seat, const char* name)
{
    (void)data;
    (void)seat;
    (void)name;
}

constexpr wl_seat_listener kSeatListener = {
    .capabilities = &seatCapabilities,
    .name = &seatName,
};

// ------------------------------------------------------------- xdg surface

void wmBasePing(void* data, xdg_wm_base* wmBase, std::uint32_t serial)
{
    (void)data;
    // Not optional. A client that does not pong is declared unresponsive, and
    // the compositor is entitled to kill it.
    xdg_wm_base_pong(wmBase, serial);
}

constexpr xdg_wm_base_listener kWmBaseListener = {
    .ping = &wmBasePing,
};

void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, std::uint32_t serial)
{
    WaylandWindow& impl = implOf(data);
    xdg_surface_ack_configure(xdgSurface, serial);

    // Zero means "you choose", which is what the first configure of an ordinary
    // toplevel carries; the size the caller asked for then stands.
    const std::uint32_t newWidth = (impl.pendingWidth != 0) ? impl.pendingWidth : impl.width;
    const std::uint32_t newHeight = (impl.pendingHeight != 0) ? impl.pendingHeight : impl.height;
    if (newWidth != impl.width || newHeight != impl.height) {
        impl.width = newWidth;
        impl.height = newHeight;
        impl.resized = true;
    }

    if (!impl.placeholderAttached) {
        impl.attachPlaceholderBuffer();
    }
    impl.configured = true;
}

constexpr xdg_surface_listener kXdgSurfaceListener = {
    .configure = &xdgSurfaceConfigure,
};

void toplevelConfigure(
    void* data, xdg_toplevel* toplevel, std::int32_t width, std::int32_t height, wl_array* states)
{
    (void)toplevel;
    (void)states;
    WaylandWindow& impl = implOf(data);
    // Recorded, not applied: xdg_surface.configure is the event that makes a
    // configure sequence real, and it arrives afterwards.
    impl.pendingWidth = (width > 0) ? static_cast<std::uint32_t>(width) : 0;
    impl.pendingHeight = (height > 0) ? static_cast<std::uint32_t>(height) : 0;
}

void toplevelClose(void* data, xdg_toplevel* toplevel)
{
    (void)toplevel;
    implOf(data).closeRequested = true;
}

void toplevelConfigureBounds(void* data, xdg_toplevel* toplevel, std::int32_t width, std::int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

void toplevelWmCapabilities(void* data, xdg_toplevel* toplevel, wl_array* capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

constexpr xdg_toplevel_listener kToplevelListener = {
    .configure = &toplevelConfigure,
    .close = &toplevelClose,
    .configure_bounds = &toplevelConfigureBounds,
    .wm_capabilities = &toplevelWmCapabilities,
};

void shmFormat(void* data, wl_shm* shm, std::uint32_t format)
{
    (void)data;
    (void)shm;
    (void)format;
}

constexpr wl_shm_listener kShmListener = {
    .format = &shmFormat,
};

// ---------------------------------------------------------------- registry

void registryGlobal(
    void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version)
{
    WaylandWindow& impl = implOf(data);
    const auto bindVersion = [version](std::uint32_t wanted) {
        return (version < wanted) ? version : wanted;
    };

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        impl.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, bindVersion(kCompositorVersion)));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        impl.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(impl.shm, &kShmListener, &impl);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        impl.wmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, bindVersion(kWmBaseVersion)));
        xdg_wm_base_add_listener(impl.wmBase, &kWmBaseListener, &impl);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        impl.seatVersion = bindVersion(kMaxSeatVersion);
        impl.seat =
            static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, impl.seatVersion));
        wl_seat_add_listener(impl.seat, &kSeatListener, &impl);
    } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        impl.relativePointerManager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    } else if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        impl.pointerConstraints = static_cast<zwp_pointer_constraints_v1*>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    }
}

void registryGlobalRemove(void* data, wl_registry* registry, std::uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
    // Nothing this window binds is hot-unpluggable in practice; a compositor
    // that withdrew wl_compositor has bigger problems than this file.
}

constexpr wl_registry_listener kRegistryListener = {
    .global = &registryGlobal,
    .global_remove = &registryGlobalRemove,
};

} // namespace

// ⚑ The state above is a namespace-scope struct rather than `Window::Impl`
// itself, and the reason is a C++ rule rather than a design preference:
// `Impl` is a PRIVATE nested type, so the thirty listener callbacks - which are
// free functions, because that is the only shape a C function pointer accepts -
// cannot name it. The Win32 twin sidesteps this by having exactly one callback
// and making it a static member. Deriving keeps every `m_impl->field` below
// reading the same as its Win32 counterpart; the `static_cast` at each
// add_listener site is what makes the `void*` genuinely point at the base.
struct Window::Impl : WaylandWindow
{
};

void WaylandWindow::ensureRelativePointer()
{
    if (relativePointerManager == nullptr || pointer == nullptr || relativePointer != nullptr) {
        return;
    }
    relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relativePointerManager, pointer);
    zwp_relative_pointer_v1_add_listener(relativePointer, &kRelativePointerListener, this);
}

void WaylandWindow::applyCursorLock()
{
    if (cursorLocked) {
        if (lockedPointer == nullptr && pointerConstraints != nullptr && pointer != nullptr &&
            surface != nullptr) {
            // PERSISTENT rather than ONESHOT: mouse-look lasts as long as the
            // player is flying, and a lock that dissolved the first time the
            // compositor deactivated the surface would come back from an
            // alt-tab with the cursor loose in the middle of a dogfight.
            lockedPointer =
                zwp_pointer_constraints_v1_lock_pointer(pointerConstraints,
                                                        surface,
                                                        pointer,
                                                        nullptr,
                                                        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        }
    } else if (lockedPointer != nullptr) {
        zwp_locked_pointer_v1_destroy(lockedPointer);
        lockedPointer = nullptr;
    }
    applyCursorImage();
}

void WaylandWindow::applyCursorImage()
{
    if (pointer == nullptr || !havePointerEnterSerial) {
        return;
    }
    if (cursorLocked) {
        // A null surface is how Wayland spells "hide the cursor".
        wl_pointer_set_cursor(pointer, pointerEnterSerial, nullptr, 0, 0);
        return;
    }
    if (defaultCursor == nullptr || cursorSurface == nullptr || defaultCursor->image_count == 0) {
        return;
    }
    // ⚑ The unlock half is why a cursor theme is loaded at all. Hiding needs
    // nothing but a null surface; SHOWING needs an actual image, because the
    // compositor does not remember what the pointer looked like before the
    // client took it over. Without this the first mouse-look would hide the
    // cursor permanently.
    wl_cursor_image* image = defaultCursor->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr) {
        return;
    }
    wl_surface_attach(cursorSurface, buffer, 0, 0);
    wl_surface_damage(cursorSurface,
                      0,
                      0,
                      static_cast<std::int32_t>(image->width),
                      static_cast<std::int32_t>(image->height));
    wl_surface_commit(cursorSurface);
    wl_pointer_set_cursor(pointer,
                          pointerEnterSerial,
                          cursorSurface,
                          static_cast<std::int32_t>(image->hotspot_x),
                          static_cast<std::int32_t>(image->hotspot_y));
}

bool WaylandWindow::createPlaceholderBuffer(std::uint32_t bufferWidth, std::uint32_t bufferHeight)
{
    if (shm == nullptr || bufferWidth == 0 || bufferHeight == 0) {
        return false;
    }
    const auto stride = static_cast<std::size_t>(bufferWidth) * 4;
    const std::size_t size = stride * bufferHeight;

    const int fd = memfd_create("sol-placeholder", MFD_CLOEXEC);
    if (fd < 0) {
        SOL_LOG_ERROR("memfd_create for the placeholder buffer failed");
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        SOL_LOG_ERROR("ftruncate of the placeholder buffer failed");
        return false;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        SOL_LOG_ERROR("mmap of the placeholder buffer failed");
        return false;
    }
    // Flat near-black, deliberately not pure black: on a dark desktop a black
    // window and a window that failed to map look the same, and telling those
    // two apart is the whole point of this stage.
    auto* words = static_cast<std::uint32_t*>(pixels);
    for (std::size_t i = 0; i < size / 4; ++i) {
        words[i] = 0xFF101418u;
    }
    munmap(pixels, size);

    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(size));
    placeholderBuffer = wl_shm_pool_create_buffer(pool,
                                                  0,
                                                  static_cast<std::int32_t>(bufferWidth),
                                                  static_cast<std::int32_t>(bufferHeight),
                                                  static_cast<std::int32_t>(stride),
                                                  WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return placeholderBuffer != nullptr;
}

void WaylandWindow::attachPlaceholderBuffer()
{
    if (surface == nullptr || width == 0 || height == 0) {
        return;
    }
    if (placeholderBuffer == nullptr && !createPlaceholderBuffer(width, height)) {
        return;
    }
    wl_surface_attach(surface, placeholderBuffer, 0, 0);
    wl_surface_damage(surface, 0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
    wl_surface_commit(surface);
    // ⚑ Once, and never again. A resize before the swapchain exists leaves this
    // buffer at the old size, which a strict compositor may reject - but
    // re-attaching it after Vulkan has started presenting would be far worse,
    // and there is no event that says "the swapchain has taken over".
    placeholderAttached = true;
}

Window::Window() : m_impl(std::make_unique<Impl>())
{
}

Window::~Window()
{
    destroy();
}

bool Window::create(const WindowDesc& desc)
{
    SOL_ASSERT(m_impl->display == nullptr);

    m_impl->display = wl_display_connect(nullptr);
    if (m_impl->display == nullptr) {
        SOL_LOG_ERROR("wl_display_connect failed; is WAYLAND_DISPLAY set?");
        return false;
    }

    m_impl->xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (m_impl->xkbContext == nullptr) {
        SOL_LOG_ERROR("xkb_context_new failed");
        destroy();
        return false;
    }

    m_impl->registry = wl_display_get_registry(m_impl->display);
    wl_registry_add_listener(m_impl->registry, &kRegistryListener, static_cast<WaylandWindow*>(m_impl.get()));
    // Two round trips, and both are needed. The first delivers the globals; the
    // second delivers what those globals then send us - the seat's capabilities
    // and, through them, the keyboard's keymap.
    wl_display_roundtrip(m_impl->display);
    wl_display_roundtrip(m_impl->display);

    if (m_impl->compositor == nullptr || m_impl->wmBase == nullptr) {
        // Named rather than generic, because the two failures have different
        // fixes: no wl_compositor means the socket is not a Wayland one, and no
        // xdg_wm_base means a compositor that cannot show an ordinary window.
        SOL_LOG_ERROR("Wayland compositor is missing %s",
                      (m_impl->compositor == nullptr) ? "wl_compositor" : "xdg_wm_base");
        destroy();
        return false;
    }
    if (m_impl->relativePointerManager == nullptr) {
        SOL_LOG_WARN("no zwp_relative_pointer_v1; mouse look falls back to cursor motion");
    }
    if (m_impl->pointerConstraints == nullptr) {
        SOL_LOG_WARN("no zwp_pointer_constraints_v1; the cursor cannot be locked for mouse look");
    }

    m_impl->surface = wl_compositor_create_surface(m_impl->compositor);
    if (m_impl->surface == nullptr) {
        SOL_LOG_ERROR("wl_compositor_create_surface failed");
        destroy();
        return false;
    }

    m_impl->xdgSurface = xdg_wm_base_get_xdg_surface(m_impl->wmBase, m_impl->surface);
    xdg_surface_add_listener(
        m_impl->xdgSurface, &kXdgSurfaceListener, static_cast<WaylandWindow*>(m_impl.get()));
    m_impl->toplevel = xdg_surface_get_toplevel(m_impl->xdgSurface);
    xdg_toplevel_add_listener(
        m_impl->toplevel, &kToplevelListener, static_cast<WaylandWindow*>(m_impl.get()));
    xdg_toplevel_set_title(m_impl->toplevel, desc.title);
    xdg_toplevel_set_app_id(m_impl->toplevel, "sol");

    m_impl->width = desc.width;
    m_impl->height = desc.height;

    if (m_impl->shm != nullptr) {
        m_impl->cursorTheme = wl_cursor_theme_load(nullptr, 24, m_impl->shm);
        if (m_impl->cursorTheme != nullptr) {
            m_impl->defaultCursor = wl_cursor_theme_get_cursor(m_impl->cursorTheme, "left_ptr");
            m_impl->cursorSurface = wl_compositor_create_surface(m_impl->compositor);
        }
    }
    if (m_impl->defaultCursor == nullptr) {
        // Not fatal: the pointer simply stays whatever the compositor drew, and
        // only the unhide after a mouse-look is affected.
        SOL_LOG_WARN("no cursor theme; the pointer will not be restored after mouse look");
    }

    // The first commit with no buffer is what asks for a configure; the buffer
    // goes on in the configure handler, once the size has been agreed.
    wl_surface_commit(m_impl->surface);
    wl_display_roundtrip(m_impl->display);

    if (!m_impl->configured) {
        SOL_LOG_ERROR("the compositor never configured the surface");
        destroy();
        return false;
    }

    // ⚑ "offers" rather than "has", deliberately. WSLg offers the relative
    // pointer and never sends a single event, so a line claiming the feature is
    // present would be the log agreeing with the wrong assumption.
    SOL_LOG_INFO("Wayland window %ux%u (seat v%u; compositor offers%s%s)",
                 m_impl->width,
                 m_impl->height,
                 m_impl->seatVersion,
                 (m_impl->relativePointer != nullptr) ? " relative-pointer" : "",
                 (m_impl->pointerConstraints != nullptr) ? " pointer-constraints" : "");
    return true;
}

void Window::destroy()
{
    if (!m_impl || m_impl->display == nullptr) {
        return;
    }
    setCursorLocked(false);

    if (m_impl->relativePointer != nullptr) {
        zwp_relative_pointer_v1_destroy(m_impl->relativePointer);
        m_impl->relativePointer = nullptr;
    }
    if (m_impl->pointer != nullptr) {
        wl_pointer_release(m_impl->pointer);
        m_impl->pointer = nullptr;
    }
    if (m_impl->keyboard != nullptr) {
        wl_keyboard_release(m_impl->keyboard);
        m_impl->keyboard = nullptr;
    }
    xkb_state_unref(m_impl->xkbState);
    m_impl->xkbState = nullptr;
    xkb_keymap_unref(m_impl->keymap);
    m_impl->keymap = nullptr;
    xkb_context_unref(m_impl->xkbContext);
    m_impl->xkbContext = nullptr;

    if (m_impl->cursorSurface != nullptr) {
        wl_surface_destroy(m_impl->cursorSurface);
        m_impl->cursorSurface = nullptr;
    }
    if (m_impl->cursorTheme != nullptr) {
        wl_cursor_theme_destroy(m_impl->cursorTheme);
        m_impl->cursorTheme = nullptr;
        m_impl->defaultCursor = nullptr;
    }
    if (m_impl->placeholderBuffer != nullptr) {
        wl_buffer_destroy(m_impl->placeholderBuffer);
        m_impl->placeholderBuffer = nullptr;
    }

    // ⚑ Innermost first. xdg_toplevel is a child of xdg_surface which is a
    // child of wl_surface, and destroying a parent before its children is a
    // protocol error rather than a leak - the compositor disconnects.
    if (m_impl->toplevel != nullptr) {
        xdg_toplevel_destroy(m_impl->toplevel);
        m_impl->toplevel = nullptr;
    }
    if (m_impl->xdgSurface != nullptr) {
        xdg_surface_destroy(m_impl->xdgSurface);
        m_impl->xdgSurface = nullptr;
    }
    if (m_impl->surface != nullptr) {
        wl_surface_destroy(m_impl->surface);
        m_impl->surface = nullptr;
    }
    if (m_impl->pointerConstraints != nullptr) {
        zwp_pointer_constraints_v1_destroy(m_impl->pointerConstraints);
        m_impl->pointerConstraints = nullptr;
    }
    if (m_impl->relativePointerManager != nullptr) {
        zwp_relative_pointer_manager_v1_destroy(m_impl->relativePointerManager);
        m_impl->relativePointerManager = nullptr;
    }
    if (m_impl->seat != nullptr) {
        wl_seat_release(m_impl->seat);
        m_impl->seat = nullptr;
    }
    if (m_impl->wmBase != nullptr) {
        xdg_wm_base_destroy(m_impl->wmBase);
        m_impl->wmBase = nullptr;
    }
    if (m_impl->shm != nullptr) {
        wl_shm_destroy(m_impl->shm);
        m_impl->shm = nullptr;
    }
    if (m_impl->compositor != nullptr) {
        wl_compositor_destroy(m_impl->compositor);
        m_impl->compositor = nullptr;
    }
    if (m_impl->registry != nullptr) {
        wl_registry_destroy(m_impl->registry);
        m_impl->registry = nullptr;
    }
    wl_display_disconnect(m_impl->display);
    m_impl->display = nullptr;
    m_impl->placeholderAttached = false;
    m_impl->configured = false;
}

void Window::pumpEvents()
{
    m_impl->relativeDeltaX = 0.0f;
    m_impl->relativeDeltaY = 0.0f;
    m_impl->motionDeltaX = 0.0f;
    m_impl->motionDeltaY = 0.0f;
    m_impl->wheel = 0.0f;
    m_impl->textInput.clear();

    if (m_impl->display == nullptr) {
        return;
    }

    // ⚑ The prepare_read / poll / read_events dance, not wl_display_dispatch.
    // dispatch BLOCKS until an event arrives, which in a game loop means the
    // frame stops whenever the player stops touching anything. This sequence is
    // the documented non-blocking form, and the loop around prepare_read is
    // required rather than defensive: it fails while this thread still has
    // events queued, and the only correct answer is to drain them and ask again.
    while (wl_display_prepare_read(m_impl->display) != 0) {
        wl_display_dispatch_pending(m_impl->display);
    }
    wl_display_flush(m_impl->display);

    pollfd fds = {};
    fds.fd = wl_display_get_fd(m_impl->display);
    fds.events = POLLIN;
    if (poll(&fds, 1, 0) > 0 && (fds.revents & POLLIN) != 0) {
        wl_display_read_events(m_impl->display);
    } else {
        // Cancelling is not optional either: prepare_read takes a per-display
        // read lock, and leaving it held deadlocks the next pump.
        wl_display_cancel_read(m_impl->display);
    }
    wl_display_dispatch_pending(m_impl->display);

    if (wl_display_get_error(m_impl->display) != 0) {
        SOL_LOG_ERROR("the Wayland connection failed; closing");
        m_impl->closeRequested = true;
    }
}

bool Window::shouldClose() const
{
    // The display being gone counts as closed, which is what lets a caller that
    // ignores create()'s failure exit after one frame instead of spinning.
    return m_impl->closeRequested || m_impl->display == nullptr;
}

std::uint32_t Window::width() const
{
    return m_impl->width;
}

std::uint32_t Window::height() const
{
    return m_impl->height;
}

bool Window::isMinimized() const
{
    // Wayland has no "minimized" event - a compositor that hides a window just
    // stops asking it for frames. A zero-sized configure is the nearest thing,
    // and it means the same to the caller: there is nothing worth rendering.
    return m_impl->width == 0 || m_impl->height == 0;
}

bool Window::consumeResize()
{
    const bool resized = m_impl->resized;
    m_impl->resized = false;
    return resized;
}

bool Window::isKeyDown(Key key) const
{
    SOL_ASSERT(key < Key::Count);
    return m_impl->keyDown[static_cast<int>(key)];
}

bool Window::isMouseButtonDown(MouseButton button) const
{
    SOL_ASSERT(button < MouseButton::Count);
    return m_impl->mouseDown[static_cast<int>(button)];
}

core::Vec2 Window::mouseDelta() const
{
    if (m_impl->sawRelativeMotion) {
        return {m_impl->relativeDeltaX, m_impl->relativeDeltaY};
    }
    return {m_impl->motionDeltaX, m_impl->motionDeltaY};
}

core::Vec2 Window::mousePosition() const
{
    // Tracked rather than queried, which is the opposite of the Win32 twin:
    // there is no Wayland equivalent of GetCursorPos, by design - a client may
    // know where the pointer is over its own surface and nowhere else.
    return {m_impl->mouseX, m_impl->mouseY};
}

float Window::wheelDelta() const
{
    return m_impl->wheel;
}

std::string_view Window::textInput() const
{
    return m_impl->textInput;
}

void Window::setCursorLocked(bool locked)
{
    if (m_impl->cursorLocked == locked) {
        return;
    }
    m_impl->cursorLocked = locked;
    m_impl->applyCursorLock();
}

bool Window::isCursorLocked() const
{
    return m_impl->cursorLocked;
}

void Window::setMessageHook(MessageHook hook)
{
    // ⚑ Stored and never called, which is a decision rather than an oversight.
    // MessageHook is the one Win32-SHAPED thing in this interface (window.hpp
    // mirrors HWND/UINT/WPARAM/LPARAM), and Phase 21 decided against
    // refactoring it rather than against supporting it. Wayland has no message
    // to offer it, so the Wayland dev UI polls this class instead.
    m_impl->messageHook = hook;
}

NativeWindowHandle Window::nativeHandle() const
{
    // wl_surface and wl_display, in the slots HWND and HINSTANCE fill on
    // Windows and in the same order: the thing on screen, then the connection.
    // vkCreateWaylandSurfaceKHR wants exactly this pair.
    return NativeWindowHandle{m_impl->surface, m_impl->display};
}

} // namespace sol::platform
