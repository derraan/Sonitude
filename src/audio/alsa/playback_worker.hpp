#pragma once

#include <span>
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
  PlaybackWorker(AlsaPcmDevice* device, dsp::IStereoResampler* resampler,
                 dsp::AsrcController* controller, rt::TelemetryCounters* counters,
                 bool asrc_enabled, std::size_t capture_period_frames, double asrc_max_ratio);

  bool writeStereo(std::span<const dsp::StereoSample> input, std::size_t occupancy_frames);
  bool writeStereo(const std::vector<dsp::StereoSample>& input, std::size_t occupancy_frames);
  static std::size_t CalculateRequiredScratchFrames(std::size_t capture_period_frames,
                                                    std::size_t playback_period_frames,
                                                    double asrc_max_ratio);
  std::size_t scratchCapacityFrames() const
  {
    return pending_.size();
  }

private:
  AlsaPcmDevice* device_ = nullptr;
  dsp::IStereoResampler* resampler_ = nullptr;
  dsp::AsrcController* controller_ = nullptr;
  rt::TelemetryCounters* counters_ = nullptr;
  bool asrc_enabled_ = true;
  std::size_t pending_count_ = 0;
  std::vector<dsp::StereoSample> pending_;
  std::vector<dsp::StereoSample> resampled_;
  std::vector<float> interleaved_float_;
  std::vector<std::uint8_t> interleaved_bytes_;
};
} // namespace sonitude::audio::alsa
