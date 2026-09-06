#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app/config.hpp"
#include "audio/audio_types.hpp"

namespace sonitude::dsp
{
// Analytic near-field microphone-array geometry (not a KEMAR HRTF table).
//   tau_m = (||s - p_m|| - ||s - p_ref||) / c,  D_m = fs * tau_m
// Amplitude ratios are available for diagnostics; adaptive MVDR uses delay-derived
// unit-magnitude steering only. HRTF direction lookup is confined to BinauralRenderer.
class GeometricSteeringLut
{
 public:
  using DelayArray = std::array<double, audio::kMicChannels>;
  using AmplitudeArray = std::array<float, audio::kMicChannels>;

  void configure(const app::GeometryConfig& geometry,
                 const app::SteeringConfig& steering,
                 std::uint32_t sample_rate_hz);
  [[nodiscard]] DelayArray computeNearFieldDelays(audio::BeamformerSteering target,
                                                  std::size_t reference_mic_index) const;
  [[nodiscard]] AmplitudeArray computeNearFieldAmplitudes(audio::BeamformerSteering target,
                                                          std::size_t reference_mic_index) const;

 private:
  std::uint32_t sample_rate_hz_ = 0;
  float speed_of_sound_mps_ = 343.0F;
  float source_distance_m_ = 0.45F;
  std::array<std::array<double, 3>, audio::kMicChannels> mic_positions_{};
};

using KemarSteeringLut = GeometricSteeringLut;
}  // namespace sonitude::dsp
