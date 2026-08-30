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

  explicit SnapshotBuffer(const T& initial) : last_(initial) {}

  void publish(const T& value)
  {
    const std::size_t write = write_.load(std::memory_order_relaxed);
    const std::size_t next = (write + 1U) % kSlots;
    if (next == read_.load(std::memory_order_acquire))
    {
      return;  // Keep the last complete snapshot rather than overwrite reader-owned data.
    }
    slots_[write] = value;
    write_.store(next, std::memory_order_release);
  }

  T acquire() const
  {
    std::size_t read = read_.load(std::memory_order_relaxed);
    const std::size_t write = write_.load(std::memory_order_acquire);
    T value = last_;
    while (read != write)
    {
      value = slots_[read];
      read = (read + 1U) % kSlots;
    }
    last_ = value;
    read_.store(read, std::memory_order_release);
    return value;
  }

 private:
  static constexpr std::size_t kSlots = 4;
  std::array<T, kSlots> slots_{};
  std::atomic<std::size_t> write_{0};
  mutable std::atomic<std::size_t> read_{0};
  mutable T last_{};
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
