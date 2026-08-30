#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace sonitude::dsp
{
constexpr std::size_t kGuardLooks = 3;
constexpr std::array<float, kGuardLooks> kGuardAzimuthOffsetDeg = {90.0F, -90.0F, 180.0F};

// Legacy time-domain guard views. These remain temporarily for non-runtime
// tests while the shared spectral path replaces PCM guard handoff.
using GuardLookSpans = std::array<std::span<float>, kGuardLooks>;
using GuardLookConstSpans = std::array<std::span<const float>, kGuardLooks>;

struct GuardSpectrumConstSpans
{
  std::array<std::span<const float>, kGuardLooks> re{};
  std::array<std::span<const float>, kGuardLooks> im{};
};

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
