#include "control/zones.hpp"

#include <utility>

#include "spatial/angles.hpp"

namespace sonitude::control
{
ZoneMap::ZoneMap(std::vector<app::ZoneConfig> zones) : zones_(std::move(zones)) {}

std::optional<std::string> ZoneMap::zoneFor(const float azimuth_deg) const
{
  for (const auto& zone : zones_)
  {
    if (spatial::AngleWithinIntervalDeg(
            azimuth_deg, static_cast<double>(zone.azimuth_min_deg), static_cast<double>(zone.azimuth_max_deg)))
    {
      return zone.name;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> ZoneMap::zoneIndexFor(const float azimuth_deg) const
{
  for (std::size_t i = 0; i < zones_.size(); ++i)
  {
    if (spatial::AngleWithinIntervalDeg(azimuth_deg,
                                        static_cast<double>(zones_[i].azimuth_min_deg),
                                        static_cast<double>(zones_[i].azimuth_max_deg)))
    {
      return i;
    }
  }
  return std::nullopt;
}

const std::string& ZoneMap::zoneName(const std::size_t index) const
{
  static const std::string kUnknown = "unknown";
  if (index >= zones_.size())
  {
    return kUnknown;
  }
  return zones_[index].name;
}

bool ZoneMap::contains(const std::string& zone_name, const float azimuth_deg) const
{
  for (const auto& zone : zones_)
  {
    if (zone.name != zone_name)
    {
      continue;
    }
    return spatial::AngleWithinIntervalDeg(
        azimuth_deg, static_cast<double>(zone.azimuth_min_deg), static_cast<double>(zone.azimuth_max_deg));
  }
  return false;
}
}  // namespace sonitude::control
