# Renderer tests and the headless capability report.

# Pure capability-check tests. No Vulkan loader, no device, no window: this executable runs
# anywhere, which is the property that lets it exercise device classes the project does not
# own. It is the negative control the P1b evidence plan requires.
add_executable(SolRenderCapabilityTests CapabilityCheckTests.cpp BaselineDeviceProfiles.h)
sol_apply_project_options(SolRenderCapabilityTests)
target_link_libraries(SolRenderCapabilityTests PRIVATE sol::render)
target_include_directories(SolRenderCapabilityTests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.capability-check COMMAND SolRenderCapabilityTests)

# Enumerates the real machine. Requires a Vulkan loader, so it is registered as a test that
# reports honestly when no loader is present rather than one that fails the suite on a
# machine without a GPU driver.
add_executable(SolRenderCapabilityReport CapabilityReportMain.cpp)
sol_apply_project_options(SolRenderCapabilityReport)
target_link_libraries(SolRenderCapabilityReport PRIVATE sol::render)
target_include_directories(SolRenderCapabilityReport PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.capability-report COMMAND SolRenderCapabilityReport)
