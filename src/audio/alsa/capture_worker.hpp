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
  Ready = 0,
  Interrupted = 1,
  Timeout = 2,
  Error = 3,
};

class CaptureWorker
{
 public:
  CaptureWorker(AlsaPcmDevice* device, const app::RuntimeConfig* config, rt::TelemetryCounters* counters);
  std::size_t periodFrames() const { return period_frames_; }
  bool readBlock(std::span<audio::MicFrame> out_frames, std::size_t* frames_read);
  bool readBlock(std::vector<audio::MicFrame>& out_frames);

#if defined(__linux__)
  bool prepareWaiting();
  bool waitingSupported() const { return alsa_descriptor_count_ > 0; }
  CaptureWait waitForData(int wake_fd, int timeout_ms);
#endif

 private:
  AlsaPcmDevice* device_ = nullptr;
  const app::RuntimeConfig* config_ = nullptr;
  rt::TelemetryCounters* counters_ = nullptr;
  std::size_t period_frames_ = 0;
  std::vector<std::uint8_t> interleaved_;
#if defined(__linux__)
  std::vector<pollfd> poll_descriptors_;
  int alsa_descriptor_count_ = 0;
#endif
};
}  // namespace sonitude::audio::alsa
