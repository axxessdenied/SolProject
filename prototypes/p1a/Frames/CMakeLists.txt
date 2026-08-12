# P1a increment A2 frame and precision library.
#
# Like the harness, this is a prototype library rather than SolEngine code: it lives under
# prototypes/ and in sol::proto::frames, and nothing here may be promoted into engine/ or
# game/ without an explicit review. Unlike the harness, it is *not* free of domain concepts --
# it knows about ellipsoids, ephemeris fixtures, and Earth orientation -- so promoting it would
# be a larger decision, not a smaller one.
#
# Explicit source lists per ADR 0001; file(GLOB) is forbidden.

add_library(SolProtoFrames STATIC
    include/Sol/Proto/Frames/AscentProfile.h
    include/Sol/Proto/Frames/Ellipsoid.h
    include/Sol/Proto/Frames/FlatFrameModel.h
    include/Sol/Proto/Frames/FrameGraph.h
    include/Sol/Proto/Frames/FrameId.h
    include/Sol/Proto/Frames/FrameTransform.h
    include/Sol/Proto/Frames/HierarchicalFrameModel.h
    include/Sol/Proto/Frames/Mat3.h
    include/Sol/Proto/Frames/ReferenceData.h
    include/Sol/Proto/Frames/Sha256.h
    include/Sol/Proto/Frames/TextKernel.h
    include/Sol/Proto/Frames/TimeScales.h
    include/Sol/Proto/Frames/Units.h
    include/Sol/Proto/Frames/Vec3.h
    src/AscentProfile.cpp
    src/Ellipsoid.cpp
    src/FlatFrameModel.cpp
    src/FrameGraph.cpp
    src/FrameTransform.cpp
    src/HierarchicalFrameModel.cpp
    src/ReferenceData.cpp
    src/Sha256.cpp
    src/TextKernel.cpp
    src/TimeScales.cpp
    src/Units.cpp)

add_library(sol::proto::frames ALIAS SolProtoFrames)

sol_apply_project_options(SolProtoFrames)

target_include_directories(SolProtoFrames PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

# The default fixture location is baked in so a scenario can be run straight from the build
# tree with no arguments, which is what CTest does. It is an absolute path into the source
# tree: acceptable for a disposable prototype whose evidence is produced on the machine that
# built it, and every scenario still accepts --fixtures to override it. A shipping target would
# not do this; it would resolve data relative to the executable.
target_compile_definitions(SolProtoFrames
    PUBLIC "SOL_PROTO_FIXTURE_ROOT=\"${CMAKE_SOURCE_DIR}/fixtures/p1a\"")
