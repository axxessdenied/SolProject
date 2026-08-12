#pragma once

#include "Sol/Proto/Frames/FrameGraph.h"

#include <array>
#include <string_view>

namespace sol::proto::frames {

/// Candidate model B: parent-relative storage, converted by walking to a common ancestor.
///
/// Each frame keeps only its transform against its immediate parent, so the numbers stored
/// are of the scale of the relationship they describe: the vehicle sits metres from its local
/// origin, the launch site kilometres from Earth's centre, Earth 4.7e6 m from the Earth-Moon
/// barycentre. Barycentric magnitude is formed only when a conversion actually asks to cross
/// that boundary.
///
/// The cost is a variable-length walk. Converting between two deep frames on the same branch
/// is cheaper than the flat model; converting from the surface to the root is more expensive,
/// because the same five boundaries are applied one at a time instead of being pre-composed.
/// A2 measures both directions rather than assuming which one dominates.
///
/// See FlatFrameModel for why the two models share no base class.
class HierarchicalFrameModel {
public:
    HierarchicalFrameModel() = default;
    explicit HierarchicalFrameModel(const FrameGraphSnapshot& snapshot) { rebuild(snapshot); }

    /// Stores the snapshot's parent-relative transforms directly. No composition happens here,
    /// which is the whole point: this model never forms a transform it was not given.
    void rebuild(const FrameGraphSnapshot& snapshot);

    /// Converts @p state into @p target by walking up to the lowest common ancestor and back
    /// down. Throws on an epoch mismatch, as FlatFrameModel does.
    [[nodiscard]] StateVector convert(const StateVector& state, FrameId target) const;

    /// Number of transform applications convert() performs for this pair: the sum of the two
    /// path lengths to the lowest common ancestor.
    [[nodiscard]] static int transformApplications(FrameId from, FrameId to) noexcept;

    /// The lowest common ancestor of two frames.
    [[nodiscard]] static FrameId lowestCommonAncestor(FrameId a, FrameId b) noexcept;

    [[nodiscard]] static std::string_view modelName() noexcept { return "hierarchical-parent-relative"; }

    [[nodiscard]] TdbEpoch epoch() const noexcept { return m_epoch; }

    [[nodiscard]] const FrameTransform& parentToFrame(FrameId frame) const noexcept
    {
        return m_parentToFrame[static_cast<std::size_t>(frame)];
    }

private:
    std::array<FrameTransform, kFrameCount> m_parentToFrame{};
    TdbEpoch m_epoch{};
};

} // namespace sol::proto::frames
