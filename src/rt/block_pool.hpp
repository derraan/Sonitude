#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "rt/spsc_ring.hpp"

namespace sonitude::rt
{
template <typename T>
class BlockPool
{
 public:
  explicit BlockPool(const std::size_t block_count) : blocks_(block_count), free_indices_(NextPow2(block_count + 1U))
  {
    for (std::size_t i = 0; i < block_count; ++i)
    {
      (void)free_indices_.push(i);
    }
  }

  T* acquire()
  {
    std::size_t index = 0;
    if (!free_indices_.pop(index))
    {
      return nullptr;
    }
    return &blocks_[index];
  }

  bool release(T* ptr)
  {
    if (ptr == nullptr)
    {
      return false;
    }
    const auto begin = blocks_.data();
    const auto end = blocks_.data() + blocks_.size();
    if (ptr < begin || ptr >= end)
    {
      return false;
    }
    const std::size_t index = static_cast<std::size_t>(ptr - begin);
    return free_indices_.push(index);
  }

  std::size_t blockCount() const { return blocks_.size(); }

 private:
  static std::size_t NextPow2(std::size_t value)
  {
    std::size_t p = 1;
    while (p < value)
    {
      p <<= 1U;
    }
    return p;
  }

  std::vector<T> blocks_;
  SpscRing<std::size_t> free_indices_;
};
}  // namespace sonitude::rt
