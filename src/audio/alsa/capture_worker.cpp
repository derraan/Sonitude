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

  // ExtractActiveMicFrames validates the channel map on every call and throws if
  // it is malformed. Neither the map nor the negotiated container can change
  // after this point, so checking here once is what makes those throws
  // unreachable from the realtime loop rather than merely unlikely.
  if (config_->active_channel_map.size() != audio::kMicChannels)
  {
    throw std::runtime_error("active_channel_map must contain six channels");
  }
  for (const std::size_t index : config_->active_channel_map)
  {
    if (index >= negotiated.channels)
    {
      throw std::runtime_error(
          "active_channel_map index exceeds the negotiated capture channel count");
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
  // One extra slot for the shutdown descriptor. Allocated here, at startup, and
  // never resized afterwards.
  poll_descriptors_.assign(static_cast<std::size_t>(count) + 1U, pollfd{});
  if (!device_->fillPollDescriptors(poll_descriptors_.data(), count))
  {
    poll_descriptors_.clear();
    return false;
  }
  alsa_descriptor_count_ = count;
  // A poll-driven capture must be explicitly started: without this the device
  // would only begin on the first read, so the first poll would block until the
  // timeout rather than reporting data.
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
  // ALSA may hand out different descriptors after a recovery, so refresh them
  // each wait. This is a userspace copy into an already-sized buffer.
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
    // Shutdown wins over data: the caller is about to tear the device down.
    if ((poll_descriptors_[wake_index].revents & POLLIN) != 0)
    {
      return CaptureWait::Interrupted;
    }
    unsigned short revents = 0;
    if (!device_->pollRevents(poll_descriptors_.data(), alsa_descriptor_count_, &revents))
    {
      return CaptureWait::Error;
    }
    if ((revents & POLLERR) != 0)
    {
      // Let the read path observe and recover the xrun.
      return CaptureWait::Ready;
    }
    if ((revents & POLLIN) != 0)
    {
      return CaptureWait::Ready;
    }
    // Spurious wakeup with nothing to report; wait again.
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
    // Keep the bounds check, but do not throw from a steady-state realtime path:
    // unwinding here would allocate and would escape into the audio loop. The
    // destination is sized from the negotiated period, so this indicates a
    // wiring error and is surfaced as a counted, non-fatal refusal instead.
    counters_->capture_overflow_refusals.fetch_add(1, std::memory_order_relaxed);
    return false;
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
