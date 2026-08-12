#include "Sol/Proto/Frames/HierarchicalFrameModel.h"

#include <stdexcept>

namespace sol::proto::frames {

void HierarchicalFrameModel::rebuild(const FrameGraphSnapshot& snapshot)
{
    m_epoch = snapshot.epoch;
    m_parentToFrame = snapshot.parentToFrame;
}

FrameId HierarchicalFrameModel::lowestCommonAncestor(FrameId a, FrameId b) noexcept
{
    int depthA = frameDepth(a);
    int depthB = frameDepth(b);

    while (depthA > depthB) {
        a = parentFrame(a);
        --depthA;
    }
    while (depthB > depthA) {
        b = parentFrame(b);
        --depthB;
    }
    while (a != b) {
        a = parentFrame(a);
        b = parentFrame(b);
    }
    return a;
}

StateVector HierarchicalFrameModel::convert(const StateVector& state, FrameId target) const
{
    if (state.epoch.secondsPastJ2000() != m_epoch.secondsPastJ2000()) {
        throw std::runtime_error(
            "HierarchicalFrameModel: state epoch does not match the model's snapshot");
    }
    if (state.frame == target) {
        return state;
    }

    const FrameId ancestor = lowestCommonAncestor(state.frame, target);

    StateVector current = state;
    while (current.frame != ancestor) {
        const FrameId parent = parentFrame(current.frame);
        current = toParent(m_parentToFrame[static_cast<std::size_t>(current.frame)], current, parent);
    }

    // Descending needs the path from the ancestor down to the target, which the parent links
    // only give in the opposite direction. The path is at most kFrameCount long, so it is
    // collected into a fixed array rather than a heap allocation -- allocation counts are a
    // mandatory P1a measurement, and a conversion that allocates would put a heap operation
    // inside the measured region of every scenario in the increment.
    FrameId descent[kFrameCount]{};
    std::size_t descentLength = 0;
    for (FrameId frame = target; frame != ancestor; frame = parentFrame(frame)) {
        descent[descentLength++] = frame;
    }
    while (descentLength > 0) {
        const FrameId frame = descent[--descentLength];
        current = toChild(m_parentToFrame[static_cast<std::size_t>(frame)], current, frame);
    }

    return current;
}

int HierarchicalFrameModel::transformApplications(FrameId from, FrameId to) noexcept
{
    if (from == to) {
        return 0;
    }
    const FrameId ancestor = lowestCommonAncestor(from, to);
    return (frameDepth(from) - frameDepth(ancestor)) + (frameDepth(to) - frameDepth(ancestor));
}

} // namespace sol::proto::frames
