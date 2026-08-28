# Phase 24 stage V. Asserts that an installed TOOL tree is the thing this
# project means by "the Forge" - and, just as importantly, that it is ONLY that.
#
# The sibling of `check_layout.cmake`, which does the same for the game, and it
# exists because `010-forge-ships.md` narrowed a promise rather than deleting
# one. Phase 9 said "the shipping binary carries no editor"; the Forge shipping
# to end users turned that into "the GAME binary carries no editor", and the
# price of narrowing it was a second assertion over the other package. Without
# this file the Forge's archive would be the only shipped artifact in the tree
# with nothing saying what belongs in it.
#
# Run by `package.forge.layout` against a prefix that `package.forge.install`
# has just produced with `--component forge`.
#
# Expects SOL_PREFIX and SOL_EXE_NAME on the command line.

if(NOT DEFINED SOL_PREFIX OR NOT DEFINED SOL_EXE_NAME)
    message(FATAL_ERROR "check_tool_layout.cmake needs -DSOL_PREFIX=<dir> -DSOL_EXE_NAME=<name>")
endif()

set(failures "")

function(sol_require_file relative)
    if(NOT EXISTS "${SOL_PREFIX}/${relative}")
        set(failures "${failures}\n  MISSING: ${relative}" PARENT_SCOPE)
    endif()
endfunction()

function(sol_forbid_glob pattern description)
    file(GLOB found "${SOL_PREFIX}/${pattern}")
    if(found)
        set(failures "${failures}\n  PRESENT BUT MUST NOT BE (${description}): ${found}" PARENT_SCOPE)
    endif()
endfunction()

# The tool, and the shaders without which it cannot open a window. Named files
# rather than the directory: install(DIRECTORY) over a missing source creates an
# empty destination without complaining, so "shaders/ exists" proves nothing.
sol_require_file("${SOL_EXE_NAME}")
sol_require_file("shaders/mesh.vert.spv")
sol_require_file("shaders/mesh.frag.spv")
# ⚑ The tonemap resolves from HERE and from nowhere else - it is the last entry
# of the search path and a project must not be able to replace it, because
# nothing declares what its descriptor set contains (project_paths.hpp). Its
# absence would be a black viewport in a tool whose whole purpose is looking.
# ⚑⚑ ITS VERTEX STAGE IS `fullscreen.vert.spv`, NOT `tonemap.vert.spv`. The
# first draft of this file asserted the name the pair implies and the test
# failed on a correct install, which is the "an assertion is a claim about the
# code" rule arriving from the test's side: the tonemap shares the fullscreen
# triangle with anything else that wants one, so the two stages of one pipeline
# do not share a stem.
sol_require_file("shaders/fullscreen.vert.spv")
sol_require_file("shaders/tonemap.frag.spv")
sol_require_file("shaders/debug_line.vert.spv")
sol_require_file("shaders/debug_line.frag.spv")
# ⚑ The engine's own material shaders, which are the FALLBACK half of the same
# search path: a mod's `[[material]]` row may name `membrane` or `cockpit` and
# ship only the stage it replaces, so these have to be reachable from a project
# that contains one file.
sol_require_file("shaders/membrane.frag.spv")
sol_require_file("shaders/cockpit.frag.spv")
sol_require_file("LICENSE")
sol_require_file("THIRD-PARTY.txt")
sol_require_file("README.txt")

# The whole shader set, not a sample of it. A material can name any stem the
# engine ships, so a partial install is a mod that works here and not there.
file(GLOB shaderFiles "${SOL_PREFIX}/shaders/*.spv")
list(LENGTH shaderFiles shaderCount)
if(shaderCount LESS 14)
    set(failures "${failures}\n  shaders/ holds ${shaderCount} file(s); expected the full set (14+)")
endif()

# ⚑⚑ THE EXCLUSIONS, AND THIS HALF IS WHY THE FILE EXISTS. Every one of these
# is absent because nothing names it in an install() rule with COMPONENT forge,
# and an omission is invisible to everything except an assertion like this one.
sol_forbid_glob("sol.exe" "the game is a separate package (010-forge-ships.md)")
sol_forbid_glob("sol" "the game is a separate package (010-forge-ships.md)")
sol_forbid_glob("cooker*" "the cook is a library call inside the Forge since stage T, not a second binary")
sol_forbid_glob("data" "the tool edits a PROJECT; it does not carry the game's defs")
sol_forbid_glob("mods" "a mod folder is the author's, and lives in their game install")
sol_forbid_glob("cooked" "cooked assets belong to a project, not to the tool")
sol_forbid_glob("*.sav" "saves are the game's and are never packaged")
# ⚑ Decision 011: a mod ships SPIR-V and no GLSL compiler is distributed by this
# project. AGENTS §5 scopes glslang/shaderc to build-time only, and shipping one
# beside this binary would be a dependency scope change nobody approved.
sol_forbid_glob("glslc*" "no GLSL compiler is distributed (011-mod-shaders-spirv.md)")
sol_forbid_glob("glslang*" "no GLSL compiler is distributed (011-mod-shaders-spirv.md)")
sol_forbid_glob("shaderc*" "no GLSL compiler is distributed (011-mod-shaders-spirv.md)")
sol_forbid_glob("*_tests*" "test binaries")
sol_forbid_glob("*_bench*" "benchmark binaries")
sol_forbid_glob("*.pdb" "debug databases are not shipped")
sol_forbid_glob("*.ilk" "incremental link files are not shipped")
sol_forbid_glob("*.lib" "import/static libraries are not shipped")

# ⚑⚑⚑ AND THE DEFECT THIS WHOLE STAGE EXISTED TO FIX, ASSERTED IN THE BINARY
# ITSELF RATHER THAN IN THE BUILD FILES THAT CAUSE IT. Before stage V,
# forge.exe carried three absolute source-tree paths including the developer's
# user name, because none of its three path defines was gated on
# SOL_DEV_DATA_PATHS. A gate is easy to write and easy to delete; this reads the
# shipped bytes and says whether a source tree is in them, which is the claim
# that actually matters and the one nothing else in this repo could make.
#
# ⚑⚑⚑ THE THREE DIRECTORIES BY NAME, AND NOT THE REPO ROOT - THE FIRST VERSION
# OF THIS CHECK LOOKED FOR THE ROOT AND WAS **TRUE BUT MISLEADING**, WHICH IS
# THE DEFECT SHAPE PHASE 25 STAGE E RECORDED, ARRIVING IN A TEST. It passed on
# Windows and failed on Linux saying "a path define is not gated on
# SOL_DEV_DATA_PATHS" - and the define was gated. What it had actually found was
# six `__FILE__` strings from a `VK_CHECK`-style macro in `engine/rhi/src`, i.e.
# SOURCE file names rather than data directories. The tell was that the shipped
# GAME binary carries the identical six, and has since Phase 22 - so whatever it
# was, it was not this stage's defect, and a check that cannot tell them apart
# would have sent every future reader to the wrong line. (Windows missed it only
# because MSVC writes `__FILE__` with backslashes.)
#
# ⚑ These three ARE the defect: they are the values `tools/forge/CMakeLists.txt`
# bakes, they end in a directory rather than a `.cpp`, and no macro can produce
# them. Naming them costs generality - a define added later pointing somewhere
# else is not covered - and buys a failure that names the actual cause.
file(READ "${SOL_PREFIX}/${SOL_EXE_NAME}" toolBytes HEX)
get_filename_component(repoRoot "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
foreach(bakedPath "assets" "game/data" "blender-inbox")
    string(HEX "${repoRoot}/${bakedPath}" bakedNeedle)
    string(FIND "${toolBytes}" "${bakedNeedle}" foundAt)
    if(foundAt GREATER_EQUAL 0)
        set(failures
            "${failures}\n  ${SOL_EXE_NAME} bakes the build machine's '${repoRoot}/${bakedPath}' - a path define in tools/forge/CMakeLists.txt is not gated on SOL_DEV_DATA_PATHS")
    endif()
endforeach()

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "installed tool layout is wrong at ${SOL_PREFIX}:${failures}")
endif()

message(STATUS "installed tool layout OK: ${SOL_PREFIX} (${shaderCount} shaders)")
