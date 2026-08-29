#pragma once

#include <algorithm>
#include <cmath>

#include "spatial/angles.hpp"

namespace sonitude::spatial
{
// Authoritative head-centric frame:
// - azimuth 0 deg points forward (+Y)
// - positive azimuth turns clockwise toward listener-right (+X)
// - elevation positive is upward (+Z)
inline double NormalizeHeadAzimuthDeg(const double azimuth_deg)
{
  return NormalizeAzimuthDeg(azimuth_deg);
}

inline double LateralAngleDeg(const double azimuth_deg, const double elevation_deg)
{
  const double az = NormalizeHeadAzimuthDeg(azimuth_deg) * (kPi / 180.0);
  const double el = elevation_deg * (kPi / 180.0);
  const double lateral_sine = std::clamp(std::sin(az) * std::cos(el), -1.0, 1.0);
  return std::asin(lateral_sine) * (180.0 / kPi);
}
}  // namespace sonitude::spatial
