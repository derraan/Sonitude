#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace sonitude::rt
{
template <typename T>
class SnapshotBuffer
{
 public:
  explicit SnapshotBuffer(const T& initial) : slots_{initial, initial} {}

  void publish(const T& value)
  {
    const std::uint64_t seq0 = sequence_.load(std::memory_order_relaxed);
    sequence_.store(seq0 + 1U, std::memory_order_release);  // writer busy (odd)
    const std::size_t slot = static_cast<std::size_t>(((seq0 / 2U) + 1U) % 2U);
    slots_[slot] = value;
    sequence_.store(seq0 + 2U, std::memory_order_release);  // publish complete (even)
  }

  T acquire() const
  {
    for (;;)
    {
      const std::uint64_t seq1 = sequence_.load(std::memory_order_acquire);
      if ((seq1 & 1U) != 0U)
      {
        continue;
      }
      const std::size_t slot = static_cast<std::size_t>((seq1 / 2U) % 2U);
      const T value = slots_[slot];
      const std::uint64_t seq2 = sequence_.load(std::memory_order_acquire);
      if (seq1 == seq2)
      {
        return value;
      }
    }
  }

 private:
  mutable std::atomic<std::uint64_t> sequence_{0};
  std::array<T, 2> slots_{};
};

template <typename T>
class SnapshotPublisher
{
 public:
  explicit SnapshotPublisher(SnapshotBuffer<T>* buffer) : buffer_(buffer) {}
  void publish(const T& value) { buffer_->publish(value); }

 private:
  SnapshotBuffer<T>* buffer_ = nullptr;
};

template <typename T>
class SnapshotReader
{
 public:
  explicit SnapshotReader(const SnapshotBuffer<T>* buffer) : buffer_(buffer) {}
  T acquire() const { return buffer_->acquire(); }

 private:
  const SnapshotBuffer<T>* buffer_ = nullptr;
};
}  // namespace sonitude::rt
