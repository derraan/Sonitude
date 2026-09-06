#pragma once

#include <cstdint>
#include <type_traits>

#include "audio/audio_types.hpp"

namespace sonitude::control
{
inline constexpr std::int16_t kNoZoneId = -1;

struct SteeringSnapshot
{
  audio::BeamformerSteering target{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  float speech_probability = 0.0F;
  std::uint64_t generation = 0;
  // Zone identity for the audio/telemetry side; names stay on the slow path.
  std::int16_t zone_id = kNoZoneId;
  bool failsafe = true;
};

static_assert(std::is_trivially_copyable_v<SteeringSnapshot>,
              "SteeringSnapshot must remain trivially copyable for SnapshotBuffer");
}  // namespace sonitude::control
