#pragma once

#include <array>
#include <cmath>

namespace sonitude::spatial
{
constexpr double kPi = 3.14159265358979323846;

inline double NormalizeAzimuthDeg(double angle_deg)
{
  double out = std::fmod(angle_deg, 360.0);
  if (out > 180.0)
  {
    out -= 360.0;
  }
  if (out <= -180.0)
  {
    out += 360.0;
  }
  return out;
}

inline double SignedAngularDistanceDeg(double from_deg, double to_deg)
{
  return NormalizeAzimuthDeg(to_deg - from_deg);
}

inline double AngularDistanceAbsDeg(double a_deg, double b_deg)
{
  return std::fabs(SignedAngularDistanceDeg(a_deg, b_deg));
}

inline bool AngleWithinIntervalDeg(double angle_deg, double min_deg, double max_deg)
{
  const double a = NormalizeAzimuthDeg(angle_deg);
  const double min_n = NormalizeAzimuthDeg(min_deg);
  const double max_n = NormalizeAzimuthDeg(max_deg);
  if (min_n <= max_n)
  {
    return a >= min_n && a <= max_n;
  }
  return (a >= min_n) || (a <= max_n);
}

inline std::array<double, 3> UnitVectorFromAzElDeg(double azimuth_deg, double elevation_deg)
{
  const double az = azimuth_deg * (kPi / 180.0);
  const double el = elevation_deg * (kPi / 180.0);
  const double cos_el = std::cos(el);
  return {cos_el * std::cos(az), cos_el * std::sin(az), std::sin(el)};
}
}  // namespace sonitude::spatial
