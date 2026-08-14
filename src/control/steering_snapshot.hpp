#pragma once

#include <cstdint>
#include <type_traits>

#include "audio/audio_types.hpp"

namespace sonitude::control
{
enum class ZoneId : std::uint8_t
{
  None = 0,
  FrontAutoFocus = 1,
  RightAssist = 2,
  LeftAssist = 3,
  RearAmbient = 4
};

struct SteeringSnapshot
{
  audio::BeamformerSteering target{};
  audio::BeamformerSteering distractor{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  float speech_probability = 0.0F;
  std::uint64_t generation = 0;
  ZoneId zone_id = ZoneId::None;
  bool failsafe = true;
  bool has_distractor = false;
};

static_assert(std::is_trivially_copyable_v<ZoneId>, "ZoneId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<SteeringSnapshot>,
              "SteeringSnapshot must remain trivially copyable for RT snapshots");
}  // namespace sonitude::control
