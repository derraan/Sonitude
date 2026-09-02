#include "ovd/own_voice_detector.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::ovd
{
namespace
{
bool Elapsed(const std::uint64_t now_ns, const std::uint64_t start_ns,
             const std::uint64_t duration_ns) noexcept
{
  return now_ns >= start_ns && (now_ns - start_ns) >= duration_ns;
}
} // namespace

void OwnVoiceDetector::configure(const OwnVoiceDetectorConfig& config,
                                 const StatisticalOvtfModel& model)
{
  config_ = config;
  model_ = model;
  state_ = {};
  last_sequence_ = 0;
  last_observed_ns_ = 0;
  has_observation_ = false;
  activation_candidate_ns_ = 0;
  release_candidate_ns_ = 0;
  activation_candidate_valid_ = false;
  release_candidate_valid_ = false;

  if (!config_.enabled)
  {
    state_.health = OwnVoiceHealth::Disabled;
    configured_ = true;
    return;
  }
  if (!model_.calibrated)
  {
    throw std::runtime_error("enabled OVD requires a calibrated statistical OVTF model");
  }
  if (!std::isfinite(config_.activation_probability) ||
      !std::isfinite(config_.release_probability) || config_.activation_probability <= 0.0F ||
      config_.activation_probability > 1.0F || config_.release_probability < 0.0F ||
      config_.release_probability >= config_.activation_probability)
  {
    throw std::runtime_error("OVD probabilities must satisfy 0 <= release < activation <= 1");
  }
  if (config_.activation_hold_ns == 0U || config_.release_hold_ns == 0U ||
      config_.stale_timeout_ns == 0U)
  {
    throw std::runtime_error("enabled OVD hold and stale windows must be non-zero");
  }

  float weight_sum = 0.0F;
  for (std::size_t feature = 0; feature < kOwnVoiceFeatureCount; ++feature)
  {
    if (!std::isfinite(model_.feature_mean[feature]) ||
        !std::isfinite(model_.feature_weight[feature]) || model_.feature_weight[feature] < 0.0F)
    {
      throw std::runtime_error(
          "OVD model statistics must have finite means and non-negative weights");
    }

    if (model_.feature_weight[feature] == 0.0F)
    {
      continue;
    }

    if (!std::isfinite(model_.feature_inverse_variance[feature]) ||
        model_.feature_inverse_variance[feature] <= 0.0F)
    {
      throw std::runtime_error("enabled OVD features must have finite positive inverse variance");
    }

    weight_sum += model_.feature_weight[feature];
  }
  if (!(weight_sum > 0.0F) || !std::isfinite(weight_sum))
  {
    throw std::runtime_error("OVD model must enable at least one finite feature weight");
  }
  state_.health = OwnVoiceHealth::ModelUnavailable;
  configured_ = true;
}

float OwnVoiceDetector::probability(const OwnVoiceObservation& observation) const noexcept
{
  double weighted_distance = 0.0;
  double weight_sum = 0.0;
  for (std::size_t feature = 0; feature < kOwnVoiceFeatureCount; ++feature)
  {
    const double weight = static_cast<double>(model_.feature_weight[feature]);
    if (weight <= 0.0)
    {
      continue;
    }
    const double inverse_variance = static_cast<double>(model_.feature_inverse_variance[feature]);
    if (!(inverse_variance > 0.0) || !std::isfinite(inverse_variance))
    {
      return 0.0F;
    }
    const double delta = static_cast<double>(observation.features[feature]) -
                         static_cast<double>(model_.feature_mean[feature]);
    weighted_distance += weight * delta * delta * inverse_variance;
    weight_sum += weight;
  }
  if (!(weight_sum > 0.0) || !std::isfinite(weighted_distance))
  {
    return 0.0F;
  }
  return std::clamp(static_cast<float>(std::exp(-0.5 * weighted_distance / weight_sum)), 0.0F,
                    1.0F);
}

OwnVoiceState OwnVoiceDetector::makeUnhealthy(const OwnVoiceHealth health,
                                              const std::uint64_t observed_ns) noexcept
{
  ++state_.generation;
  state_.probability = 0.0F;
  state_.observed_ns = observed_ns;
  state_.active = false;
  state_.health = health;
  activation_candidate_ns_ = 0;
  release_candidate_ns_ = 0;
  activation_candidate_valid_ = false;
  release_candidate_valid_ = false;
  return state_;
}

OwnVoiceState OwnVoiceDetector::evaluate(const OwnVoiceObservation& observation,
                                         const std::uint64_t now_ns) noexcept
{
  if (!configured_ || !config_.enabled)
  {
    return makeUnhealthy(configured_ ? OwnVoiceHealth::Disabled : OwnVoiceHealth::ModelUnavailable,
                         observation.observed_ns);
  }
  if (!observation.valid)
  {
    return makeUnhealthy(OwnVoiceHealth::InvalidObservation, observation.observed_ns);
  }
  if (now_ns < observation.observed_ns)
  {
    return makeUnhealthy(OwnVoiceHealth::InvalidObservation, observation.observed_ns);
  }
  if (has_observation_ &&
      (observation.sequence <= last_sequence_ || observation.observed_ns < last_observed_ns_))
  {
    return makeUnhealthy(OwnVoiceHealth::InvalidObservation, observation.observed_ns);
  }
  if ((now_ns - observation.observed_ns) > config_.stale_timeout_ns)
  {
    return makeUnhealthy(OwnVoiceHealth::Stale, observation.observed_ns);
  }

  const float current_probability = probability(observation);
  if (!std::isfinite(current_probability))
  {
    return makeUnhealthy(OwnVoiceHealth::InvalidObservation, observation.observed_ns);
  }

  if (!state_.active)
  {
    release_candidate_ns_ = 0;
    release_candidate_valid_ = false;
    if (current_probability >= config_.activation_probability)
    {
      if (!activation_candidate_valid_)
      {
        activation_candidate_ns_ = observation.observed_ns;
        activation_candidate_valid_ = true;
      }
      if (Elapsed(observation.observed_ns, activation_candidate_ns_, config_.activation_hold_ns))
      {
        state_.active = true;
        activation_candidate_ns_ = 0;
        activation_candidate_valid_ = false;
      }
    }
    else
    {
      activation_candidate_ns_ = 0;
      activation_candidate_valid_ = false;
    }
  }
  else
  {
    activation_candidate_ns_ = 0;
    activation_candidate_valid_ = false;
    if (current_probability <= config_.release_probability)
    {
      if (!release_candidate_valid_)
      {
        release_candidate_ns_ = observation.observed_ns;
        release_candidate_valid_ = true;
      }
      if (Elapsed(observation.observed_ns, release_candidate_ns_, config_.release_hold_ns))
      {
        state_.active = false;
        release_candidate_ns_ = 0;
        release_candidate_valid_ = false;
      }
    }
    else
    {
      release_candidate_ns_ = 0;
      release_candidate_valid_ = false;
    }
  }

  has_observation_ = true;
  last_sequence_ = observation.sequence;
  last_observed_ns_ = observation.observed_ns;
  ++state_.generation;
  state_.probability = current_probability;
  state_.observed_ns = observation.observed_ns;
  state_.health = OwnVoiceHealth::Healthy;
  return state_;
}

OwnVoiceState OwnVoiceDetector::stateForTime(const std::uint64_t now_ns) noexcept
{
  if (!configured_ || !config_.enabled)
  {
    return state_;
  }
  if (state_.health != OwnVoiceHealth::Healthy)
  {
    return state_;
  }
  if (!IsFreshAndHealthy(state_, now_ns, config_.stale_timeout_ns))
  {
    return makeUnhealthy(OwnVoiceHealth::Stale, state_.observed_ns);
  }
  return state_;
}
} // namespace sonitude::ovd
