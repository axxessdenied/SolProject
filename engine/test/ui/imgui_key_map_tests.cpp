// The Key -> ImGuiKey table (Phase 21 stage C). Linux-only, and headless by
// construction: the table is plain data, so none of this needs a compositor, a
// window, a GPU or even an ImGui context.
//
// ⚑⚑ WHY THIS SUITE EXISTS. It is the xkb suite's argument one layer up.
// `xkb_keys.cpp` decides which Key a physical key is; this decides what ImGui
// thinks that Key is - and a wrong row here is just as invisible. The console
// stops backspacing, or Home jumps to the end of the line, and nothing fails,
// nothing logs, and no screenshot looks wrong. There is no counterpart on
// Windows because `imgui_impl_win32` owns the equivalent table upstream, which
// is why the suite is guarded by `if(LINUX)` rather than run everywhere.
//
// ⚑⚑⚑ THE TEST THAT EARNS ITS PLACE IS THE LAST ONE, AND THE REASON IS THAT
// THE OTHERS CANNOT SEE A SWAP. Every shape check below passes happily if
// Backspace and Delete trade rows: the table still covers every Key, still
// claims no ImGuiKey twice, and still has exactly the right number of rows.
// So the last test stops asking about shape and checks each row against an
// INDEPENDENT source - ImGui's own name for the key it was handed, against
// this engine's own name for the Key it was asked about. Two spellings of the
// same key, neither of them copied from the table under test.
//
// ⚑⚑ FOUR MUTATIONS WERE RUN AND THE MATRIX IS WORTH KEEPING, because it says
// which test is doing the work and which two are only the floor:
//
//     mutation                                caught by
//     deleted row (Home has no ImGuiKey)      1, 3, 4
//     duplicate target (Home -> End)          2, 4
//     stray extra row (second row for W)      3 ONLY
//     swapped rows (Backspace <-> Delete)     4 ONLY
//
// So 3 and 4 each catch something nothing else does, and 1 and 2 catch nothing
// 4 misses. They are kept anyway, and deliberately rather than by oversight:
// test 4 rests on a spelling bridge between two vocabularies, so an upstream
// ImGui key RENAME would break it for a reason that is not a bug - and at that
// moment 1 and 2 are the only guards still standing over this table.

#include "../../ui/src/linux/imgui_keys.hpp"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <string_view>

#include <sol/platform/input_bindings.hpp>
#include <sol/platform/window.hpp>
#include <sol/test/test.hpp>

using sol::platform::chordName;
using sol::platform::InputChord;
using sol::platform::Key;
using sol::ui::imguiKeyFor;
using sol::ui::ImGuiKeyRow;
using sol::ui::imguiKeyRowCount;
using sol::ui::imguiKeyRows;

namespace {

std::size_t rowsFor(Key key)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < imguiKeyRowCount(); ++i) {
        if (imguiKeyRows()[i].key == key) {
            ++count;
        }
    }
    return count;
}

// Every Key except the two bookends, which name no key: Unknown is the "no
// match" answer and Count is the size of the enum.
std::size_t realKeyCount()
{
    return static_cast<std::size_t>(Key::Count) - 1;
}

// The bridge between two naming conventions, and it is deliberately tiny so
// that it cannot launder a wrong answer. `sol::platform` writes modifiers with
// a space ("Left Ctrl") where ImGui runs them together ("LeftCtrl"), and names
// the arrows for the direction alone ("Left") where ImGui appends the noun
// ("LeftArrow"). Nothing else differs, and no two keys collide once both are
// removed - so a row that survives this really was mapped to the key it names.
std::string normalizedName(const char* name)
{
    std::string out;
    for (const char* c = name; *c != '\0'; ++c) {
        if (*c != ' ') {
            out.push_back(*c);
        }
    }
    constexpr std::string_view kArrow = "Arrow";
    if (out.size() > kArrow.size() && out.compare(out.size() - kArrow.size(), kArrow.size(), kArrow) == 0) {
        out.resize(out.size() - kArrow.size());
    }
    return out;
}

} // namespace

// The mutation this catches is a Key with no ImGuiKey at all - a key that is
// live everywhere else in the game and simply does nothing once the console has
// focus. `io.AddKeyEvent(ImGuiKey_None, ...)` returns immediately, so the frame
// looks perfectly healthy from the outside.
SOL_TEST(imgui_keys_every_key_in_the_enum_is_mapped)
{
    for (int i = static_cast<int>(Key::Unknown) + 1; i < static_cast<int>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        SOL_CHECK(imguiKeyFor(key) != ImGuiKey_None);
    }
}

// The other half of the same mutation: one ImGuiKey claimed by two Keys means
// ImGui is told that key is down and up in the same frame, and which answer
// wins depends on the row order rather than on anything a reader would notice.
SOL_TEST(imgui_keys_no_imgui_key_is_claimed_twice)
{
    for (std::size_t i = 0; i < imguiKeyRowCount(); ++i) {
        for (std::size_t j = i + 1; j < imguiKeyRowCount(); ++j) {
            SOL_CHECK(imguiKeyRows()[i].imguiKey != imguiKeyRows()[j].imguiKey);
        }
    }
}

// ⚑ The check that exists only to catch a STRAY EXTRA ROW - the classic
// copy-paste typo, and the one mutation every other test in this file passes.
// A second row for a Key that already has one is unreachable: `imguiKeyFor`
// returns the first match, so the table still covers every Key, still claims no
// ImGuiKey twice, and still agrees with every name. Only the count notices.
// Same reasoning as `expectedKeysymRowCount` in the xkb suite next door, and
// simpler here because no Key is allowed a second row at all.
SOL_TEST(imgui_keys_the_table_has_exactly_one_row_per_key)
{
    SOL_CHECK(imguiKeyRowCount() == realKeyCount());
    for (int i = static_cast<int>(Key::Unknown) + 1; i < static_cast<int>(Key::Count); ++i) {
        SOL_CHECK(rowsFor(static_cast<Key>(i)) == 1);
    }
}

// ⚑⚑ THE ONE THAT CATCHES A SWAP, AND THE ONLY TEST HERE THAT LOOKS AT WHAT A
// ROW MEANS RATHER THAN AT WHERE IT SITS. Two rows trading their ImGuiKeys is
// the likeliest way this table goes wrong and the hardest to see by reading it,
// because both halves still look reasonable. Asking each side for its own name
// for the key settles it without restating the table: `chordName` walks
// `sol::platform`'s switch, `ImGui::GetKeyName` indexes ImGui's own array, and
// neither of them has ever heard of the file under test.
SOL_TEST(imgui_keys_each_row_names_the_same_key_on_both_sides)
{
    for (int i = static_cast<int>(Key::Unknown) + 1; i < static_cast<int>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        const std::string ours = normalizedName(chordName(InputChord::ofKey(key)));
        const std::string theirs = normalizedName(ImGui::GetKeyName(imguiKeyFor(key)));
        SOL_CHECK(ours == theirs);
    }
}
