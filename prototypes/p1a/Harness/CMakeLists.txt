# P1a measurement harness.
#
# This is the only durable code in prototypes/p1a/. The scenario executables that consume it
# are disposable; the measurement format, provenance capture, and determinism discipline
# implemented here are what later increments and milestones inherit.
#
# Explicit source lists per ADR 0001; file(GLOB) is forbidden.

add_library(SolProtoHarness STATIC
    include/Sol/Proto/Harness/AllocationCounter.h
    include/Sol/Proto/Harness/Check.h
    include/Sol/Proto/Harness/HostInfo.h
    include/Sol/Proto/Harness/JsonWriter.h
    include/Sol/Proto/Harness/MetricSeries.h
    include/Sol/Proto/Harness/ProcessMetrics.h
    include/Sol/Proto/Harness/ScenarioReport.h
    src/AllocationCounter.cpp
    src/Check.cpp
    src/HostInfo.cpp
    src/JsonWriter.cpp
    src/MetricSeries.cpp
    src/ProcessMetrics.cpp
    src/ScenarioReport.cpp)

add_library(sol::proto::harness ALIAS SolProtoHarness)

sol_apply_project_options(SolProtoHarness)

target_include_directories(SolProtoHarness PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

# Interface-linked so the generated toolchain-facts header reaches consumers that report it.
target_link_libraries(SolProtoHarness
    PUBLIC sol::proto::toolchainFacts
    PRIVATE psapi)

# The replacement global operator new/delete live in AllocationCounter.cpp. A static library
# only contributes an object file that something references, so an executable that never
# calls into sol::proto::allocations would silently get the default operators and report
# zero allocations. /WHOLEARCHIVE forces every object in, which makes the accounting
# unconditional rather than dependent on call-site coincidence.
target_link_options(SolProtoHarness INTERFACE
    "/WHOLEARCHIVE:$<TARGET_FILE:SolProtoHarness>")
