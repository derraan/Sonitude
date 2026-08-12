#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "audio/audio_types.hpp"
#include "audio/format_convert.hpp"

namespace sonitude::audio
{
inline void ExtractActiveMicFrames(const std::uint8_t* interleaved,
                                   const std::size_t frame_count,
                                   const std::size_t container_channels,
                                   const std::vector<std::size_t>& active_map,
                                   const PcmFormat format,
                                   std::span<MicFrame> out_frames)
{
  if (active_map.size() != kMicChannels)
  {
    throw std::runtime_error("active map must contain six channels");
  }
  for (const std::size_t index : active_map)
  {
    if (index >= container_channels)
    {
      throw std::runtime_error("active map index exceeds container channel count");
    }
  }
  if (out_frames.size() < frame_count)
  {
    throw std::runtime_error("output frame span is smaller than requested frame_count");
  }

  const std::size_t bps = BytesPerSample(format);
  const std::size_t frame_stride = container_channels * bps;
  for (std::size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx)
  {
    MicFrame frame{};
    const std::uint8_t* frame_ptr = interleaved + (frame_idx * frame_stride);
    for (std::size_t mic = 0; mic < kMicChannels; ++mic)
    {
      const std::size_t src_channel = active_map[mic];
      frame[mic] = DecodeOneSample(frame_ptr + (src_channel * bps), format);
    }
    out_frames[frame_idx] = frame;
  }
}

inline std::vector<MicFrame> ExtractActiveMicFrames(const std::uint8_t* interleaved,
                                                    const std::size_t frame_count,
                                                    const std::size_t container_channels,
                                                    const std::vector<std::size_t>& active_map,
                                                    const PcmFormat format)
{
  std::vector<MicFrame> out(frame_count);
  ExtractActiveMicFrames(
      interleaved, frame_count, container_channels, active_map, format, std::span<MicFrame>(out));
  return out;
}
}  // namespace sonitude::audio
