#include "dsp/multi_reference_suppressor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::dsp
{
namespace
{
constexpr float kNormFloor = 1.0e-8F;

bool InUnitInterval(const float value) noexcept
{
  return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}
} // namespace

void MultiReferenceSuppressor::configure(const MultiReferenceSuppressorConfig& config)
{
  if (config.taps == 0U || config.taps > kMaxMultiReferenceTaps)
  {
    throw std::runtime_error("multi-reference suppressor taps must be in [1, 32]");
  }
  if (!InUnitInterval(config.step_size) || config.step_size == 0.0F ||
      !InUnitInterval(config.leakage) || !std::isfinite(config.coefficient_limit) ||
      config.coefficient_limit <= 0.0F || !std::isfinite(config.max_attenuation_db) ||
      config.max_attenuation_db < 0.0F || config.max_attenuation_db > 24.0F)
  {
    throw std::runtime_error("invalid multi-reference suppressor configuration");
  }

  config_ = config;
  output_floor_gain_ = std::pow(10.0F, -config_.max_attenuation_db / 20.0F);
  references_ = {};
  configured_ = true;
}

float MultiReferenceSuppressor::historyAt(const AdaptiveReference& reference,
                                          const std::size_t tap) const noexcept
{
  const std::size_t index =
      (reference.cursor + kMaxMultiReferenceTaps - tap) % kMaxMultiReferenceTaps;
  return reference.history[index];
}

float MultiReferenceSuppressor::pushAndPredict(AdaptiveReference& reference,
                                               const float sample) noexcept
{
  reference.cursor = (reference.cursor + 1U) % kMaxMultiReferenceTaps;
  reference.history[reference.cursor] = sample;
  float predicted = 0.0F;
  for (std::size_t tap = 0; tap < config_.taps; ++tap)
  {
    predicted += reference.weights[tap] * historyAt(reference, tap);
  }
  return predicted;
}

void MultiReferenceSuppressor::update(AdaptiveReference& reference, const float error,
                                      const float norm) noexcept
{
  const float normalized_step = config_.step_size / std::max(norm, kNormFloor);
  for (std::size_t tap = 0; tap < config_.taps; ++tap)
  {
    const float updated = ((1.0F - config_.leakage) * reference.weights[tap]) +
                          (normalized_step * error * historyAt(reference, tap));
    reference.weights[tap] =
        std::clamp(updated, -config_.coefficient_limit, config_.coefficient_limit);
  }
}

void MultiReferenceSuppressor::decay(AdaptiveReference& reference) noexcept
{
  for (std::size_t tap = 0; tap < config_.taps; ++tap)
  {
    reference.weights[tap] *= (1.0F - config_.leakage);
  }
}

bool MultiReferenceSuppressor::process(const std::span<float> target_reference,
                                       const std::span<const float> own_reference,
                                       const std::span<const float> distractor_reference,
                                       const MultiReferenceControl& control) noexcept
{
  if (!configured_ || target_reference.size() != own_reference.size() ||
      target_reference.size() != distractor_reference.size())
  {
    return false;
  }
  for (const float mix : control.cancellation_mix)
  {
    if (!InUnitInterval(mix))
    {
      return false;
    }
  }

  // Validate before mutating either the target span or adaptive state. A bad
  // period therefore fails atomically from the caller's point of view.
  for (std::size_t frame = 0; frame < target_reference.size(); ++frame)
  {
    if (!std::isfinite(target_reference[frame]) || !std::isfinite(own_reference[frame]) ||
        !std::isfinite(distractor_reference[frame]))
    {
      return false;
    }
  }

  for (std::size_t frame = 0; frame < target_reference.size(); ++frame)
  {
    const float primary = target_reference[frame];
    const std::array<float, kMaxInterferenceReferences> input{own_reference[frame],
                                                              distractor_reference[frame]};
    std::array<float, kMaxInterferenceReferences> predicted{};
    std::array<float, kMaxInterferenceReferences> norm{kNormFloor, kNormFloor};
    for (std::size_t reference_index = 0; reference_index < references_.size(); ++reference_index)
    {
      AdaptiveReference& reference = references_[reference_index];
      predicted[reference_index] = pushAndPredict(reference, input[reference_index]);
      for (std::size_t tap = 0; tap < config_.taps; ++tap)
      {
        const float history = historyAt(reference, tap);
        norm[reference_index] += history * history;
      }
      if (control.state[reference_index] == AdaptationState::Bypass)
      {
        predicted[reference_index] = 0.0F;
      }
    }

    float estimated_interference = 0.0F;
    for (std::size_t reference_index = 0; reference_index < references_.size(); ++reference_index)
    {
      estimated_interference +=
          control.cancellation_mix[reference_index] * predicted[reference_index];
    }

    // Bound per-sample removal. This is a final safety net, not evidence that
    // target distortion is acceptable; hardware replay metrics remain required.
    const float max_removal = (1.0F - output_floor_gain_) * std::fabs(primary);
    estimated_interference = std::clamp(estimated_interference, -max_removal, max_removal);
    const float error = primary - estimated_interference;

    for (std::size_t reference_index = 0; reference_index < references_.size(); ++reference_index)
    {
      switch (control.state[reference_index])
      {
      case AdaptationState::Adapt:
        update(references_[reference_index], error, norm[reference_index]);
        break;
      case AdaptationState::Decay:
      case AdaptationState::Bypass:
        decay(references_[reference_index]);
        break;
      case AdaptationState::Hold:
        break;
      }
    }
    target_reference[frame] = error;
  }
  return true;
}

std::array<float, kMaxInterferenceReferences>
MultiReferenceSuppressor::coefficientEnergy() const noexcept
{
  std::array<float, kMaxInterferenceReferences> energy{};
  for (std::size_t reference_index = 0; reference_index < references_.size(); ++reference_index)
  {
    for (std::size_t tap = 0; tap < config_.taps; ++tap)
    {
      energy[reference_index] +=
          references_[reference_index].weights[tap] * references_[reference_index].weights[tap];
    }
  }
  return energy;
}
} // namespace sonitude::dsp
