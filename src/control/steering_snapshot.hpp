#pragma once

#include <cstdint>
#include <string>

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
  std::string zone_name;
  bool failsafe = true;
  bool has_distractor = false;
};
}  // namespace sonitude::control
