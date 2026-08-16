#include "ovd/own_voice_features.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonitude::ovd {
namespace {
constexpr double kEnergyFloor = 1.0e-12;
}

OwnVoiceFeatureExtractor::OwnVoiceFeatureExtractor(
    const std::size_t reference_mic_index)
    : reference_mic_index_(reference_mic_index) {
  if (reference_mic_index_ >= audio::kMicChannels) {
    throw std::invalid_argument(
        "own-voice feature reference microphone is out of range");
  }
}

OwnVoiceObservation OwnVoiceFeatureExtractor::extract(
    const std::span<const audio::MicFrame> calibrated_input,
    const std::span<const float> own_reference, const std::uint64_t sequence,
    const std::uint64_t observed_ns) const noexcept {
  OwnVoiceObservation observation{};
  observation.sequence = sequence;
  observation.observed_ns = observed_ns;
  if (calibrated_input.empty() ||
      calibrated_input.size() != own_reference.size()) {
    return observation;
  }

  std::array<double, audio::kMicChannels> energy{};
  std::array<double, audio::kMicChannels> reference_cross{};
  double own_energy = 0.0;

  for (std::size_t frame_index = 0; frame_index < calibrated_input.size();
       ++frame_index) {
    const float own_sample = own_reference[frame_index];
    if (!std::isfinite(own_sample)) {
      return observation;
    }
    own_energy +=
        static_cast<double>(own_sample) * static_cast<double>(own_sample);

    const float reference_sample =
        calibrated_input[frame_index][reference_mic_index_];
    if (!std::isfinite(reference_sample)) {
      return observation;
    }
    for (std::size_t channel = 0; channel < audio::kMicChannels; ++channel) {
      const float sample = calibrated_input[frame_index][channel];
      if (!std::isfinite(sample)) {
        return observation;
      }
      energy[channel] +=
          static_cast<double>(sample) * static_cast<double>(sample);
      reference_cross[channel] +=
          static_cast<double>(sample) * static_cast<double>(reference_sample);
    }
  }

  double mean_channel_energy = 0.0;
  for (const double channel_energy : energy) {
    mean_channel_energy += channel_energy;
  }
  mean_channel_energy /= static_cast<double>(audio::kMicChannels);
  observation.features[0] = static_cast<float>(std::sqrt(
      (own_energy + kEnergyFloor) / (mean_channel_energy + kEnergyFloor)));

  std::size_t level_feature = 1U;
  std::size_t correlation_feature = 1U + kRelativeLevelFeatureCount;
  for (std::size_t channel = 0; channel < audio::kMicChannels; ++channel) {
    if (channel == reference_mic_index_) {
      continue;
    }
    observation.features[level_feature++] = static_cast<float>(
        0.5 * std::log((energy[channel] + kEnergyFloor) /
                       (energy[reference_mic_index_] + kEnergyFloor)));
    const double denominator =
        std::sqrt((energy[channel] + kEnergyFloor) *
                  (energy[reference_mic_index_] + kEnergyFloor));
    observation.features[correlation_feature++] = static_cast<float>(
        std::clamp(reference_cross[channel] / denominator, -1.0, 1.0));
  }

  for (const float feature : observation.features) {
    if (!std::isfinite(feature)) {
      return OwnVoiceObservation{
          .sequence = sequence, .observed_ns = observed_ns, .valid = false};
    }
  }
  observation.valid = true;
  return observation;
}
} // namespace sonitude::ovd
