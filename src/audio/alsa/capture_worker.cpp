#include "audio/alsa/capture_worker.hpp"

#include <stdexcept>

namespace sonitude::audio::alsa
{
CaptureWorker::CaptureWorker(AlsaPcmDevice* device,
                             const app::RuntimeConfig* config,
                             rt::TelemetryCounters* counters)
    : device_(device), config_(config), counters_(counters)
{
  const auto negotiated = device_->negotiated();
  period_frames_ = negotiated.period_frames;
  const std::size_t bytes_per_sample = audio::BytesPerSample(negotiated.format);
  const std::size_t frame_bytes = negotiated.channels * bytes_per_sample;
  interleaved_.assign(period_frames_ * frame_bytes, 0);
}

bool CaptureWorker::readBlock(std::span<audio::MicFrame> out_frames, std::size_t* frames_read)
{
  const auto negotiated = device_->negotiated();
  const std::int64_t frame_count = device_->readInterleaved(interleaved_.data(), negotiated.period_frames);
  if (frame_count < 0)
  {
    (void)device_->recoverXrun(static_cast<int>(frame_count));
    counters_->capture_xruns.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const std::size_t produced = static_cast<std::size_t>(frame_count);
  if (out_frames.size() < produced)
  {
    throw std::runtime_error("capture output span does not have enough capacity");
  }

  audio::ExtractActiveMicFrames(interleaved_.data(),
                                produced,
                                negotiated.channels,
                                config_->active_channel_map,
                                negotiated.format,
                                out_frames);
  counters_->capture_frames.fetch_add(static_cast<std::uint64_t>(produced), std::memory_order_relaxed);
  if (frames_read != nullptr)
  {
    *frames_read = produced;
  }
  return true;
}

bool CaptureWorker::readBlock(std::vector<audio::MicFrame>& out_frames)
{
  out_frames.resize(period_frames_);
  std::size_t produced = 0;
  if (!readBlock(std::span<audio::MicFrame>(out_frames), &produced))
  {
    return false;
  }
  out_frames.resize(produced);
  return true;
}
}  // namespace sonitude::audio::alsa
