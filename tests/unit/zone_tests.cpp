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
      {"front", -45.0F, 45.0F, sonitude::app::ZonePolicy::Focus},
      {"rear", 100.0F, 260.0F, sonitude::app::ZonePolicy::Ambient},
  };
  sonitude::control::ZoneMap map(zones);
  Require(map.contains("rear", -100.0F), "rear zone should include wrapped -100");
  Require(map.contains("rear", 180.0F), "rear zone should include 180");
  Require(map.contains("front", 0.0F), "front zone should include 0");
  Require(!map.contains("front", 120.0F), "front zone should exclude 120");
  const auto resolved = map.resolve(-100.0F);
  Require(resolved.has_value(), "wrapped angle should resolve to a zone");
  Require(resolved->index == 1, "resolved zone must preserve its configured index");
  Require(resolved->policy == sonitude::app::ZonePolicy::Ambient,
          "resolved zone must preserve its policy");
}
} // namespace

void RunZoneTests()
{
  TestZoneBoundariesWrap();
}
