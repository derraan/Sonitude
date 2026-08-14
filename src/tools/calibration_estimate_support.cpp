#include "tools/calibration_estimate_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sonitude::tools::calibration
{
namespace
{
constexpr double kMinEnergy = 1e-12;
constexpr float kMinRms = 1e-6F;

std::vector<float> Demean(std::span<const float> input, const float mean)
{
  std::vector<float> out(input.size(), 0.0F);
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    out[i] = input[i] - mean;
  }
  return out;
}

double NormalizedCorrelationAtLag(const std::vector<float>& reference,
                                  const std::vector<float>& channel,
                                  const int lag)
{
  const std::int64_t n = static_cast<std::int64_t>(reference.size());
  const std::int64_t i_start = std::max<std::int64_t>(0, -static_cast<std::int64_t>(lag));
  const std::int64_t i_end = std::min<std::int64_t>(n, n - static_cast<std::int64_t>(lag));
  if (i_start >= i_end)
  {
    return 0.0;
  }

  double dot = 0.0;
  double energy_ref = 0.0;
  double energy_ch = 0.0;
  for (std::int64_t i = i_start; i < i_end; ++i)
  {
    const float a = reference[static_cast<std::size_t>(i)];
    const float b = channel[static_cast<std::size_t>(i + static_cast<std::int64_t>(lag))];
    dot += static_cast<double>(a) * static_cast<double>(b);
    energy_ref += static_cast<double>(a) * static_cast<double>(a);
    energy_ch += static_cast<double>(b) * static_cast<double>(b);
  }

  const double denom = std::sqrt(energy_ref * energy_ch);
  if (!(denom > kMinEnergy))
  {
    return 0.0;
  }
  return dot / denom;
}
}  // namespace

ChannelMoments ComputeChannelMoments(const std::span<const float> samples)
{
  if (samples.empty())
  {
    throw std::runtime_error("calibration_estimate requires non-empty channel samples");
  }

  double sum = 0.0;
  for (const float x : samples)
  {
    if (!std::isfinite(x))
    {
      throw std::runtime_error("calibration_estimate received non-finite channel sample");
    }
    sum += x;
  }
  const float mean = static_cast<float>(sum / static_cast<double>(samples.size()));

  double squared_sum = 0.0;
  for (const float x : samples)
  {
    const double centered = static_cast<double>(x) - static_cast<double>(mean);
    squared_sum += centered * centered;
  }

  const float rms = static_cast<float>(std::sqrt(squared_sum / static_cast<double>(samples.size())));
  if (!(rms >= kMinRms))
  {
    throw std::runtime_error("calibration_estimate channel energy is too low for stable estimation");
  }
  return {.mean = mean, .rms = rms};
}

DelayPolarityEstimate EstimateDelayAndPolarity(const std::span<const float> reference,
                                               const std::span<const float> channel,
                                               const int max_lag_samples)
{
  if (reference.empty() || channel.empty() || reference.size() != channel.size())
  {
    throw std::runtime_error("cross-correlation requires non-empty equal-length channels");
  }
  if (max_lag_samples <= 0)
  {
    throw std::runtime_error("cross-correlation requires max_lag_samples > 0");
  }

  const int max_usable_lag = std::min<int>(max_lag_samples, static_cast<int>(reference.size()) - 1);
  if (max_usable_lag <= 0)
  {
    throw std::runtime_error("cross-correlation requires at least two samples");
  }

  const ChannelMoments ref_moments = ComputeChannelMoments(reference);
  const ChannelMoments ch_moments = ComputeChannelMoments(channel);
  const std::vector<float> ref_zero_mean = Demean(reference, ref_moments.mean);
  const std::vector<float> ch_zero_mean = Demean(channel, ch_moments.mean);

  std::vector<double> corr(static_cast<std::size_t>((2 * max_usable_lag) + 1), 0.0);
  int best_lag = 0;
  double best_value = 0.0;
  double best_abs = -1.0;
  for (int lag = -max_usable_lag; lag <= max_usable_lag; ++lag)
  {
    const double value = NormalizedCorrelationAtLag(ref_zero_mean, ch_zero_mean, lag);
    corr[static_cast<std::size_t>(lag + max_usable_lag)] = value;
    const double magnitude = std::fabs(value);
    if (magnitude > best_abs)
    {
      best_abs = magnitude;
      best_value = value;
      best_lag = lag;
    }
  }

  if (!(best_abs > 0.0))
  {
    throw std::runtime_error("cross-correlation failed to produce a stable delay estimate");
  }

  double fractional_offset = 0.0;
  if (best_lag > -max_usable_lag && best_lag < max_usable_lag)
  {
    const double c_prev = corr[static_cast<std::size_t>((best_lag - 1) + max_usable_lag)];
    const double c_peak = corr[static_cast<std::size_t>(best_lag + max_usable_lag)];
    const double c_next = corr[static_cast<std::size_t>((best_lag + 1) + max_usable_lag)];
    const double denom = c_prev - (2.0 * c_peak) + c_next;
    if (std::fabs(denom) > std::numeric_limits<double>::epsilon())
    {
      fractional_offset = 0.5 * (c_prev - c_next) / denom;
      fractional_offset = std::clamp(fractional_offset, -1.0, 1.0);
    }
  }

  DelayPolarityEstimate estimate;
  estimate.delay_samples = static_cast<float>(-static_cast<double>(best_lag) - fractional_offset);
  estimate.polarity = (best_value >= 0.0) ? 1 : -1;
  estimate.peak_correlation = static_cast<float>(best_abs);
  return estimate;
}
}  // namespace sonitude::tools::calibration
