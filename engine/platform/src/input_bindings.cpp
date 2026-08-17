#include "sol/platform/input_bindings.hpp"

namespace sol::platform {

namespace {

const char* keyName(Key key)
{
    switch (key) {
    case Key::A: return "A";
    case Key::B: return "B";
    case Key::C: return "C";
    case Key::D: return "D";
    case Key::E: return "E";
    case Key::F: return "F";
    case Key::G: return "G";
    case Key::H: return "H";
    case Key::I: return "I";
    case Key::J: return "J";
    case Key::K: return "K";
    case Key::L: return "L";
    case Key::M: return "M";
    case Key::N: return "N";
    case Key::O: return "O";
    case Key::P: return "P";
    case Key::Q: return "Q";
    case Key::R: return "R";
    case Key::S: return "S";
    case Key::T: return "T";
    case Key::U: return "U";
    case Key::V: return "V";
    case Key::W: return "W";
    case Key::X: return "X";
    case Key::Y: return "Y";
    case Key::Z: return "Z";
    case Key::Num0: return "0";
    case Key::Num1: return "1";
    case Key::Num2: return "2";
    case Key::Num3: return "3";
    case Key::Num4: return "4";
    case Key::Num5: return "5";
    case Key::Num6: return "6";
    case Key::Num7: return "7";
    case Key::Num8: return "8";
    case Key::Num9: return "9";
    case Key::Escape: return "Escape";
    case Key::Space: return "Space";
    case Key::Enter: return "Enter";
    case Key::Tab: return "Tab";
    case Key::LeftShift: return "Left Shift";
    case Key::LeftControl: return "Left Ctrl";
    case Key::LeftAlt: return "Left Alt";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::Backspace: return "Backspace";
    case Key::Delete: return "Delete";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::F1: return "F1";
    case Key::F2: return "F2";
    case Key::F3: return "F3";
    case Key::F4: return "F4";
    case Key::F5: return "F5";
    case Key::F6: return "F6";
    case Key::F7: return "F7";
    case Key::F8: return "F8";
    case Key::F9: return "F9";
    case Key::F10: return "F10";
    case Key::F11: return "F11";
    case Key::F12: return "F12";
    case Key::Unknown:
    case Key::Count:
        break;
    }
    return "None";
}

const char* mouseName(MouseButton button)
{
    switch (button) {
    case MouseButton::Left: return "Left Mouse";
    case MouseButton::Right: return "Right Mouse";
    case MouseButton::Middle: return "Middle Mouse";
    case MouseButton::Count: break;
    }
    return "None";
}

// Key::Unknown is not a chord, so the key range starts at 1.
constexpr std::size_t kFirstKey = 1;
constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count) - kFirstKey;
constexpr std::size_t kMouseCount = static_cast<std::size_t>(MouseButton::Count);

} // namespace

const char* chordName(InputChord chord)
{
    switch (chord.kind) {
    case ChordKind::Key: return keyName(chord.asKey());
    case ChordKind::Mouse: return mouseName(chord.asMouse());
    case ChordKind::None: break;
    }
    return "None";
}

std::size_t chordUniverseSize()
{
    return kKeyCount + kMouseCount;
}

InputChord chordAt(std::size_t index)
{
    if (index < kKeyCount) {
        return InputChord::ofKey(static_cast<Key>(index + kFirstKey));
    }
    const std::size_t mouseIndex = index - kKeyCount;
    if (mouseIndex < kMouseCount) {
        return InputChord::ofMouse(static_cast<MouseButton>(mouseIndex));
    }
    return {};
}

InputChord chordFromName(std::string_view name)
{
    // Searched against chordName rather than against a second table: one
    // definition of a chord's name means the round trip cannot drift, which is
    // the same anti-drift rule 8j applied to the radar projection.
    if (name.empty()) {
        return {};
    }
    const std::size_t count = chordUniverseSize();
    for (std::size_t i = 0; i < count; ++i) {
        const InputChord chord = chordAt(i);
        if (name == chordName(chord)) {
            return chord;
        }
    }
    return {};
}

void InputSnapshot::clear()
{
    for (std::uint64_t& word : m_keys) {
        word = 0;
    }
    m_mouse = 0;
}

void InputSnapshot::sample(const Window& window)
{
    clear();
    for (std::size_t i = kFirstKey; i < static_cast<std::size_t>(Key::Count); ++i) {
        const Key key = static_cast<Key>(i);
        if (window.isKeyDown(key)) {
            m_keys[i / 64] |= std::uint64_t{1} << (i % 64);
        }
    }
    for (std::size_t i = 0; i < kMouseCount; ++i) {
        if (window.isMouseButtonDown(static_cast<MouseButton>(i))) {
            m_mouse |= 1u << i;
        }
    }
}

void InputSnapshot::setDown(InputChord chord, bool down)
{
    switch (chord.kind) {
    case ChordKind::Key: {
        const std::size_t index = chord.code;
        if (index >= static_cast<std::size_t>(Key::Count)) {
            return;
        }
        const std::uint64_t bit = std::uint64_t{1} << (index % 64);
        if (down) {
            m_keys[index / 64] |= bit;
        } else {
            m_keys[index / 64] &= ~bit;
        }
        break;
    }
    case ChordKind::Mouse: {
        if (chord.code >= kMouseCount) {
            return;
        }
        const std::uint32_t bit = 1u << chord.code;
        if (down) {
            m_mouse |= bit;
        } else {
            m_mouse &= ~bit;
        }
        break;
    }
    case ChordKind::None:
        break;
    }
}

bool InputSnapshot::down(InputChord chord) const
{
    switch (chord.kind) {
    case ChordKind::Key: {
        const std::size_t index = chord.code;
        if (index >= static_cast<std::size_t>(Key::Count)) {
            return false;
        }
        return (m_keys[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }
    case ChordKind::Mouse:
        return chord.code < kMouseCount && (m_mouse & (1u << chord.code)) != 0;
    case ChordKind::None:
        break;
    }
    return false;
}

void BindingTable::setActionCount(std::uint32_t count)
{
    m_chords.assign(count, InputChord{});
}

void BindingTable::bind(std::uint32_t action, InputChord chord)
{
    if (action >= m_chords.size()) {
        return;
    }
    m_chords[action] = chord;
}

std::uint32_t BindingTable::assign(std::uint32_t action, InputChord chord)
{
    if (action >= m_chords.size()) {
        return kNoAction;
    }
    if (!chord.bound()) {
        m_chords[action] = {};
        return kNoAction;
    }
    const std::uint32_t holder = find(chord);
    // Reassigning an action the chord it already holds must not unbind it -
    // the obvious off-by-one in a steal implementation, and the reason this
    // check is here rather than left to the caller.
    if (holder == action) {
        return kNoAction;
    }
    if (holder != kNoAction) {
        m_chords[holder] = {};
    }
    m_chords[action] = chord;
    return holder;
}

void BindingTable::unbind(std::uint32_t action)
{
    if (action < m_chords.size()) {
        m_chords[action] = {};
    }
}

InputChord BindingTable::chordFor(std::uint32_t action) const
{
    return action < m_chords.size() ? m_chords[action] : InputChord{};
}

std::uint32_t BindingTable::find(InputChord chord) const
{
    if (!chord.bound()) {
        return kNoAction;
    }
    for (std::size_t i = 0; i < m_chords.size(); ++i) {
        if (m_chords[i] == chord) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoAction;
}

void BindingTable::beginFrame(const InputSnapshot& now)
{
    m_previous = m_current;
    m_current = now;
}

bool BindingTable::held(std::uint32_t action) const
{
    const InputChord chord = chordFor(action);
    return chord.bound() && m_current.down(chord);
}

bool BindingTable::pressed(std::uint32_t action) const
{
    const InputChord chord = chordFor(action);
    return chord.bound() && m_current.down(chord) && !m_previous.down(chord);
}

bool BindingTable::released(std::uint32_t action) const
{
    const InputChord chord = chordFor(action);
    return chord.bound() && !m_current.down(chord) && m_previous.down(chord);
}

InputChord BindingTable::captured() const
{
    const std::size_t count = chordUniverseSize();
    for (std::size_t i = 0; i < count; ++i) {
        const InputChord chord = chordAt(i);
        if (m_current.down(chord) && !m_previous.down(chord)) {
            return chord;
        }
    }
    return {};
}

} // namespace sol::platform
