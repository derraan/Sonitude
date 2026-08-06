#include <stdexcept>
#include <string>
#include <vector>

#include "control/control_loop.hpp"
#include "rt/param_snapshot.hpp"
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

  sonitude::rt::SnapshotBuffer<sonitude::control::SteeringSnapshot> buffer({});
  sonitude::rt::SnapshotPublisher<sonitude::control::SteeringSnapshot> writer(&buffer);
  sonitude::rt::SnapshotReader<sonitude::control::SteeringSnapshot> reader(&buffer);

  sonitude::control::ControlLoop loop(
      &provider,
      writer,
      {.failsafe_timeout_ns = 300'000'000ULL, .ambient_floor_linear = 0.25F});

  loop.tick(100'000'000ULL);
  auto s = reader.acquire();
  Require(!s.failsafe, "control loop should steer when observations are present");

  provider.setFrozen(true);
  loop.tick(450'000'000ULL);
  s = reader.acquire();
  Require(s.failsafe, "control loop should switch to failsafe after freeze timeout");
  Require(s.ambient_mix >= 0.25F, "failsafe should use configured ambient floor");

  provider.setFrozen(false);
  provider.setNowNs(600'000'000ULL);
  loop.tick(600'000'000ULL);
  s = reader.acquire();
  Require(!s.failsafe, "control loop should recover from failsafe when provider resumes");
}
}  // namespace

void RunControlLoopTests()
{
  TestControlLoopTracksAndFailsafe();
}
