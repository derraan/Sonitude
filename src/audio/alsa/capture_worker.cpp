#include "audio/alsa/capture_worker.hpp"

#include <stdexcept>

#if defined(__linux__)
#include <cerrno>
#endif

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

  if (config_->active_channel_map.size() != audio::kMicChannels)
  {
    throw std::runtime_error("active_channel_map must contain six channels");
  }
  for (const std::size_t index : config_->active_channel_map)
  {
    if (index >= negotiated.channels)
    {
      throw std::runtime_error("active_channel_map index exceeds the negotiated capture channel count");
    }
  }
}

#if defined(__linux__)
bool CaptureWorker::prepareWaiting()
{
  const int count = device_->pollDescriptorCount();
  if (count <= 0)
  {
    return false;
  }
  poll_descriptors_.assign(static_cast<std::size_t>(count) + 1U, pollfd{});
  if (!device_->fillPollDescriptors(poll_descriptors_.data(), count))
  {
    poll_descriptors_.clear();
    return false;
  }
  alsa_descriptor_count_ = count;
  const int start_rc = device_->start();
  if (start_rc < 0 && start_rc != -EBADFD)
  {
    (void)device_->prepare();
    (void)device_->start();
  }
  return true;
}

CaptureWait CaptureWorker::waitForData(const int wake_fd, const int timeout_ms)
{
  if (alsa_descriptor_count_ <= 0)
  {
    return CaptureWait::Error;
  }
  if (!device_->fillPollDescriptors(poll_descriptors_.data(), alsa_descriptor_count_))
  {
    return CaptureWait::Error;
  }
  const std::size_t wake_index = static_cast<std::size_t>(alsa_descriptor_count_);
  poll_descriptors_[wake_index].fd = wake_fd;
  poll_descriptors_[wake_index].events = POLLIN;
  poll_descriptors_[wake_index].revents = 0;

  for (;;)
  {
    const int rc = ::poll(poll_descriptors_.data(),
                          static_cast<nfds_t>(poll_descriptors_.size()),
                          timeout_ms);
    if (rc < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      return CaptureWait::Error;
    }
    if (rc == 0)
    {
      return CaptureWait::Timeout;
    }
    if ((poll_descriptors_[wake_index].revents & POLLIN) != 0)
    {
      return CaptureWait::Interrupted;
    }
    unsigned short revents = 0;
    if (!device_->pollRevents(poll_descriptors_.data(), alsa_descriptor_count_, &revents))
    {
      return CaptureWait::Error;
    }
    if ((revents & POLLERR) != 0 || (revents & POLLIN) != 0)
    {
      return CaptureWait::Ready;
    }
  }
}
#endif

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
