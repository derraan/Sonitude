#include "tests/support/alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
std::atomic<std::uint64_t> g_allocation_count{0};

void* CountedMalloc(const std::size_t n)
{
  g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}
}  // namespace

#if !defined(SONITUDE_DISABLE_ALLOC_COUNTER_GLOBAL_NEW)
void* operator new(std::size_t n)
{
  return CountedMalloc(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
  g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n == 0 ? 1 : n);
}

void* operator new[](std::size_t n)
{
  return CountedMalloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
  g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n == 0 ? 1 : n);
}

void operator delete(void* p) noexcept
{
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  std::free(p);
}
#endif

namespace sonitude::tests::support
{
std::uint64_t AllocationCount()
{
  return g_allocation_count.load(std::memory_order_relaxed);
}

void ResetAllocationCount()
{
  g_allocation_count.store(0, std::memory_order_relaxed);
}
}  // namespace sonitude::tests::support
