#pragma once

#include <cstdint>
#include <type_traits>

#include "audio/audio_types.hpp"

namespace sonitude::control
{
inline constexpr std::int16_t kNoZoneId = -1;

// The only steering payload the audio thread is ever allowed to see.
//
// Deliberately a flat aggregate of scalars: a message that crosses into a
// realtime thread must not own heap memory, must not run a copy constructor,
// and must be safe to hand over by a single trivial copy into a queue slot.
// The human-readable zone name stays on the slow side; the audio thread only
// needs the identity, so it carries an index instead of a string.
struct RtSteeringSnapshot
{
  audio::BeamformerSteering target{};
  audio::BeamformerSteering distractor{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  float speech_probability = 0.0F;
  std::uint64_t generation = 0;
  // Monotonic nanoseconds, taken by the control thread from the same clock base
  // the audio thread uses, so snapshot age is measurable.
  std::uint64_t published_ns = 0;
  std::int16_t zone_id = kNoZoneId;
  std::uint8_t control_state = 0;
  bool failsafe = true;
  bool has_distractor = false;
};

static_assert(std::is_trivially_copyable_v<RtSteeringSnapshot>,
              "RtSteeringSnapshot crosses into a realtime thread and must be trivially copyable");
static_assert(std::is_trivially_destructible_v<RtSteeringSnapshot>,
              "RtSteeringSnapshot must not run a destructor on the realtime thread");
static_assert(std::is_standard_layout_v<RtSteeringSnapshot>,
              "RtSteeringSnapshot must stay a flat aggregate of scalars");
} // namespace sonitude::control
