#include "dsp/binaural_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "spatial/angles.hpp"
#include "spatial/head_frame.hpp"

namespace sonitude::dsp
{
namespace
{
constexpr double kSpeedOfSoundMps = 343.0;

void NormalizeWeights(std::array<float, audio::kMicChannels>& weights)
{
  float sum = 0.0F;
  for (const float w : weights)
  {
    sum += w;
  }
  if (sum <= 1.0e-12F)
  {
    return;
  }
  for (float& w : weights)
  {
    w /= sum;
  }
}

void AssignHemisphereSplit(ArrayDownmixWeights& out)
{
  constexpr float kThird = 1.0F / 3.0F;
  out = {};
  for (std::size_t i = 0; i < 3; ++i)
  {
    out.left[i] = kThird;
    out.right[i + 3] = kThird;
  }
}
}  // namespace

ArrayDownmixWeights MakeArrayDownmixWeights(const std::span<const double> mic_x_m)
{
  ArrayDownmixWeights out{};
  const std::size_t n = std::min(mic_x_m.size(), audio::kMicChannels);
  if (n < audio::kMicChannels)
  {
    AssignHemisphereSplit(out);
    return out;
  }

  double min_x = mic_x_m[0];
  double max_x = mic_x_m[0];
  double max_abs = std::fabs(mic_x_m[0]);
  for (std::size_t i = 1; i < n; ++i)
  {
    min_x = std::min(min_x, mic_x_m[i]);
    max_x = std::max(max_x, mic_x_m[i]);
    max_abs = std::max(max_abs, std::fabs(mic_x_m[i]));
  }
  const double span = max_x - min_x;
  if (span < 1.0e-9)
  {
    AssignHemisphereSplit(out);
    return out;
  }
  const double abs_ref = std::max(max_abs, 1.0e-9);

  for (std::size_t i = 0; i < n; ++i)
  {
    const double t = (mic_x_m[i] - min_x) / span;
    const double presence = 0.35 + (0.65 * (std::fabs(mic_x_m[i]) / abs_ref));
    out.left[i] = static_cast<float>((1.0 - t) * presence);
    out.right[i] = static_cast<float>(t * presence);
  }
  NormalizeWeights(out.left);
  NormalizeWeights(out.right);
  return out;
}

namespace
{
float DbToLinear(const float db)
{
  return std::pow(10.0F, db / 20.0F);
}

void ResetEarState(BinauralRenderer::EarState& ear)
{
  ear.delay.reset();
  std::fill(ear.fir_state.begin(), ear.fir_state.end(), 0.0F);
  ear.fir_write = 0;
}

void ResetPathState(BinauralRenderer::PathState& state)
{
  ResetEarState(state.left);
  ResetEarState(state.right);
}

float ProcessEar(const float sample,
                 const BinauralRenderer::EarParams& params,
                 BinauralRenderer::EarState& state)
{
  const float delayed = state.delay.process(sample, params.delay_samples);
  if (params.fir.empty())
  {
    return delayed * params.gain;
  }

  state.fir_state[state.fir_write] = delayed;
  double acc = 0.0;
  for (std::size_t tap = 0; tap < params.fir.size(); ++tap)
  {
    const std::size_t idx =
        (state.fir_write + state.fir_state.size() - tap) % state.fir_state.size();
    acc += static_cast<double>(params.fir[tap]) * static_cast<double>(state.fir_state[idx]);
  }
  state.fir_write = (state.fir_write + 1U) % state.fir_state.size();
  return static_cast<float>(acc * static_cast<double>(params.gain));
}

float ProcessPath(const float sample,
                  const BinauralRenderer::EarParams& left_params,
                  const BinauralRenderer::EarParams& right_params,
                  BinauralRenderer::EarState& left_state,
                  BinauralRenderer::EarState& right_state,
                  const bool left_channel)
{
  if (left_channel)
  {
    return ProcessEar(sample, left_params, left_state);
  }
  return ProcessEar(sample, right_params, right_state);
}
}  // namespace

void BinauralRenderer::configure(const BinauralConfig& config)
{
  if (config.sample_rate_hz == 0)
  {
    throw std::runtime_error("Binaural renderer sample_rate_hz must be non-zero");
  }
  if (config.max_block_frames == 0)
  {
    throw std::runtime_error("Binaural renderer max_block_frames must be non-zero");
  }
  if (config.transition_ms < 0.0F)
  {
    throw std::runtime_error("Binaural renderer transition_ms must be non-negative");
  }
  if (config.itd_ild.head_radius_m <= 0.0)
  {
    throw std::runtime_error("Binaural renderer head_radius_m must be positive");
  }
  if ((config.backend == BinauralBackend::CompactHrtf ||
       config.backend == BinauralBackend::FullHrtfReference) &&
      (config.table == nullptr || config.table->empty()))
  {
    throw std::runtime_error("Requested HRTF backend requires a valid coefficient table");
  }
  if (config.table != nullptr && config.table->sample_rate_hz != config.sample_rate_hz &&
      (config.backend == BinauralBackend::CompactHrtf ||
       config.backend == BinauralBackend::FullHrtfReference))
  {
    throw std::runtime_error("HRTF table sample rate does not match renderer sample rate");
  }

  config_ = config;
  array_weights_ = config.array_downmix;
  float left_sum = 0.0F;
  float right_sum = 0.0F;
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    left_sum += array_weights_.left[i];
    right_sum += array_weights_.right[i];
  }
  if (config.backend == BinauralBackend::ArrayDownmix && (left_sum <= 1.0e-12F || right_sum <= 1.0e-12F))
  {
    AssignHemisphereSplit(array_weights_);
  }
  else if (config.backend == BinauralBackend::ArrayDownmix)
  {
    NormalizeWeights(array_weights_.left);
    NormalizeWeights(array_weights_.right);
  }

  ramp_samples_ = std::max<std::size_t>(
      1U, static_cast<std::size_t>((config.transition_ms * 0.001F) * config.sample_rate_hz));
  fade_cursor_ = 0;
  crossfading_ = false;
  current_direction_ = {};
  pending_direction_ = {};

  double max_abs_delay = 0.0;
  if (config.backend == BinauralBackend::ItdIld)
  {
    const double max_tau =
        (config.itd_ild.head_radius_m / kSpeedOfSoundMps) * ((spatial::kPi * 0.5) + 1.0);
    max_abs_delay = max_tau * static_cast<double>(config.sample_rate_hz);
  }
  else if (config.backend == BinauralBackend::CompactHrtf ||
           config.backend == BinauralBackend::FullHrtfReference)
  {
    for (const auto& direction : config.table->directions)
    {
      max_abs_delay =
          std::max(max_abs_delay, std::fabs(static_cast<double>(direction.delay_left_samples)));
      max_abs_delay =
          std::max(max_abs_delay, std::fabs(static_cast<double>(direction.delay_right_samples)));
    }
  }
  base_delay_samples_ = max_abs_delay + 2.0;
  const double max_delay = base_delay_samples_ + max_abs_delay + 2.0;

  current_state_.left.delay.configure(max_delay);
  current_state_.right.delay.configure(max_delay);
  pending_state_.left.delay.configure(max_delay);
  pending_state_.right.delay.configure(max_delay);

  auto make_path = [&](PathParams& params, PathState& state) {
    params.left.fir.clear();
    params.right.fir.clear();
    if (config.backend == BinauralBackend::CompactHrtf ||
        config.backend == BinauralBackend::FullHrtfReference)
    {
      params.left.fir.resize(config.table->taps_per_ear, 0.0F);
      params.right.fir.resize(config.table->taps_per_ear, 0.0F);
    }
    state.left.fir_state.assign(std::max<std::size_t>(1U, params.left.fir.size()), 0.0F);
    state.right.fir_state.assign(std::max<std::size_t>(1U, params.right.fir.size()), 0.0F);
  };
  make_path(current_params_, current_state_);
  make_path(pending_params_, pending_state_);

  configured_ = true;
  direction_applied_ = false;
  setDirection({0.0F, 0.0F});
  current_params_ = pending_params_;
  ResetPathState(current_state_);
  pending_params_ = current_params_;
  ResetPathState(pending_state_);
  crossfading_ = false;
}

void BinauralRenderer::reset()
{
  if (!configured_)
  {
    throw std::runtime_error("Binaural renderer used before configure");
  }
  current_direction_ = {};
  pending_direction_ = {};
  direction_applied_ = false;
  fade_cursor_ = 0;
  crossfading_ = false;
  ResetPathState(current_state_);
  ResetPathState(pending_state_);
  setDirection({0.0F, 0.0F});
  current_params_ = pending_params_;
  pending_params_ = current_params_;
}

void BinauralRenderer::setDirection(audio::BeamformerSteering direction)
{
  if (!configured_)
  {
    throw std::runtime_error("Binaural renderer used before configure");
  }
  if (config_.backend == BinauralBackend::ArrayDownmix)
  {
    return;
  }

  direction.azimuth_deg = static_cast<float>(spatial::NormalizeHeadAzimuthDeg(direction.azimuth_deg));
  if (direction_applied_ &&
      spatial::SteeringApproximatelyEqual(direction.azimuth_deg,
                                          direction.elevation_deg,
                                          pending_direction_.azimuth_deg,
                                          pending_direction_.elevation_deg))
  {
    return;
  }
  direction_applied_ = true;
  pending_direction_ = direction;
  pending_params_.left.gain = 1.0F;
  pending_params_.right.gain = 1.0F;
  pending_params_.left.delay_samples = base_delay_samples_;
  pending_params_.right.delay_samples = base_delay_samples_;

  if (config_.backend == BinauralBackend::MonoReference)
  {
    pending_params_.left.fir.clear();
    pending_params_.right.fir.clear();
  }
  else if (config_.backend == BinauralBackend::ItdIld)
  {
    const double lateral_deg =
        spatial::LateralAngleDeg(direction.azimuth_deg, direction.elevation_deg);
    const double lambda = lateral_deg * (spatial::kPi / 180.0);
    const double tau_sec = (config_.itd_ild.head_radius_m / kSpeedOfSoundMps) * (lambda + std::sin(lambda));
    const double tau_samples = tau_sec * static_cast<double>(config_.sample_rate_hz);

    pending_params_.left.delay_samples = base_delay_samples_ + std::max(0.0, tau_samples);
    pending_params_.right.delay_samples = base_delay_samples_ + std::max(0.0, -tau_samples);

    const float ild_db = static_cast<float>(
        std::clamp((lateral_deg / 90.0) * static_cast<double>(config_.itd_ild.max_ild_db),
                   -static_cast<double>(config_.itd_ild.max_ild_db),
                   static_cast<double>(config_.itd_ild.max_ild_db)));
    pending_params_.left.gain = DbToLinear(-0.5F * ild_db);
    pending_params_.right.gain = DbToLinear(0.5F * ild_db);
  }
  else
  {
    std::size_t best = 0;
    double best_error = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < config_.table->directions.size(); ++i)
    {
      const auto& d = config_.table->directions[i];
      const double da = spatial::AngularDistanceAbsDeg(d.azimuth_deg, direction.azimuth_deg);
      const double de = std::fabs(static_cast<double>(d.elevation_deg - direction.elevation_deg));
      const double err = da + de;
      if (err < best_error)
      {
        best_error = err;
        best = i;
      }
    }
    const auto& d = config_.table->directions[best];
    pending_params_.left.delay_samples = base_delay_samples_ + d.delay_left_samples;
    pending_params_.right.delay_samples = base_delay_samples_ + d.delay_right_samples;
    const std::size_t taps = config_.table->taps_per_ear;
    const std::size_t left_offset = best * 2U * taps;
    const std::size_t right_offset = left_offset + taps;
    std::copy(config_.table->fir.begin() + static_cast<std::ptrdiff_t>(left_offset),
              config_.table->fir.begin() + static_cast<std::ptrdiff_t>(left_offset + taps),
              pending_params_.left.fir.begin());
    std::copy(config_.table->fir.begin() + static_cast<std::ptrdiff_t>(right_offset),
              config_.table->fir.begin() + static_cast<std::ptrdiff_t>(right_offset + taps),
              pending_params_.right.fir.begin());
  }

  fade_cursor_ = 0;
  crossfading_ = true;
}

void BinauralRenderer::process(const std::span<const float> mono,
                               const std::span<float> left,
                               const std::span<float> right)
{
  if (!configured_)
  {
    throw std::runtime_error("Binaural renderer used before configure");
  }
  if (left.size() < mono.size() || right.size() < mono.size())
  {
    throw std::runtime_error("Binaural renderer output spans are too small");
  }

  for (std::size_t i = 0; i < mono.size(); ++i)
  {
    if (config_.backend == BinauralBackend::MonoReference ||
        config_.backend == BinauralBackend::ArrayDownmix)
    {
      left[i] = mono[i];
      right[i] = mono[i];
      continue;
    }

    const float left_current = ProcessPath(mono[i],
                                           current_params_.left,
                                           current_params_.right,
                                           current_state_.left,
                                           current_state_.right,
                                           true);
    const float right_current = ProcessPath(mono[i],
                                            current_params_.left,
                                            current_params_.right,
                                            current_state_.left,
                                            current_state_.right,
                                            false);
    if (!crossfading_)
    {
      left[i] = left_current;
      right[i] = right_current;
      continue;
    }

    const float left_pending = ProcessPath(mono[i],
                                           pending_params_.left,
                                           pending_params_.right,
                                           pending_state_.left,
                                           pending_state_.right,
                                           true);
    const float right_pending = ProcessPath(mono[i],
                                            pending_params_.left,
                                            pending_params_.right,
                                            pending_state_.left,
                                            pending_state_.right,
                                            false);
    const float alpha =
        static_cast<float>(fade_cursor_) / static_cast<float>(std::max<std::size_t>(1U, ramp_samples_));
    left[i] = ((1.0F - alpha) * left_current) + (alpha * left_pending);
    right[i] = ((1.0F - alpha) * right_current) + (alpha * right_pending);

    ++fade_cursor_;
    if (fade_cursor_ >= ramp_samples_)
    {
      crossfading_ = false;
      fade_cursor_ = 0;
      current_direction_ = pending_direction_;
      current_params_ = pending_params_;
      std::swap(current_state_, pending_state_);
      ResetPathState(pending_state_);
    }
  }
}

void BinauralRenderer::processArray(const std::span<const audio::MicFrame> frames,
                                    const std::span<float> left,
                                    const std::span<float> right)
{
  if (!configured_)
  {
    throw std::runtime_error("Binaural renderer used before configure");
  }
  if (left.size() < frames.size() || right.size() < frames.size())
  {
    throw std::runtime_error("Binaural renderer output spans are too small");
  }

  for (std::size_t i = 0; i < frames.size(); ++i)
  {
    float l = 0.0F;
    float r = 0.0F;
    for (std::size_t ch = 0; ch < audio::kMicChannels; ++ch)
    {
      l += array_weights_.left[ch] * frames[i][ch];
      r += array_weights_.right[ch] * frames[i][ch];
    }
    left[i] = l;
    right[i] = r;
  }
}

std::size_t BinauralRenderer::stateBytes() const
{
  std::size_t bytes = 0;
  bytes += current_state_.left.delay.stateBytes();
  bytes += current_state_.right.delay.stateBytes();
  bytes += pending_state_.left.delay.stateBytes();
  bytes += pending_state_.right.delay.stateBytes();
  bytes += current_state_.left.fir_state.size() * sizeof(float);
  bytes += current_state_.right.fir_state.size() * sizeof(float);
  bytes += pending_state_.left.fir_state.size() * sizeof(float);
  bytes += pending_state_.right.fir_state.size() * sizeof(float);
  bytes += current_params_.left.fir.size() * sizeof(float);
  bytes += current_params_.right.fir.size() * sizeof(float);
  bytes += pending_params_.left.fir.size() * sizeof(float);
  bytes += pending_params_.right.fir.size() * sizeof(float);
  return bytes;
}

std::size_t BinauralRenderer::coefficientBytes() const
{
  if (config_.table == nullptr)
  {
    return 0;
  }
  return config_.table->fir.size() * sizeof(float);
}

std::size_t BinauralRenderer::algorithmicLatencySamples() const
{
  if (config_.backend == BinauralBackend::MonoReference ||
      config_.backend == BinauralBackend::ArrayDownmix)
  {
    return 0;
  }

  auto fir_onset = [](const std::vector<float>& fir) -> std::size_t {
    for (std::size_t tap = 0; tap < fir.size(); ++tap)
    {
      if (std::fabs(fir[tap]) > 1.0e-6F)
      {
        return tap;
      }
    }
    return 0;
  };

  const double min_delay =
      std::min(current_params_.left.delay_samples, current_params_.right.delay_samples);
  std::size_t onset = 0;
  if (!current_params_.left.fir.empty() || !current_params_.right.fir.empty())
  {
    onset = std::min(fir_onset(current_params_.left.fir), fir_onset(current_params_.right.fir));
  }
  const std::size_t delay_floor =
      static_cast<std::size_t>(std::floor(std::max(0.0, min_delay)));
  return delay_floor + FractionalDelayLine::kFirstArrivalTapOffset + onset;
}
}  // namespace sonitude::dsp
