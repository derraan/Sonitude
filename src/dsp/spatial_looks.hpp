#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace sonitude::dsp
{
// Internal steered looks used only as spectral references. Not mixed
// into the audible target-beam output.
constexpr std::size_t kGuardLooks = 3;
constexpr std::array<float, kGuardLooks> kGuardAzimuthOffsetDeg = {90.0F, -90.0F, 180.0F};

using GuardLookSpans = std::array<std::span<float>, kGuardLooks>;
using GuardLookConstSpans = std::array<std::span<const float>, kGuardLooks>;

inline GuardLookSpans BindGuardLooks(std::array<std::vector<float>, kGuardLooks>& buffers,
                                     const std::size_t frames)
{
  GuardLookSpans out{};
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    if (buffers[g].size() < frames)
    {
      buffers[g].assign(frames, 0.0F);
    }
    out[g] = std::span<float>(buffers[g].data(), frames);
  }
  return out;
}

inline GuardLookConstSpans ConstGuardLooks(const GuardLookSpans& looks)
{
  GuardLookConstSpans out{};
  for (std::size_t g = 0; g < kGuardLooks; ++g)
  {
    out[g] = looks[g];
  }
  return out;
}
}  // namespace sonitude::dsp
