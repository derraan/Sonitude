#include "audio/alsa/playback_worker.hpp"

#include <cmath>
#include <vector>

#include "audio/format_convert.hpp"

namespace sonitude::audio::alsa
{
PlaybackWorker::PlaybackWorker(AlsaPcmDevice* device,
                               dsp::IStereoResampler* resampler,
                               dsp::AsrcController* controller,
                               rt::TelemetryCounters* counters)
    : device_(device), resampler_(resampler), controller_(controller), counters_(counters)
{
}

bool PlaybackWorker::writeStereo(const std::vector<dsp::StereoSample>& input,
                                 const std::size_t occupancy_frames)
{
  if (input.empty())
  {
    return true;
  }
  const auto negotiated = device_->negotiated();
  const double ratio = controller_->update(static_cast<double>(occupancy_frames));
  counters_->asrc_ratio_ppm.store(static_cast<std::int64_t>(std::llround((ratio - 1.0) * 1'000'000.0)),
                                  std::memory_order_relaxed);
  counters_->ring_occupancy_frames.store(static_cast<std::int64_t>(occupancy_frames),
                                         std::memory_order_relaxed);

  std::vector<dsp::StereoSample> resampled(input.size() * 2U);
  const auto rr = resampler_->process(input.data(), input.size(), resampled.data(), resampled.size(), ratio);
  std::vector<float> interleaved;
  interleaved.reserve(rr.produced * 2U);
  for (std::size_t i = 0; i < rr.produced; ++i)
  {
    interleaved.push_back(resampled[i].left);
    interleaved.push_back(resampled[i].right);
  }
  auto bytes = audio::FloatToInterleaved(interleaved, negotiated.format);
  const std::int64_t written =
      device_->writeInterleaved(bytes.data(), static_cast<std::uint32_t>(rr.produced));
  if (written < 0)
  {
    (void)device_->recoverXrun(static_cast<int>(written));
    counters_->playback_xruns.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  counters_->playback_frames.fetch_add(static_cast<std::uint64_t>(written), std::memory_order_relaxed);
  return true;
}
}  // namespace sonitude::audio::alsa
