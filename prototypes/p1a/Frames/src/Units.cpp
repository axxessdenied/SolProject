#include "Sol/Proto/Frames/Units.h"

#include <cmath>

namespace sol::proto::frames {

CampaignTime CampaignTime::fromSecondsRounded(double value) noexcept
{
    // std::llround rather than a cast: a cast truncates toward zero, which turns a value that
    // is one ULP below a whole nanosecond into the previous nanosecond and makes an evenly
    // spaced sample schedule quietly uneven.
    return CampaignTime::fromNanoseconds(static_cast<std::int64_t>(std::llround(value * 1e9)));
}

} // namespace sol::proto::frames
