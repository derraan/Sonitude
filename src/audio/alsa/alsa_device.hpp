#pragma once

#include <cstdint>
#include <string>

#include "app/config.hpp"
#include "audio/format_convert.hpp"

namespace sonitude::audio::alsa
{
struct NegotiatedParams
{
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t channels = 0;
  std::uint32_t period_frames = 0;
  std::uint32_t buffer_frames = 0;
  PcmFormat format = PcmFormat::S16_LE;
};

class AlsaPcmDevice
{
 public:
  AlsaPcmDevice() = default;
  ~AlsaPcmDevice();
  AlsaPcmDevice(const AlsaPcmDevice&) = delete;
  AlsaPcmDevice& operator=(const AlsaPcmDevice&) = delete;

  void openCapture(const app::DeviceConfig& config);
  void openPlayback(const app::DeviceConfig& config);
  void close();

  NegotiatedParams negotiated() const { return negotiated_; }
  int recoverXrun(int error_code) const;
  std::int64_t readInterleaved(std::uint8_t* dst, std::uint32_t frames) const;
  std::int64_t writeInterleaved(const std::uint8_t* src, std::uint32_t frames) const;
  std::int64_t availFrames() const;
  std::size_t playbackQueuedFrames() const;
  void dropStream() const;

 private:
  void configure(const app::DeviceConfig& config, bool is_capture);
  void* pcm_ = nullptr;
  NegotiatedParams negotiated_{};
};
}  // namespace sonitude::audio::alsa
