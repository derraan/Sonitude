#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace sonitude::rt
{
// Bounded single-producer / single-consumer queue.
//
// Ownership and happens-before model:
//   * head_ is written only by the producer; tail_ is written only by the
//     consumer. Each index has exactly one writer, so neither index can be lost.
//   * The producer writes storage_[head] only after observing that advancing
//     head would not collide with tail. The consumer reads storage_[tail] only
//     after observing tail != head. Because head only moves forward and the
//     producer refuses to close the gap, the element the producer writes is
//     never the element the consumer reads: producer and consumer never access
//     the same object concurrently, so there is no data race on storage_.
//   * head_.store(release) publishes the element contents; the consumer's
//     head_.load(acquire) is the matching acquire, so the element write
//     happens-before the element read.
//   * tail_.store(release) publishes "this slot is free again"; the producer's
//     tail_.load(acquire) is the matching acquire, so the consumer's read of a
//     slot happens-before the producer's next write to that slot.
//
// One element is always left unused so that "full" and "empty" are
// distinguishable without a third shared variable, hence capacityUsable().
template <typename T> class SpscRing
{
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing elements cross a realtime boundary: they must be trivially copyable so a "
                "slot handoff never runs user code, allocates, or owns heap memory");
  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "SpscRing indices are touched by realtime threads and must be lock-free");

public:
  explicit SpscRing(const std::size_t capacity_pow2)
      : capacity_(ValidatedCapacity(capacity_pow2)), mask_(capacity_ - 1U), storage_(capacity_)
  {
  }

  // Total slot count. One slot is reserved, so capacityUsable() elements fit.
  std::size_t capacity() const noexcept
  {
    return capacity_;
  }
  std::size_t capacityUsable() const noexcept
  {
    return capacity_ - 1U;
  }

  // Producer only.
  bool push(const T& value) noexcept
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

  // Consumer only.
  bool pop(T& out) noexcept
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

  // Safe on either side, but only exact on the side that owns the index it does
  // not have to re-read: the producer sees "at least this many queued", the
  // consumer sees "at most this many queued". Both bounds are conservative in
  // the direction that matters for backpressure decisions.
  std::size_t size() const noexcept
  {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head + capacity_ - tail) & mask_;
  }

  bool empty() const noexcept
  {
    return size() == 0U;
  }

private:
  static std::size_t ValidatedCapacity(const std::size_t capacity_pow2)
  {
    if (capacity_pow2 < 2U || (capacity_pow2 & (capacity_pow2 - 1U)) != 0U)
    {
      throw std::invalid_argument("SpscRing capacity must be a power of two and at least 2");
    }
    return capacity_pow2;
  }

  static constexpr std::size_t kCacheLine = 64;

  std::size_t capacity_;
  std::size_t mask_;
  std::vector<T> storage_;
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};
} // namespace sonitude::rt
