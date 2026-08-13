# P1a increment A3 orbit and time-warp library.
#
# A prototype library, on the same terms as Harness and Frames: it lives under prototypes/,
# in sol::proto::orbit, and nothing here may be promoted into engine/ or game/ without an
# explicit review. Like Frames and unlike Harness it is full of domain concepts -- conics,
# spheres of influence, campaign time -- so promoting it is a larger decision, not a smaller one.
#
# It depends on Frames for Vec3 and the reference-data loader. That dependency is deliberate
# and one-directional: A3 measures propagation *within* the frame model A2 selected, and
# duplicating a vector type to avoid the edge would have made the two increments' numbers
# incomparable.
#
# Explicit source lists per ADR 0001; file(GLOB) is forbidden.

add_library(SolProtoOrbit STATIC
    include/Sol/Proto/Orbit/BodySystem.h
    include/Sol/Proto/Orbit/CampaignClock.h
    include/Sol/Proto/Orbit/ConicEphemeris.h
    include/Sol/Proto/Orbit/HybridPropagator.h
    include/Sol/Proto/Orbit/Integrator.h
    include/Sol/Proto/Orbit/KeplerPropagator.h
    include/Sol/Proto/Orbit/OrbitalElements.h
    include/Sol/Proto/Orbit/Stumpff.h
    include/Sol/Proto/Orbit/TwoBody.h
    src/BodySystem.cpp
    src/CampaignClock.cpp
    src/ConicEphemeris.cpp
    src/HybridPropagator.cpp
    src/Integrator.cpp
    src/KeplerPropagator.cpp
    src/OrbitalElements.cpp
    src/Stumpff.cpp
    src/TwoBody.cpp)

add_library(sol::proto::orbit ALIAS SolProtoOrbit)

sol_apply_project_options(SolProtoOrbit)

target_include_directories(SolProtoOrbit PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

target_link_libraries(SolProtoOrbit PUBLIC sol::proto::frames)
