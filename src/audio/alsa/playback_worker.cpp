#include "audio/alsa/playback_worker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sonitude::audio::alsa
{
namespace
{
constexpr std::size_t kSrcLatencyScratchFrames = 64;
} // namespace

std::size_t PlaybackWorker::CalculateRequiredScratchFrames(const std::size_t capture_period_frames,
                                                           const std::size_t playback_period_frames,
                                                           const double asrc_max_ratio)
{
  const std::size_t base_period = std::max(capture_period_frames, playback_period_frames);
  const std::size_t ratio_scale =
      std::max<std::size_t>(1U, static_cast<std::size_t>(std::ceil(asrc_max_ratio)));
  const std::size_t headroom_frames = base_period;
  return (base_period * ratio_scale) + kSrcLatencyScratchFrames + headroom_frames;
}

PlaybackWorker::PlaybackWorker(AlsaPcmDevice* device, dsp::IStereoResampler* resampler,
                               dsp::AsrcController* controller, rt::TelemetryCounters* counters,
                               const bool asrc_enabled, const std::size_t capture_period_frames,
                               const double asrc_max_ratio)
    : device_(device), resampler_(resampler), controller_(controller), counters_(counters),
      asrc_enabled_(asrc_enabled)
{
  const auto negotiated = device_->negotiated();
  const std::size_t scratch_frames = CalculateRequiredScratchFrames(
      capture_period_frames, negotiated.period_frames, asrc_max_ratio);
  const std::size_t bytes_per_sample = audio::BytesPerSample(negotiated.format);
  pending_.assign(scratch_frames, {});
  resampled_.assign(scratch_frames, {});
  interleaved_float_.assign(resampled_.size() * 2U, 0.0F);
  interleaved_bytes_.assign(resampled_.size() * 2U * bytes_per_sample, 0U);
  if (!asrc_enabled_)
  {
    controller_->reset();
    resampler_->reset();
  }
}

bool PlaybackWorker::writeStereo(std::span<const dsp::StereoSample> input,
                                 const std::size_t occupancy_frames)
{
  if (input.empty())
  {
    return true;
  }

  double ratio = 1.0;
  const dsp::StereoSample* output_samples = input.data();
  std::size_t output_count = input.size();
  if (asrc_enabled_)
  {
    ratio = controller_->update(static_cast<double>(occupancy_frames));
    if (pending_count_ + input.size() > pending_.size())
    {
      return false;
    }
    std::copy(input.begin(), input.end(),
              pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_));
    pending_count_ += input.size();
    const auto rr = resampler_->process(pending_.data(), pending_count_, resampled_.data(),
                                        resampled_.size(), ratio);
    if (rr.consumed > pending_count_)
    {
      return false;
    }
    pending_count_ -= rr.consumed;
    if (pending_count_ > 0U)
    {
      std::memmove(pending_.data(), pending_.data() + rr.consumed,
                   pending_count_ * sizeof(dsp::StereoSample));
    }
    output_samples = resampled_.data();
    output_count = rr.produced;
  }

  const std::int64_t asrc_ratio_ppm =
      static_cast<std::int64_t>(std::llround((ratio - 1.0) * 1'000'000.0));
  counters_->asrc_ratio_ppm.store(asrc_ratio_ppm, std::memory_order_relaxed);
  if (counters_->asrc_ratio_ppm_has_sample.load(std::memory_order_relaxed) == 0U)
  {
    counters_->asrc_ratio_ppm_min.store(asrc_ratio_ppm, std::memory_order_relaxed);
    counters_->asrc_ratio_ppm_max.store(asrc_ratio_ppm, std::memory_order_relaxed);
    counters_->asrc_ratio_ppm_has_sample.store(1U, std::memory_order_relaxed);
  }
  else
  {
    rt::StoreMinRelaxed(counters_->asrc_ratio_ppm_min, asrc_ratio_ppm);
    rt::StoreMaxRelaxed(counters_->asrc_ratio_ppm_max, asrc_ratio_ppm);
  }
  counters_->ring_occupancy_frames.store(static_cast<std::int64_t>(occupancy_frames),
                                         std::memory_order_relaxed);

  if (output_count > resampled_.size())
  {
    return false;
  }

  for (std::size_t i = 0; i < output_count; ++i)
  {
    interleaved_float_[2U * i] = output_samples[i].left;
    interleaved_float_[2U * i + 1U] = output_samples[i].right;
  }
  const auto negotiated = device_->negotiated();
  const std::size_t bps = audio::BytesPerSample(negotiated.format);
  for (std::size_t i = 0; i < output_count * 2U; ++i)
  {
    audio::EncodeOneSample(interleaved_float_[i], negotiated.format,
                           interleaved_bytes_.data() + (i * bps));
  }

  std::size_t total_written = 0;
  const std::size_t frame_bytes = 2U * bps;
  while (total_written < output_count)
  {
    const std::int64_t written =
        device_->writeInterleaved(interleaved_bytes_.data() + (total_written * frame_bytes),
                                  static_cast<std::uint32_t>(output_count - total_written));
    if (written < 0)
    {
      counters_->playback_xruns.fetch_add(1, std::memory_order_relaxed);
      if (device_->recoverXrun(static_cast<int>(written)) < 0)
      {
        controller_->reset();
        resampler_->reset();
        pending_count_ = 0;
        return false;
      }
      continue;
    }
    if (written == 0)
    {
      return false;
    }
    total_written += static_cast<std::size_t>(written);
  }
  counters_->playback_frames.fetch_add(static_cast<std::uint64_t>(total_written),
                                       std::memory_order_relaxed);
  return true;
}

bool PlaybackWorker::writeStereo(const std::vector<dsp::StereoSample>& input,
                                 const std::size_t occupancy_frames)
{
  return writeStereo(std::span<const dsp::StereoSample>(input), occupancy_frames);
}
} // namespace sonitude::audio::alsa
