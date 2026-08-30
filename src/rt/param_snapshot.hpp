#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sonitude::rt
{
template <typename T>
class SnapshotBuffer
{
 public:
  static_assert(std::is_trivially_copyable_v<T>, "RT snapshots must be trivially copyable");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "RT snapshots require lock-free 32-bit atomics");

  explicit SnapshotBuffer(const T& initial) { StoreWords(initial); }

  void publish(const T& value)
  {
    const std::uint32_t seq = sequence_.load(std::memory_order_relaxed);
    sequence_.store(seq + 1U, std::memory_order_release);
    StoreWords(value);
    sequence_.store(seq + 2U, std::memory_order_release);
  }

  T acquire() const
  {
    for (;;)
    {
      const std::uint32_t before = sequence_.load(std::memory_order_acquire);
      if ((before & 1U) != 0U)
      {
        continue;
      }
      WordArray words{};
      for (std::size_t i = 0; i < kWords; ++i)
      {
        words[i] = payload_[i].load(std::memory_order_relaxed);
      }
      const std::uint32_t after = sequence_.load(std::memory_order_acquire);
      if (before == after)
      {
        T value{};
        std::memcpy(&value, words.data(), sizeof(T));
        return value;
      }
    }
  }

 private:
  static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint32_t) - 1U) / sizeof(std::uint32_t);
  using WordArray = std::array<std::uint32_t, kWords>;

  void StoreWords(const T& value)
  {
    WordArray words{};
    std::memcpy(words.data(), &value, sizeof(T));
    for (std::size_t i = 0; i < kWords; ++i)
    {
      payload_[i].store(words[i], std::memory_order_relaxed);
    }
  }

  std::atomic<std::uint32_t> sequence_{0};
  std::array<std::atomic<std::uint32_t>, kWords> payload_{};
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
