#include <stdexcept>
#include <string>
#include <vector>

#include "control/zones.hpp"

namespace
{
void Require(bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestZoneBoundariesWrap()
{
  std::vector<sonitude::app::ZoneConfig> zones = {
      {"front_auto_focus", -45.0F, 45.0F, sonitude::app::ZonePolicy::Focus},
      {"rear_ambient", 100.0F, 260.0F, sonitude::app::ZonePolicy::Ambient},
  };
  sonitude::control::ZoneMap map(zones);
  Require(map.contains("rear_ambient", -100.0F), "rear zone should include wrapped -100");
  Require(map.contains("rear_ambient", 180.0F), "rear zone should include 180");
  Require(map.contains("front_auto_focus", 0.0F), "front zone should include 0");
  Require(!map.contains("front_auto_focus", 120.0F), "front zone should exclude 120");
  const auto resolved = map.zoneFor(-100.0F);
  Require(resolved.has_value(), "wrapped angle should resolve to a zone");
  Require(resolved->policy == sonitude::app::ZonePolicy::Ambient, "zone policy should be preserved");
  Require(resolved->zone_id == sonitude::control::ZoneId::RearAmbient,
          "known zone names should map to stable realtime zone ids");
}
}  // namespace

void RunZoneTests()
{
  TestZoneBoundariesWrap();
}
