#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "rt/param_snapshot.hpp"

namespace
{
void Require(bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

struct SnapshotPayload
{
  std::uint64_t generation = 0;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
};

void TestSnapshotStress()
{
  const SnapshotPayload initial{0, 0xA5A5A5A5A5A5A5A5ULL, 0x5A5A5A5A5A5A5A5AULL};
  sonitude::rt::SnapshotBuffer<SnapshotPayload> buffer(initial);

  constexpr std::uint64_t kIterations = 120000;
  std::atomic<bool> writer_done{false};
  std::atomic<bool> saw_torn{false};
  std::atomic<bool> saw_regression{false};
  std::uint64_t max_seen = 0;

  std::thread producer([&]() {
    for (std::uint64_t i = 1; i <= kIterations; ++i)
    {
      const SnapshotPayload payload{
          i,
          i ^ 0xA5A5A5A5A5A5A5A5ULL,
          (i ^ 0xA5A5A5A5A5A5A5A5ULL) ^ 0xFFFFFFFFFFFFFFFFULL,
      };
      buffer.publish(payload);
    }
    writer_done.store(true, std::memory_order_release);
  });

  while (!writer_done.load(std::memory_order_acquire))
  {
    const SnapshotPayload payload = buffer.acquire();
    const std::uint64_t expected_b = payload.a ^ 0xFFFFFFFFFFFFFFFFULL;
    if (payload.b != expected_b)
    {
      saw_torn.store(true, std::memory_order_release);
    }
    if (payload.generation < max_seen)
    {
      saw_regression.store(true, std::memory_order_release);
    }
    max_seen = payload.generation;
  }

  producer.join();
  const SnapshotPayload final_payload = buffer.acquire();
  Require(!saw_torn.load(std::memory_order_acquire), "snapshot read was torn");
  Require(!saw_regression.load(std::memory_order_acquire), "snapshot generation regressed");
  Require(final_payload.generation == kIterations, "snapshot final generation mismatch");
}
}  // namespace

void RunSnapshotTests()
{
  TestSnapshotStress();
}
