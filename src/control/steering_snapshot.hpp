#pragma once

#include <cstdint>
#include <type_traits>

#include "audio/audio_types.hpp"

namespace sonitude::control
{
struct SteeringSnapshot
{
  audio::BeamformerSteering target{};
  audio::BeamformerSteering distractor{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  float speech_probability = 0.0F;
  std::uint64_t generation = 0;
  bool failsafe = true;
  bool has_distractor = false;
};

static_assert(std::is_trivially_copyable_v<SteeringSnapshot>);
}  // namespace sonitude::control
