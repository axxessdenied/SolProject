// The cooked-asset search path and mod layer discovery (engine plan Phase 24
// stage S).
//
// ⚑⚑ WHY THIS SUITE EXISTS AT ALL, AND IT IS A BUG REPORT AS MUCH AS A TEST.
// Layer discovery lived inside `GameContent::initialize` from Phase 5 until
// stage S - a function that needs a World, a VM and a filesystem, so no suite
// could reach it. It contained a branch that, when its prefix match failed,
// carried on and took the first path segment of whatever it had: an absolute
// Windows path yielded a mod layer literally named `C:`, which is what the
// first shipping build produced. The separator mismatch behind it was fixed in
// `platform` (Phase 22); the FALLTHROUGH was not, because nothing could see it.
// It is a free function taking a listing now, and the pathological inputs below
// are the ones that were previously unreachable.
//
// ⚑ The ORDER tests are the other half and guard a different kind of silence:
// asset precedence backwards is invisible until two mods ship the same stem,
// which is a bug that reaches a player rather than a build.

#include "asset_paths.hpp"

#include <string>
#include <vector>

#include <sol/test/test.hpp>

using game::assetCandidates;
using game::cookedSearchPath;
using game::describeSearchPath;
using game::layeredSearchPath;
using game::modLayerNames;
using game::shaderSearchPath;

namespace {

[[nodiscard]] bool contains(const std::vector<std::string>& haystack, const std::string& needle)
{
    for (const std::string& entry : haystack) {
        if (entry == needle) {
            return true;
        }
    }
    return false;
}

} // namespace

SOL_TEST(a_mod_is_a_first_level_subdirectory_and_a_loose_file_is_not)
{
    const std::vector<std::string> listing = {
        "/game/mods/README.md",           // documented non-layer: no '/' after the prefix
        "/game/mods/alpha/ships.toml",    //
        "/game/mods/alpha/scripts/x.lua", // deeper files still name their own layer
        "/game/mods/zebra/weapons.toml",
    };
    const std::vector<std::string> names = modLayerNames("/game/mods", listing);

    SOL_REQUIRE(names.size() == 2);
    SOL_CHECK(names[0] == "alpha");
    SOL_CHECK(names[1] == "zebra");
}

SOL_TEST(many_files_in_one_mod_yield_one_layer)
{
    const std::vector<std::string> listing = {
        "/game/mods/one/a.toml",
        "/game/mods/one/b.toml",
        "/game/mods/one/scripts/init.lua",
        "/game/mods/one/cooked/hull.stex",
    };
    const std::vector<std::string> names = modLayerNames("/game/mods", listing);

    SOL_REQUIRE(names.size() == 1);
    SOL_CHECK(names[0] == "one");
}

// ⚑⚑⚑ THE `C:` REGRESSION, WHICH IS THE WHOLE REASON THIS FUNCTION MOVED.
// A listing entry that does not sit under the mods directory has no layer name
// to recover. The old code took `substr(0, find('/'))` of the unmatched path
// and produced the drive letter - a mod layer named `C:` that the game then
// tried to merge defs from. It must be refused, and it must be REPORTED, or
// the player installs four mods and silently gets three.
SOL_TEST(a_path_outside_the_mods_directory_is_refused_rather_than_guessed_at)
{
    const std::vector<std::string> listing = {
        "C:/Users/somebody/elsewhere/ships.toml", // the shape that produced "C:"
        "/game/mods/real/ships.toml",
    };
    std::vector<std::string> unrelated;
    const std::vector<std::string> names = modLayerNames("/game/mods", listing, &unrelated);

    SOL_REQUIRE(names.size() == 1);
    SOL_CHECK(names[0] == "real");
    SOL_CHECK(!contains(names, "C:"));
    SOL_REQUIRE(unrelated.size() == 1);
    SOL_CHECK(unrelated[0] == "C:/Users/somebody/elsewhere/ships.toml");
}

// The contract says '/' on every platform, and it holds today - but the one
// time it slipped it cost the bug above, so a backslashed directory must not
// be able to turn every entry into an unrelated path.
SOL_TEST(a_backslashed_directory_still_matches_a_forward_slashed_listing)
{
    const std::vector<std::string> listing = {"C:/dev/game/mods/alpha/ships.toml"};
    std::vector<std::string> unrelated;
    const std::vector<std::string> names = modLayerNames("C:\\dev\\game\\mods", listing, &unrelated);

    SOL_REQUIRE(names.size() == 1);
    SOL_CHECK(names[0] == "alpha");
    SOL_CHECK(unrelated.empty());
}

SOL_TEST(a_trailing_separator_on_the_mods_directory_changes_nothing)
{
    const std::vector<std::string> listing = {"/game/mods/alpha/ships.toml"};
    const std::vector<std::string> withSlash = modLayerNames("/game/mods/", listing);
    const std::vector<std::string> without = modLayerNames("/game/mods", listing);

    SOL_REQUIRE(withSlash.size() == 1);
    SOL_CHECK(withSlash == without);
}

SOL_TEST(no_mods_installed_is_not_an_error)
{
    const std::vector<std::string> listing = {"/game/mods/README.md"};
    SOL_CHECK(modLayerNames("/game/mods", listing).empty());
}

// ⚑⚑⚑ THE PRECEDENCE TEST. Defs merge base-FIRST so a later layer overwrites
// in place; assets are found rather than merged, so the same precedence means
// searching the last layer FIRST and stopping. Reversing this compiles, runs,
// and is wrong only when two layers ship the same stem.
SOL_TEST(the_search_path_is_the_reverse_of_the_layer_order_with_the_base_game_last)
{
    const std::vector<std::string> layers = {"/game/mods/alpha", "/game/mods/zebra"};
    const std::vector<std::string> path = cookedSearchPath("/install/cooked/", layers);

    SOL_REQUIRE(path.size() == 3);
    SOL_CHECK(path[0] == "/game/mods/zebra/cooked/"); // last name wins
    SOL_CHECK(path[1] == "/game/mods/alpha/cooked/");
    SOL_CHECK(path[2] == "/install/cooked/"); // base game is the fallback
}

SOL_TEST(with_no_mods_the_search_path_is_just_the_base_game)
{
    const std::vector<std::string> path = cookedSearchPath("/install/cooked/", {});

    SOL_REQUIRE(path.size() == 1);
    SOL_CHECK(path[0] == "/install/cooked/");
}

// ⚑ `file_io.hpp` is explicit that dropping a trailing separator "silently
// moves the save file, the settings and the cooked directory one level up",
// because every caller concatenates straight onto it. The inputs here
// genuinely differ - `executableDirectory()` supplies a trailing '/' and a
// layer path does not - so the normalisation is load-bearing, not tidiness.
SOL_TEST(every_search_path_entry_ends_in_exactly_one_separator)
{
    const std::vector<std::string> layers = {"/game/mods/alpha/", "/game/mods/zebra"};
    for (const std::string& entry : cookedSearchPath("/install/cooked", layers)) {
        SOL_REQUIRE(!entry.empty());
        SOL_CHECK(entry.back() == '/');
        SOL_CHECK(entry.size() < 2 || entry[entry.size() - 2] != '/');
    }
}

SOL_TEST(candidates_are_the_search_path_with_the_name_appended_in_order)
{
    const std::vector<std::string> layers = {"/game/mods/alpha"};
    const std::vector<std::string> path = cookedSearchPath("/install/cooked/", layers);
    const std::vector<std::string> candidates = assetCandidates(path, "hull.stex");

    SOL_REQUIRE(candidates.size() == 2);
    SOL_CHECK(candidates[0] == "/game/mods/alpha/cooked/hull.stex");
    SOL_CHECK(candidates[1] == "/install/cooked/hull.stex");
}

// A LOD sibling is looked up by the same call as its level 0, so the two
// cannot disagree about which layer they came from. Asserting the SHAPE here
// is what stops a future "just probe the base directory" shortcut: that
// version passes every other test in this file.
SOL_TEST(a_lod_sibling_searches_every_layer_exactly_as_level_zero_does)
{
    const std::vector<std::string> layers = {"/game/mods/alpha"};
    const std::vector<std::string> path = cookedSearchPath("/install/cooked/", layers);

    const std::vector<std::string> level0 = assetCandidates(path, "ship.smesh");
    const std::vector<std::string> level1 = assetCandidates(path, "ship.lod1.smesh");

    SOL_REQUIRE(level0.size() == level1.size());
    SOL_CHECK(level1[0] == "/game/mods/alpha/cooked/ship.lod1.smesh");
    SOL_CHECK(level1[1] == "/install/cooked/ship.lod1.smesh");
}

SOL_TEST(the_search_path_describes_itself_for_an_error_message)
{
    const std::vector<std::string> layers = {"/game/mods/alpha"};
    const std::string described = describeSearchPath(cookedSearchPath("/install/cooked/", layers));

    SOL_CHECK(described == "/game/mods/alpha/cooked/, /install/cooked/");
}

// ---------------------------------------------------------------------------
// The shader search path (engine plan Phase 25 stage E).
//
// ⚑ A mod's SPIR-V is found the same way and in the same order as its cooked
// assets, one directory over. These tests are deliberately the cooked ones
// restated, because the two paths now share `layeredSearchPath` and the thing
// worth guarding is that sharing it did not quietly change either.

SOL_TEST(a_mods_shaders_are_searched_before_the_installs)
{
    const std::vector<std::string> layers = {"/game/mods/alpha", "/game/mods/zebra"};
    const std::vector<std::string> path = shaderSearchPath("/install/shaders/", layers);

    SOL_REQUIRE(path.size() == 3);
    SOL_CHECK(path[0] == "/game/mods/zebra/shaders/"); // last name wins, as with cooked/
    SOL_CHECK(path[1] == "/game/mods/alpha/shaders/");
    SOL_CHECK(path[2] == "/install/shaders/");
}

SOL_TEST(with_no_mods_the_shader_search_path_is_just_the_install)
{
    const std::vector<std::string> path = shaderSearchPath("/install/shaders", {});

    SOL_REQUIRE(path.size() == 1);
    SOL_CHECK(path[0] == "/install/shaders/");
}

// ⚑⚑ THE INVARIANT ANOTHER FILE DEPENDS ON, ASSERTED HERE BY NAME BECAUSE THAT
// IS WHAT MAKES IT SAFE TO DEPEND ON. `SceneRenderer::initialize` takes this
// list's LAST entry as the install's own shader directory and hands it to the
// six renderers that are not materials, so that a mod cannot replace
// `tonemap.frag.spv` underneath a pipeline with no declaration to check it
// against. If the base ever stops being last, those six start resolving into a
// mod layer - which compiles, runs, and is wrong only on a machine that has a
// mod installed.
SOL_TEST(the_base_directory_is_last_which_is_what_scene_renderer_reads_as_the_install)
{
    const std::vector<std::string> layers = {"/game/mods/alpha", "/game/mods/zebra"};

    SOL_CHECK(shaderSearchPath("/install/shaders/", layers).back() == "/install/shaders/");
    SOL_CHECK(cookedSearchPath("/install/cooked/", layers).back() == "/install/cooked/");
    SOL_CHECK(shaderSearchPath("/install/shaders/", {}).back() == "/install/shaders/");
}

// ⚑ The separator rule is the shader path's too, and its inputs differ the same
// way: `executableDirectory()` supplies a trailing '/' and a layer path does
// not. A missed one here concatenates into `/install/sharedersmesh.vert.spv`.
SOL_TEST(every_shader_search_entry_ends_in_exactly_one_separator)
{
    const std::vector<std::string> layers = {"/game/mods/alpha/", "/game/mods/zebra"};
    for (const std::string& entry : shaderSearchPath("/install/shaders", layers)) {
        SOL_REQUIRE(!entry.empty());
        SOL_CHECK(entry.back() == '/');
        SOL_CHECK(entry.size() < 2 || entry[entry.size() - 2] != '/');
    }
}

// ⚑⚑ THE TWO PATHS DIFFER IN EXACTLY ONE THING, AND SAYING SO IS THE POINT OF
// FACTORING THEM. A second copy of the reverse-order loop would compile and
// pass every test above while disagreeing about precedence - the same drift
// `outputs.cpp` refuses when it names a mesh level through `meshLevelPath`
// rather than rebuilding the pattern, on the grounds that the copy that drifts
// is the one that decides what gets deleted. Here it would decide which mod's
// shader a player runs.
SOL_TEST(the_cooked_and_shader_paths_are_the_same_rule_with_a_different_subdirectory)
{
    const std::vector<std::string> layers = {"/game/mods/alpha", "/game/mods/zebra"};
    const std::vector<std::string> cooked = cookedSearchPath("/install/cooked", layers);
    const std::vector<std::string> shaders = shaderSearchPath("/install/shaders", layers);

    SOL_REQUIRE(cooked.size() == shaders.size());
    SOL_CHECK(cooked == layeredSearchPath("/install/cooked", layers, "cooked"));
    SOL_CHECK(shaders == layeredSearchPath("/install/shaders", layers, "shaders"));
    // Layer for layer, the only difference is the directory name.
    for (std::size_t i = 0; i + 1 < cooked.size(); ++i) {
        SOL_CHECK(cooked[i] == shaders[i].substr(0, shaders[i].size() - 8) + "cooked/");
    }
}
