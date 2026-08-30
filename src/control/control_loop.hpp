#pragma once

#include <cstdint>
#include <vector>

#include "control/steering_snapshot.hpp"
#include "control/conversation_state_machine.hpp"
#include "rt/param_snapshot.hpp"
#include "spatial/doa_provider.hpp"
#include "spatial/source_tracker.hpp"

namespace sonitude::control
{
struct ControlLoopConfig
{
  std::uint64_t failsafe_timeout_ns = 1'500'000'000ULL;
  float ambient_floor_linear = 0.25F;
};

class ControlLoop
{
 public:
  ControlLoop(spatial::IDoaProvider* provider,
              rt::SnapshotPublisher<SteeringSnapshot> publisher,
              ControlLoopConfig config,
              ConversationStateMachine* conversation = nullptr);
  void tick(std::uint64_t now_ns);

 private:
  spatial::IDoaProvider* provider_ = nullptr;
  rt::SnapshotPublisher<SteeringSnapshot> publisher_;
  ControlLoopConfig config_{};
  ConversationStateMachine* conversation_ = nullptr;
  spatial::SourceTracker tracker_{1'500'000'000ULL};
  std::uint64_t last_observation_ns_ = 0;
  std::uint64_t generation_ = 0;
  SteeringSnapshot last_snapshot_{};
};
}  // namespace sonitude::control
