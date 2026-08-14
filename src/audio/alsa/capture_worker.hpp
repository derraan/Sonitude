#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "app/config.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/audio_types.hpp"
#include "audio/channel_extractor.hpp"
#include "rt/telemetry.hpp"

#if defined(__linux__)
#include <poll.h>
#endif

namespace sonitude::audio::alsa
{
enum class CaptureWait : std::uint8_t
{
  Ready = 0,       // the device has a period available
  Interrupted = 1, // the shutdown descriptor fired
  Timeout = 2,     // no data within the bound; caller decides whether that is fatal
  Error = 3,
};

class CaptureWorker
{
public:
  CaptureWorker(AlsaPcmDevice* device, const app::RuntimeConfig* config,
                rt::TelemetryCounters* counters);
  std::size_t periodFrames() const
  {
    return period_frames_;
  }

  bool readBlock(std::span<audio::MicFrame> out_frames, std::size_t* frames_read);
  bool readBlock(std::vector<audio::MicFrame>& out_frames);

#if defined(__linux__)
  // Prepares the poll descriptor set and starts the stream. Called once before
  // the realtime loop, because it allocates. Returns false when the device
  // cannot be polled, in which case waitForData() must not be used.
  bool prepareWaiting();
  bool waitingSupported() const
  {
    return alsa_descriptor_count_ > 0;
  }

  // Blocks until a capture period is available or `wake_fd` becomes readable.
  // This is how a blocked capture is interrupted: the shutdown descriptor is
  // part of the same poll(), so no separate helper thread and no timer polling
  // is needed to break the thread out of the device wait.
  CaptureWait waitForData(int wake_fd, int timeout_ms);
#endif

private:
  AlsaPcmDevice* device_ = nullptr;
  const app::RuntimeConfig* config_ = nullptr;
  rt::TelemetryCounters* counters_ = nullptr;
  std::size_t period_frames_ = 0;
  std::vector<std::uint8_t> interleaved_;
#if defined(__linux__)
  // [0, alsa_descriptor_count_) are the device's descriptors; the last entry is
  // the shutdown descriptor. Sized once, never resized on the realtime path.
  std::vector<pollfd> poll_descriptors_;
  int alsa_descriptor_count_ = 0;
#endif
};
} // namespace sonitude::audio::alsa
