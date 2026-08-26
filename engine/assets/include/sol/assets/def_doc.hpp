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
//   - 27 values across the nine committed def files are TOML INTEGERS, and
//     `optionalUint` requires `isInteger()` - so `slots_engine = 1` written
//     back as `1.0` is a LOAD FAILURE, not a reformat. Fifteen of them are in
//     `ships.toml`, which is a file this stage writes to.
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
    // Splices `v` into the value's range, keeping the prefix and any suffix.
    void setValue(std::string_view v);
};

// One `[[type]]` element and the keys under it, in file order.
struct DefRow
{
    std::string leading; // trivia above the `[[type]]` line
    std::string type;    // "model", "ship", "station", ... without the brackets
    std::string header;  // the `[[type]]` line itself, verbatim
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

} // namespace sol::assets
