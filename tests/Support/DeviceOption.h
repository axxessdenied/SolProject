/// @file
/// The `--device` option every presenting render harness accepts.
///
/// The P1b [reference-hardware evidence
/// plan](../../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) requires
/// every gating threshold to be measured on **both** available devices, under their own names
/// and never as proxies for the absent baseline classes. Nothing could satisfy that while the
/// renderer's device choice was internal, so the harnesses share one spelling of the option
/// rather than each inventing its own.
///
/// Shared for a second reason: a harness that parsed `--device` slightly differently from its
/// neighbours would make two evidence runs incomparable in a way that no output field would
/// reveal.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"
#include "Sol/Render/Renderer.h"

#include <cstring>
#include <string>

namespace sol::testing {

/// Parses `--device <token>` out of `argv`, leaving every other argument alone.
///
/// Accepted tokens:
///   - `discrete`   — any discrete GPU
///   - `integrated` — any integrated GPU
///   - anything else — a case-insensitive substring of the device name, e.g. `UHD` or `4060`
///
/// Absent, the renderer's production policy applies. An unmatched selection is **not** silently
/// ignored: `Renderer::create` fails and names every device it enumerated. That is the whole
/// point of the option — a harness that asked for the integrated GPU and quietly measured the
/// discrete one would file its numbers under the wrong device.
inline sol::render::DeviceSelection parseDeviceOption(int argc, char** argv)
{
    sol::render::DeviceSelection selection;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--device") != 0 || i + 1 >= argc) {
            continue;
        }
        const std::string token = argv[++i];
        if (token == "discrete") {
            selection.kind = sol::render::DeviceKind::DiscreteGpu;
        } else if (token == "integrated") {
            selection.kind = sol::render::DeviceKind::IntegratedGpu;
        } else {
            selection.nameContains = token;
        }
    }
    return selection;
}

/// One line naming the selection, for a harness to print beside its results.
///
/// Every P1b report records the device actually selected, read back from
/// `Renderer::selectedDevice()` rather than from what was requested. This line records the
/// *request*, which is the other half: together they show that the run measured what it meant
/// to, rather than only what it got.
inline std::string describeDeviceRequest(const sol::render::DeviceSelection& selection)
{
    return "device requested: " + sol::render::toString(selection);
}

} // namespace sol::testing
