#pragma once

// What a cook is entitled to leave in the output directory, and what it is not.
//
// ⚑⚑ THE GAP THIS CLOSES: THE COOKER WALKS SOURCES, SO IT CANNOT SEE A SOURCE
// THAT IS GONE. Deleting `foo.forge` leaves `foo.smesh` - and `foo.lod1.smesh`,
// and `foo.lod2.smesh` - in the cooked directory forever, because nothing ever
// visits an asset that no longer exists. A rebuild will not clear them. The
// game ignores them (no `[[model]]` row names them) but the Forge lists cooked
// meshes, so they show up as rows that can be opened and never edited, and
// stage K1's grouped list counts them.
//
// ⚑ Stage F already solved the NEIGHBOURING case - `writeMeshLevels` deletes
// levels above what the cook just produced - but that only runs for an asset
// that still cooks. This is the same rule one level up: over the whole
// directory rather than over one asset's level chain.
//
// ⚑ Pure, and here rather than in `main.cpp`, for the reason this file's
// neighbours already record: `cooker.unit` links the LIBRARY, and a rule that
// lives in the executable is a rule with no test. `main.cpp` keeps the
// filesystem - the listing and the deleting - and this keeps the decision.

#include <string>
#include <vector>

namespace sol::cooker {

// Every FILENAME a cook of these job outputs may leave behind: each output's
// own name, plus - for a mesh - its whole possible LOD sibling range.
//
// ⚑ NAMES, NOT PATHS, and that is deliberate: the output directory is flat, so
// a filename is already a unique key, and comparing paths would mean agreeing
// with `platform::listFiles` about separators and absolute-vs-relative. The
// cooker builds `out + "/" + stem + ext` from an argv string that may be
// `.\build\bin\cooked`, which lists back as something else entirely.
//
// ⚑ THE WHOLE SIBLING RANGE IS CLAIMED WHETHER OR NOT IT EXISTS, because how
// many levels an asset generates is not known until it cooks - and a cook is
// SKIPPED when the output is up to date. Claiming the range keeps a live
// asset's levels safe without running it; trimming a chain that got shorter is
// `writeMeshLevels`'s job and it still does it.
[[nodiscard]] std::vector<std::string> expectedOutputNames(const std::vector<std::string>& jobOutputs);

// The filenames in `present` that the cooker produces but no job claims.
//
// ⚑ Anything the cooker does not produce is left alone, however odd it looks.
// The output directory is not ours alone - `.spv` shaders and whatever a future
// step writes share it - and a sweep that deletes what it does not recognise is
// a sweep nobody can safely extend.
[[nodiscard]] std::vector<std::string> strayOutputNames(const std::vector<std::string>& expected,
                                                        const std::vector<std::string>& present);

} // namespace sol::cooker
