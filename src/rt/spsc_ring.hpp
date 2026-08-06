#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonitude::rt
{
template <typename T>
class SpscRing
{
 public:
  explicit SpscRing(const std::size_t capacity_pow2)
      : capacity_(capacity_pow2), mask_(capacity_pow2 - 1U), storage_(capacity_pow2)
  {
  }

  std::size_t capacity() const { return capacity_; }

  bool push(const T& value)
  {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1U) & mask_;
    if (next == tail_.load(std::memory_order_acquire))
    {
      return false;
    }
    storage_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T& out)
  {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire))
    {
      return false;
    }
    out = storage_[tail];
    tail_.store((tail + 1U) & mask_, std::memory_order_release);
    return true;
  }

  std::size_t size() const
  {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head + capacity_ - tail) & mask_;
  }

 private:
  std::size_t capacity_;
  std::size_t mask_;
  std::vector<T> storage_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};
}  // namespace sonitude::rt
