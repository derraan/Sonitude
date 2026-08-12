#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "spatial/angles.hpp"

namespace sonitude::tests::support
{
inline float DelayReadLinear(const std::vector<float>& signal, const std::size_t index, const double delay)
{
  const double pos = static_cast<double>(index) - delay;
  const std::ptrdiff_t i0 = static_cast<std::ptrdiff_t>(std::floor(pos));
  const std::ptrdiff_t i1 = i0 + 1;
  const double frac = pos - static_cast<double>(i0);
  const auto sample_at = [&](const std::ptrdiff_t i) -> float {
    if (i < 0 || static_cast<std::size_t>(i) >= signal.size())
    {
      return 0.0F;
    }
    return signal[static_cast<std::size_t>(i)];
  };
  const float s0 = sample_at(i0);
  const float s1 = sample_at(i1);
  return static_cast<float>((1.0 - frac) * static_cast<double>(s0) + frac * static_cast<double>(s1));
}

inline std::vector<float> GenerateSine(const std::size_t frames,
                                       const std::uint32_t sample_rate_hz,
                                       const double freq_hz)
{
  std::vector<float> out(frames, 0.0F);
  const double w = 2.0 * spatial::kPi * freq_hz / static_cast<double>(sample_rate_hz);
  for (std::size_t i = 0; i < frames; ++i)
  {
    out[i] = static_cast<float>(std::sin(w * static_cast<double>(i)));
  }
  return out;
}

inline std::vector<audio::MicFrame> GeneratePlaneWave(const std::vector<float>& mono_source,
                                                      const app::GeometryConfig& geometry,
                                                      const std::uint32_t sample_rate_hz,
                                                      const std::size_t reference_mic_index,
                                                      const float azimuth_deg,
                                                      const float elevation_deg,
                                                      const float speed_of_sound_mps)
{
  const auto u = spatial::UnitVectorFromAzElDeg(azimuth_deg, elevation_deg);
  const auto& ref = geometry.microphones[reference_mic_index];
  const double ref_dot = (u[0] * ref.x) + (u[1] * ref.y) + (u[2] * ref.z);

  std::array<double, audio::kMicChannels> delays{};
  for (std::size_t m = 0; m < audio::kMicChannels; ++m)
  {
    const auto& mic = geometry.microphones[m];
    const double dot = (u[0] * mic.x) + (u[1] * mic.y) + (u[2] * mic.z);
    const double tau_sec = -((dot - ref_dot) / static_cast<double>(speed_of_sound_mps));
    delays[m] = tau_sec * static_cast<double>(sample_rate_hz);
  }

  std::vector<audio::MicFrame> out(mono_source.size());
  for (std::size_t i = 0; i < mono_source.size(); ++i)
  {
    audio::MicFrame frame{};
    for (std::size_t m = 0; m < audio::kMicChannels; ++m)
    {
      frame[m] = DelayReadLinear(mono_source, i, -delays[m]);
    }
    out[i] = frame;
  }
  return out;
}

inline double ComputeRms(const std::vector<float>& signal, const std::size_t skip = 0)
{
  if (signal.empty() || skip >= signal.size())
  {
    return 0.0;
  }
  double sum = 0.0;
  for (std::size_t i = skip; i < signal.size(); ++i)
  {
    const double v = signal[i];
    sum += v * v;
  }
  return std::sqrt(sum / static_cast<double>(signal.size() - skip));
}

inline double MaxSecondDifference(const std::vector<float>& signal)
{
  if (signal.size() < 3)
  {
    return 0.0;
  }
  double max_jump = 0.0;
  for (std::size_t i = 2; i < signal.size(); ++i)
  {
    const double d0 = static_cast<double>(signal[i - 1]) - static_cast<double>(signal[i - 2]);
    const double d1 = static_cast<double>(signal[i]) - static_cast<double>(signal[i - 1]);
    max_jump = std::max(max_jump, std::fabs(d1 - d0));
  }
  return max_jump;
}
}  // namespace sonitude::tests::support
