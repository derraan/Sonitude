#pragma once

#include <cstdint>

#include "audio/audio_types.hpp"

namespace sonitude::control
{
struct SteeringSnapshot
{
  audio::BeamformerSteering target{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  std::uint64_t generation = 0;
  bool failsafe = true;
};
}  // namespace sonitude::control
