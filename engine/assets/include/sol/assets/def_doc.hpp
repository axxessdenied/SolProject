#pragma once

// A def document as WRITTEN (engine plan Phase 9 stage H): the `[[model]]` and
// `[[ship]]` rows of `game/data/*.toml` carried with their comments, their key
// order and their numbers exactly as an author typed them, so the Forge can add
// or change one line of one row and leave every other byte of the file alone.
//
// ⚑ THIS IS NOT A SECOND SCHEMA, AND IT INTERPRETS NO VALUE. `DefDatabase` is
// the schema - strict, typed, range-checked, and the very thing the game loads
// through - and the tool validates a candidate by handing it there. What this
// file adds is the half a typed parse necessarily throws away: which keys were
// present, in what order, under which comments, spelled which way.
//
// ⚑⚑ VALUES ARE VERBATIM TEXT, AND THE REASON IS MEASURED RATHER THAN
// STYLISTIC. Re-emitting them from a parsed `double`, which is what
// `writeForge` safely does for a file the tool itself wrote, corrupts a
// hand-authored one three different ways:
//   - values across the committed def files are TOML INTEGERS, and
//     `optionalUint` requires `isInteger()` - so `crew_berths = 1` written
//     back as `1.0` is a LOAD FAILURE, not a reformat. (The count was 27 when
//     this was measured, of which fifteen were in `ships.toml`; Phase 31 stage
//     B deleted the four `slots_*` keys per hull, which is why the witness
//     `def_doc_tests` uses is `crew_berths` now. The hazard is unchanged.)
//   - `scan_range = 2.5e8` expands to `250000000.0`, because `appendNumber`
//     deliberately prefers a plain form while one round-trips - a rule that
//     exists to stop `90` becoming `9e+01`, correct there and wrong here.
//   - `alpha = 0.30` and `emissive = 0.10` silently lose their zero.
// A writer that echoes can do none of that. It also cannot delete a key an
// author typed at its schema default - the loss the E checkpoint found in
// `writeForge` - because nothing here ever decides what to emit.
//
// ⚑ The trivia rule is `forge_doc`'s, and for the same reason: comments and
// blank lines together, verbatim, attached to whatever comes BELOW them, which
// is how a person reads a heading.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sol::assets {

// ⚑⚑ ONE ENTRY OF AN INLINE TABLE, AS A RANGE RATHER THAN A COPY - which is
// `DefKey` itself one level down, for the same reason (Phase 25 stage D).
// `materials.toml` writes a material's texture slots and its tunable numbers as
// `textures = { glow = "cockpit_glow" }` and `params = { glow_strength = 2.2 }`,
// because `def_doc` models a def file as a flat list of rows and REFUSES a
// nested `[material.textures]` header - so an inline table is where those keys
// have to live, and the Forge has to be able to change one of them.
//
// ⚑ A WHOLE-TABLE REWRITE WAS THE OBVIOUS IMPLEMENTATION AND IT IS THE WRONG
// ONE. Re-emitting `{ a = 1.0, b = 2.0 }` from parsed values loses exactly what
// this file exists to keep: the author's spacing, their `2.2` that a double
// would print as `2.2000000476837158`, and their `0.30` that would come back as
// `0.3`. Splicing one value keeps every other byte, which is the same bargain
// `DefKey::setValue` already makes for a whole line.
struct DefInlineEntry
{
    std::string name;
    std::uint32_t valueBegin = 0;
    std::uint32_t valueEnd = 0; // one past the last byte, in the KEY'S text
};

// One `key = value` line, kept as the line rather than as a value.
//
// ⚑ `text` is the raw line with its trailing newline stripped, and it can span
// several source lines: `factions.toml` wraps four `station_bias` arrays across
// two lines each. The value is a RANGE inside it rather than a copy, so a write
// splices and everything on either side - the key's own spelling, the run of
// spaces before an aligned comment, the comment itself - survives untouched.
// Four committed lines carry a trailing comment (`ships.toml` explains
// `scan_range` on the line it sets it), and those are exactly the comments
// `ForgeDoc` has to flag as unplaceable; here they are just text.
struct DefKey
{
    std::string leading; // comments and blank lines above this line, verbatim
    std::string name;
    std::string text;
    std::uint32_t valueBegin = 0;
    std::uint32_t valueEnd = 0; // one past the last byte of the value

    [[nodiscard]] std::string_view value() const;
    // A string value with its quotes removed; the value itself when unquoted.
    [[nodiscard]] std::string_view unquoted() const;
    // The entries of an inline-table value, in written order. Empty when this
    // value is not an inline table, which is the same answer as an empty one -
    // a caller that cares knows which key it asked for.
    //
    // ⚑ ONE LEVEL DEEP, DELIBERATELY. A nested table or an array inside an
    // entry is not something any def file writes and not something this can
    // represent, so a value containing `{` or `[` is left as one opaque entry
    // rather than half-parsed into something a splice would corrupt.
    [[nodiscard]] std::vector<DefInlineEntry> inlineEntries() const;
    // Splices `v` into one inline-table entry, keeping every other byte of the
    // line. False when this value is not an inline table or has no such entry -
    // it never CREATES one, because a key that is not there is a schema change
    // and this file interprets no schema.
    bool setInlineValue(std::string_view entryName, std::string_view v);
    // Splices `v` into the value's range, keeping the prefix and any suffix.
    void setValue(std::string_view v);
};

// One `[[type]]` element and the keys under it, in file order.
struct DefRow
{
    std::string leading; // trivia above the `[[type]]` line
    std::string type;    // "model", "ship", "station", ... without the brackets
    std::string header;  // the `[[type]]` line itself, verbatim
    // The whitespace this row's header line opens with, and the prefix put on
    // any line the TOOL writes into it (Phase 31 stage D).
    //
    // ⚑ It exists because of nested rows. `ships.toml` indents every
    // `[[ship.mount]]` and its keys two spaces to show which hull they belong
    // to, and a mount the Forge creates has to look like the nineteen an author
    // wrote beside it - a flush-left row among indented siblings is valid TOML
    // and reads as damage. Existing lines are untouched either way: their text
    // is verbatim and already carries whatever indent it had.
    std::string indent;
    std::vector<DefKey> keys;

    [[nodiscard]] const DefKey* find(std::string_view name) const;
    [[nodiscard]] DefKey* find(std::string_view name);
    // The `id` key's unquoted value, or empty when the row has none.
    [[nodiscard]] std::string_view id() const;
    // Sets an existing key's value, or appends the key at the end of the row.
    // ⚑ Appending at the END rather than in schema order is deliberate: a def
    // file's key order is an author's, and a tool that sorted it would rewrite
    // every row it touched.
    void set(std::string_view name, std::string_view value);
    void remove(std::string_view name);
};

struct DefDoc
{
    // ⚑ There is no document-level `header` field, unlike `ForgeDoc`, and the
    // difference is in the formats rather than in the choice: a `.forge` opens
    // with a plain `name = ` key, while every def file opens straight into a
    // `[[table]]`. So a file's header comment is the FIRST ROW's `leading` -
    // which in `models.toml` is a 12-line preamble, part of the 57 comment
    // lines out of 107 that are where this project's reasoning about its own
    // assets is written down.
    std::vector<DefRow> rows;
    // Trivia after the last key, which belongs to no row below it.
    //
    // ⚑ A note written UNDER its subject lands here or on the next row's
    // `leading`, which is byte-exact but semantically wrong - `factions.toml`
    // explains why the Navy has no `station_bias` in three lines that sit after
    // its last key. It costs nothing while rows are only APPENDED, and it is
    // why they are.
    std::string trailer;

    // Whether the source's last line carried a newline. Kept because a writer
    // that adds one is a writer that changes a file it was asked not to.
    bool endsWithNewline = true;

    // ⚑ KNOWN LIMIT, named rather than half-handled: a line's ending lives
    // inside its own `text` (a CRLF file's '\r' rides there), so an EXISTING
    // line comes back exactly and a line the tool CREATES ends with a bare
    // '\n'. Editing a CRLF def file therefore mixes endings. Every def file in
    // this game is LF, so it cannot happen today; the fix, if one ever is CRLF,
    // is to carry the document's ending and let `set` and `append` use it.

    // True when a comment sits INSIDE a multi-line value, where this model has
    // nowhere to put it: the text round-trips exactly, but setting that value
    // would write over the comment. No committed def file has one - the four
    // wrapped `station_bias` arrays carry none - and a known gap a person is
    // told about is a different thing from a silent loss.
    bool hasUnplaceableComments = false;

    [[nodiscard]] const DefRow* find(std::string_view type, std::string_view id) const;
    [[nodiscard]] DefRow* find(std::string_view type, std::string_view id);
    [[nodiscard]] std::size_t count(std::string_view type) const;
    // A new row at the END of the document, with one blank line before it.
    DefRow& append(std::string_view type);

    // No such row. Distinct from index 0, which is a real row.
    static constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

    // Where a row sits, so a caller that has to reason about ORDER can. Every
    // reader before Phase 31 stage D wanted a row by name and nothing else.
    [[nodiscard]] std::size_t indexOf(std::string_view type, std::string_view id) const;

    // ⚑⚑ A NEW ROW IMMEDIATELY AFTER `index`, WHICH IS WHAT A NESTED ROW
    // NEEDS AND `append` CANNOT GIVE IT (Phase 31 stage D). A `[[ship.mount]]`
    // belongs to the `[[ship]]` above it, so a mount appended at the end of
    // `ships.toml` is a mount on the LAST hull in the file whatever the tool
    // meant - silently, and only visible after a reload.
    //
    // ⚑ It takes the new row's `indent` explicitly rather than copying the
    // neighbour's, because the two differ exactly where this is used: a mount
    // inserted after its HULL row (the hull has none) is the first mount on
    // that hull, and it still has to be indented like the ones on every other.
    DefRow& insertAfter(std::size_t index, std::string_view type, std::string_view indent = {});

    // Removes one row. ⚑ Its trivia moves DOWN to the row that inherits its
    // place, the same rule `DefRow::remove` applies to a key and for the same
    // reason - a comment above a row is about the file at that point. Removing
    // the LAST row leaves the trivia in the document's trailer.
    //
    // ⚑⚑ SO A NOTE CAN END UP OVER SOMETHING IT IS NOT ABOUT, AND THAT IS THE
    // DELIBERATE HALF. `ships.toml` explains most of its mounts in several lines
    // above the header, and deleting the mount is not the author saying to
    // delete the paragraph - the alternative loses their reasoning byte-exactly
    // and unrecoverably in a gesture that looked like removing one hardpoint. A
    // note left standing over the wrong row is visible and fixable; a note eaten
    // is neither. Same ruling as `def_surface.hpp`'s on a moved key's trivia.
    void eraseRow(std::size_t index);
};

// Parses a def document. Fails only on a line that is neither trivia, a table
// header nor `key = value` - the schema's job is `DefDatabase`'s, and running
// two validators over one file is how they come to disagree.
[[nodiscard]] bool parseDefs(
    const char* text, std::size_t length, const char* sourceName, DefDoc& out, std::string* error = nullptr);

// Serialises back. ⚑ For a document that came from `parseDefs`, all nine
// committed `game/data/*.toml` files come back BYTE FOR BYTE - comments, blank
// lines, key order, integer-vs-float spelling and exponent forms included.
// `assets.unit` asserts it against the real files, because a fixture the writer
// built agrees with the writer by construction.
[[nodiscard]] std::string writeDefs(const DefDoc& doc);

// Value formatters, so the callers that set a value do not each invent one.
// Applied only to values the tool is CHANGING, where the author's own spelling
// is being replaced anyway - everything untouched keeps its own text.
//
// ⚑ It takes a FLOAT deliberately. Every number `DefDatabase` reads lands in a
// float field, so the extra digits a double writer emits are not precision -
// they are the decimal expansion of a binary value carrying no more
// information, and a measured radius would arrive in the file as
// `1.1584000587463379`. The shortest form that survives a round trip through
// float is `1.1584`, which is also what a person would have typed.
//
// Prefers a plain form over an exponent, and always carries a `.` so TOML reads
// a float rather than an integer. Both rules are `forge_doc`'s, and the first
// exists because the shortest form of 90 is `9e+01`.
[[nodiscard]] std::string defNumber(float value);

// ⚑ The same, rounded to `decimals` places first, so a def file gets exactly
// the number the panel DISPLAYED. This is Phase 14's rule arriving in a second
// format: a measured radius written at full float precision reads
// `1.1583778` in a file whose neighbours carry one decimal, and it makes a
// button labelled "use measured 1.1584 m" write something else - a small lie in
// the UI on top of an unreadable line.
//
// Rounding cannot re-open the mismatch it is used to close: `ModelMatch`
// agrees within 0.1% and half a unit in the fourth decimal of a metre is 0.004%
// of the smallest radius in this game, three orders of magnitude inside it.
[[nodiscard]] std::string defNumber(float value, int decimals);
[[nodiscard]] std::string defString(std::string_view value);
[[nodiscard]] std::string defBool(bool value);

// ⚑⚑ THE FIRST INTEGER WRITER IN THIS FILE, AND IT IS NEEDED RATHER THAN TIDY
// (Phase 32 stage A). `defNumber` ALWAYS emits a `.`, on the argument above
// that every number the def schema reads lands in a float - but two keys are
// read with `optionalUint`, which checks `isInteger` and therefore REFUSES
// `1.0`. A tool writing a hull class through `defNumber` would produce a file
// its own validation pass rejects, one edit after the author touched a combo.
[[nodiscard]] std::string defInteger(std::int64_t value);

} // namespace sol::assets
