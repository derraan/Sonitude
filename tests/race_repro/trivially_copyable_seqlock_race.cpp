// Reproducer for the residual data race at the tip of PR #23 (commit 2e59349).
// Not part of the CMake build. See README.md in this directory.
//
// That commit removed `std::string zone_name` from SteeringSnapshot and added
// static_assert(std::is_trivially_copyable_v<SteeringSnapshot>), but kept
// src/rt/param_snapshot.hpp -- the seqlock -- as the control-to-audio handoff.
//
// This file is that exact combination: the trivially copyable snapshot, published
// through the surviving seqlock. Trivial copyability removes the double-free and
// dangling-pointer failure modes that came with the string. It does not make the
// handoff data-race-free, because the writer still assigns to a non-atomic object
// while the reader copies the same object, and no happens-before relation orders
// those two accesses. That is a data race under [intro.races], so the program has
// undefined behaviour and a sequence re-check cannot recover defined behaviour
// after the fact.

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <type_traits>

namespace pr23_tip
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
  bool failsafe = true;
  bool has_distractor = false;
};

// This is the assertion the upstream commit added. It passes, and it is not
// sufficient: it constrains the payload, not the handoff.
static_assert(std::is_trivially_copyable_v<SteeringSnapshot>);

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
    slots_[slot] = value;  // non-atomic write
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
      const T value = slots_[slot];  // non-atomic read, concurrent with the write
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
}  // namespace pr23_tip

int main()
{
  pr23_tip::SnapshotBuffer<pr23_tip::SteeringSnapshot> buffer({});
  std::atomic<bool> done{false};

  std::thread writer([&] {
    for (std::uint64_t i = 1; i <= 400000; ++i)
    {
      pr23_tip::SteeringSnapshot snapshot;
      snapshot.generation = i;
      snapshot.target.azimuth_deg = static_cast<float>(i % 360U);
      snapshot.confidence = static_cast<float>(i % 100U) / 100.0F;
      snapshot.failsafe = (i % 2U) == 0U;
      buffer.publish(snapshot);
    }
    done.store(true, std::memory_order_release);
  });

  std::thread reader([&] {
    std::uint64_t checksum = 0;
    while (!done.load(std::memory_order_acquire))
    {
      const pr23_tip::SteeringSnapshot snapshot = buffer.acquire();
      checksum += snapshot.generation;
    }
    std::cout << "reader checksum " << checksum << '\n';
  });

  writer.join();
  reader.join();
  std::cout << "no crash, which proves nothing: run this under ThreadSanitizer\n";
  return 0;
}
