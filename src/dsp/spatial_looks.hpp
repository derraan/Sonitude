#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace sonitude::dsp
{
constexpr std::size_t kGuardLooks = 3;
constexpr std::array<float, kGuardLooks> kGuardAzimuthOffsetDeg = {90.0F, -90.0F, 180.0F};

struct GuardSpectrumConstSpans
{
  std::array<std::span<const float>, kGuardLooks> re{};
  std::array<std::span<const float>, kGuardLooks> im{};
};

inline GuardSpectrumConstSpans BindGuardSpectra(
    const std::array<std::vector<float>, kGuardLooks>& re,
    const std::array<std::vector<float>, kGuardLooks>& im)
{
  GuardSpectrumConstSpans out{};
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    out.re[g] = re[g];
    out.im[g] = im[g];
  }
  return out;
}
}  // namespace sonitude::dsp
