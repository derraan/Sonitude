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
