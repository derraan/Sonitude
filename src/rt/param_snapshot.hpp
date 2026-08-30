#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace sonitude::rt
{
template <typename T>
class SnapshotBuffer
{
 public:
  static_assert(std::is_trivially_copyable_v<T>, "RT snapshots must be trivially copyable");

  explicit SnapshotBuffer(const T& initial) : slots_{initial, initial, initial} {}

  void publish(const T& value)
  {
    const std::size_t published = published_.load(std::memory_order_acquire);
    const std::size_t reading = reading_.load(std::memory_order_acquire);
    std::size_t slot = 0;
    while (slot == published || slot == reading)
    {
      ++slot;
    }
    slots_[slot] = value;
    published_.store(slot, std::memory_order_release);
  }

  T acquire() const
  {
    for (;;)
    {
      const std::size_t slot = published_.load(std::memory_order_acquire);
      reading_.store(slot, std::memory_order_release);
      if (published_.load(std::memory_order_acquire) != slot)
      {
        reading_.store(kNotReading, std::memory_order_release);
        continue;
      }
      const T value = slots_[slot];
      reading_.store(kNotReading, std::memory_order_release);
      return value;
    }
  }

 private:
  static constexpr std::size_t kNotReading = 3;
  std::array<T, 3> slots_{};
  std::atomic<std::size_t> published_{0};
  mutable std::atomic<std::size_t> reading_{kNotReading};
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
