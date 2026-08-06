#include <cstdint>
#include <stdexcept>
#include <string>
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

void TestBlockPool()
{
  sonitude::rt::BlockPool<int> pool(8);
  int* ptr = pool.acquire();
  Require(ptr != nullptr, "pool acquire failed");
  *ptr = 42;
  Require(pool.release(ptr), "pool release failed");
}
}  // namespace

void RunRtPrimitiveTests()
{
  TestRingPushPop();
  TestBlockPool();
}
