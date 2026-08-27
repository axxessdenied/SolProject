# Phase 22 stage C. Asserts that an installed tree is the thing this project
# means by "the game" - and, just as importantly, that it is ONLY that.
#
# Run by the `package.layout` test against a prefix that the `package.install`
# fixture has just produced. Both halves matter and the second is the one that
# needs a test at all: what the package must NOT contain is enforced in
# game/CMakeLists.txt by omission, and an omission is invisible. Someone adding
# an install(TARGETS) line for the Forge would break a promise this project has
# made since Phase 9 - "the shipping binary carries no editor" - and nothing
# else in the tree would notice.
#
# Expects SOL_PREFIX and SOL_EXE_NAME on the command line.

if(NOT DEFINED SOL_PREFIX OR NOT DEFINED SOL_EXE_NAME)
    message(FATAL_ERROR "check_layout.cmake needs -DSOL_PREFIX=<dir> -DSOL_EXE_NAME=<name>")
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

# The game itself, and one representative file from every directory the game
# reads at runtime. Naming real files rather than just the directories is
# deliberate: install(DIRECTORY) over a missing source creates an empty
# destination without complaining, so "the directory exists" proves nothing.
sol_require_file("${SOL_EXE_NAME}")
sol_require_file("data/ships.toml")
sol_require_file("data/scripts/init.lua")
sol_require_file("shaders/mesh.vert.spv")
sol_require_file("shaders/ui.frag.spv")
# ⚑ Phase 25 stage B: the first shader a DEF names rather than C++, and the
# file that names it. A shader reached by a hardcoded filename cannot go
# missing without the build breaking; one reached by a `[[material]]` row can,
# and the only symptom would be a gate you can see straight through.
sol_require_file("data/materials.toml")
sol_require_file("shaders/membrane.frag.spv")
# ⚑ Phase 25 stage C: the first shader a def names that also needs a SECOND
# FILE beside it. `cockpit.frag.spv` going missing costs the cabin its
# instruments; `cooked/cockpit_glow.stex` going missing costs the same thing
# through a completely different path - the material's descriptor set never
# gets written - and neither is reachable from a hardcoded filename anywhere in
# the build. The cooked glob below would catch the second only if a cook ran.
sol_require_file("shaders/cockpit.frag.spv")
sol_require_file("cooked/cockpit_glow.stex")
sol_require_file("cooked/ui.sfont")
sol_require_file("LICENSE")
sol_require_file("THIRD-PARTY.txt")
sol_require_file("README.txt")

# mods/ ships empty-but-present, so the README is what proves it arrived.
# listFiles cannot tell a missing directory from an empty one, which is how
# game/mods managed not to exist at all for seventeen phases.
sol_require_file("mods/README.md")

# cooked/ is generated into the build tree, so its contents depend on a cook
# having run. A named file is not enough - assert the whole set is there.
file(GLOB cookedFiles "${SOL_PREFIX}/cooked/*")
list(LENGTH cookedFiles cookedCount)
if(cookedCount LESS 20)
    set(failures "${failures}\n  cooked/ holds ${cookedCount} file(s); expected the full set (20+)")
endif()

# ⚑ The exclusions. Dev tools and test binaries are absent because nothing
# names them in an install() rule, and this is the only thing that can notice
# if that changes. Extensionless names are matched too, for Linux.
sol_forbid_glob("cooker*" "the asset cooker is a build tool")
sol_forbid_glob("forge*" "the Forge is a dev tool; the shipping binary carries no editor")
sol_forbid_glob("*_tests*" "test binaries")
sol_forbid_glob("*_bench*" "benchmark binaries")
sol_forbid_glob("*.pdb" "debug databases are not shipped")
sol_forbid_glob("*.ilk" "incremental link files are not shipped")
sol_forbid_glob("*.lib" "import/static libraries are not shipped")

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "installed layout is wrong at ${SOL_PREFIX}:${failures}")
endif()

message(STATUS "installed layout OK: ${SOL_PREFIX} (${cookedCount} cooked files)")
