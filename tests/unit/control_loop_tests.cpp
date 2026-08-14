#include <stdexcept>
#include <string>
#include <vector>

#include "control/control_loop.hpp"
#include "control/steering_channel.hpp"
#include "spatial/mock_doa_provider.hpp"

namespace
{
void Require(bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestControlLoopTracksAndFailsafe()
{
  std::vector<sonitude::spatial::MockDoaEvent> events;
  events.push_back({100'000'000ULL, {11, 20.0F, 0.0F, 0.8F, 100'000'000ULL}});
  events.push_back({150'000'000ULL, {11, 25.0F, 0.0F, 0.9F, 150'000'000ULL}});
  events.push_back({550'000'000ULL, {12, -15.0F, 0.0F, 0.85F, 550'000'000ULL}});
  sonitude::spatial::MockDoaProvider provider(std::move(events));

  sonitude::control::SteeringChannel channel;

  sonitude::control::ControlLoop loop(
      &provider,
      &channel,
      {.failsafe_timeout_ns = 300'000'000ULL, .ambient_floor_linear = 0.25F});

  // The constructor seeds a failsafe publication, so drain it before asserting
  // on what each tick produces.
  sonitude::control::RtSteeringSnapshot s{};
  Require(channel.drainLatest(s), "construction should publish an initial snapshot");
  Require(s.failsafe, "the initial published snapshot should be failsafe");

  loop.tick(100'000'000ULL);
  Require(channel.drainLatest(s), "a tick should publish a snapshot");
  Require(!s.failsafe, "control loop should steer when observations are present");

  provider.setFrozen(true);
  loop.tick(450'000'000ULL);
  Require(channel.drainLatest(s), "a tick should publish a snapshot");
  Require(s.failsafe, "control loop should switch to failsafe after freeze timeout");
  Require(s.ambient_mix >= 0.25F, "failsafe should use configured ambient floor");

  provider.setFrozen(false);
  provider.setNowNs(600'000'000ULL);
  loop.tick(600'000'000ULL);
  Require(channel.drainLatest(s), "a tick should publish a snapshot");
  Require(!s.failsafe, "control loop should recover from failsafe when provider resumes");

  // A stopped or failed control loop must leave the audio thread safe.
  loop.publishFailsafe(700'000'000ULL);
  Require(channel.drainLatest(s), "publishFailsafe should publish a snapshot");
  Require(s.failsafe, "publishFailsafe must publish a failsafe snapshot");
  Require(s.target.azimuth_deg == 0.0F && s.target.elevation_deg == 0.0F,
          "publishFailsafe must recentre the beam");
  Require(!s.has_distractor, "publishFailsafe must clear the distractor");
}
}  // namespace

void RunControlLoopTests()
{
  TestControlLoopTracksAndFailsafe();
}
