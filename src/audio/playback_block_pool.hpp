#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "dsp/resampler.hpp"
#include "rt/spsc_ring.hpp"

namespace sonitude::audio
{
struct PlaybackBlockRef
{
  std::size_t slot = 0;
  std::size_t frames = 0;
};

static_assert(std::is_trivially_copyable_v<PlaybackBlockRef>);

class PlaybackBlockPool
{
 public:
  PlaybackBlockPool(const std::size_t block_count, const std::size_t frames_per_block)
      : blocks_(block_count, std::vector<dsp::StereoSample>(frames_per_block)),
        free_blocks_(NextPow2(block_count + 1U)),
        filled_blocks_(NextPow2(block_count + 1U))
  {
    assert(block_count > 0U);
    assert(frames_per_block > 0U);
    for (std::size_t slot = 0; slot < block_count; ++slot)
    {
      const bool pushed = free_blocks_.push(slot);
      assert(pushed);
      (void)pushed;
    }
  }

  bool acquire(std::size_t& slot) { return free_blocks_.pop(slot); }

  std::span<dsp::StereoSample> writable(const std::size_t slot)
  {
    assert(slot < blocks_.size());
    return blocks_[slot];
  }

  std::span<const dsp::StereoSample> readable(const PlaybackBlockRef& block) const
  {
    assert(block.slot < blocks_.size());
    assert(block.frames <= blocks_[block.slot].size());
    return {blocks_[block.slot].data(), block.frames};
  }

  bool publish(const std::size_t slot, const std::size_t frames)
  {
    if (slot >= blocks_.size() || frames > blocks_[slot].size())
    {
      return false;
    }

    queued_frames_.fetch_add(frames, std::memory_order_relaxed);
    if (filled_blocks_.push({slot, frames}))
    {
      return true;
    }
    queued_frames_.fetch_sub(frames, std::memory_order_relaxed);
    return false;
  }

  bool consume(PlaybackBlockRef& block)
  {
    if (!filled_blocks_.pop(block))
    {
      return false;
    }
    const std::size_t previous = queued_frames_.fetch_sub(block.frames, std::memory_order_relaxed);
    assert(previous >= block.frames);
    (void)previous;
    return true;
  }

  bool release(const std::size_t slot)
  {
    if (slot >= blocks_.size())
    {
      return false;
    }
    return free_blocks_.push(slot);
  }

  std::size_t filledCount() const { return filled_blocks_.size(); }
  std::size_t queuedFrames() const { return queued_frames_.load(std::memory_order_relaxed); }
  std::size_t blockCount() const { return blocks_.size(); }

 private:
  static std::size_t NextPow2(const std::size_t minimum)
  {
    std::size_t value = 1U;
    while (value < minimum)
    {
      value <<= 1U;
    }
    return value;
  }

  std::vector<std::vector<dsp::StereoSample>> blocks_;
  rt::SpscRing<std::size_t> free_blocks_;
  rt::SpscRing<PlaybackBlockRef> filled_blocks_;
  std::atomic<std::size_t> queued_frames_{0};
};
}  // namespace sonitude::audio
