#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/hrtf_table.hpp"

namespace sonitude::dsp
{
// KEMAR/HRTF-indexed near-field steering LUT for MVDR.
// Precomputes per-direction, per-microphone relative delays from a spherical
// point source at steering.source_distance_m. Runtime snaps az/el to the
// nearest HRTF table direction and supplies ear-specific ITD offsets for
// binaural MVDR output paths.
class KemarSteeringLut
{
 public:
  using DelayArray = std::array<double, audio::kMicChannels>;

  void configure(const HrtfTable& table,
                 const app::GeometryConfig& geometry,
                 const app::SteeringConfig& steering,
                 std::uint32_t sample_rate_hz);
  void configureAnalytic(const app::GeometryConfig& geometry,
                         const app::SteeringConfig& steering,
                         std::uint32_t sample_rate_hz);
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  [[nodiscard]] std::size_t lookupIndex(float azimuth_deg, float elevation_deg) const;
  [[nodiscard]] audio::BeamformerSteering snappedDirection(std::size_t index) const;
  [[nodiscard]] const DelayArray& delaysForIndex(std::size_t index) const;
  [[nodiscard]] float leftEarOffsetSamples(std::size_t index) const;
  [[nodiscard]] float rightEarOffsetSamples(std::size_t index) const;

  // Analytic near-field delays (used when LUT disabled or for binaural ear refs).
  [[nodiscard]] DelayArray computeNearFieldDelays(audio::BeamformerSteering target,
                                                  std::size_t reference_mic_index) const;

 private:
  bool enabled_ = false;
  std::uint32_t sample_rate_hz_ = 0;
  float speed_of_sound_mps_ = 343.0F;
  float source_distance_m_ = 0.45F;
  std::size_t reference_mic_index_ = 0;
  std::array<std::array<double, 3>, audio::kMicChannels> mic_positions_{};

  std::vector<DelayArray> lut_delays_;
  std::vector<HrtfDirection> lut_directions_;
  std::vector<float> left_ear_offsets_;
  std::vector<float> right_ear_offsets_;
};
}  // namespace sonitude::dsp
