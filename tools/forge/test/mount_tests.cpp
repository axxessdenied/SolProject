// The mount tool's two halves that a headless suite can reach (engine plan
// Phase 31 stage D): where a `[[ship.mount]]` row goes, and the camera a
// viewport drag is computed against.
//
// ⚑ AGENTS.md 7 says the tool's UI is verified by running it, and that still
// holds - there is no suite here for a window. What IS here is everything the
// stage deliberately kept OUT of `mount_tool.cpp` so that it could be: the
// document rules in `mount_rows.hpp` and the camera in `viewport_pick.hpp`.
// Both are pure functions, and both are the halves that would fail silently -
// a mount appended to the wrong hull still loads, and a drag against a wrong
// basis still moves something.

#include "mount_rows.hpp"
#include "viewport_pick.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/assets/def_doc.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace sol;
using assets::DefDoc;
using assets::DefRow;

namespace {

[[nodiscard]] std::string readShips()
{
    const std::string path = std::string(SOL_MODEL_DATA_DIR) + "/ships.toml";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  cannot open %s\n", path.c_str());
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

[[nodiscard]] bool parseShips(DefDoc& out)
{
    const std::string source = readShips();
    if (source.empty()) {
        return false;
    }
    std::string error;
    if (assets::parseDefs(source.c_str(), source.size(), "ships.toml", out, &error)) {
        return true;
    }
    std::printf("  unexpected parse failure: %s\n", error.c_str());
    return false;
}

// The game's own schema, which is what the tool validates a candidate through.
[[nodiscard]] bool schemaAccepts(const DefDoc& doc, std::string& error)
{
    const std::string text = assets::writeDefs(doc);
    assets::DefDatabase defs;
    return defs.mergeToml(text.c_str(), text.size(), "candidate.toml", &error);
}

[[nodiscard]] bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::fabs(a - b) <= tolerance;
}

} // namespace

// ---------------------------------------------------------------------------
// Where a mount row lives
// ---------------------------------------------------------------------------

SOL_TEST(mountRowsBelongToTheHullAboveThemAndStopAtTheNextOne)
{
    DefDoc doc;
    SOL_REQUIRE(parseShips(doc));

    const std::size_t shuttle = doc.indexOf("ship", "sol.shuttle");
    const std::size_t interceptor = doc.indexOf("ship", "sol.interceptor");
    const std::size_t freighter = doc.indexOf("ship", "sol.freighter");
    SOL_REQUIRE(shuttle != DefDoc::kNoRow && interceptor != DefDoc::kNoRow && freighter != DefDoc::kNoRow);

    // ⚑ THE WHOLE POINT OF THE FUNCTION IS THAT THESE THREE ARE DIFFERENT
    // NUMBERS. `doc.count("ship.mount")` is nineteen for every hull in the file,
    // so a walk that filtered by TYPE would hand all three the same list and
    // every claim about "this hull's mounts" would be true by accident.
    SOL_CHECK(forge::mountRowsOf(doc, shuttle).size() == 5);
    SOL_CHECK(forge::mountRowsOf(doc, interceptor).size() == 5);
    SOL_CHECK(forge::mountRowsOf(doc, freighter).size() == 9);
    SOL_CHECK(doc.count("ship.mount") == 19);

    // And the run really is contiguous and in file order.
    const std::vector<std::size_t> rows = forge::mountRowsOf(doc, shuttle);
    SOL_REQUIRE(rows.size() == 5);
    SOL_CHECK(rows.front() == shuttle + 1);
    SOL_CHECK(doc.rows[rows.front()].id() == "gun_nose");
    for (std::size_t i = 1; i < rows.size(); ++i) {
        SOL_CHECK(rows[i] == rows[i - 1] + 1);
    }
}

SOL_TEST(aMountIdIsUniqueOnItsHullAndNotInTheDocument)
{
    DefDoc doc;
    SOL_REQUIRE(parseShips(doc));
    const std::size_t shuttle = doc.indexOf("ship", "sol.shuttle");
    const std::size_t freighter = doc.indexOf("ship", "sol.freighter");
    SOL_REQUIRE(shuttle != DefDoc::kNoRow && freighter != DefDoc::kNoRow);

    // ⚑ `gun_nose` IS A REAL MOUNT ON TWO SHIPPED HULLS, which is what makes
    // this measurable rather than hypothetical: a document-wide uniqueness rule
    // - `def_editor.cpp`'s `uniqueId`, correct for a ship id - would refuse the
    // second one, and `DefDoc::find("ship.mount", "gun_nose")` answers with
    // whichever hull happens to come first in the file.
    SOL_CHECK(forge::findMountRow(doc, shuttle, "gun_nose") != DefDoc::kNoRow);
    SOL_CHECK(forge::findMountRow(doc, freighter, "gun_nose") == DefDoc::kNoRow);
    SOL_CHECK(forge::uniqueMountId(doc, shuttle, "gun_nose") == "gun_nose_2");
    SOL_CHECK(forge::uniqueMountId(doc, freighter, "gun_nose") == "gun_nose");

    // `drive_main` is on the shuttle and the freighter but not the interceptor,
    // which spends its budget on two.
    const std::size_t interceptor = doc.indexOf("ship", "sol.interceptor");
    SOL_CHECK(forge::uniqueMountId(doc, interceptor, "drive_main") == "drive_main");
    SOL_CHECK(forge::uniqueMountId(doc, freighter, "drive_main") == "drive_main_2");
}

SOL_TEST(aPlacedMountIsNamedTheWayTheShippedOnesAre)
{
    DefDoc doc;
    SOL_REQUIRE(parseShips(doc));
    const std::size_t shuttle = doc.indexOf("ship", "sol.shuttle");
    const std::size_t freighter = doc.indexOf("ship", "sol.freighter");
    SOL_REQUIRE(shuttle != DefDoc::kNoRow && freighter != DefDoc::kNoRow);

    // ⚑ NOT THE KIND'S OWN SPELLING, which is what the first drive produced: a
    // mount with `id = "fixed"` and `kind = "fixed"`. The id is what a save
    // names a fitting by, so it outlives the session that created it.
    SOL_CHECK(std::string(forge::mountIdStem(assets::MountKind::Fixed)) == "gun");
    SOL_CHECK(std::string(forge::mountIdStem(assets::MountKind::Turret)) == "turret");
    SOL_CHECK(std::string(forge::mountIdStem(assets::MountKind::Engine)) == "drive");
    SOL_CHECK(std::string(forge::mountIdStem(assets::MountKind::Subsystem)) == "core");

    // Every kind answers with something, including the sentinel - a switch that
    // fell through would hand a whole class of mount an empty id, and an empty
    // `id` is a schema refusal rather than a bad name.
    for (std::size_t i = 0; i <= assets::kMountKindCount; ++i) {
        const char* stem = forge::mountIdStem(static_cast<assets::MountKind>(i));
        SOL_REQUIRE(stem != nullptr);
        SOL_CHECK(*stem != 0);
    }

    // And it is unique per hull, so a second gun on the shuttle is `gun_2`
    // while the first gun on the freighter is still `gun`.
    const std::string first =
        forge::uniqueMountId(doc, shuttle, forge::mountIdStem(assets::MountKind::Fixed));
    SOL_CHECK(first == "gun");
    DefRow& row = doc.insertAfter(forge::mountInsertPoint(doc, shuttle), forge::kMountRowType, "  ");
    forge::MountDraft draft;
    draft.id = first;
    draft.kind = assets::MountKind::Fixed;
    draft.size = assets::MountSize::Small;
    forge::writeMountDraft(row, draft, 4);
    SOL_CHECK(forge::uniqueMountId(doc, shuttle, "gun") == "gun_2");
    SOL_CHECK(forge::uniqueMountId(doc, freighter, "gun") == "gun");
}

SOL_TEST(aNewMountGoesAfterTheHullsLastOneAndIsIndentedLikeIt)
{
    DefDoc doc;
    SOL_REQUIRE(parseShips(doc));
    const std::size_t shuttle = doc.indexOf("ship", "sol.shuttle");
    SOL_REQUIRE(shuttle != DefDoc::kNoRow);

    const std::vector<std::size_t> before = forge::mountRowsOf(doc, shuttle);
    SOL_REQUIRE(before.size() == 5);
    SOL_CHECK(forge::mountInsertPoint(doc, shuttle) == before.back());
    SOL_CHECK(forge::mountIndent(doc, shuttle) == "  ");

    forge::MountDraft draft;
    draft.id = forge::uniqueMountId(doc, shuttle, "turret");
    draft.kind = assets::MountKind::Turret;
    draft.size = assets::MountSize::Small;
    draft.external = true;
    draft.at[0] = 0.0f;
    draft.at[1] = 1.25f;
    draft.at[2] = -1.5f;

    DefRow& row = doc.insertAfter(
        forge::mountInsertPoint(doc, shuttle), forge::kMountRowType, forge::mountIndent(doc, shuttle));
    forge::writeMountDraft(row, draft, 4);

    std::string error;
    SOL_CHECK(schemaAccepts(doc, error));
    if (!error.empty()) {
        std::printf("  schema refused: %s\n", error.c_str());
    }

    // It landed on the shuttle, in mount order, at the end.
    const std::vector<std::size_t> after = forge::mountRowsOf(doc, shuttle);
    SOL_REQUIRE(after.size() == 6);
    SOL_CHECK(doc.rows[after.back()].id() == "turret");
    SOL_CHECK(doc.rows[after.back()].header == "  [[ship.mount]]");

    // And the values are the file's own spelling rather than a float's.
    const assets::DefKey* at = doc.rows[after.back()].find("at");
    SOL_REQUIRE(at != nullptr);
    SOL_CHECK(at->value() == "[0.0, 1.25, -1.5]");
    SOL_CHECK(at->text == "  at = [0.0, 1.25, -1.5]");
}

SOL_TEST(aHullWithNoMountsTakesItsFirstOneRightUnderItself)
{
    const std::string source = "[[ship]]\nid = \"sol.bare\"\nname = \"Bare\"\n";
    DefDoc doc;
    SOL_REQUIRE(assets::parseDefs(source.c_str(), source.size(), "ships.toml", doc, nullptr));
    const std::size_t bare = doc.indexOf("ship", "sol.bare");
    SOL_REQUIRE(bare != DefDoc::kNoRow);

    // ⚑ The insert point is the HULL row, not "the last mount minus one" - a
    // hull with no mounts has no mount to go after, and getting this wrong puts
    // the first hardpoint of a new ship above the row that owns it.
    SOL_CHECK(forge::mountRowsOf(doc, bare).empty());
    SOL_CHECK(forge::mountInsertPoint(doc, bare) == bare);
    SOL_CHECK(forge::mountIndent(doc, bare) == "  ");

    forge::MountDraft draft;
    draft.id = "core";
    draft.kind = assets::MountKind::Subsystem;
    draft.size = assets::MountSize::Medium;
    draft.external = false;

    DefRow& row = doc.insertAfter(
        forge::mountInsertPoint(doc, bare), forge::kMountRowType, forge::mountIndent(doc, bare));
    forge::writeMountDraft(row, draft, 4);

    // ⚑ NO `at`, WHICH IS WHAT MAKES IT INTERNAL (decisions/014 rule 2). A
    // draft that wrote one unconditionally could not express this mount at all,
    // and the freighter's `core_sensor` is exactly this shape.
    SOL_CHECK(row.find("at") == nullptr);

    std::string error;
    SOL_CHECK(schemaAccepts(doc, error));
    if (!error.empty()) {
        std::printf("  schema refused: %s\n", error.c_str());
    }
    SOL_CHECK(assets::writeDefs(doc) == "[[ship]]\n"
                                        "id = \"sol.bare\"\n"
                                        "name = \"Bare\"\n"
                                        "\n"
                                        "  [[ship.mount]]\n"
                                        "  id = \"core\"\n"
                                        "  kind = \"subsystem\"\n"
                                        "  size = \"medium\"\n");
}

SOL_TEST(aPlacedMountCarriesTheFacingOfTheSurfaceAndNotTheSchemasDefault)
{
    DefDoc doc;
    SOL_REQUIRE(parseShips(doc));
    const std::size_t freighter = doc.indexOf("ship", "sol.freighter");
    SOL_REQUIRE(freighter != DefDoc::kNoRow);

    // A turret dropped on the dorsal hull faces up, because that is where the
    // surface it was clicked on faces. The numbers are the ones the live drive
    // produced against the shipped `ship` mesh.
    forge::MountDraft dorsal;
    dorsal.id = forge::uniqueMountId(doc, freighter, forge::mountIdStem(assets::MountKind::Turret));
    dorsal.kind = assets::MountKind::Turret;
    dorsal.size = assets::MountSize::Small;
    dorsal.external = true;
    // ⚑ VALUES THAT SPELL DIFFERENTLY AT THREE DECIMALS AND AT FOUR, which the
    // first draft of this test did not have: `defNumber` trims to the shortest
    // form that round-trips, so 0.997 comes out "0.997" at either precision and
    // the assertion below agreed with both. A real mesh normal is not a round
    // number; these are what the shipped `ship` mesh actually produced.
    dorsal.at[0] = 0.45412f;
    dorsal.at[1] = 1.63481f;
    dorsal.at[2] = 0.61754f;
    dorsal.hasAim = true;
    dorsal.aim[0] = 0.0f;
    dorsal.aim[1] = 0.997563f;
    dorsal.aim[2] = -0.083149f;

    DefRow& row = doc.insertAfter(forge::mountInsertPoint(doc, freighter), forge::kMountRowType, "  ");
    forge::writeMountDraft(row, dorsal, 4);

    const assets::DefKey* at = row.find("at");
    const assets::DefKey* aim = row.find("aim");
    SOL_REQUIRE(at != nullptr && aim != nullptr);
    // ⚑ FOUR DECIMALS FOR A POSITION AND THREE FOR A FACING, ON THE SAME ROW,
    // which is the whole reason the two constants are separate: `at` is metres
    // on Phase 14's 0.1 mm grid, and `aim` is a unit direction whose components
    // are fractions of one. Both spellings below change if either precision
    // does, which is what makes this an assertion rather than a restatement.
    SOL_CHECK(at->value() == "[0.4541, 1.6348, 0.6175]");
    SOL_CHECK(aim->value() == "[0.0, 0.998, -0.083]");

    std::string error;
    SOL_CHECK(schemaAccepts(doc, error));
    if (!error.empty()) {
        std::printf("  schema refused: %s\n", error.c_str());
    }

    assets::DefDatabase defs;
    const std::string written = assets::writeDefs(doc);
    SOL_REQUIRE(defs.mergeToml(written.c_str(), written.size(), "candidate.toml", nullptr));
    const assets::ShipDef* hull = defs.findShip("sol.freighter");
    SOL_REQUIRE(hull != nullptr);
    const assets::ShipMount* placed = hull->findMount(dorsal.id);
    SOL_REQUIRE(placed != nullptr);
    SOL_CHECK(near(placed->aim[1], 0.998f, 1e-4f));
    SOL_CHECK(placed->arc == 0.0f); // a placement bolts it down; the arc is authored after
}

SOL_TEST(aMountFacingTheDefaultWritesNoAimAndAnInternalOneCannotCarryOne)
{
    // A gun placed on a nose that faces -Z is a gun facing the schema's own
    // default, and a key at its default is a key an author would not have
    // typed. `def_editor.cpp` follows the same rule for every other key.
    DefDoc doc;
    const std::string bareHull = "[[ship]]\nid = \"sol.bare\"\nname = \"Bare\"\n";
    SOL_REQUIRE(assets::parseDefs(bareHull.c_str(), bareHull.size(), "ships.toml", doc, nullptr));
    const std::size_t bare = doc.indexOf("ship", "sol.bare");
    SOL_REQUIRE(bare != DefDoc::kNoRow);

    forge::MountDraft nose;
    nose.id = "gun";
    nose.kind = assets::MountKind::Fixed;
    nose.size = assets::MountSize::Small;
    nose.external = true;
    nose.at[2] = -6.6f;
    nose.hasAim = false;
    DefRow& first = doc.insertAfter(forge::mountInsertPoint(doc, bare), forge::kMountRowType, "  ");
    forge::writeMountDraft(first, nose, 4);
    SOL_CHECK(first.find("at") != nullptr);
    SOL_CHECK(first.find("aim") == nullptr);

    // ⚑⚑ AND AN INTERNAL MOUNT WRITES NEITHER, EVEN WHEN THE DRAFT CARRIES AN
    // AIM. decisions/014 rule 2 read backwards: `parseShip` REFUSES `aim`
    // without an `at`, by name, because a facing on something that is never
    // drawn and never aimed at is a key an author would read as having been
    // eaten. A draft that wrote the aim anyway would be a document the game
    // will not load, produced by the button for adding an internal mount.
    forge::MountDraft core;
    core.id = "core";
    core.kind = assets::MountKind::Subsystem;
    core.size = assets::MountSize::Small;
    core.external = false;
    core.hasAim = true;
    core.aim[1] = 1.0f;
    DefRow& second = doc.insertAfter(forge::mountInsertPoint(doc, bare), forge::kMountRowType, "  ");
    forge::writeMountDraft(second, core, 4);
    SOL_CHECK(second.find("at") == nullptr);
    SOL_CHECK(second.find("aim") == nullptr);

    std::string error;
    SOL_CHECK(schemaAccepts(doc, error));
    if (!error.empty()) {
        std::printf("  schema refused: %s\n", error.c_str());
    }
}

// ---------------------------------------------------------------------------
// The camera a drag is computed against
// ---------------------------------------------------------------------------

SOL_TEST(theCameraBasisComesBackOutOfTheViewMatrixItWentIn)
{
    // ⚑⚑ AT AN ORIENTATION THAT IS NOT THE IDENTITY, WHICH IS THE ONLY WAY
    // THIS CAN FAIL. Phase 31 stage C2 found the same trap one layer down: a
    // basis read out of the wrong rows, or an eye recovered without
    // transposing, is exactly right when the camera is on an axis looking down
    // -Z, and wrong the moment anybody orbits.
    const core::Vec3 eye{3.0f, 4.0f, -5.0f};
    const core::Vec3 target{-1.0f, 0.5f, 2.0f};
    const core::Mat4 view = core::lookAt(eye, target, {0.0f, 1.0f, 0.0f});

    const core::Vec3 recovered = forge::cameraEye(view);
    SOL_CHECK(near(recovered.x, eye.x, 1e-3f));
    SOL_CHECK(near(recovered.y, eye.y, 1e-3f));
    SOL_CHECK(near(recovered.z, eye.z, 1e-3f));

    // The backward axis points at the viewer, which is the sign that is easy to
    // get inverted and impossible to see in a still picture.
    const core::Vec3 backward = forge::cameraBackward(view);
    const core::Vec3 toEye = core::normalize(eye - target);
    SOL_CHECK(near(backward.x, toEye.x, 1e-3f));
    SOL_CHECK(near(backward.y, toEye.y, 1e-3f));
    SOL_CHECK(near(backward.z, toEye.z, 1e-3f));

    // And the three are orthonormal, so a drag in the view plane is a drag in
    // the view plane rather than a shear.
    const core::Vec3 right = forge::cameraRight(view);
    const core::Vec3 up = forge::cameraUp(view);
    SOL_CHECK(near(core::length(right), 1.0f, 1e-4f));
    SOL_CHECK(near(core::length(up), 1.0f, 1e-4f));
    SOL_CHECK(near(core::dot(right, up), 0.0f, 1e-4f));
    SOL_CHECK(near(core::dot(right, backward), 0.0f, 1e-4f));
}

SOL_TEST(aDragMovesTheHandsDistanceAndAnAxisLockKeepsOnlyThatComponent)
{
    // Looking down -Z from +Z, so world X is screen right and world Y screen up
    // and the arithmetic below is checkable by hand.
    const core::Mat4 view = core::lookAt({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    constexpr float kFov = 1.2f;
    constexpr float kHeight = 720.0f;
    constexpr float kDepth = 10.0f;
    const float perPixel = forge::metresPerPixel(kDepth, kFov, kHeight);
    SOL_CHECK(perPixel > 0.0f);

    const core::Vec3 free = forge::dragDelta(view, {100.0f, -50.0f}, kDepth, kFov, kHeight, -1);
    SOL_CHECK(near(free.x, 100.0f * perPixel, 1e-3f));
    // Screen Y grows DOWNWARD, so a cursor moving up moves the point up.
    SOL_CHECK(near(free.y, 50.0f * perPixel, 1e-3f));
    SOL_CHECK(near(free.z, 0.0f, 1e-3f));

    // Locked to world X: the y component goes and the x component STAYS what it
    // was - the lock drops the rest of the move rather than rescaling what is
    // left, so a locked drag tracks the hand exactly along its own axis.
    const core::Vec3 lockedX = forge::dragDelta(view, {100.0f, -50.0f}, kDepth, kFov, kHeight, 0);
    SOL_CHECK(near(lockedX.x, free.x, 1e-3f));
    SOL_CHECK(near(lockedX.y, 0.0f, 1e-3f));
    SOL_CHECK(near(lockedX.z, 0.0f, 1e-3f));

    // ⚑ AN AXIS THE CAMERA IS LOOKING ALONG YIELDS NOTHING, and that is correct
    // rather than broken: there is no cursor movement in the view plane that
    // means "toward the eye". A tool that tried to invent one would move the
    // point by an amount the hand never expressed.
    const core::Vec3 lockedZ = forge::dragDelta(view, {100.0f, -50.0f}, kDepth, kFov, kHeight, 2);
    SOL_CHECK(near(lockedZ.x, 0.0f, 1e-3f));
    SOL_CHECK(near(lockedZ.y, 0.0f, 1e-3f));
    SOL_CHECK(near(lockedZ.z, 0.0f, 1e-3f));
}

SOL_TEST(aPointToSegmentDistanceIsMeasuredToTheSegmentAndNotToItsLine)
{
    // Beyond an end, the nearest point is the END - which is what stops an edge
    // pick claiming a cursor a hundred pixels off the end of a short edge.
    SOL_CHECK(near(forge::distanceToSegment({0.0f, 0.0f}, {10.0f, 0.0f}, {5.0f, 3.0f}), 3.0f));
    SOL_CHECK(near(forge::distanceToSegment({0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}), 10.0f));
    SOL_CHECK(near(forge::distanceToSegment({0.0f, 0.0f}, {10.0f, 0.0f}, {-6.0f, 8.0f}), 10.0f));
    // A segment whose ends project to one pixel is a point, not a divide by zero.
    SOL_CHECK(near(forge::distanceToSegment({4.0f, 4.0f}, {4.0f, 4.0f}, {4.0f, 7.0f}), 3.0f));
}
