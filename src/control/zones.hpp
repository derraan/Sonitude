#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/config.hpp"

namespace sonitude::control
{
class ZoneMap
{
 public:
  explicit ZoneMap(std::vector<app::ZoneConfig> zones);

  std::optional<std::string> zoneFor(float azimuth_deg) const;
  bool contains(const std::string& zone_name, float azimuth_deg) const;

 private:
  std::vector<app::ZoneConfig> zones_;
};
}  // namespace sonitude::control
