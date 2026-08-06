#include "audio/alsa/capture_worker.hpp"

namespace sonitude::audio::alsa
{
CaptureWorker::CaptureWorker(AlsaPcmDevice* device,
                             const app::RuntimeConfig* config,
                             rt::TelemetryCounters* counters)
    : device_(device), config_(config), counters_(counters)
{
}

bool CaptureWorker::readBlock(std::vector<audio::MicFrame>& out_frames)
{
  const auto negotiated = device_->negotiated();
  const std::size_t bytes_per_sample = audio::BytesPerSample(negotiated.format);
  const std::size_t frame_bytes = negotiated.channels * bytes_per_sample;
  std::vector<std::uint8_t> interleaved(negotiated.period_frames * frame_bytes, 0);
  const auto frames_read = device_->readInterleaved(interleaved.data(), negotiated.period_frames);
  if (frames_read < 0)
  {
    (void)device_->recoverXrun(static_cast<int>(frames_read));
    counters_->capture_xruns.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  out_frames = audio::ExtractActiveMicFrames(interleaved.data(),
                                             static_cast<std::size_t>(frames_read),
                                             negotiated.channels,
                                             config_->active_channel_map,
                                             negotiated.format);
  counters_->capture_frames.fetch_add(static_cast<std::uint64_t>(frames_read), std::memory_order_relaxed);
  return true;
}
}  // namespace sonitude::audio::alsa
