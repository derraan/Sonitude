#pragma once

#include <array>
#include <cstdint>

#include "ovd/own_voice_features.hpp"
#include "ovd/own_voice_state.hpp"

namespace sonitude::ovd
{
struct StatisticalOvtfModel
{
  std::array<float, kOwnVoiceFeatureCount> feature_mean{};
  std::array<float, kOwnVoiceFeatureCount> feature_inverse_variance{};
  std::array<float, kOwnVoiceFeatureCount> feature_weight{};
  std::uint32_t calibration_revision = 0;
  bool calibrated = false;
};

struct OwnVoiceDetectorConfig
{
  bool enabled = false;
  float activation_probability = 0.0F;
  float release_probability = 0.0F;
  std::uint64_t activation_hold_ns = 0;
  std::uint64_t release_hold_ns = 0;
  std::uint64_t stale_timeout_ns = 0;
};

// Slow-thread deterministic matched-statistics detector. The model describes
// a distribution, not an exact delay/phase/frequency fingerprint.
class OwnVoiceDetector
{
public:
  void configure(const OwnVoiceDetectorConfig& config, const StatisticalOvtfModel& model);
  OwnVoiceState evaluate(const OwnVoiceObservation& observation, std::uint64_t now_ns) noexcept;
  OwnVoiceState stateForTime(std::uint64_t now_ns) noexcept;

private:
  OwnVoiceState makeUnhealthy(OwnVoiceHealth health, std::uint64_t observed_ns) noexcept;
  float probability(const OwnVoiceObservation& observation) const noexcept;

  OwnVoiceDetectorConfig config_{};
  StatisticalOvtfModel model_{};
  OwnVoiceState state_{};
  std::uint64_t activation_candidate_ns_ = 0;
  std::uint64_t release_candidate_ns_ = 0;
  bool configured_ = false;
};
} // namespace sonitude::ovd
