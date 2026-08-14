#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include "audio/playback_block_pool.hpp"
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

void TestPlaybackPoolBackpressurePreservesOutstandingBlocks()
{
  constexpr std::size_t kBlocks = 4;
  constexpr std::size_t kFrames = 8;
  sonitude::audio::PlaybackBlockPool pool(kBlocks, kFrames);

  for (std::size_t sequence = 0; sequence < kBlocks; ++sequence)
  {
    std::size_t slot = 0;
    Require(pool.acquire(slot), "playback producer should acquire every initially free block");
    for (auto& sample : pool.writable(slot))
    {
      sample = {static_cast<float>(sequence), -static_cast<float>(sequence)};
    }
    Require(pool.publish(slot, kFrames), "playback producer should publish an owned block");
  }

  std::size_t unavailable_slot = 0;
  Require(!pool.acquire(unavailable_slot),
          "playback producer must receive deterministic backpressure when all blocks are outstanding");
  Require(pool.queuedFrames() == kBlocks * kFrames, "queued frame accounting should include every block");

  for (std::size_t sequence = 0; sequence < kBlocks; ++sequence)
  {
    sonitude::audio::PlaybackBlockRef block;
    Require(pool.consume(block), "playback consumer should drain every published block");
    for (const auto& sample : pool.readable(block))
    {
      Require(sample.left == static_cast<float>(sequence) &&
                  sample.right == -static_cast<float>(sequence),
              "outstanding playback storage was overwritten");
    }
    Require(pool.release(block.slot), "playback consumer should return the consumed block");
  }
  Require(pool.queuedFrames() == 0U, "queued frame accounting should return to zero after drain");
}

void TestPlaybackPoolConcurrentOwnershipAndAccounting()
{
  constexpr std::size_t kBlocks = 8;
  constexpr std::size_t kFrames = 16;
  constexpr std::size_t kIterations = 100000;
  sonitude::audio::PlaybackBlockPool pool(kBlocks, kFrames);
  std::array<std::atomic<bool>, kBlocks> outstanding{};
  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};
  std::atomic<std::size_t> backpressure_count{0};

  std::thread producer([&]()
  {
    for (std::size_t sequence = 0; sequence < kIterations; ++sequence)
    {
      std::size_t slot = 0;
      while (!pool.acquire(slot))
      {
        backpressure_count.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
      }
      if (outstanding[slot].exchange(true, std::memory_order_acq_rel))
      {
        failed.store(true, std::memory_order_relaxed);
      }
      for (auto& sample : pool.writable(slot))
      {
        sample = {static_cast<float>(sequence), -static_cast<float>(sequence)};
      }
      if (!pool.publish(slot, kFrames))
      {
        failed.store(true, std::memory_order_relaxed);
        break;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::size_t expected = 0;
  while (backpressure_count.load(std::memory_order_acquire) == 0U &&
         !producer_done.load(std::memory_order_acquire) &&
         !failed.load(std::memory_order_relaxed))
  {
    std::this_thread::yield();
  }
  while (!producer_done.load(std::memory_order_acquire) || pool.filledCount() > 0U)
  {
    sonitude::audio::PlaybackBlockRef block;
    if (!pool.consume(block))
    {
      std::this_thread::yield();
      continue;
    }
    if (!outstanding[block.slot].load(std::memory_order_acquire))
    {
      failed.store(true, std::memory_order_relaxed);
    }
    if (pool.queuedFrames() > kBlocks * kFrames)
    {
      failed.store(true, std::memory_order_relaxed);
    }
    for (const auto& sample : pool.readable(block))
    {
      if (sample.left != static_cast<float>(expected) ||
          sample.right != -static_cast<float>(expected))
      {
        failed.store(true, std::memory_order_relaxed);
      }
    }
    outstanding[block.slot].store(false, std::memory_order_release);
    if (!pool.release(block.slot))
    {
      failed.store(true, std::memory_order_relaxed);
    }
    ++expected;
    std::this_thread::yield();
  }

  producer.join();
  Require(!failed.load(std::memory_order_relaxed),
          "playback pool duplicated ownership, overwrote a block, or underflowed accounting");
  Require(expected == kIterations, "playback consumer should receive every published block");
  Require(backpressure_count.load(std::memory_order_relaxed) > 0U,
          "stress test should exercise producer backpressure");
  Require(pool.queuedFrames() == 0U, "concurrent queued frame accounting should finish at zero");

  std::array<bool, kBlocks> returned{};
  for (std::size_t i = 0; i < kBlocks; ++i)
  {
    std::size_t slot = 0;
    Require(pool.acquire(slot), "every playback slot should eventually return to the producer");
    Require(!returned[slot], "returned playback slot must be unique");
    returned[slot] = true;
  }
}
}  // namespace

void RunRtPrimitiveTests()
{
  TestRingPushPop();
  TestRingFullEmpty();
  TestRingConcurrentOrder();
  TestBlockPool();
  TestBlockPoolConcurrentRecycle();
  TestPlaybackPoolBackpressurePreservesOutstandingBlocks();
  TestPlaybackPoolConcurrentOwnershipAndAccounting();
}
