#pragma once

// Where a cooked asset is looked for, now that a mod layer can carry one
// (engine plan Phase 24 stage S).
//
// ⚑⚑ THE GAP THIS CLOSES. Mod layers have carried `*.toml` defs and
// `scripts/*.lua` since Phase 5 and nothing else. Every asset resolved against
// ONE flat `cooked/` beside the executable, so a mod's `[[model]]` row naming
// `mymesh` looked for `cooked/mymesh.smesh` - a file only the BUILD produces.
// A mod could therefore describe content it had no way to supply, which is
// Phase 9 stage A's wall one level out: that stage removed "the runtime cannot
// reference a mesh it was not compiled to know about", and this removes "the
// runtime cannot reference an asset that was not in the build".
//
// ⚑ PURE WHERE IT CAN BE, AND HERE RATHER THAN IN `main.cpp`, for the reason
// `cooker/src/outputs.hpp` already records about its own move: a rule that
// lives in the executable is a rule with no test. `main.cpp` keeps the
// filesystem - the scanning and the ordering of startup - and this keeps the
// decisions. `resolveAsset` is the one function here that touches a disk, and
// it is three lines over the pure one above it.

#include <span>
#include <string>
#include <vector>

namespace game {

// The mod layer NAMES found in `listing`, sorted and unique. A mod is a
// first-level subdirectory of `modsDirectory`; a file sitting directly in it
// (`README.md`) is not one and is skipped.
//
// ⚑⚑ HANDED A LISTING RATHER THAN READING ONE, WHICH IS WHAT MAKES THE `C:`
// BUG TESTABLE. The version this replaces prefix-matched `platform::listFiles`
// output against a constructed directory path and, when the match FAILED, fell
// through to "take the first path segment of whatever this is" - so a
// separator mismatch produced a mod layer literally named `C:` in the first
// shipping build. The mismatch itself was fixed in `platform` (Phase 22), and
// this keeps the second half of the lesson: a path that is not under
// `modsDirectory` is REPORTED AND SKIPPED, never guessed at. `outUnrelated`
// collects those so the caller can say so out loud instead of quietly
// discovering fewer mods than the player installed.
[[nodiscard]] std::vector<std::string> modLayerNames(const std::string& modsDirectory,
                                                     std::span<const std::string> listing,
                                                     std::vector<std::string>* outUnrelated = nullptr);

// Scans `modsDirectory` and returns the full path of each mod layer, in name
// order - the order the def layering already applies them in, where a later
// name wins. A missing directory yields none, which is normal.
[[nodiscard]] std::vector<std::string> discoverModLayers(const std::string& modsDirectory);

// The directories to search for one KIND of asset, HIGHEST PRIORITY FIRST:
// each mod layer's `<layerSubdirectory>/`, last-named layer first, then
// `baseDirectory`. The two wrappers below are the only callers, and they exist
// so that this rule is written once.
//
// ⚑⚑ THE ORDER IS THE REVERSE OF THE DEF LAYER ORDER AND THAT IS NOT A
// MISTAKE. Defs merge base-first so that a later layer OVERWRITES what an
// earlier one set; assets are not merged, they are FOUND, so the same
// precedence means looking at the last layer first and stopping. Getting this
// backwards is invisible until two mods ship the same stem, which is exactly
// the kind of bug that reaches a player rather than a test - so the order is
// asserted rather than commented.
//
// ⚑⚑ AND THE BASE DIRECTORY IS LAST, WHICH IS LOAD-BEARING BEYOND PRECEDENCE
// SINCE PHASE 25 STAGE E. `SceneRenderer` takes the shader path's LAST entry
// as the install's own shader directory, because the six renderers that are
// not materials must not resolve into a mod (see its `initialize`). That makes
// "the base is the fallback and therefore last" a fact another file depends on
// rather than a property of this one, so it is asserted here by name.
//
// ⚑ Every returned directory ends with exactly one '/'. `file_io.hpp` is
// explicit that dropping a trailing separator "silently moves the save file,
// the settings and the cooked directory one level up", because every caller
// concatenates straight onto it - and here the inputs genuinely differ
// (`executableDirectory()` supplies one, a layer path does not).
[[nodiscard]] std::vector<std::string> layeredSearchPath(const std::string& baseDirectory,
                                                         std::span<const std::string> modLayerDirectories,
                                                         const std::string& layerSubdirectory);

// Where a cooked asset is looked for (Phase 24 stage S): `mods/<name>/cooked/`
// over the base game's `cooked/`.
[[nodiscard]] std::vector<std::string> cookedSearchPath(const std::string& baseCookedDirectory,
                                                        std::span<const std::string> modLayerDirectories);

// Where a material's SPIR-V is looked for (Phase 25 stage E):
// `mods/<name>/shaders/` over the install's own `shaders/`.
//
// ⚑⚑ `shaders/` AND NOT `cooked/`, AND THE REASON IS THAT `cooked/` HAS AN
// OWNER. The cooker writes exactly four extensions - `.smesh`, `.stex`,
// `.saud`, `.sfont` - and `strayOutputNames` calls that list exhaustive,
// because a cooked format missing from it is never swept and one added to it
// wrongly deletes real files. A `.spv` is produced by the author's glslc
// (decision 011) and by nothing the cooker knows about, so it is not a cooked
// asset and a directory whose contract is "what the cooker wrote" is the wrong
// place to say it is. `mods/<name>/shaders/` mirrors the install layout the
// base game already has, one directory per kind, and it is also where decision
// 011's "ship the .glsl beside the .spv" convention puts the GLSL - which is
// this repo's own `shaders/` directory exactly.
[[nodiscard]] std::vector<std::string> shaderSearchPath(const std::string& baseShaderDirectory,
                                                        std::span<const std::string> modLayerDirectories);

// Every path `name` could be at, in search order. Pure.
[[nodiscard]] std::vector<std::string> assetCandidates(std::span<const std::string> searchPath,
                                                       const std::string& name);

// The full path of the first candidate that exists on disk; empty when none
// does. ⚑ An existence probe rather than a load: the caller decides whether a
// miss is fatal, and `SceneRenderer`'s LOD chain relies on a miss being the
// ordinary case rather than an error worth logging.
[[nodiscard]] std::string resolveAsset(std::span<const std::string> searchPath, const std::string& name);

// "cooked/, mods/foo/cooked/" - for an error message that names where the
// game looked. ⚑ Phase 22's lesson about the data directory, applied one
// level down: when a load fails, WHERE it looked is most of the answer.
[[nodiscard]] std::string describeSearchPath(std::span<const std::string> searchPath);

} // namespace game
