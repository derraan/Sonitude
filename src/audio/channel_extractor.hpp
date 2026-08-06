#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "audio/audio_types.hpp"
#include "audio/format_convert.hpp"

namespace sonitude::audio
{
inline std::vector<MicFrame> ExtractActiveMicFrames(const std::uint8_t* interleaved,
                                                    const std::size_t frame_count,
                                                    const std::size_t container_channels,
                                                    const std::vector<std::size_t>& active_map,
                                                    const PcmFormat format)
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

  std::vector<MicFrame> out(frame_count);
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
    out[frame_idx] = frame;
  }
  return out;
}
}  // namespace sonitude::audio
