#pragma once

#include <array>
#include <cstdint>

namespace sonitude::audio
{
constexpr std::size_t kMicChannels = 6;

using Sample = float;
using MicFrame = std::array<Sample, kMicChannels>;

struct CaptureSequenceInfo
{
  std::uint64_t sequence = 0;
  std::uint64_t capture_timestamp_ns = 0;
};

struct BeamformerSteering
{
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
};

struct StereoFrame
{
  Sample left = 0.0F;
  Sample right = 0.0F;
};
}  // namespace sonitude::audio
