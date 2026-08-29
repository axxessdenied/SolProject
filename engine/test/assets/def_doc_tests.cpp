#include "sol/assets/data_defs.hpp"
#include "sol/assets/def_doc.hpp"
#include "sol/test/test.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sol;
using assets::DefDoc;
using assets::DefRow;

namespace {

[[nodiscard]] bool parses(const std::string& text, DefDoc& out)
{
    std::string error;
    if (assets::parseDefs(text.c_str(), text.size(), "test.toml", out, &error)) {
        return true;
    }
    std::printf("  unexpected parse failure: %s\n", error.c_str());
    return false;
}

[[nodiscard]] std::string rejects(const std::string& text)
{
    DefDoc doc;
    std::string error;
    if (assets::parseDefs(text.c_str(), text.size(), "test.toml", doc, &error)) {
        return {};
    }
    return error.empty() ? "rejected" : error;
}

[[nodiscard]] std::string readWholeFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  cannot open %s\n", path.c_str());
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void reportFirstDifferingLine(const char* name, const std::string& expected, const std::string& actual)
{
    std::size_t line = 1;
    std::size_t start = 0;
    const std::size_t shortest = expected.size() < actual.size() ? expected.size() : actual.size();
    for (std::size_t i = 0; i < shortest; ++i) {
        if (expected[i] != actual[i]) {
            const std::size_t expectedEnd = expected.find('\n', start);
            const std::size_t actualEnd = actual.find('\n', start);
            std::printf("  %s line %zu\n    source: %s\n    writer: %s\n",
                        name,
                        line,
                        expected.substr(start, expectedEnd - start).c_str(),
                        actual.substr(start, actualEnd - start).c_str());
            return;
        }
        if (expected[i] == '\n') {
            ++line;
            start = i + 1;
        }
    }
    std::printf("  %s: identical for %zu bytes, then source has %zu and writer %zu\n",
                name,
                shortest,
                expected.size(),
                actual.size());
}

// Every def file the base game ships. The list is spelled out rather than
// globbed so that a file DISAPPEARING is a failure rather than a shorter loop.
[[nodiscard]] std::vector<std::string> committedDefs()
{
    // ⚑ `systems` joined this list in Phase 29 stage D, and it is the first
    // committed def file with a NESTED array-of-tables header in it
    // (`[[system.planet]]`). Phase 25 stage A cost a session to a sub-header
    // that parsed fine for the game and broke this document model - so the
    // question got asked before the file shipped rather than after: `[[a.b]]`
    // is still a `[[table]]`, and DefDoc keeps it as a row of its own with its
    // header line raw, which round-trips. A plain `[a.b]` is what it refuses,
    // and no def kind in this game uses one.
    return {"commodities",
            "crew",
            "factions",
            "models",
            "modules",
            "ships",
            "sounds",
            "stations",
            "systems",
            "weapons"};
}

[[nodiscard]] std::string defPath(const std::string& stem)
{
    return std::string(SOL_DEF_DATA_DIR) + "/" + stem + ".toml";
}

// The schema the tool validates through, over one document.
[[nodiscard]] bool schemaAccepts(const std::string& text, std::string& error)
{
    assets::DefDatabase defs;
    return defs.mergeToml(text.c_str(), text.size(), "candidate.toml", &error);
}

} // namespace

// ⚑ THE STAGE's LOAD-BEARING TEST. A writer that reformats a committed file is
// one nobody will let near a committed file, and this is the only assertion
// that can tell the difference between "parses" and "preserves".
SOL_TEST(defDocumentsRoundTripTheCommittedFilesByteForByte)
{
    for (const std::string& stem : committedDefs()) {
        const std::string source = readWholeFile(defPath(stem));
        SOL_REQUIRE(!source.empty());
        DefDoc doc;
        SOL_REQUIRE(parses(source, doc));
        const std::string written = assets::writeDefs(doc);
        if (written != source) {
            reportFirstDifferingLine(stem.c_str(), source, written);
        }
        SOL_CHECK(written == source);
        // Nothing in this game writes a comment inside a multi-line array, and
        // if one appears the tool must say so rather than overwrite it.
        SOL_CHECK(!doc.hasUnplaceableComments);
    }
}

SOL_TEST(defDocumentsKeepEveryRowAndKeyTheSchemaSees)
{
    const std::string source = readWholeFile(defPath("models"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    // ⚑ Derived, not pinned. This asserts what the test is NAMED for - the
    // document sees every row the schema does - and a literal 7 asserted the
    // number of models the game happens to ship, which is a different claim
    // that fails the first time anyone adds one. Phase 16 retired exactly this
    // shape for the shipped meshes; adding `freighter_cockpit` found two more
    // of it here.
    assets::DefDatabase schema;
    std::string schemaError;
    SOL_REQUIRE(schema.mergeToml(source.c_str(), source.size(), "models.toml", &schemaError));
    SOL_CHECK(doc.count("model") == schema.models().size());
    SOL_CHECK(doc.count("model") >= 7); // the set it shipped with, as a floor

    const DefRow* gate = doc.find("model", "gate");
    SOL_REQUIRE(gate != nullptr);
    SOL_CHECK(gate->type == "model");
    SOL_CHECK(gate->id() == "gate");
    // Key ORDER is the author's, and it is preserved rather than normalised.
    SOL_REQUIRE(gate->keys.size() == 5);
    SOL_CHECK(gate->keys[0].name == "id");
    SOL_CHECK(gate->keys[1].name == "mesh");
    SOL_CHECK(gate->keys[2].name == "texture");
    SOL_CHECK(gate->keys[3].name == "radius");
    SOL_CHECK(gate->keys[4].name == "solid");
    SOL_CHECK(gate->keys[3].value() == "106.7");
    SOL_CHECK(gate->keys[4].value() == "false");
    SOL_CHECK(gate->keys[1].unquoted() == "gate");

    // The file's header comment is the FIRST ROW's leading trivia, because a
    // def file opens straight into a table rather than into a plain key.
    SOL_REQUIRE(!doc.rows.empty());
    SOL_CHECK(doc.rows.front().leading.find("Drawable models (Phase 9)") != std::string::npos);
    // And the 14 lines above the gate belong to the gate.
    SOL_CHECK(gate->leading.find("doorway you fly THROUGH") != std::string::npos);
}

// ⚑⚑ THE THREE HAZARDS THAT DECIDED THE DESIGN, ASSERTED ON THE REAL FILES.
// Each one is a way a value-parsing writer corrupts a hand-authored def, and
// each is measured rather than imagined - so if somebody later "simplifies"
// this document into a typed one, these are what fail.

// 27 committed values are TOML INTEGERS, and `optionalUint` rejects a float.
// This is a LOAD FAILURE rather than a cosmetic reformat, which is why it is
// asserted against the schema and not only against the bytes.
SOL_TEST(defDocumentKeepsAnIntegerAnInteger)
{
    const std::string source = readWholeFile(defPath("ships"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    const DefRow* shuttle = doc.find("ship", "sol.shuttle");
    SOL_REQUIRE(shuttle != nullptr);
    const assets::DefKey* slots = shuttle->find("slots_engine");
    SOL_REQUIRE(slots != nullptr);
    SOL_CHECK(slots->value() == "1");
    SOL_CHECK(slots->value() != "1.0");

    // The proof that the distinction is load-bearing: spell it as a float and
    // the game's own schema refuses the file.
    std::string mangled = source;
    const std::size_t at = mangled.find("slots_engine = 1");
    SOL_REQUIRE(at != std::string::npos);
    mangled.replace(at, std::strlen("slots_engine = 1"), "slots_engine = 1.0");
    std::string error;
    SOL_CHECK(!schemaAccepts(mangled, error));
    SOL_CHECK(error.find("slots_engine") != std::string::npos);

    // And the document as parsed still satisfies the schema, which is what the
    // tool relies on when it validates before writing.
    std::string ok;
    SOL_CHECK(schemaAccepts(assets::writeDefs(doc), ok));
}

// `scan_range = 2.5e8` would expand to `250000000.0` through a number, because
// the .forge writer prefers a plain form while one round-trips.
SOL_TEST(defDocumentKeepsAnExponentForm)
{
    const std::string source = readWholeFile(defPath("ships"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    const DefRow* shuttle = doc.find("ship", "sol.shuttle");
    SOL_REQUIRE(shuttle != nullptr);
    const assets::DefKey* range = shuttle->find("scan_range");
    SOL_REQUIRE(range != nullptr);
    SOL_CHECK(range->value() == "2.5e8");
    // What a value-parsing writer would have produced instead.
    SOL_CHECK(assets::defNumber(2.5e8f) == "250000000.0");
    SOL_CHECK(range->value() != assets::defNumber(2.5e8f));
}

// `alpha = 0.30` and `emissive = 0.10` lose their zero through a number.
//
// ⚑ It reads materials.toml since Phase 25 stage B: the membrane's surface
// keys moved onto `sol.gate_membrane` when the row took its own shader, and
// they moved with their SPELLING intact - which is the whole claim here.
SOL_TEST(defDocumentKeepsAFractionalTrailingZero)
{
    const std::string source = readWholeFile(defPath("materials"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    const DefRow* membrane = doc.find("material", "sol.gate_membrane");
    SOL_REQUIRE(membrane != nullptr);
    const assets::DefKey* alpha = membrane->find("alpha");
    SOL_REQUIRE(alpha != nullptr);
    SOL_CHECK(alpha->value() == "0.30");
    SOL_CHECK(assets::defNumber(0.30f) == "0.3");
    SOL_CHECK(alpha->value() != assets::defNumber(0.30f));
}

// A trailing comment sits on four committed lines. It is not trivia this model
// has to place - it is part of the line - and an edit must write UNDER it.
SOL_TEST(defDocumentEditKeepsATrailingCommentOnTheSameLine)
{
    const std::string source = readWholeFile(defPath("ships"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    DefRow* shuttle = doc.find("ship", "sol.shuttle");
    SOL_REQUIRE(shuttle != nullptr);
    assets::DefKey* speed = shuttle->find("scan_speed");
    SOL_REQUIRE(speed != nullptr);
    SOL_CHECK(speed->text == "scan_speed = 1.0     # target-scan rate multiplier");
    speed->setValue("1.25");
    SOL_CHECK(speed->text == "scan_speed = 1.25     # target-scan rate multiplier");
    SOL_CHECK(speed->value() == "1.25");
    // The aligned comment on the id line survives an untouched save too.
    const assets::DefKey* id = shuttle->find("id");
    SOL_REQUIRE(id != nullptr);
    SOL_CHECK(id->text.find("# the player's ship") != std::string::npos);
    SOL_CHECK(assets::writeDefs(doc).find("scan_speed = 1.25     # target-scan") != std::string::npos);
}

SOL_TEST(defDocumentEditChangesOnlyTheLineItTouched)
{
    const std::string source = readWholeFile(defPath("models"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    DefRow* asteroid = doc.find("model", "asteroid");
    SOL_REQUIRE(asteroid != nullptr);
    asteroid->set("radius", assets::defNumber(1.1584f));

    const std::string written = assets::writeDefs(doc);
    SOL_CHECK(written != source);
    SOL_CHECK(written.find("radius = 1.1584") != std::string::npos);
    // Exactly one line differs, which is the property the whole document exists
    // for: a five-line edit buried under a rewritten file is not reviewable.
    std::size_t differing = 0;
    std::size_t a = 0;
    std::size_t b = 0;
    while (a < source.size() || b < written.size()) {
        const std::size_t aEnd = source.find('\n', a);
        const std::size_t bEnd = written.find('\n', b);
        const std::string left = source.substr(a, aEnd - a);
        const std::string right = written.substr(b, bEnd - b);
        differing += static_cast<std::size_t>(left != right);
        if (aEnd == std::string::npos || bEnd == std::string::npos) {
            break;
        }
        a = aEnd + 1;
        b = bEnd + 1;
    }
    SOL_CHECK(differing == 1);
    std::string error;
    SOL_CHECK(schemaAccepts(written, error));
}

SOL_TEST(defDocumentAddsAKeyAtTheEndOfItsRow)
{
    const std::string source = readWholeFile(defPath("models"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    DefRow* ship = doc.find("model", "ship");
    SOL_REQUIRE(ship != nullptr);
    const std::size_t before = ship->keys.size();
    ship->set("emissive", assets::defNumber(0.05f));
    SOL_CHECK(ship->keys.size() == before + 1);
    SOL_CHECK(ship->keys.back().name == "emissive");

    const std::string written = assets::writeDefs(doc);
    SOL_CHECK(written.find("radius = 8.0\nemissive = 0.05\n") != std::string::npos);
    std::string error;
    SOL_CHECK(schemaAccepts(written, error));
}

SOL_TEST(defDocumentAppendsARowAfterTheLastOne)
{
    const std::string source = readWholeFile(defPath("models"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    // The count BEFORE, so the assertion below is "appending adds one" rather
    // than "the game ships eight models".
    assets::DefDatabase beforeDefs;
    std::string beforeError;
    SOL_REQUIRE(beforeDefs.mergeToml(source.c_str(), source.size(), "models.toml", &beforeError));
    const std::size_t modelsBefore = beforeDefs.models().size();

    DefRow& row = doc.append("model");
    row.set("id", assets::defString("beacon"));
    row.set("mesh", assets::defString("gate"));
    row.set("texture", assets::defString("hull"));
    row.set("radius", assets::defNumber(12.0f));

    const std::string written = assets::writeDefs(doc);
    SOL_CHECK(written.rfind("\n[[model]]\nid = \"beacon\"\nmesh = \"gate\"\ntexture = \"hull\"\n"
                            "radius = 12.0\n") != std::string::npos);
    // The row the game would load, and the count it would see.
    assets::DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeToml(written.c_str(), written.size(), "models.toml", &error));
    SOL_CHECK(defs.models().size() == modelsBefore + 1);
    const assets::ModelDef* beacon = defs.findModel("beacon");
    SOL_REQUIRE(beacon != nullptr);
    SOL_CHECK(beacon->mesh == "gate");
    SOL_CHECK(beacon->radius > 11.9f && beacon->radius < 12.1f);
    // A default the row never mentioned stays at its schema value and does NOT
    // appear in the file - the writer says only what the document says.
    SOL_CHECK(beacon->solid);
    SOL_CHECK(written.find("solid = true") == std::string::npos);
}

// ⚑ Appending must not adopt a note the author left UNDER the last row. In
// `models.toml` there is none; the case is built, because the one file that has
// such a note (`factions.toml`) is not one this stage writes to.
SOL_TEST(defDocumentAppendKeepsATrailingNoteAboveTheNewRow)
{
    const std::string source = "[[model]]\nid = \"a\"\nmesh = \"a\"\ntexture = \"h\"\n"
                               "\n# a note about the row above\n";
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(doc.trailer == "\n# a note about the row above\n");
    DefRow& row = doc.append("model");
    row.set("id", assets::defString("b"));
    row.set("mesh", assets::defString("b"));
    row.set("texture", assets::defString("h"));
    const std::string written = assets::writeDefs(doc);
    SOL_CHECK(written == source + "[[model]]\nid = \"b\"\nmesh = \"b\"\ntexture = \"h\"\n");
    SOL_CHECK(doc.trailer.empty());
}

SOL_TEST(defDocumentReadsAValueWrappedAcrossLines)
{
    const std::string source = readWholeFile(defPath("factions"));
    SOL_REQUIRE(!source.empty());
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    const DefRow* guild = doc.find("faction", "sol.guild");
    SOL_REQUIRE(guild != nullptr);
    const assets::DefKey* bias = guild->find("station_bias");
    SOL_REQUIRE(bias != nullptr);
    // One key, one value, two source lines - and the second line's indentation
    // is inside the value rather than lost to a reformat.
    SOL_CHECK(bias->value().find('\n') != std::string_view::npos);
    SOL_CHECK(bias->value().find("sol.station_refinery:0.7\"]") != std::string_view::npos);
    // 16 spaces, aligning under the opening bracket. The exact run matters: it
    // is inside the VALUE, so a writer that rebuilt the array would silently
    // restyle four committed lines.
    SOL_CHECK(bias->text.find("\n                \"sol.station_refinery") != std::string::npos);
    SOL_CHECK(bias->value().back() == ']');
}

SOL_TEST(defDocumentKeepsTheCommentAboveAKeyWithTheKeyBelowIt)
{
    const std::string source = "[[model]]\nid = \"a\"\n# why the radius is what it is\n"
                               "radius = 3.0\nmesh = \"a\"\ntexture = \"h\"\n";
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_REQUIRE(doc.rows.size() == 1);
    const assets::DefKey* radius = doc.rows[0].find("radius");
    SOL_REQUIRE(radius != nullptr);
    SOL_CHECK(radius->leading == "# why the radius is what it is\n");
    SOL_CHECK(assets::writeDefs(doc) == source);
}

// ⚑⚑ THE CAN-FAIL TEST THAT HAD TO BE BUILT. Trimming the raw line is
// invisible to every other assertion in this file, because NO COMMITTED DEF
// FILE HAS TRAILING WHITESPACE ON ANY LINE - measured, all nine, key lines and
// comment lines alike. A mutation no asset can see still needs a test, and the
// input for it does not exist in the repo.
//
// It is not a hypothetical hazard either: an editor that leaves a trailing
// space is exactly the difference between a save that shows a one-line diff and
// one that shows the whole file, which is the property this document exists for.
SOL_TEST(defDocumentKeepsTrailingWhitespaceAndCarriageReturnsOnALine)
{
    const std::string source = "[[model]]\nid = \"a\"   \nmesh = \"a\"\t\ntexture = \"h\"\n";
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(assets::writeDefs(doc) == source);
    SOL_REQUIRE(doc.rows.size() == 1);
    const assets::DefKey* id = doc.rows[0].find("id");
    SOL_REQUIRE(id != nullptr);
    // The VALUE stops before the spaces; the LINE keeps them.
    SOL_CHECK(id->value() == "\"a\"");
    SOL_CHECK(id->text == "id = \"a\"   ");
    doc.rows[0].set("id", assets::defString("b"));
    SOL_CHECK(doc.rows[0].find("id")->text == "id = \"b\"   ");

    // The same argument for CRLF, which a Windows editor introduces without
    // being asked. The '\r' rides in the line and out of the value.
    const std::string crlf = "[[model]]\r\nid = \"a\"\r\nradius = 2.0\r\n";
    DefDoc windows;
    SOL_REQUIRE(parses(crlf, windows));
    SOL_CHECK(assets::writeDefs(windows) == crlf);
    SOL_REQUIRE(windows.rows.size() == 1);
    const assets::DefKey* radius = windows.rows[0].find("radius");
    SOL_REQUIRE(radius != nullptr);
    SOL_CHECK(radius->value() == "2.0");
}

SOL_TEST(defDocumentSurvivesAFileWithNoTrailingNewline)
{
    const std::string source = "[[model]]\nid = \"a\"\nmesh = \"a\"\ntexture = \"h\"";
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(!doc.endsWithNewline);
    SOL_CHECK(assets::writeDefs(doc) == source);
}

SOL_TEST(defDocumentFlagsACommentItCannotPlace)
{
    // Inside a still-open array, where setting the value would write over it.
    const std::string source = "[[faction]]\nid = \"a\"\nrelations = [\"x:1\", # why\n"
                               "  \"y:2\"]\n";
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(doc.hasUnplaceableComments);
    // It still round-trips: the comment is part of the value's text.
    SOL_CHECK(assets::writeDefs(doc) == source);
}

SOL_TEST(defDocumentDoesNotSplitAValueAtAHashInsideAString)
{
    const std::string source = "[[faction]]\nid = \"a\"\nname = \"Red # Squadron\"\n";
    DefDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_REQUIRE(doc.rows.size() == 1);
    const assets::DefKey* name = doc.rows[0].find("name");
    SOL_REQUIRE(name != nullptr);
    SOL_CHECK(name->value() == "\"Red # Squadron\"");
    SOL_CHECK(name->unquoted() == "Red # Squadron");
    SOL_CHECK(!doc.hasUnplaceableComments);
}

SOL_TEST(defDocumentRefusesWhatItCannotRepresent)
{
    // A key with no table above it would otherwise be silently dropped.
    SOL_CHECK(!rejects("radius = 1.0\n").empty());
    // A plain [table] header: parsing on would reassign its keys to the row
    // above, which is worse than refusing.
    SOL_CHECK(!rejects("[[model]]\nid = \"a\"\n[build]\nweld = true\n").empty());
    SOL_CHECK(!rejects("[[model]]\nid\n").empty());
    SOL_CHECK(!rejects("[[model]]\nid = \n").empty());
    SOL_CHECK(!rejects("[[]]\nid = \"a\"\n").empty());
    // An unterminated array runs off the end of the file.
    SOL_CHECK(!rejects("[[faction]]\nrelations = [\"a:1\",\n").empty());
    // And an empty document is legal - a mod layer may define nothing.
    SOL_CHECK(rejects("").empty());
    SOL_CHECK(rejects("# nothing but a note\n").empty());
}

SOL_TEST(defNumberWritesWhatAPersonWouldType)
{
    SOL_CHECK(assets::defNumber(1.0f) == "1.0");
    SOL_CHECK(assets::defNumber(8.0f) == "8.0");
    SOL_CHECK(assets::defNumber(106.7f) == "106.7");
    SOL_CHECK(assets::defNumber(1.1584f) == "1.1584");
    SOL_CHECK(assets::defNumber(0.35f) == "0.35");
    // ⚑ The round numbers an author is most likely to type are exactly the ones
    // a naive shortest-form writer mangles: 90 has the shortest form `9e+01`.
    SOL_CHECK(assets::defNumber(90.0f) == "90.0");
    SOL_CHECK(assets::defNumber(100.0f) == "100.0");
    // ⚑ And the reason it takes a float: the asteroid's MEASURED radius is what
    // the "use measured radius" button writes, and through a double it would
    // arrive in the file as 1.1584000587463379.
    const float measured = 1.1584f;
    SOL_CHECK(assets::defNumber(measured) == "1.1584");
    SOL_CHECK(assets::defNumber(102.0f) == "102.0");
    SOL_CHECK(assets::defString("sol.beacon") == "\"sol.beacon\"");
    SOL_CHECK(assets::defBool(false) == "false");
}

// ⚑ Phase 14's rule in a second format: the file gets what the panel SHOWED.
// The asteroid's measured radius is what the "use measured" button writes, and
// unrounded it lands in models.toml as 1.1583778 among neighbours carrying one
// decimal - while the button's own label says 1.1584.
SOL_TEST(defNumberRoundsToThePrecisionThePanelShows)
{
    const float measured = 1.1583778f;
    SOL_CHECK(assets::defNumber(measured) == "1.1583778");
    SOL_CHECK(assets::defNumber(measured, 4) == "1.1584");
    SOL_CHECK(assets::defNumber(102.00194f, 4) == "102.0019");
    SOL_CHECK(assets::defNumber(0.3499f, 3) == "0.35");
    // A whole number keeps its dot, so the schema still reads a float.
    SOL_CHECK(assets::defNumber(8.00001f, 4) == "8.0");
    SOL_CHECK(assets::defNumber(100.0f, 4) == "100.0");

    // ⚑ And the rounding cannot re-open the mismatch the button exists to
    // close. Half a unit in the fourth decimal is 0.00005 m; `ModelMatch`
    // agrees within 0.1%, so on the SMALLEST radius in this game (1.0 m) the
    // worst rounding error is 0.005% - three orders of magnitude inside it.
    const float worst = 0.00005f / 1.0f;
    SOL_CHECK(worst * 100.0f < 0.001f * 100.0f);
}

// ⚑⚑ PHASE 25 STAGE D: THE INLINE TABLE, WHICH IS WHERE A MATERIAL'S TEXTURE
// SLOTS AND TUNABLE NUMBERS HAVE TO LIVE. `def_doc` refuses a nested
// `[material.params]` header rather than silently reassigning its keys to the
// row above, so `materials.toml` writes them as one-line inline tables - and
// the Forge cannot author a material without being able to change one entry of
// one of them.
SOL_TEST(defDocReadsAnInlineTablesEntries)
{
    DefDoc doc;
    SOL_CHECK(parses("[[material]]\n"
                     "id = \"sol.cockpit\"\n"
                     "textures = { glow = \"cockpit_glow\" }\n"
                     "params = { glow_strength = 2.2, rim = 0.5 }\n",
                     doc));
    const assets::DefKey* textures = doc.rows[0].find("textures");
    SOL_CHECK(textures != nullptr);
    const std::vector<assets::DefInlineEntry> slots = textures->inlineEntries();
    SOL_CHECK(slots.size() == 1);
    SOL_CHECK(slots[0].name == "glow");
    SOL_CHECK(textures->text.substr(slots[0].valueBegin, slots[0].valueEnd - slots[0].valueBegin) ==
              "\"cockpit_glow\"");

    const assets::DefKey* params = doc.rows[0].find("params");
    SOL_CHECK(params != nullptr);
    const std::vector<assets::DefInlineEntry> tuned = params->inlineEntries();
    SOL_CHECK(tuned.size() == 2);
    SOL_CHECK(tuned[0].name == "glow_strength");
    SOL_CHECK(params->text.substr(tuned[0].valueBegin, tuned[0].valueEnd - tuned[0].valueBegin) == "2.2");
    SOL_CHECK(tuned[1].name == "rim");
    SOL_CHECK(params->text.substr(tuned[1].valueBegin, tuned[1].valueEnd - tuned[1].valueBegin) == "0.5");

    // ⚑ A value that is not a table is not half-read into one. `id` would parse
    // as a single nameless entry under a looser rule, and a caller that then
    // spliced it would rewrite the id.
    SOL_CHECK(doc.rows[0].find("id")->inlineEntries().empty());
}

// ⚑⚑ THE WHOLE REASON THIS SPLICES RATHER THAN RE-EMITS. Rewriting the table
// from parsed doubles is the obvious implementation and it loses exactly what
// this file exists to keep: the author's spacing, and a `2.2` that a double
// prints back as something longer. Every byte outside the one value survives.
SOL_TEST(defDocSplicesOneInlineEntryAndLeavesTheRestByteExact)
{
    DefDoc doc;
    SOL_CHECK(parses("[[material]]\n"
                     "params = { glow_strength = 2.2, rim = 0.5 }  # tuned by eye\n",
                     doc));
    assets::DefKey* params = doc.rows[0].find("params");
    SOL_CHECK(params != nullptr);
    SOL_CHECK(params->setInlineValue("glow_strength", "4.125"));
    SOL_CHECK(params->text == "params = { glow_strength = 4.125, rim = 0.5 }  # tuned by eye");

    // ⚑ AND THE KEY'S OWN RANGE MOVED WITH IT. A splice that left `valueEnd`
    // behind would give back a table with its closing brace cut off, which
    // parses as no entries at all - so the NEXT edit would silently do nothing
    // and the failure would surface one gesture later than its cause.
    SOL_CHECK(params->value() == "{ glow_strength = 4.125, rim = 0.5 }");
    SOL_CHECK(params->setInlineValue("rim", "0.75"));
    SOL_CHECK(params->text == "params = { glow_strength = 4.125, rim = 0.75 }  # tuned by eye");
    SOL_CHECK(assets::writeDefs(doc) == "[[material]]\n"
                                        "params = { glow_strength = 4.125, rim = 0.75 }  # tuned by eye\n");

    // It never creates an entry: a key that is not there is a declaration
    // change, and this file interprets no schema.
    SOL_CHECK(!params->setInlineValue("absent", "1.0"));
    SOL_CHECK(params->text == "params = { glow_strength = 4.125, rim = 0.75 }  # tuned by eye");
}

// ⚑ A COMMA INSIDE A QUOTED VALUE. `textures = { a = "x,y" }` is legal TOML,
// and the obvious scan-to-the-next-comma cuts the entry in half - then splices
// into the middle of somebody's filename, which is the worst available failure
// because the file still parses.
SOL_TEST(defDocInlineTableScansAQuotedValueRatherThanSplittingOnItsComma)
{
    DefDoc doc;
    SOL_CHECK(parses("[[material]]\ntextures = { odd = \"a,b\", glow = \"cockpit_glow\" }\n", doc));
    assets::DefKey* textures = doc.rows[0].find("textures");
    const std::vector<assets::DefInlineEntry> slots = textures->inlineEntries();
    SOL_CHECK(slots.size() == 2);
    SOL_CHECK(slots[0].name == "odd");
    SOL_CHECK(slots[1].name == "glow");
    SOL_CHECK(textures->setInlineValue("glow", "\"checker\""));
    SOL_CHECK(textures->text == "textures = { odd = \"a,b\", glow = \"checker\" }");
}
