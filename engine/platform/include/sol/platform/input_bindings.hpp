#pragma once

// Rebindable controls (Phase 8k). Nothing here knows what any action *means*:
// the table is keyed by an opaque action id the game layer assigns, so this
// header can live beside the Key enum it must not outlive while the vocabulary
// of "autopilot" and "scan pulse" stays in the game.

#include "sol/platform/window.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace sol::platform {

enum class ChordKind : std::uint8_t
{
    None = 0, // unbound: a legal, visible state, not an error
    Key,
    Mouse,
};

// One input: a key or a mouse button. Deliberately not a modifier combination
// - nothing in the game uses one, and LeftShift is *boost*, a binding in its
// own right rather than a prefix.
struct InputChord
{
    ChordKind kind = ChordKind::None;
    std::uint8_t code = 0;

    [[nodiscard]] static InputChord ofKey(Key key)
    {
        return {ChordKind::Key, static_cast<std::uint8_t>(key)};
    }

    [[nodiscard]] static InputChord ofMouse(MouseButton button)
    {
        return {ChordKind::Mouse, static_cast<std::uint8_t>(button)};
    }

    [[nodiscard]] bool bound() const { return kind != ChordKind::None; }

    [[nodiscard]] Key asKey() const { return static_cast<Key>(code); }

    [[nodiscard]] MouseButton asMouse() const { return static_cast<MouseButton>(code); }

    [[nodiscard]] bool operator==(const InputChord& other) const
    {
        return kind == other.kind && (kind == ChordKind::None || code == other.code);
    }

    [[nodiscard]] bool operator!=(const InputChord& other) const { return !(*this == other); }
};

// The stable name a chord is written to disk under, and shown in the UI. It is
// a *name* rather than the enum ordinal on purpose: an ordinal would silently
// reassign every binding in every settings file the next time a key is inserted
// into the middle of the Key enum.
[[nodiscard]] const char* chordName(InputChord chord);

// Inverse of chordName. An unknown name yields an unbound chord rather than a
// garbage key, so a stale or hand-edited settings file degrades to "this action
// needs a binding" instead of to nonsense.
[[nodiscard]] InputChord chordFromName(std::string_view name);

// Every chord that exists, in the order the enums declare them. Used to scan
// for "what did the player just press" and to drive the name round-trip test.
[[nodiscard]] std::size_t chordUniverseSize();
[[nodiscard]] InputChord chordAt(std::size_t index);

// A snapshot of the up/down state of every chord. Filled from a Window in the
// game, and by hand in tests - which is the whole reason it is a type rather
// than a pile of calls inside the table.
class InputSnapshot
{
public:
    void sample(const Window& window);
    void clear();

    void setDown(InputChord chord, bool down);
    [[nodiscard]] bool down(InputChord chord) const;

private:
    static constexpr std::size_t kKeyWords = (static_cast<std::size_t>(Key::Count) + 63) / 64;

    std::uint64_t m_keys[kKeyWords] = {};
    std::uint32_t m_mouse = 0;
};

// Action id -> chord, plus the three operations that are easy to get subtly
// wrong: which action holds a chord, assignment that steals it, and the press
// edge that used to be a hand-written `previousX` bool per key.
class BindingTable
{
public:
    static constexpr std::uint32_t kNoAction = 0xFFFFFFFFu;

    void setActionCount(std::uint32_t count);

    [[nodiscard]] std::uint32_t actionCount() const { return static_cast<std::uint32_t>(m_chords.size()); }

    // Sets a binding without conflict handling - for installing defaults,
    // where the caller is asserting the layout is already coherent.
    void bind(std::uint32_t action, InputChord chord);

    // Assigns `chord` to `action`, taking it from whoever held it. Returns the
    // action left unbound, or kNoAction if the chord was free. Assigning an
    // action the chord it already holds is a no-op and does NOT unbind it.
    std::uint32_t assign(std::uint32_t action, InputChord chord);

    void unbind(std::uint32_t action);

    [[nodiscard]] InputChord chordFor(std::uint32_t action) const;
    [[nodiscard]] std::uint32_t find(InputChord chord) const;

    // Rotates this frame's state over last frame's. Call once per frame,
    // before anything asks held()/pressed()/captured().
    void beginFrame(const InputSnapshot& now);

    [[nodiscard]] bool held(std::uint32_t action) const;
    [[nodiscard]] bool pressed(std::uint32_t action) const;
    // The release edge. Rare, but the bookmark prompt opens on release rather
    // than press (Phase 8h), and that is a binding like any other.
    [[nodiscard]] bool released(std::uint32_t action) const;

    // The first chord that went down this frame, whatever it is bound to -
    // this is "press a key or mouse button..." on the Controls screen.
    [[nodiscard]] InputChord captured() const;

private:
    std::vector<InputChord> m_chords;
    InputSnapshot m_current;
    InputSnapshot m_previous;
};

} // namespace sol::platform
