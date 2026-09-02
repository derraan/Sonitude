#pragma once

#include <cstdint>

#include "audio/audio_types.hpp"
#include "control/rt_steering_snapshot.hpp"

namespace sonitude::control
{
// Slow-path steering state, owned by the control thread and never read by the
// audio thread. It is converted to an RtSteeringSnapshot before publication.
//
// This type previously carried a std::string zone_name that was declared but
// never assigned, and it was copied wholesale by the audio thread. It now
// carries a zone index instead: identity is all the audio side can use, and a
// scalar keeps the whole structure trivially copyable.
struct SteeringSnapshot
{
  audio::BeamformerSteering target{};
  audio::BeamformerSteering distractor{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  float speech_probability = 0.0F;
  std::uint64_t generation = 0;
  std::int16_t zone_id = kNoZoneId;
  bool failsafe = true;
  bool has_distractor = false;
};

// Builds the realtime-facing message. `published_ns` and `control_state` are
// supplied by the control thread so the audio thread can measure snapshot age
// and telemetry can report the state machine without touching control memory.
inline RtSteeringSnapshot ToRtSnapshot(const SteeringSnapshot& snapshot,
                                       const std::uint64_t published_ns,
                                       const std::uint8_t control_state) noexcept
{
  RtSteeringSnapshot out{};
  out.target = snapshot.target;
  out.distractor = snapshot.distractor;
  out.ambient_mix = snapshot.ambient_mix;
  out.confidence = snapshot.confidence;
  out.speech_probability = snapshot.speech_probability;
  out.generation = snapshot.generation;
  out.published_ns = published_ns;
  out.zone_id = snapshot.zone_id;
  out.control_state = control_state;
  out.failsafe = snapshot.failsafe;
  out.has_distractor = snapshot.has_distractor;
  return out;
}
} // namespace sonitude::control
