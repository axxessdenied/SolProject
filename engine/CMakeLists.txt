# SolEngine: project-internal static libraries linked into the game and test executables.
#
# ADR 0005 - no stable C++ binary ABI, no shared-library export surface.
# ADR 0006 - one owned subsystem namespace per module, aligned with its target and include path.

add_subdirectory(platform)
add_subdirectory(render)
