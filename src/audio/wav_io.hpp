#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/format_convert.hpp"

namespace sonitude::audio
{
struct WavData
{
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t channels = 0;
  PcmFormat format = PcmFormat::FLOAT32_LE;
  std::vector<float> interleaved;
};

void WriteWavFile(const std::string& path, const WavData& data);
WavData ReadWavFile(const std::string& path);
}  // namespace sonitude::audio
