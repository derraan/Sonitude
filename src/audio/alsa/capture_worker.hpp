#pragma once

#include <atomic>
#include <cstdint>
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
  bool readBlock(std::vector<audio::MicFrame>& out_frames);

 private:
  AlsaPcmDevice* device_ = nullptr;
  const app::RuntimeConfig* config_ = nullptr;
  rt::TelemetryCounters* counters_ = nullptr;
};
}  // namespace sonitude::audio::alsa
