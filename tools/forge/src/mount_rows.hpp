#pragma once

// Where a `[[ship.mount]]` row lives in `ships.toml`, and how a tool adds one
// (engine plan Phase 31 stage D).
//
// ⚑⚑ A NESTED ROW IS THE FIRST THING IN THIS PROGRAMME THAT HAS AN OWNER, AND
// EVERY RULE BELOW IS A CONSEQUENCE. `DefDoc` models a def file as a flat list
// of rows, which is exactly right for `[[model]]` and `[[ship]]` - they are
// unordered, and `find(type, id)` is the whole of what a caller ever wanted. A
// `[[ship.mount]]` is not unordered: it belongs to the `[[ship]]` above it and
// to nothing else, so *position* carries meaning that `find` cannot see.
//
//   - a mount is found by hull AND id, never by id alone. `gun_nose` is a real
//     mount on TWO shipped hulls, and `DefDoc::find("ship.mount", "gun_nose")`
//     answers with whichever comes first in the file.
//   - a mount is INSERTED, never appended. `DefDoc::append` puts a row at the
//     end of the document, which in `ships.toml` means the last hull in the
//     file - silently, and visibly only after a reload.
//   - a mount id is unique on its HULL, not in the document. `def_editor.cpp`'s
//     `uniqueId` is document-wide, which is right for a ship id and would
//     refuse the second hull a `gun_nose` here.
//
// ⚑ PURE FUNCTIONS OF A DOCUMENT, WHICH IS WHY THIS IS ITS OWN HEADER rather
// than four more methods on `DefEditor`. `sol_forge_tests` links no ImGui, no
// device and no window, so anything living in `def_editor.cpp` is untestable in
// the only suite that could test it - the same ruling `part_pick.hpp` and
// `list_layout.hpp` were written under.

#include "sol/assets/data_defs.hpp"
#include "sol/assets/def_doc.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace forge {

// The def spelling of a nested mount row, in one place.
inline constexpr const char* kMountRowType = "ship.mount";

// Every key a mount may carry, in the schema's own order - the same set
// `parseShip`'s `rejectUnknownKeys` allows, so a key added there and not here
// is a key this tool would silently never write.
inline constexpr const char* kMountKeys[] = {"id", "kind", "size", "at", "aim", "arc", "fit"};

// ⚑ What a mount row the tool creates is indented with when the hull has no
// mount to copy from. `ships.toml` uses two spaces on all nineteen; a file that
// uses something else is followed rather than corrected, which is what
// `mountIndent` below is for.
inline constexpr const char* kDefaultMountIndent = "  ";

// The `[[ship.mount]]` rows under the hull row at `hull`, in file order.
//
// ⚑ IT STOPS AT THE FIRST ROW THAT IS NOT A MOUNT, which is the whole test. The
// rows are flat, so "this hull's mounts" is a RUN rather than a set, and a walk
// that filtered the document by type instead would hand every hull all nineteen.
[[nodiscard]] inline std::vector<std::size_t> mountRowsOf(const sol::assets::DefDoc& doc, std::size_t hull)
{
    std::vector<std::size_t> rows;
    if (hull >= doc.rows.size()) {
        return rows;
    }
    for (std::size_t i = hull + 1; i < doc.rows.size() && doc.rows[i].type == kMountRowType; ++i) {
        rows.push_back(i);
    }
    return rows;
}

// One mount on one hull, or `kNoRow`.
[[nodiscard]] inline std::size_t
findMountRow(const sol::assets::DefDoc& doc, std::size_t hull, std::string_view mountId)
{
    for (const std::size_t row : mountRowsOf(doc, hull)) {
        if (doc.rows[row].id() == mountId) {
            return row;
        }
    }
    return sol::assets::DefDoc::kNoRow;
}

// The row a new mount is inserted AFTER: the hull's last mount, or the hull row
// itself when it has none.
[[nodiscard]] inline std::size_t mountInsertPoint(const sol::assets::DefDoc& doc, std::size_t hull)
{
    const std::vector<std::size_t> rows = mountRowsOf(doc, hull);
    return rows.empty() ? hull : rows.back();
}

// The indentation a new mount should carry: whatever this hull's mounts already
// use, and two spaces when it has none.
[[nodiscard]] inline std::string mountIndent(const sol::assets::DefDoc& doc, std::size_t hull)
{
    const std::vector<std::size_t> rows = mountRowsOf(doc, hull);
    return rows.empty() ? std::string(kDefaultMountIndent) : doc.rows[rows.front()].indent;
}

// A mount id not taken on THIS hull, derived from `base`.
[[nodiscard]] inline std::string
uniqueMountId(const sol::assets::DefDoc& doc, std::size_t hull, std::string_view base)
{
    const std::string stem(base.empty() ? std::string_view("mount") : base);
    if (findMountRow(doc, hull, stem) == sol::assets::DefDoc::kNoRow) {
        return stem;
    }
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = stem + "_" + std::to_string(n);
        if (findMountRow(doc, hull, candidate) == sol::assets::DefDoc::kNoRow) {
            return candidate;
        }
    }
    return stem;
}

// Writes a three-vector the way `ships.toml` spells one.
//
// ⚑ `defNumber(value, decimals)` and not the full-precision form, for Phase 14's
// reason arriving in a third format: a position dragged in the viewport lands at
// something like `-6.5999999` and its neighbours in the file carry one decimal.
// The panel shows four; the file gets exactly what the panel showed.
inline void writeMountVector(sol::assets::DefRow& row, const char* key, const float (&v)[3], int decimals)
{
    std::string text = "[";
    for (int i = 0; i < 3; ++i) {
        text += sol::assets::defNumber(v[i], decimals);
        if (i < 2) {
            text += ", ";
        }
    }
    text += "]";
    row.set(key, text);
}

// ⚑⚑ WHAT A MOUNT OF EACH KIND IS CALLED IN THIS GAME, AND THE DRIVE IS WHAT
// ASKED FOR IT. The first placement produced a mount whose id was `fixed` on a
// mount whose kind was `fixed`, which reads as a tool that could not think of a
// name - and this id is not cosmetic: a save names a fitting by it, so it is the
// string a player's ship carries for as long as that campaign lasts.
//
// The stems below are read off `ships.toml`'s own nineteen (`gun_nose`,
// `drive_main`, `shield_core`, `bay_port`, `rack_spine`, `turret_dorsal`,
// `hold_forward`, `core_sensor`), so a mount the Forge creates is named the way
// a mount an author wrote is. `uniqueMountId` then suffixes it, giving `gun`,
// `gun_2`, `gun_3` - which is a worse name than `gun_nose` and a much better one
// than `fixed`, and is a default rather than a decision: the id field is right
// above it.
[[nodiscard]] inline const char* mountIdStem(sol::assets::MountKind kind)
{
    switch (kind) {
    case sol::assets::MountKind::Turret:
        return "turret";
    case sol::assets::MountKind::Fixed:
        return "gun";
    case sol::assets::MountKind::Launcher:
        return "rack";
    case sol::assets::MountKind::Bay:
        return "bay";
    case sol::assets::MountKind::Engine:
        return "drive";
    case sol::assets::MountKind::Thruster:
        return "thruster";
    case sol::assets::MountKind::Shield:
        return "shield";
    case sol::assets::MountKind::Armor:
        return "armor";
    case sol::assets::MountKind::Utility:
        return "hold";
    case sol::assets::MountKind::Subsystem:
        return "core";
    case sol::assets::MountKind::Hangar:
        return "hangar";
    case sol::assets::MountKind::Dock:
        return "dock";
    case sol::assets::MountKind::Count:
        break;
    }
    return "mount";
}

// A mount as the tool is about to write it, before any row exists.
//
// ⚑ `kind` AND `size` HAVE NO DEFAULT IN THE SCHEMA AND SO HAVE NONE HERE
// EITHER - `parseShip` refuses a mount missing either, deliberately, because a
// kind the parser invented is a hull silently accepting kit its author never
// said it takes. The tool always writes both, which is what makes a created
// mount valid by construction rather than by the author remembering.
struct MountDraft
{
    std::string id;
    sol::assets::MountKind kind = sol::assets::MountKind::Utility;
    sol::assets::MountSize size = sol::assets::MountSize::Small;
    // Present means external: drawn on the hull, shot at where it sits, and -
    // since stage C1 - the muzzle a gun fitted here fires from. Absent means
    // internal (decisions/014 rule 2).
    bool external = false;
    float at[3] = {0.0f, 0.0f, 0.0f};
};

// Writes a draft into a fresh row, in the schema's key order.
//
// ⚑ ONLY THE KEYS THE DRAFT HAS. A row carrying `at` when the author asked for
// an internal mount is a mount that gets drawn and shot at, and one carrying
// `aim` without `at` is a parse ERROR - the schema refuses that pair by name
// rather than dropping it, so a tool that wrote both unconditionally would be
// a tool that cannot create an internal mount at all.
inline void writeMountDraft(sol::assets::DefRow& row, const MountDraft& draft, int decimals)
{
    row.set("id", sol::assets::defString(draft.id));
    row.set("kind", sol::assets::defString(sol::assets::mountKindName(draft.kind)));
    row.set("size", sol::assets::defString(sol::assets::mountSizeName(draft.size)));
    if (draft.external) {
        writeMountVector(row, "at", draft.at, decimals);
    }
}

} // namespace forge
