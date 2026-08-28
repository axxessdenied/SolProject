#pragma once

// Where the Forge reads and writes, now that somebody who has never built this
// repo can install it (engine plan Phase 24 stage V).
//
// ⚑⚑⚑ THE DEFECT THIS CLOSES, AND IT IS A SHIPPING ONE RATHER THAN A TIDY-UP.
// `tools/forge/CMakeLists.txt` baked three absolute source-tree paths into
// EVERY configuration, release included, and none of them was gated the way
// Phase 22 gated the game's. Measured in `build/release/bin/forge.exe` before
// this file existed: three strings, one of them carrying the developer's user
// name. `sol.exe` and `cooker.exe` in the same directory carried zero. So the
// shipping tool read directories that exist on exactly one machine in the
// world - and it worked perfectly on that machine, which is packaging's
// characteristic failure.
//
// ⚑⚑ A PROJECT IS A MOD DIRECTORY, WHICH IS THE SHAPE THE GAME ALREADY LOADS.
// `game/mods/README.md` has defined one since Phase 22: def `*.toml` at the top
// level, `assets/` beside `cooked/` beside `shaders/`. Decision 012 assumed it
// out loud - "even a retexture mod cooks into its own `cooked/`" - so pointing
// the tool at one means the Cook button writes where the game looks, with no
// copying step in between. The alternative shapes were measured and rejected:
// the repo is `assets/` at the root with `game/data` and `game/mods` under it,
// an install is `data/ cooked/ shaders/ mods/` with no `assets/` at all, and
// the spec's own "a directory containing assets/ and data/" is a fourth shape
// that nothing in this project produces.
//
// ⚑⚑ FIVE DIRECTORIES, NOT THE THREE THE SPEC COUNTED, and the two with no
// define were the ones that needed this most. `cooked/` and `shaders/` were
// spelled `executableDirectory() + "..."` straight into `main.cpp`, so an
// installed tool would have cooked an author's mod INTO THE TOOL'S OWN INSTALL
// FOLDER, where the game never looks. That is the difference between authoring
// a mod and authoring a mod the game can load.
//
// Pure, and here rather than in `main.cpp`, for the reason `asset_paths.hpp`
// and `cooker/src/outputs.hpp` both already record about their own moves: a
// rule that lives in the executable is a rule with no test.

#include <span>
#include <string>
#include <vector>

namespace forge {

// The five directories the tool works in. Every one ends with exactly one '/'
// - `file_io.hpp` is explicit that dropping the separator "silently moves the
// save file, the settings and the cooked directory one level up", because
// every caller concatenates straight onto it.
struct ProjectPaths
{
    // Authored sources: `.forge`, `.tex`, `.png`, `.wav`.
    std::string assets;
    // What the game loads, and what the Cook button writes.
    std::string cooked;
    // The def documents - `models.toml` and its four siblings. In a project
    // this IS the project directory, because a mod's defs sit at its top level.
    std::string data;
    // Where the Blender bridge drops a `.glb`.
    std::string inbox;
    // Highest priority FIRST, the install's own directory LAST.
    //
    // ⚑⚑ THE SAME ORDER AND THE SAME REASON AS THE GAME'S, WHICH PHASE 25
    // STAGE E MADE A FACT ANOTHER FILE DEPENDS ON. A material may name one of
    // the engine's own stems (`mesh`, `membrane`, `cockpit`) and bring only
    // the stage it replaces, so the install's `shaders/` has to be reachable
    // from a project that ships one file. And the renderers that are NOT
    // materials - tonemap and the debug lines, in this tool - take the LAST
    // entry, because their descriptor sets and push blocks are C++ contracts
    // that no declaration checks.
    std::vector<std::string> shaderSearchPath;

    // True when these describe a PROJECT rather than the repo's own three
    // directories. The caller needs it to decide whether to create what is
    // missing, because the repo's directories are not this tool's to invent.
    //
    // ⚑⚑ A FIELD RATHER THAN SOMETHING THE CALLER RE-DERIVES, WHICH IS THIS
    // PROJECT'S MOST RECENT LESSON APPLIED (Phase 25 stage E: "two ways to
    // reach one state, counted at different granularities"). The first version
    // of `main.cpp` asked `!explicitProject.empty() || dev.assets.empty()` -
    // the same question spelled with ONE of the three dev paths where
    // `resolveProjectPaths` uses all three. A half-baked configuration would
    // then have resolved as a project and had none of its directories created.
    // Unreachable today, and unreachable is exactly how long that lasts.
    bool isProject = false;
};

// The source-tree paths a dev build bakes in, or empty strings in a shipping
// build. Passed in rather than read from the macros here so the rule can be
// tested from both sides: `forge.unit` runs in a dev configuration, and the
// branch a PLAYER takes would otherwise never be executed by anything. That is
// the exact trap Phase 22 recorded - "unconditional for seventeen phases, so
// no build ever took the fallback and the shipping path had never once been
// executed; the branch was written, reviewed, and dead."
struct DevPaths
{
    std::string assets;
    std::string data;
    std::string inbox;
};

// Where a project directory puts each of the five.
//
// ⚑ `data` IS THE PROJECT ITSELF and that is not an oversight. A mod's
// `models.toml` sits at its top level, beside `assets/` rather than inside a
// `data/` - see `game/mods/README.md`. `DefEditor` reads five fixed filenames
// out of one directory, so this is the whole of what pointing it at a mod costs.
[[nodiscard]] ProjectPaths projectPathsFor(const std::string& projectDirectory,
                                           const std::string& executableDirectory);

// What a dev build works in: the repo's own three directories, with `cooked/`
// and `shaders/` beside the binary where the build targets generate them.
[[nodiscard]] ProjectPaths devPathsFor(const DevPaths& dev, const std::string& executableDirectory);

// The one rule, in priority order:
//
//   1. an explicit project directory, honoured in EVERY configuration;
//   2. otherwise the baked dev paths, when this build has them;
//   3. otherwise the executable's own directory, treated as the project.
//
// ⚑⚑ (1) COMES FIRST IN A DEV BUILD TOO, DELIBERATELY. It is what makes the
// shipping arrangement drivable from the build tree - the branch a player
// takes gets exercised by the developer who wrote it, rather than first
// running on a stranger's machine.
//
// ⚑ (3) is the tool's own folder rather than the working directory: on Windows
// a double-click sets the two the same and a terminal launch does not, and a
// tool that reads a different project depending on how it was started is worse
// than one that reads an empty one.
[[nodiscard]] ProjectPaths resolveProjectPaths(const std::string& explicitProject,
                                               const DevPaths& dev,
                                               const std::string& executableDirectory);

// `--project <dir>`, or a single bare path argument - which is what dragging a
// folder onto `forge.exe` passes. Empty when neither is present.
//
// ⚑ The bare form is not a convenience. The audience for a shipped Forge is a
// mod author on Windows who has never opened a terminal, and drag-a-folder-
// onto-the-exe is the gesture they already have. `--frames` and `--open` both
// take a value, so their operands must not be mistaken for one.
[[nodiscard]] std::string parseProjectArgument(std::span<const std::string> arguments);

// Ensures `assets/`, `assets/meshes/`, `assets/textures/`, `assets/sounds/`,
// `cooked/`, `shaders/` and the inbox exist under a project. Returns false if
// any could not be created.
//
// ⚑⚑ A NEW MOD IS AN EMPTY DIRECTORY, AND EVERY LISTING IN THIS TOOL READS A
// DIRECTORY THAT MIGHT NOT BE THERE. `listFiles` cannot tell a missing
// directory from an empty one - the property `game/mods/README.md` records as
// the reason `mods/` ships empty-but-present - so creating them is what makes
// the Blender bridge's drop target and the Cook button's output exist before
// anything has been authored. Never called for the dev tree: those directories
// are the repo's and are not the tool's to invent.
[[nodiscard]] bool createProjectDirectories(const ProjectPaths& paths);

} // namespace forge
