#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sonitude::audio
{
enum class PcmFormat
{
  S16_LE,
  S24_3LE,
  S32_LE,
  FLOAT32_LE,
};

inline std::size_t BytesPerSample(const PcmFormat format)
{
  switch (format)
  {
  case PcmFormat::S16_LE:
    return 2;
  case PcmFormat::S24_3LE:
    return 3;
  case PcmFormat::S32_LE:
  case PcmFormat::FLOAT32_LE:
    return 4;
  }
  throw std::runtime_error("Unsupported PCM format");
}

inline float ClampFloat(const float value)
{
  return std::max(-1.0F, std::min(1.0F, value));
}

inline float DecodeOneSample(const std::uint8_t* bytes, const PcmFormat format)
{
  switch (format)
  {
  case PcmFormat::S16_LE:
  {
    const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                                     (static_cast<std::uint16_t>(bytes[1]) << 8));
    return static_cast<float>(v) / 32768.0F;
  }
  case PcmFormat::S24_3LE:
  {
    std::uint32_t raw = static_cast<std::uint32_t>(bytes[0]) |
                        (static_cast<std::uint32_t>(bytes[1]) << 8) |
                        (static_cast<std::uint32_t>(bytes[2]) << 16);
    if ((raw & 0x00800000U) != 0U)
    {
      raw |= 0xFF000000U;
    }
    const std::int32_t v = std::bit_cast<std::int32_t>(raw);
    return static_cast<float>(v) / 8388608.0F;
  }
  case PcmFormat::S32_LE:
  {
    const std::uint32_t raw =
        static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    const std::int32_t v = std::bit_cast<std::int32_t>(raw);
    return static_cast<float>(v) / 2147483648.0F;
  }
  case PcmFormat::FLOAT32_LE:
  {
    const std::uint32_t raw =
        static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    return ClampFloat(std::bit_cast<float>(raw));
  }
  }
  throw std::runtime_error("Unsupported PCM format");
}

inline void EncodeOneSample(const float sample, const PcmFormat format, std::uint8_t* bytes)
{
  const float clamped = ClampFloat(sample);
  switch (format)
  {
  case PcmFormat::S16_LE:
  {
    const auto v = static_cast<std::int16_t>(std::lrint(clamped * 32767.0F));
    bytes[0] = static_cast<std::uint8_t>(v & 0xFF);
    bytes[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    return;
  }
  case PcmFormat::S24_3LE:
  {
    const auto v = static_cast<std::int32_t>(std::lrint(clamped * 8388607.0F));
    bytes[0] = static_cast<std::uint8_t>(v & 0xFF);
    bytes[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    bytes[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    return;
  }
  case PcmFormat::S32_LE:
  {
    const auto v = static_cast<std::int32_t>(std::lrint(clamped * 2147483647.0F));
    bytes[0] = static_cast<std::uint8_t>(v & 0xFF);
    bytes[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    bytes[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    bytes[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    return;
  }
  case PcmFormat::FLOAT32_LE:
  {
    const std::uint32_t raw = std::bit_cast<std::uint32_t>(clamped);
    bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((raw >> 8) & 0xFFU);
    bytes[2] = static_cast<std::uint8_t>((raw >> 16) & 0xFFU);
    bytes[3] = static_cast<std::uint8_t>((raw >> 24) & 0xFFU);
    return;
  }
  }
  throw std::runtime_error("Unsupported PCM format");
}

inline std::vector<float> InterleavedToFloat(const std::uint8_t* input, const std::size_t frames,
                                             const std::size_t channels, const PcmFormat format)
{
  const std::size_t bytes_per_sample = BytesPerSample(format);
  std::vector<float> out(frames * channels, 0.0F);
  for (std::size_t i = 0; i < out.size(); ++i)
  {
    out[i] = DecodeOneSample(input + (i * bytes_per_sample), format);
  }
  return out;
}

inline std::vector<std::uint8_t> FloatToInterleaved(const std::vector<float>& input,
                                                    const PcmFormat format)
{
  const std::size_t bytes_per_sample = BytesPerSample(format);
  std::vector<std::uint8_t> out(input.size() * bytes_per_sample, 0);
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    EncodeOneSample(input[i], format, out.data() + (i * bytes_per_sample));
  }
  return out;
}
} // namespace sonitude::audio
