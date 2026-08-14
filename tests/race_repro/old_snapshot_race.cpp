// Standalone reproducer for the data race in the removed SnapshotBuffer.
// Not part of the CMake build. See README.md in this directory.
//
// This is the deleted src/rt/param_snapshot.hpp verbatim, driven the way
// src/main.cpp drove it: the control thread publishes SteeringSnapshot values
// while the audio thread acquires them.

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace old_code
{
struct BeamformerSteering
{
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
};

struct SteeringSnapshot
{
  BeamformerSteering target{};
  BeamformerSteering distractor{};
  float ambient_mix = 0.25F;
  float confidence = 0.0F;
  float speech_probability = 0.0F;
  std::uint64_t generation = 0;
  std::string zone_name;
  bool failsafe = true;
  bool has_distractor = false;
};

template <typename T>
class SnapshotBuffer
{
 public:
  explicit SnapshotBuffer(const T& initial) : slots_{initial, initial} {}

  void publish(const T& value)
  {
    const std::uint64_t seq0 = sequence_.load(std::memory_order_relaxed);
    sequence_.store(seq0 + 1U, std::memory_order_release);
    const std::size_t slot = static_cast<std::size_t>(((seq0 / 2U) + 1U) % 2U);
    slots_[slot] = value;
    sequence_.store(seq0 + 2U, std::memory_order_release);
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
}  // namespace old_code

int main()
{
  old_code::SnapshotBuffer<old_code::SteeringSnapshot> buffer({});
  std::atomic<bool> done{false};

  // Control-thread role: publish steering updates.
  std::thread writer([&] {
    for (std::uint64_t i = 1; i <= 200000; ++i)
    {
      old_code::SteeringSnapshot snapshot;
      snapshot.generation = i;
      snapshot.target.azimuth_deg = static_cast<float>(i % 360U);
      snapshot.confidence = static_cast<float>(i % 360U);
      // The field existed on the realtime-facing snapshot; a non-empty value is
      // what the runtime would have carried once zones were wired up.
      snapshot.zone_name = "zone-" + std::to_string(i % 8U);
      buffer.publish(snapshot);
    }
    done.store(true, std::memory_order_release);
  });

  // Audio-thread role: acquire the snapshot every period.
  std::thread reader([&] {
    std::uint64_t observed = 0;
    while (!done.load(std::memory_order_acquire))
    {
      const old_code::SteeringSnapshot snapshot = buffer.acquire();
      observed += snapshot.generation;
    }
    std::cout << "reader observed checksum " << observed << '\n';
  });

  writer.join();
  reader.join();
  std::cout << "completed without crashing (which proves nothing: the defect is "
               "undefined behaviour, so run this under ThreadSanitizer)\n";
  return 0;
}
