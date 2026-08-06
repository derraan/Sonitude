#pragma once

#include <algorithm>
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
      const std::int16_t v = static_cast<std::int16_t>(
          static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8));
      return static_cast<float>(v) / 32768.0F;
    }
    case PcmFormat::S24_3LE:
    {
      std::int32_t v = static_cast<std::int32_t>(bytes[0]) |
                       (static_cast<std::int32_t>(bytes[1]) << 8) |
                       (static_cast<std::int32_t>(bytes[2]) << 16);
      if ((v & 0x00800000) != 0)
      {
        v |= 0xFF000000;
      }
      return static_cast<float>(v) / 8388608.0F;
    }
    case PcmFormat::S32_LE:
    {
      const std::int32_t v = static_cast<std::int32_t>(bytes[0]) |
                             (static_cast<std::int32_t>(bytes[1]) << 8) |
                             (static_cast<std::int32_t>(bytes[2]) << 16) |
                             (static_cast<std::int32_t>(bytes[3]) << 24);
      return static_cast<float>(v) / 2147483648.0F;
    }
    case PcmFormat::FLOAT32_LE:
    {
      float out = 0.0F;
      std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(&out);
      dst[0] = bytes[0];
      dst[1] = bytes[1];
      dst[2] = bytes[2];
      dst[3] = bytes[3];
      return ClampFloat(out);
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
      const auto* src = reinterpret_cast<const std::uint8_t*>(&clamped);
      bytes[0] = src[0];
      bytes[1] = src[1];
      bytes[2] = src[2];
      bytes[3] = src[3];
      return;
    }
  }
  throw std::runtime_error("Unsupported PCM format");
}

inline std::vector<float> InterleavedToFloat(const std::uint8_t* input,
                                             const std::size_t frames,
                                             const std::size_t channels,
                                             const PcmFormat format)
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
}  // namespace sonitude::audio
