#include "control/zones.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "spatial/angles.hpp"

namespace sonitude::control
{
namespace
{
ZoneId ZoneIdFromName(std::string name)
{
  std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char c)
  {
    if (c == '-' || c == ' ')
    {
      return '_';
    }
    return static_cast<char>(std::tolower(c));
  });
  if (name == "front_auto_focus")
  {
    return ZoneId::FrontAutoFocus;
  }
  if (name == "right_assist")
  {
    return ZoneId::RightAssist;
  }
  if (name == "left_assist")
  {
    return ZoneId::LeftAssist;
  }
  if (name == "rear_ambient")
  {
    return ZoneId::RearAmbient;
  }
  return ZoneId::None;
}
}  // namespace

ZoneMap::ZoneMap(std::vector<app::ZoneConfig> zones) : zones_(std::move(zones)) {}

std::optional<ResolvedZone> ZoneMap::zoneFor(const float azimuth_deg) const
{
  for (const auto& zone : zones_)
  {
    if (spatial::AngleWithinIntervalDeg(
            azimuth_deg, static_cast<double>(zone.azimuth_min_deg), static_cast<double>(zone.azimuth_max_deg)))
    {
      return ResolvedZone{zone.name, ZoneIdFromName(zone.name), zone.policy};
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
