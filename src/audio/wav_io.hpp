#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/format_convert.hpp"

namespace sonitude::audio
{
// Bounds the encoded PCM payload accepted by the decoder. This prevents a WAV
// chunk declaration from driving unbounded allocation; decoded float storage
// may require up to twice this amount for 16-bit input.
inline constexpr std::size_t kMaxDecodedPcmBytes = 64U * 1024U * 1024U;

struct WavData
{
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t channels = 0;
  PcmFormat format = PcmFormat::FLOAT32_LE;
  std::vector<float> interleaved;
};

void WriteWavFile(const std::string& path, const WavData& data);
WavData ReadWavFile(const std::string& path);
WavData ReadWavBytes(const std::uint8_t* data, std::size_t size);
} // namespace sonitude::audio
