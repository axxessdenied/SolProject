#include "Sol/Proto/Orbit/CampaignClock.h"

#include <cmath>

namespace sol::proto::orbit {
namespace {

[[nodiscard]] std::int64_t roundToNanoseconds(double seconds) noexcept
{
    return static_cast<std::int64_t>(std::llround(seconds * 1.0e9));
}

} // namespace

CampaignDuration CampaignDuration::fromSecondsRounded(double seconds) noexcept
{
    return CampaignDuration::fromNanoseconds(roundToNanoseconds(seconds));
}

CampaignInstant CampaignInstant::fromSecondsRounded(double seconds) noexcept
{
    return CampaignInstant::fromNanoseconds(roundToNanoseconds(seconds));
}

frames::TdbEpoch toTdb(CampaignInstant instant, frames::TdbEpoch campaignEpochTdb) noexcept
{
    // TDB is a uniform scale, so campaign nanoseconds add to it directly. The conversion is a
    // single addition precisely because the leap-second machinery lives on the UTC side of the
    // boundary, where A2 put it: a campaign tick is never a UTC second and is never treated as
    // one.
    return campaignEpochTdb.advancedBy(
        frames::Seconds::fromSeconds(instant.secondsPastCampaignEpoch()));
}

} // namespace sol::proto::orbit
