#pragma once

// The Forge's one message line (engine plan Phase 9 stage Q4). Stage Q2 moved
// this message OUT of a collapsible panel and into the bottom bar, where it
// cannot be closed, docked or tabbed away. That fixed WHERE it is drawn and
// left two things wrong with WHAT it says, both recorded at the time as
// deliberate non-features rather than discovered later.
//
// ⚑⚑ DEFECT 1 - IT WAS A STATE DRAWN AS IF IT WERE AN EVENT. The line never
// expired, so `undo: add box` sat in always-visible chrome for the rest of the
// session and read exactly as freshly as it had one second after the edit. A
// message you cannot date is worse than no message in the one case that
// matters: you press a key, glance down, and a sentence from four minutes ago
// tells you what you wanted to hear.
//
// ⚑⚑ DEFECT 2 - TWO CONSECUTIVE UNDOS OF THE SAME KIND WROTE THE IDENTICAL
// STRING, so the pixels did not change and the second press looked like it had
// done nothing. This one is nastier than it sounds, because the honest reading
// of "the bar did not change" is "the tool ignored me" - which is precisely
// what an author concludes right before they press it four more times and lose
// four more edits than they meant to.
//
// ⚑⚑⚑ BOTH ARE THE SAME DEFECT SEEN TWICE: THE LINE CARRIED THE TEXT OF AN
// EVENT AND NONE OF ITS IDENTITY. A message is not a string; it is a string
// PLUS when it arrived PLUS how many times it has arrived in a row. This class
// is those other two facts, and it exists so that no writer has to remember
// them.
//
// ⚑ ASSIGNMENT IS THE ONLY WAY IN, AND THAT IS THE WHOLE DESIGN. There are
// fourteen sites in `main.cpp` that write a message and every one of them is
// an ordinary `status = ...`. Routing them through a named setter would have
// worked and would have been forgettable at the fifteenth site; an
// `operator=` that cannot be bypassed makes a message without a timestamp
// INEXPRESSIBLE rather than merely unlikely. Same reasoning as the stage Q3
// rebuild gate, one layer up.
//
// ⚑⚑ IT HOLDS NO CLOCK ON PURPOSE. Time here would mean either ImGui's (which
// this header must not depend on, or the whole thing leaves the reach of
// `forge.unit`) or a wall clock (which makes every test sleep). Instead it
// counts a SERIAL that ticks on every write including a repeat, and the caller
// stamps its own clock when that serial moves. The class stays pure, the
// policy below stays pure, and both are testable without a window.

#include <string>
#include <utility>

namespace forge {

class StatusLine
{
public:
    // ⚑ The repeat count is what makes a second identical message VISIBLE, and
    // it deliberately counts consecutive IDENTICAL text rather than "how many
    // times has undo run". The line has no idea what an undo is - it is fed by
    // imports, opens, failures and buttons too - and `3 drop(s), none changed`
    // arriving three times running deserves the same treatment for the same
    // reason.
    StatusLine& operator=(std::string text)
    {
        if (!text.empty() && text == m_text) {
            ++m_repeat;
        } else {
            m_text = std::move(text);
            m_repeat = m_text.empty() ? 0 : 1;
        }
        // Ticks even when the text is unchanged: "the same thing happened
        // again" is an event, and it is the exact event defect 2 was about.
        ++m_serial;
        return *this;
    }

    [[nodiscard]] bool empty() const { return m_text.empty(); }
    [[nodiscard]] const std::string& text() const { return m_text; }
    [[nodiscard]] int repeat() const { return m_repeat; }
    // Moves on every write. The caller stamps its clock when this changes.
    [[nodiscard]] unsigned long long serial() const { return m_serial; }

    // What the bar actually prints. The count is suffixed rather than prefixed
    // so the message still reads as a sentence and still elides from the right
    // like every other label in the tool.
    [[nodiscard]] std::string display() const
    {
        if (m_repeat <= 1) {
            return m_text;
        }
        return m_text + "  (x" + std::to_string(m_repeat) + ")";
    }

private:
    std::string m_text;
    int m_repeat = 0;
    unsigned long long m_serial = 0;
};

// How a message of a given age should be drawn.
//
// ⚑⚑ THE FLASH IS WHAT ANSWERS DEFECT 2 AND THE EXPIRY IS WHAT ANSWERS DEFECT
// 1, and they are separate numbers because they are answering separate
// questions: "did something just happen?" and "is this still worth reading?".
// A single fade from bright to invisible would have conflated them and made
// the useful half far too short.
struct StatusAppearance
{
    bool visible = false;
    // 1 at the instant of arrival, falling to 0 as the message settles into
    // chrome. The caller lerps text colour with it.
    float highlight = 0.0f;
};

// ⚑ 1.2 s of flash: long enough to catch peripherally while your eyes are on
// the viewport, short enough that a message which is merely RECENT is not
// still shouting. 30 s of life: a message is worth reading for about as long
// as you would plausibly still be wondering what that keypress did, and the
// bar is cleaner empty than lying.
//
// ⚑⚑ THE EXPIRY ALSO CLOSES A THIRD GAP NOBODY REPORTED. `Ctrl+Z` with an
// empty history writes NOTHING - `status` is only set when a step actually
// happened - so before this, pressing undo once too often left the PREVIOUS
// success message standing as the only thing on screen. It said "undo: cell"
// while nothing had been undone. Now that message has to be less than 30 s old
// to be there at all, and it never re-flashes, so the one case that used to
// actively mislead now simply says nothing.
inline constexpr float kStatusFlashSeconds = 1.2f;
inline constexpr float kStatusLifetimeSeconds = 30.0f;

[[nodiscard]] inline StatusAppearance statusAppearance(float ageSeconds)
{
    if (ageSeconds >= kStatusLifetimeSeconds) {
        return {false, 0.0f};
    }
    if (ageSeconds <= 0.0f) {
        return {true, 1.0f};
    }
    if (ageSeconds >= kStatusFlashSeconds) {
        return {true, 0.0f};
    }
    return {true, 1.0f - ageSeconds / kStatusFlashSeconds};
}

} // namespace forge
