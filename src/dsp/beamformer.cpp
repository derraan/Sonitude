#include "dsp/beamformer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "spatial/angles.hpp"

namespace sonitude::dsp
{
namespace
{
constexpr double kFftGuardDelaySamples = 4.0;
}

void DelaySumBeamformer::configure(const app::GeometryConfig& geometry,
                                   const app::SteeringConfig& steering_config,
                                   const app::CalibrationConfig& calibration,
                                   const std::uint32_t sample_rate_hz,
                                   const std::size_t /*max_block_frames*/)
{
  if (geometry.microphones.size() != audio::kMicChannels)
  {
    throw std::runtime_error("geometry must contain exactly six microphones");
  }
  if (calibration.channels.size() != audio::kMicChannels)
  {
    throw std::runtime_error("calibration must contain exactly six channels");
  }
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("sample_rate_hz must be non-zero");
  }

  steering_config_ = steering_config;
  sample_rate_hz_ = sample_rate_hz;
  ramp_samples_ = std::max<std::size_t>(
      1U, static_cast<std::size_t>((steering_config_.steering_ramp_ms * 0.001F) *
                                   static_cast<float>(sample_rate_hz_)));

  const std::size_t ref = steering_config_.reference_mic_index;
  if (ref >= audio::kMicChannels)
  {
    throw std::runtime_error("reference_mic_index out of range");
  }

  std::unordered_map<std::string, float> cal_by_id;
  cal_by_id.reserve(calibration.channels.size());
  max_calibration_abs_delay_ = 0.0;
  for (const app::CalibrationChannel& channel : calibration.channels)
  {
    cal_by_id[channel.id] = channel.delay_samples;
    max_calibration_abs_delay_ =
        std::max(max_calibration_abs_delay_, std::fabs(static_cast<double>(channel.delay_samples)));
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const auto& mic = geometry.microphones[i];
    mic_positions_[i] = {mic.x, mic.y, mic.z};
    const auto it = cal_by_id.find(mic.id);
    if (it == cal_by_id.end())
    {
      throw std::runtime_error("geometry/calibration id mismatch");
    }
    calibration_delays_[i] = static_cast<double>(it->second);
  }

  max_aperture_delay_samples_ = 0.0;
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const double dx = mic_positions_[i][0] - mic_positions_[ref][0];
    const double dy = mic_positions_[i][1] - mic_positions_[ref][1];
    const double dz = mic_positions_[i][2] - mic_positions_[ref][2];
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double delay_samples =
        (distance / static_cast<double>(steering_config_.speed_of_sound_mps)) *
        static_cast<double>(sample_rate_hz_);
    max_aperture_delay_samples_ = std::max(max_aperture_delay_samples_, delay_samples);
  }

  // Keep all effective delays positive across steering + calibration.
  base_delay_samples_ =
      max_aperture_delay_samples_ + max_calibration_abs_delay_ + kFftGuardDelaySamples;
  const double max_configured_delay =
      base_delay_samples_ + max_aperture_delay_samples_ + max_calibration_abs_delay_;

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    current_lines_[i].configure(max_configured_delay + 2.0);
    current_lines_[i].reset();
    pending_lines_[i].configure(max_configured_delay + 2.0);
    pending_lines_[i].reset();
    reference_lines_[i].configure(max_configured_delay + 2.0);
    reference_lines_[i].reset();
  }

  current_target_ = {0.0F, 0.0F};
  current_delays_ = computeDelaysForTarget(current_target_);
  pending_delays_ = current_delays_;
  crossfading_ = false;
  fade_cursor_ = 0;
  configured_ = true;
}

DelaySumBeamformer::DelayArray
DelaySumBeamformer::computeDelaysForTarget(const audio::BeamformerSteering target) const
{
  const auto u = spatial::UnitVectorFromAzElDeg(target.azimuth_deg, target.elevation_deg);
  const std::size_t ref = steering_config_.reference_mic_index;
  const double ref_dot = (u[0] * mic_positions_[ref][0]) + (u[1] * mic_positions_[ref][1]) +
                         (u[2] * mic_positions_[ref][2]);

  DelayArray out{};
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const double dot = (u[0] * mic_positions_[i][0]) + (u[1] * mic_positions_[i][1]) +
                       (u[2] * mic_positions_[i][2]);
    const double tau_sec =
        -((dot - ref_dot) / static_cast<double>(steering_config_.speed_of_sound_mps));
    const double steering_delay = tau_sec * static_cast<double>(sample_rate_hz_);
    out[i] = base_delay_samples_ + calibration_delays_[i] + steering_delay;
  }
  return out;
}

void DelaySumBeamformer::setTarget(const audio::BeamformerSteering target)
{
  if (!configured_)
  {
    throw std::runtime_error("beamformer used before configure");
  }
  pending_delays_ = computeDelaysForTarget(target);
  current_target_ = target;
  crossfading_ = true;
  fade_cursor_ = 0;
}

float DelaySumBeamformer::renderOne(
    const audio::MicFrame& frame, const DelayArray& delays,
    std::array<FractionalDelayLine, audio::kMicChannels>& lines) const
{
  float sum = 0.0F;
  constexpr float kWeight = 1.0F / static_cast<float>(audio::kMicChannels);
  for (std::size_t ch = 0; ch < audio::kMicChannels; ++ch)
  {
    sum += kWeight * lines[ch].process(frame[ch], delays[ch]);
  }
  return sum;
}

void DelaySumBeamformer::process(const std::span<const audio::MicFrame> input,
                                 const std::span<float> mono_out)
{
  if (!configured_)
  {
    throw std::runtime_error("beamformer used before configure");
  }
  if (mono_out.size() < input.size())
  {
    throw std::runtime_error("mono_out span too small for input");
  }

  for (std::size_t i = 0; i < input.size(); ++i)
  {
    const float current_y = renderOne(input[i], current_delays_, current_lines_);
    if (!crossfading_)
    {
      mono_out[i] = current_y;
      continue;
    }

    const float pending_y = renderOne(input[i], pending_delays_, pending_lines_);
    const float alpha = static_cast<float>(fade_cursor_) /
                        static_cast<float>(std::max<std::size_t>(1U, ramp_samples_));
    mono_out[i] = ((1.0F - alpha) * current_y) + (alpha * pending_y);

    ++fade_cursor_;
    if (fade_cursor_ >= ramp_samples_)
    {
      crossfading_ = false;
      fade_cursor_ = 0;
      current_delays_ = pending_delays_;
      std::swap(current_lines_, pending_lines_);
    }
  }
}

void DelaySumBeamformer::processWithReference(const std::span<const audio::MicFrame> input,
                                              const std::span<float> focus_out,
                                              const audio::BeamformerSteering distractor_target,
                                              const std::span<float> reference_out)
{
  if (reference_out.size() < input.size())
  {
    throw std::runtime_error("reference_out span too small for input");
  }
  process(input, focus_out);
  const DelayArray distractor_delays = computeDelaysForTarget(distractor_target);
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    reference_out[i] = renderOne(input[i], distractor_delays, reference_lines_);
  }
}
} // namespace sonitude::dsp
