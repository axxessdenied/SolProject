#pragma once

#include "Sol/Proto/Frames/FrameGraph.h"

#include <array>
#include <string_view>

namespace sol::proto::frames {

/// Candidate model A: one global root, every frame expressed directly against it.
///
/// Each frame's transform is composed down from SsbIcrf at construction, so a conversion is
/// always source -> root -> target: at most two transform applications regardless of how deep
/// the two frames are or how close together they sit.
///
/// The cost of that uniformity is that every conversion passes through barycentric magnitude.
/// Converting a vehicle's state into the launch-site frame 100 m away still forms coordinates
/// of order 1.5e11 m on the way, where one ULP of a double is about 33 micrometres. This is
/// the model's defining property, and A2 measures it rather than arguing about it.
///
/// Deliberately not related by inheritance to HierarchicalFrameModel. The two expose the same
/// member signatures and scenarios template over them, so the cost comparison contains no
/// virtual dispatch on either side -- the P1a measurement rules forbid instrumentation that
/// materially distorts the result, and a virtual call is a meaningful fraction of a
/// conversion this small.
class FlatFrameModel {
public:
    FlatFrameModel() = default;
    explicit FlatFrameModel(const FrameGraphSnapshot& snapshot) { rebuild(snapshot); }

    /// Recomposes every root-relative transform. Called once per timestep, not per conversion.
    void rebuild(const FrameGraphSnapshot& snapshot);

    /// Converts @p state into @p target.
    /// Throws std::runtime_error when @p state's epoch is not the snapshot's epoch, because a
    /// state converted against the wrong instant is wrong by hundreds of metres and looks
    /// entirely reasonable.
    [[nodiscard]] StateVector convert(const StateVector& state, FrameId target) const;

    /// Number of transform applications convert() performs for this pair. Recorded in
    /// evidence so the cost numbers can be read per application rather than per call.
    [[nodiscard]] static int transformApplications(FrameId from, FrameId to) noexcept;

    [[nodiscard]] static std::string_view modelName() noexcept { return "flat-ssb-rooted"; }

    [[nodiscard]] TdbEpoch epoch() const noexcept { return m_epoch; }

    /// The root-relative transform of @p frame, exposed so the precision budget can report the
    /// magnitudes each conversion actually forms.
    [[nodiscard]] const FrameTransform& rootToFrame(FrameId frame) const noexcept
    {
        return m_rootToFrame[static_cast<std::size_t>(frame)];
    }

private:
    std::array<FrameTransform, kFrameCount> m_rootToFrame{};
    TdbEpoch m_epoch{};
};

} // namespace sol::proto::frames
