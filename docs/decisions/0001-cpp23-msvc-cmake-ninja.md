# ADR 0001 — C++23, MSVC, CMake, and Ninja

**Status:** Accepted

**Date:** 2026-08-12

## Context

SolEngine is a Windows x64-first C++ engine developed by a solo creator with AI agents. The project needs one reproducible command-line build path, separated Debug/Release outputs, good Visual Studio debugging, and access to modern language/library features without depending on compiler extensions.

## Decision

- Use C++23 for project-owned C++.
- Use the current supported MSVC toolset chosen for the development environment; record and raise the minimum toolset deliberately when features require it.
- Use CMake as the project/build description.
- Use the single-configuration `Ninja` generator with separate Debug and Release presets/build directories.
- Set `CXX_STANDARD 23`, `CXX_STANDARD_REQUIRED ON`, and `CXX_EXTENSIONS OFF` on project-owned targets so CMake cannot silently decay to an older standard.
- Keep explicit source lists and run configure/build through checked-in presets once implementation begins.

## Alternatives considered

- **C++20:** mature and sufficient, but rejected because the project is starting fresh and has accepted C++23 as its baseline.
- **Visual Studio generator:** convenient IDE integration, but rejected as the canonical generator in favor of consistent command-line Ninja builds. Visual Studio may still open/debug the CMake project.
- **Ninja Multi-Config:** valid, but separate single-configuration trees make configuration-specific artifacts and diagnostics simpler for this project.
- **Clang-cl:** useful as a future conformance/diagnostic build, but not the primary compiler.

## Consequences

- Dependency proposals must demonstrate C++23/MSVC compatibility.
- Code must not assume every C++23 library facility is complete merely because `/std:c++latest` or a language mode exists; required facilities need compile probes or a documented minimum toolset.
- CI and developer checks should eventually add Clang-cl where its value exceeds maintenance cost.

## Validation

- The first build milestone must compile a small conformance target using each required C++23 facility.
- CMake supports `CXX_STANDARD 23`; requiring the standard prevents fallback to an older mode. See the [official CMake `CXX_STANDARD` documentation](https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD.html).
- CMake documents `Ninja` as single-configuration and `Ninja Multi-Config` as multi-configuration. See the [official CMake generator guidance](https://cmake.org/cmake/help/latest/guide/tutorial/Before%20You%20Begin.html).
- MSVC continues to add C++23 conformance across toolset releases, so the exact minimum must be based on used features. See [Microsoft's current MSVC conformance notes](https://learn.microsoft.com/en-us/cpp/overview/msvc-conformance-improvements?view=msvc-170).
