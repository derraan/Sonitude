#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "control/steering_snapshot.hpp"

namespace sonitude::control
{
struct ResolvedZone
{
  std::string name;
  ZoneId zone_id = ZoneId::None;
  app::ZonePolicy policy = app::ZonePolicy::Focus;
};

class ZoneMap
{
 public:
  explicit ZoneMap(std::vector<app::ZoneConfig> zones);

  std::optional<ResolvedZone> zoneFor(float azimuth_deg) const;
  bool contains(const std::string& zone_name, float azimuth_deg) const;

 private:
  std::vector<app::ZoneConfig> zones_;
};
}  // namespace sonitude::control
