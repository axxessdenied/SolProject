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

# Without this, a machine with no Vulkan loader reports a green pass for the one test that
# would have exercised real Vulkan, and that pass is counted in a suite total quoted as
# evidence. 77 makes the absence visible in the summary instead.
set_tests_properties(render.capability-report PROPERTIES SKIP_RETURN_CODE 77)

# Opens a real window and presents real frames. Registered as a test because a renderer that
# cannot complete a frame is the most useful thing to find out about automatically, but it
# needs both a display and a loader, so it reports a skip when either is missing.
add_executable(SolRenderLoop RenderLoopMain.cpp)
sol_apply_project_options(SolRenderLoop)
target_link_libraries(SolRenderLoop PRIVATE sol::render sol::platform)
add_test(NAME render.loop COMMAND SolRenderLoop)
set_tests_properties(render.loop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# The screen-space jitter gate. Registered as a test because it is a pass/fail threshold, not
# a benchmark: it either holds 0.25 px or it does not, and a regression should break the build
# rather than wait to be noticed in a report.
add_executable(SolJitterHarness JitterHarnessMain.cpp)
sol_apply_project_options(SolJitterHarness)
target_link_libraries(SolJitterHarness PRIVATE sol::render sol::platform)
add_test(NAME render.jitter-gate COMMAND SolJitterHarness)
set_tests_properties(render.jitter-gate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# The depth gate. Carries its own negative control: a conventional finite-far projection run
# through the identical harness, which must collapse. A depth test that cannot fail would say
# nothing about the one that passes.
add_executable(SolDepthGate DepthGateMain.cpp)
sol_apply_project_options(SolDepthGate)
target_link_libraries(SolDepthGate PRIVATE sol::render sol::platform)
add_test(NAME render.depth-gate COMMAND SolDepthGate)
set_tests_properties(render.depth-gate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# The LOD continuity gate. Runs the same descent twice — once with continuous morphing and once
# with it disabled — because a popping detector that never fires proves nothing about the
# renderer until it has been shown to fire on the scheme morphing replaced.
add_executable(SolLodGate LodGateMain.cpp)
sol_apply_project_options(SolLodGate)
target_link_libraries(SolLodGate PRIVATE sol::render sol::platform)
add_test(NAME render.lod-gate COMMAND SolLodGate)
set_tests_properties(render.lod-gate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# DISABLED, deliberately and temporarily. The memory half of this gate is measured and passing;
# the popping half is not trustworthy yet, and marking it disabled with a reason is honest
# where either leaving the suite permanently red or quietly deleting the test would not be.
#
# The metric has since been rebuilt and is now believed correct: per-pixel change distribution
# rather than the frame mean, and a transition the harness locates for itself from the
# renderer's patch count instead of a hand-picked altitude band. What blocks the gate is no
# longer the instrument.
#
# It is the scene. Across four quality settings and three altitude bands, disabling LOD
# morphing produces **no pixel anywhere changing by more than 15 of 255 luminance levels** in
# any single step. The transitions are geometrically too small to see: a level change adds
# roughly 180 m of detail at a distance of 18 km, and even under derivative-based lighting that
# moves the image by about a luminance level. The control therefore cannot demonstrate popping,
# and a "no popping" verdict measured against a control that never fires certifies nothing.
#
# What would make this measurable is terrain with far steeper relief at grid scale — the
# fine-octave amplitude here is about 180 m over a 4 km wavelength, a 2.5-degree slope, where
# real terrain is far rougher. That is a change to the test scene, not to the renderer or the
# metric, and it is the next thing to try.
set_tests_properties(render.lod-gate PROPERTIES DISABLED TRUE)
