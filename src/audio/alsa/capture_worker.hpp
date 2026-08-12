#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "app/config.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/audio_types.hpp"
#include "audio/channel_extractor.hpp"
#include "rt/telemetry.hpp"

namespace sonitude::audio::alsa
{
class CaptureWorker
{
 public:
  CaptureWorker(AlsaPcmDevice* device, const app::RuntimeConfig* config, rt::TelemetryCounters* counters);
  std::size_t periodFrames() const { return period_frames_; }
  bool readBlock(std::span<audio::MicFrame> out_frames, std::size_t* frames_read);
  bool readBlock(std::vector<audio::MicFrame>& out_frames);

 private:
  AlsaPcmDevice* device_ = nullptr;
  const app::RuntimeConfig* config_ = nullptr;
  rt::TelemetryCounters* counters_ = nullptr;
  std::size_t period_frames_ = 0;
  std::vector<std::uint8_t> interleaved_;
};
}  // namespace sonitude::audio::alsa
