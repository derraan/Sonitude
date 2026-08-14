#pragma once

#include <cstdint>
#include <vector>

#include "control/conversation_state_machine.hpp"
#include "control/steering_channel.hpp"
#include "control/steering_snapshot.hpp"
#include "spatial/doa_provider.hpp"
#include "spatial/source_tracker.hpp"

namespace sonitude::control
{
struct ControlLoopConfig
{
  std::uint64_t failsafe_timeout_ns = 1'500'000'000ULL;
  float ambient_floor_linear = 0.25F;
};

// Runs on a SCHED_OTHER thread. Free to allocate, parse, and block on the
// network; the only thing it hands to the audio thread is a trivially copyable
// RtSteeringSnapshot pushed into a bounded queue.
class ControlLoop
{
 public:
  ControlLoop(spatial::IDoaProvider* provider,
              SteeringChannel* channel,
              ControlLoopConfig config,
              ConversationStateMachine* conversation = nullptr);
  void tick(std::uint64_t now_ns);

  // Publishes the safe default immediately, without consulting the provider.
  // Used when the control thread is torn down or has failed, so the audio thread
  // is never left steering on stale data after control stops progressing.
  void publishFailsafe(std::uint64_t now_ns);

 private:
  void publish(const SteeringSnapshot& snapshot, std::uint64_t now_ns);

  spatial::IDoaProvider* provider_ = nullptr;
  SteeringChannel* channel_ = nullptr;
  ControlLoopConfig config_{};
  ConversationStateMachine* conversation_ = nullptr;
  spatial::SourceTracker tracker_{1'500'000'000ULL};
  std::uint64_t last_observation_ns_ = 0;
  std::uint64_t generation_ = 0;
  SteeringSnapshot last_snapshot_{};
  std::vector<spatial::SourceObservation> observations_;
};
}  // namespace sonitude::control
