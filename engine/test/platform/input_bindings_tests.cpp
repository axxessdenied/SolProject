#include <sol/platform/input_bindings.hpp>

#include <sol/test/test.hpp>

#include <cstring>
#include <string>

using sol::platform::BindingTable;
using sol::platform::chordAt;
using sol::platform::chordFromName;
using sol::platform::chordName;
using sol::platform::chordUniverseSize;
using sol::platform::InputChord;
using sol::platform::InputSnapshot;
using sol::platform::Key;
using sol::platform::MouseButton;

namespace {

// Stand-ins for the game's action ids: the table is deliberately ignorant of
// what any of them mean, so the tests are too.
constexpr std::uint32_t kAlpha = 0;
constexpr std::uint32_t kBravo = 1;
constexpr std::uint32_t kCharlie = 2;
constexpr std::uint32_t kActionCount = 3;

BindingTable tableWithDefaults()
{
    BindingTable table;
    table.setActionCount(kActionCount);
    table.bind(kAlpha, InputChord::ofKey(Key::T));
    table.bind(kBravo, InputChord::ofKey(Key::C));
    table.bind(kCharlie, InputChord::ofMouse(MouseButton::Middle));
    return table;
}

// Drives one frame of the table from a hand-built snapshot.
void frame(BindingTable& table, const InputSnapshot& snapshot)
{
    table.beginFrame(snapshot);
}

} // namespace

SOL_TEST(every_chord_name_round_trips)
{
    const std::size_t count = chordUniverseSize();
    SOL_REQUIRE(count > 0);
    for (std::size_t i = 0; i < count; ++i) {
        const InputChord chord = chordAt(i);
        SOL_REQUIRE(chord.bound());
        const char* name = chordName(chord);
        // A chord that serialises as "None" could never be read back, which
        // would strand the binding silently on the next load.
        SOL_CHECK(std::strcmp(name, "None") != 0);
        SOL_CHECK(chordFromName(name) == chord);
    }
}

SOL_TEST(chord_names_are_unique)
{
    // Two chords sharing a name would make the round trip above pass while
    // still losing one of them on load.
    const std::size_t count = chordUniverseSize();
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            SOL_CHECK(std::strcmp(chordName(chordAt(i)), chordName(chordAt(j))) != 0);
        }
    }
}

SOL_TEST(unknown_chord_name_is_unbound_not_garbage)
{
    SOL_CHECK(!chordFromName("Nonsense").bound());
    SOL_CHECK(!chordFromName("").bound());
    SOL_CHECK(!chordFromName("None").bound());
    // Case matters: a near miss must not silently land on a real key.
    SOL_CHECK(!chordFromName("left mouse").bound());
}

SOL_TEST(mouse_buttons_are_chords_like_keys)
{
    const InputChord middle = InputChord::ofMouse(MouseButton::Middle);
    SOL_CHECK(middle.bound());
    SOL_CHECK(std::strcmp(chordName(middle), "Middle Mouse") == 0);
    SOL_CHECK(chordFromName("Middle Mouse") == middle);
    SOL_CHECK(middle != InputChord::ofKey(Key::M));
}

SOL_TEST(unbound_chords_compare_equal_regardless_of_code)
{
    InputChord a;
    InputChord b;
    b.code = 7; // an unbound chord's code is meaningless
    SOL_CHECK(a == b);
    SOL_CHECK(!a.bound());
}

SOL_TEST(assign_steals_the_chord_and_reports_the_victim)
{
    BindingTable table = tableWithDefaults();
    const std::uint32_t stolen = table.assign(kBravo, InputChord::ofKey(Key::T));
    SOL_CHECK(stolen == kAlpha);
    SOL_CHECK(table.chordFor(kBravo) == InputChord::ofKey(Key::T));
    // The victim is left unbound - a legal, visible state, not a crash.
    SOL_CHECK(!table.chordFor(kAlpha).bound());
    SOL_CHECK(table.find(InputChord::ofKey(Key::T)) == kBravo);
    SOL_CHECK(table.find(InputChord::ofKey(Key::C)) == BindingTable::kNoAction);
}

SOL_TEST(assign_to_a_free_chord_steals_nothing)
{
    BindingTable table = tableWithDefaults();
    SOL_CHECK(table.assign(kAlpha, InputChord::ofKey(Key::K)) == BindingTable::kNoAction);
    SOL_CHECK(table.chordFor(kAlpha) == InputChord::ofKey(Key::K));
    SOL_CHECK(table.chordFor(kBravo) == InputChord::ofKey(Key::C));
}

SOL_TEST(assigning_an_action_its_own_chord_does_not_unbind_it)
{
    BindingTable table = tableWithDefaults();
    SOL_CHECK(table.assign(kAlpha, InputChord::ofKey(Key::T)) == BindingTable::kNoAction);
    SOL_CHECK(table.chordFor(kAlpha) == InputChord::ofKey(Key::T));
}

SOL_TEST(two_actions_can_be_swapped_in_two_assignments)
{
    // The reason the policy is steal rather than refuse: this sequence is what
    // a player does when they want T and C the other way round.
    BindingTable table = tableWithDefaults();
    SOL_CHECK(table.assign(kAlpha, InputChord::ofKey(Key::C)) == kBravo);
    SOL_CHECK(table.assign(kBravo, InputChord::ofKey(Key::T)) == BindingTable::kNoAction);
    SOL_CHECK(table.chordFor(kAlpha) == InputChord::ofKey(Key::C));
    SOL_CHECK(table.chordFor(kBravo) == InputChord::ofKey(Key::T));
}

SOL_TEST(find_and_lookup_ignore_unbound_actions)
{
    BindingTable table = tableWithDefaults();
    table.unbind(kBravo);
    SOL_CHECK(!table.chordFor(kBravo).bound());
    // An unbound action must never be the answer to "who holds nothing".
    SOL_CHECK(table.find(InputChord{}) == BindingTable::kNoAction);
    SOL_CHECK(table.find(InputChord::ofKey(Key::C)) == BindingTable::kNoAction);
}

SOL_TEST(out_of_range_actions_are_ignored_not_undefined)
{
    BindingTable table = tableWithDefaults();
    table.bind(99, InputChord::ofKey(Key::J));
    SOL_CHECK(table.assign(99, InputChord::ofKey(Key::J)) == BindingTable::kNoAction);
    SOL_CHECK(!table.chordFor(99).bound());
    SOL_CHECK(table.find(InputChord::ofKey(Key::J)) == BindingTable::kNoAction);
    SOL_CHECK(!table.held(99));
    SOL_CHECK(!table.pressed(99));
}

SOL_TEST(pressed_fires_once_per_press_and_rearms_on_release)
{
    BindingTable table = tableWithDefaults();
    InputSnapshot snapshot;

    // Nothing down.
    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kAlpha));
    SOL_CHECK(!table.held(kAlpha));

    // Key goes down: one press edge, held from now on.
    snapshot.setDown(InputChord::ofKey(Key::T), true);
    frame(table, snapshot);
    SOL_CHECK(table.pressed(kAlpha));
    SOL_CHECK(table.held(kAlpha));

    // Still down: held, but no second edge. This is the bug the hand-written
    // previousX bools existed to prevent, now in one place.
    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kAlpha));
    SOL_CHECK(table.held(kAlpha));

    // Released, then pressed again: the edge re-arms.
    snapshot.setDown(InputChord::ofKey(Key::T), false);
    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kAlpha));
    SOL_CHECK(!table.held(kAlpha));

    snapshot.setDown(InputChord::ofKey(Key::T), true);
    frame(table, snapshot);
    SOL_CHECK(table.pressed(kAlpha));
}

SOL_TEST(released_is_the_other_edge_and_does_not_overlap_pressed)
{
    // The bookmark prompt opens on release rather than press (Phase 8h), so
    // the two edges have to be distinguishable and must never coincide.
    BindingTable table = tableWithDefaults();
    InputSnapshot snapshot;
    frame(table, snapshot);
    SOL_CHECK(!table.released(kAlpha));

    snapshot.setDown(InputChord::ofKey(Key::T), true);
    frame(table, snapshot);
    SOL_CHECK(table.pressed(kAlpha));
    SOL_CHECK(!table.released(kAlpha));

    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kAlpha));
    SOL_CHECK(!table.released(kAlpha));

    snapshot.setDown(InputChord::ofKey(Key::T), false);
    frame(table, snapshot);
    SOL_CHECK(table.released(kAlpha));
    SOL_CHECK(!table.pressed(kAlpha));
    SOL_CHECK(!table.held(kAlpha));

    // One release, not one per frame the key stays up.
    frame(table, snapshot);
    SOL_CHECK(!table.released(kAlpha));
}

SOL_TEST(an_unbound_action_never_reports_a_release)
{
    // A release edge derived from "not down now, down before" must not fire
    // for an action holding nothing - that would open the bookmark prompt on
    // its own the frame after the binding was stolen.
    BindingTable table = tableWithDefaults();
    InputSnapshot snapshot;
    snapshot.setDown(InputChord::ofKey(Key::T), true);
    frame(table, snapshot);
    table.unbind(kAlpha);
    snapshot.setDown(InputChord::ofKey(Key::T), false);
    frame(table, snapshot);
    SOL_CHECK(!table.released(kAlpha));
}

SOL_TEST(a_mouse_binding_edges_exactly_like_a_key)
{
    BindingTable table = tableWithDefaults();
    InputSnapshot snapshot;
    frame(table, snapshot);

    snapshot.setDown(InputChord::ofMouse(MouseButton::Middle), true);
    frame(table, snapshot);
    SOL_CHECK(table.pressed(kCharlie));
    SOL_CHECK(table.held(kCharlie));
    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kCharlie));
    SOL_CHECK(table.held(kCharlie));
}

SOL_TEST(rebinding_moves_the_press_to_the_new_chord)
{
    // The exit criterion in miniature: after a rebind the old chord is dead
    // and the new one works, without the table being rebuilt.
    BindingTable table = tableWithDefaults();
    InputSnapshot snapshot;
    frame(table, snapshot);

    (void)table.assign(kAlpha, InputChord::ofKey(Key::K));

    snapshot.setDown(InputChord::ofKey(Key::T), true);
    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kAlpha));

    snapshot.setDown(InputChord::ofKey(Key::T), false);
    snapshot.setDown(InputChord::ofKey(Key::K), true);
    frame(table, snapshot);
    SOL_CHECK(table.pressed(kAlpha));
}

SOL_TEST(an_unbound_action_never_fires)
{
    BindingTable table = tableWithDefaults();
    table.unbind(kAlpha);
    InputSnapshot snapshot;
    frame(table, snapshot);
    // Every chord in the universe down at once: the unbound action still
    // reports nothing, rather than matching whatever code 0 happens to be.
    for (std::size_t i = 0; i < chordUniverseSize(); ++i) {
        snapshot.setDown(chordAt(i), true);
    }
    frame(table, snapshot);
    SOL_CHECK(!table.pressed(kAlpha));
    SOL_CHECK(!table.held(kAlpha));
}

SOL_TEST(captured_reports_the_chord_that_just_went_down)
{
    BindingTable table = tableWithDefaults();
    InputSnapshot snapshot;
    frame(table, snapshot);
    SOL_CHECK(!table.captured().bound());

    // Capture reports chords nothing is bound to - that is the point of it.
    snapshot.setDown(InputChord::ofKey(Key::K), true);
    frame(table, snapshot);
    SOL_CHECK(table.captured() == InputChord::ofKey(Key::K));

    // Held, not newly pressed: capture is an edge, so it does not repeat.
    frame(table, snapshot);
    SOL_CHECK(!table.captured().bound());

    snapshot.setDown(InputChord::ofKey(Key::K), false);
    snapshot.setDown(InputChord::ofMouse(MouseButton::Right), true);
    frame(table, snapshot);
    SOL_CHECK(table.captured() == InputChord::ofMouse(MouseButton::Right));
}

SOL_TEST(snapshot_tracks_chords_independently)
{
    InputSnapshot snapshot;
    // Key indices span more than one 64-bit word; a bad shift would alias
    // F-keys onto letters.
    snapshot.setDown(InputChord::ofKey(Key::A), true);
    snapshot.setDown(InputChord::ofKey(Key::F12), true);
    SOL_CHECK(snapshot.down(InputChord::ofKey(Key::A)));
    SOL_CHECK(snapshot.down(InputChord::ofKey(Key::F12)));
    SOL_CHECK(!snapshot.down(InputChord::ofKey(Key::B)));
    SOL_CHECK(!snapshot.down(InputChord::ofMouse(MouseButton::Left)));

    snapshot.setDown(InputChord::ofKey(Key::A), false);
    SOL_CHECK(!snapshot.down(InputChord::ofKey(Key::A)));
    SOL_CHECK(snapshot.down(InputChord::ofKey(Key::F12)));

    snapshot.clear();
    SOL_CHECK(!snapshot.down(InputChord::ofKey(Key::F12)));
}
