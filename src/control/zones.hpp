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

  // Index-based lookup for the control snapshot: the audio thread receives a
  // zone identity, never a zone name, so nothing string-shaped crosses the
  // realtime boundary.
  std::optional<std::size_t> zoneIndexFor(float azimuth_deg) const;
  std::size_t zoneCount() const { return zones_.size(); }
  const std::string& zoneName(std::size_t index) const;

 private:
  std::vector<app::ZoneConfig> zones_;
};
}  // namespace sonitude::control
