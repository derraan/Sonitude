#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace sonitude::tools::calibration
{
struct ChannelMoments
{
  float mean = 0.0F;
  float rms = 0.0F;
};

struct DelayPolarityEstimate
{
  float delay_samples = 0.0F;
  int polarity = 1;
  float peak_correlation = 0.0F;
};

ChannelMoments ComputeChannelMoments(std::span<const float> samples);

DelayPolarityEstimate EstimateDelayAndPolarity(std::span<const float> reference,
                                               std::span<const float> channel,
                                               int max_lag_samples);

float ParseMinimumCorrelation(const std::string& value);

void RequireMinimumCorrelation(const std::string& channel_id,
                               float measured_correlation,
                               float required_correlation);
}  // namespace sonitude::tools::calibration
