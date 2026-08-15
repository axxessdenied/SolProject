# Renderer tests and the headless capability report.

# Pure capability-check tests. No Vulkan loader, no device, no window: this executable runs
# anywhere, which is the property that lets it exercise device classes the project does not
# own. It is the negative control the P1b evidence plan requires.
add_executable(SolRenderCapabilityTests
               CapabilityCheckTests.cpp
               BaselineDeviceProfiles.h
               LunarGBaselineProfiles.h)
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
target_include_directories(SolRenderLoop PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.loop COMMAND SolRenderLoop)
set_tests_properties(render.loop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# Renderer contracts the gates depend on but none of them measures: that a root terrain patch
# has nothing to morph toward, and that a frame which drew nothing says so instead of handing
# back the last frame's pixels. Both were broken while the whole suite was green, which is the
# argument for testing them separately from any threshold.
add_executable(SolRendererContract RendererContractTests.cpp)
sol_apply_project_options(SolRendererContract)
target_link_libraries(SolRendererContract PRIVATE sol::render sol::platform)
target_include_directories(SolRendererContract PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.renderer-contract COMMAND SolRendererContract)
set_tests_properties(render.renderer-contract PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# The screen-space jitter gate. Registered as a test because it is a pass/fail threshold, not
# a benchmark: it either holds 0.25 px or it does not, and a regression should break the build
# rather than wait to be noticed in a report.
add_executable(SolJitterHarness JitterHarnessMain.cpp)
sol_apply_project_options(SolJitterHarness)
target_link_libraries(SolJitterHarness PRIVATE sol::render sol::platform)
target_include_directories(SolJitterHarness PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.jitter-gate COMMAND SolJitterHarness)
set_tests_properties(render.jitter-gate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

# The depth gate. Carries its own negative control: a conventional finite-far projection run
# through the identical harness, which must collapse. A depth test that cannot fail would say
# nothing about the one that passes.
add_executable(SolDepthGate DepthGateMain.cpp)
sol_apply_project_options(SolDepthGate)
target_link_libraries(SolDepthGate PRIVATE sol::render sol::platform)
target_include_directories(SolDepthGate PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.depth-gate COMMAND SolDepthGate)
set_tests_properties(render.depth-gate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# The LOD continuity gate. Runs the same descent twice — once with continuous morphing and once
# with it disabled — because a popping detector that never fires proves nothing about the
# renderer until it has been shown to fire on the scheme morphing replaced.
add_executable(SolLodGate LodGateMain.cpp)
sol_apply_project_options(SolLodGate)
target_link_libraries(SolLodGate PRIVATE sol::render sol::platform)
target_include_directories(SolLodGate PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.lod-gate COMMAND SolLodGate)
set_tests_properties(render.lod-gate PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# The LOD threshold's memory half. Separate from the gate above because the gate's memory reading
# was withdrawn for two reasons and only one was duration: the figure it read could not vary, so
# running it for thirty minutes would have produced thirty minutes of the same tautology. This
# measures process commit charge, which can.
#
# Registered at 45 seconds so the suite keeps a working instrument under test without paying half
# an hour for it. The 30-minute run the milestone names is performed deliberately and its output
# recorded under evidence/p1b/B1/raw/; `--minutes 30` reproduces it.
add_executable(SolMemoryTraverse MemoryTraverseMain.cpp)
sol_apply_project_options(SolMemoryTraverse)
target_link_libraries(SolMemoryTraverse PRIVATE sol::render sol::platform psapi)
target_include_directories(SolMemoryTraverse PRIVATE "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME render.memory-traverse COMMAND SolMemoryTraverse --seconds 45)
set_tests_properties(render.memory-traverse PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

# The memory gate's negative control, and the reason it leaks 4 KiB per frame rather than the
# 8 bytes the measurement method names. Leak size and run duration trade against each other: an
# 8-byte-per-frame leak is 240 KiB/min over a graded 30-minute run and -60 KiB/min over a
# 3-minute one, where the instrument's own noise swamps it. A control that fits inside the suite
# has to leak hard enough to clear that noise floor in 45 seconds, so this one is deliberately
# gross - about 35 MiB/min.
#
# What it verifies is therefore narrower than the method's sensitivity claim, and the two are not
# interchangeable: this proves continuously that the gate CAN fail, while the 8-byte control
# establishing how small a leak it catches is run deliberately at 30 minutes and recorded under
# evidence/p1b/B1/raw/. The program exits non-zero if a deliberate leak PASSES, so a gate that
# stopped being able to fail would break the suite here rather than keep awarding passes.
add_test(NAME render.memory-traverse-control
         COMMAND SolMemoryTraverse --seconds 45 --leak 4096)
set_tests_properties(render.memory-traverse-control PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

# Enabled on 2026-08-14, after the verdict moved off an instrument that could not work.
#
# It sat DISABLED because both configurations recorded zero pops, so the control could not fire
# and a "no popping" reading certified nothing. The cause was not sensitivity. A local-outlier
# test needs a transition to be an isolated event, and on a descent with hundreds of patches on
# screen they are not — an abrupt scheme raises the whole baseline instead of spiking, so there
# is nothing to flag. Re-measuring at a 20° field of view grew the separation on every other
# statistic by roughly 3x and still produced zero pops in both configurations, which is what
# distinguishes "the instrument is deaf" from "the quantity is not there".
#
# The verdict now comes from the isolated-transition sweep, which was built for this and was
# previously unusable for an unrelated reason: it located its band by the largest patch-count
# change and kept landing on horizon patches that are tiny on screen. Frustum culling fixed that
# as a side effect, since off-screen patches are no longer drawn and no longer attract the scan.
#
# What the gate now certifies is narrower than "morphing prevents visible popping", and it says
# so in its own output: the control is itself below the perceptual limit, so this scene does not
# pop visibly either way. What is certified is a 20x margin under the limit and a 4.4x response
# to switching the morph off. The memory half still reports as structural and now points at
# render.memory-traverse above, which measures the milestone's 30-minute criterion on an
# instrument that can actually move.
