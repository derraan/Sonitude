#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/fractional_delay.hpp"
#include "dsp/hrtf_table.hpp"

namespace sonitude::dsp
{
enum class BinauralBackend
{
  MonoReference,
  ItdIld,
  CompactHrtf,
  FullHrtfReference,
};

struct ItdIldModelConfig
{
  double head_radius_m = 0.0875;
  float max_ild_db = 6.0F;  // TODO(TBD_FROM_MEASUREMENT): confirm perceptual target
};

struct BinauralConfig
{
  std::uint32_t sample_rate_hz = 0;
  BinauralBackend backend = BinauralBackend::MonoReference;
  float transition_ms = 150.0F;
  std::size_t max_block_frames = 0;
  ItdIldModelConfig itd_ild{};
  const HrtfTable* table = nullptr;
};

class BinauralRenderer
{
 public:
  void configure(const BinauralConfig& config);
  void reset();
  void setDirection(audio::BeamformerSteering direction);
  void process(std::span<const float> mono, std::span<float> left, std::span<float> right);

  BinauralBackend resolvedBackend() const { return config_.backend; }
  std::size_t stateBytes() const;
  std::size_t coefficientBytes() const;
  std::size_t algorithmicLatencySamples() const;

  struct EarParams
  {
    double delay_samples = 0.0;
    float gain = 1.0F;
    std::vector<float> fir;
  };

  struct PathParams
  {
    EarParams left;
    EarParams right;
  };

  struct EarState
  {
    FractionalDelayLine delay;
    std::vector<float> fir_state;
    std::size_t fir_write = 0;
  };

  struct PathState
  {
    EarState left;
    EarState right;
  };

 private:
  BinauralConfig config_{};
  audio::BeamformerSteering current_direction_{};
  audio::BeamformerSteering pending_direction_{};
  bool configured_ = false;
  std::size_t ramp_samples_ = 1;
  std::size_t fade_cursor_ = 0;
  bool crossfading_ = false;
  double base_delay_samples_ = 0.0;

  PathParams current_params_{};
  PathParams pending_params_{};
  PathState current_state_{};
  PathState pending_state_{};
};
}  // namespace sonitude::dsp
