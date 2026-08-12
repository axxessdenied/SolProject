#include "Sol/Proto/Frames/FlatFrameModel.h"

#include <stdexcept>
#include <string>

namespace sol::proto::frames {

void FlatFrameModel::rebuild(const FrameGraphSnapshot& snapshot)
{
    m_epoch = snapshot.epoch;

    // Frames are composed in FrameId order, which is also depth order: every frame's parent
    // has a lower enum value than the frame itself. That is a property of the enum, so it is
    // asserted rather than trusted -- reordering FrameId later would otherwise compose against
    // a stale parent and produce a model that is subtly, silently wrong.
    for (std::size_t i = 0; i < kFrameCount; ++i) {
        const FrameId frame = static_cast<FrameId>(i);
        const FrameId parent = parentFrame(frame);
        if (frame == FrameId::SsbIcrf) {
            m_rootToFrame[i] = FrameTransform{};
            continue;
        }
        if (static_cast<std::size_t>(parent) >= i) {
            throw std::logic_error("FlatFrameModel: FrameId order no longer matches graph depth; "
                                   "frame " + std::string{frameName(frame)}
                                   + " precedes its parent");
        }
        m_rootToFrame[i] =
            compose(m_rootToFrame[static_cast<std::size_t>(parent)], snapshot.parentToFrame[i]);
    }
}

StateVector FlatFrameModel::convert(const StateVector& state, FrameId target) const
{
    if (state.epoch.secondsPastJ2000() != m_epoch.secondsPastJ2000()) {
        throw std::runtime_error("FlatFrameModel: state epoch does not match the model's snapshot");
    }
    if (state.frame == target) {
        return state;
    }

    StateVector atRoot = state;
    if (state.frame != FrameId::SsbIcrf) {
        atRoot = toParent(m_rootToFrame[static_cast<std::size_t>(state.frame)], state,
                          FrameId::SsbIcrf);
    }
    if (target == FrameId::SsbIcrf) {
        return atRoot;
    }
    return toChild(m_rootToFrame[static_cast<std::size_t>(target)], atRoot, target);
}

int FlatFrameModel::transformApplications(FrameId from, FrameId to) noexcept
{
    if (from == to) {
        return 0;
    }
    int applications = 0;
    if (from != FrameId::SsbIcrf) {
        ++applications;
    }
    if (to != FrameId::SsbIcrf) {
        ++applications;
    }
    return applications;
}

} // namespace sol::proto::frames
