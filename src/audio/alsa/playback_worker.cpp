#include "audio/alsa/playback_worker.hpp"

#include <cmath>

namespace sonitude::audio::alsa
{
PlaybackWorker::PlaybackWorker(AlsaPcmDevice* device,
                               dsp::IStereoResampler* resampler,
                               dsp::AsrcController* controller,
                               rt::TelemetryCounters* counters,
                               const bool asrc_enabled)
    : device_(device),
      resampler_(resampler),
      controller_(controller),
      counters_(counters),
      asrc_enabled_(asrc_enabled)
{
  const auto negotiated = device_->negotiated();
  const std::size_t period = negotiated.period_frames;
  const std::size_t bytes_per_sample = audio::BytesPerSample(negotiated.format);
  resampled_.assign(period * 2U, {});
  interleaved_float_.assign(period * 2U, 0.0F);
  interleaved_bytes_.assign(period * 2U * bytes_per_sample, 0U);
}

bool PlaybackWorker::writeStereo(std::span<const dsp::StereoSample> input, const std::size_t occupancy_frames)
{
  if (input.empty())
  {
    return true;
  }
  double ratio = 1.0;
  if (asrc_enabled_)
  {
    ratio = controller_->update(static_cast<double>(occupancy_frames));
  }
  else
  {
    controller_->reset();
    resampler_->reset();
  }
  counters_->asrc_ratio_ppm.store(static_cast<std::int64_t>(std::llround((ratio - 1.0) * 1'000'000.0)),
                                  std::memory_order_relaxed);
  counters_->ring_occupancy_frames.store(static_cast<std::int64_t>(occupancy_frames),
                                         std::memory_order_relaxed);

  if (resampled_.size() < input.size() * 2U)
  {
    resampled_.resize(input.size() * 2U);
    interleaved_float_.resize(input.size() * 2U);
    const auto negotiated = device_->negotiated();
    const std::size_t bytes_per_sample = audio::BytesPerSample(negotiated.format);
    interleaved_bytes_.resize(input.size() * 2U * bytes_per_sample);
  }
  const auto rr =
      resampler_->process(input.data(), input.size(), resampled_.data(), resampled_.size(), ratio);

  for (std::size_t i = 0; i < rr.produced; ++i)
  {
    interleaved_float_[2U * i] = resampled_[i].left;
    interleaved_float_[2U * i + 1U] = resampled_[i].right;
  }
  const auto negotiated = device_->negotiated();
  const std::size_t bps = audio::BytesPerSample(negotiated.format);
  for (std::size_t i = 0; i < rr.produced * 2U; ++i)
  {
    audio::EncodeOneSample(interleaved_float_[i], negotiated.format, interleaved_bytes_.data() + (i * bps));
  }

  const std::int64_t written =
      device_->writeInterleaved(interleaved_bytes_.data(), static_cast<std::uint32_t>(rr.produced));
  if (written < 0)
  {
    (void)device_->recoverXrun(static_cast<int>(written));
    controller_->reset();
    resampler_->reset();
    counters_->playback_xruns.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  counters_->playback_frames.fetch_add(static_cast<std::uint64_t>(written), std::memory_order_relaxed);
  return true;
}

bool PlaybackWorker::writeStereo(const std::vector<dsp::StereoSample>& input,
                                 const std::size_t occupancy_frames)
{
  return writeStereo(std::span<const dsp::StereoSample>(input), occupancy_frames);
}
}  // namespace sonitude::audio::alsa
