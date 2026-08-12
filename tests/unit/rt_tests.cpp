#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>

#include "rt/block_pool.hpp"
#include "rt/spsc_ring.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestRingPushPop()
{
  sonitude::rt::SpscRing<std::uint64_t> ring(8);
  Require(ring.push(1), "ring push 1 failed");
  Require(ring.push(2), "ring push 2 failed");
  std::uint64_t out = 0;
  Require(ring.pop(out) && out == 1, "ring pop 1 mismatch");
  Require(ring.pop(out) && out == 2, "ring pop 2 mismatch");
}

void TestRingFullEmpty()
{
  sonitude::rt::SpscRing<std::uint64_t> ring(8);
  for (std::uint64_t i = 0; i < 7; ++i)
  {
    Require(ring.push(i), "ring should accept capacity-1 elements");
  }
  Require(!ring.push(999), "ring should reject push when full");
  std::uint64_t out = 0;
  for (std::uint64_t i = 0; i < 7; ++i)
  {
    Require(ring.pop(out), "ring pop should succeed");
    Require(out == i, "ring pop order mismatch");
  }
  Require(!ring.pop(out), "ring should report empty after drain");
}

void TestRingConcurrentOrder()
{
  constexpr std::uint64_t kCount = 120000;
  sonitude::rt::SpscRing<std::uint64_t> ring(1024);
  std::atomic<bool> producer_done{false};
  std::uint64_t last_seen = 0;
  std::uint64_t consumed = 0;

  std::thread producer([&]() {
    for (std::uint64_t i = 1; i <= kCount; ++i)
    {
      while (!ring.push(i))
      {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  while (!producer_done.load(std::memory_order_acquire) || consumed < kCount)
  {
    std::uint64_t value = 0;
    if (!ring.pop(value))
    {
      std::this_thread::yield();
      continue;
    }
    Require(value > last_seen, "ring sequence must be strictly increasing");
    last_seen = value;
    ++consumed;
  }

  producer.join();
  Require(consumed == kCount, "ring should deliver all produced values");
}

void TestBlockPool()
{
  sonitude::rt::BlockPool<int> pool(8);
  int* ptr = pool.acquire();
  Require(ptr != nullptr, "pool acquire failed");
  *ptr = 42;
  Require(pool.release(ptr), "pool release failed");
}

void TestBlockPoolConcurrentRecycle()
{
  constexpr std::size_t kBlocks = 16;
  constexpr std::size_t kIterations = 40000;
  sonitude::rt::BlockPool<std::uint64_t> pool(kBlocks);
  sonitude::rt::SpscRing<std::uint64_t*> handoff(64);
  std::atomic<bool> done{false};

  std::thread producer([&]() {
    for (std::size_t i = 0; i < kIterations; ++i)
    {
      std::uint64_t* ptr = nullptr;
      while ((ptr = pool.acquire()) == nullptr)
      {
        std::this_thread::yield();
      }
      *ptr = static_cast<std::uint64_t>(i);
      while (!handoff.push(ptr))
      {
        std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });

  std::uint64_t expected = 0;
  while (!done.load(std::memory_order_acquire) || expected < kIterations)
  {
    std::uint64_t* ptr = nullptr;
    if (!handoff.pop(ptr))
    {
      std::this_thread::yield();
      continue;
    }
    Require(ptr != nullptr, "handoff pointer must not be null");
    Require(*ptr == expected, "pool handoff order mismatch");
    Require(pool.release(ptr), "pool release should succeed");
    ++expected;
  }

  producer.join();
  Require(expected == kIterations, "pool should recycle every produced block");
}
}  // namespace

void RunRtPrimitiveTests()
{
  TestRingPushPop();
  TestRingFullEmpty();
  TestRingConcurrentOrder();
  TestBlockPool();
  TestBlockPoolConcurrentRecycle();
}
