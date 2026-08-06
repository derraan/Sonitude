#pragma once

#include <vector>

#include "audio/alsa/alsa_device.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/resampler.hpp"
#include "rt/telemetry.hpp"

namespace sonitude::audio::alsa
{
class PlaybackWorker
{
 public:
  PlaybackWorker(AlsaPcmDevice* device,
                 dsp::IStereoResampler* resampler,
                 dsp::AsrcController* controller,
                 rt::TelemetryCounters* counters);

  bool writeStereo(const std::vector<dsp::StereoSample>& input, std::size_t occupancy_frames);

 private:
  AlsaPcmDevice* device_ = nullptr;
  dsp::IStereoResampler* resampler_ = nullptr;
  dsp::AsrcController* controller_ = nullptr;
  rt::TelemetryCounters* counters_ = nullptr;
};
}  // namespace sonitude::audio::alsa
