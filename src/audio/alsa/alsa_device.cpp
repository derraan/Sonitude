#include "audio/alsa/alsa_device.hpp"

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

#include <memory>
#include <stdexcept>
#include <string>

namespace sonitude::audio::alsa
{
#if defined(__linux__)
namespace
{
void RequireAlsa(const int rc, const char* what)
{
  if (rc < 0)
  {
    throw std::runtime_error(std::string("ALSA ") + what + " failed: " + snd_strerror(rc));
  }
}

PcmFormat ToPcmFormat(const snd_pcm_format_t format)
{
  switch (format)
  {
    case SND_PCM_FORMAT_S16_LE:
      return PcmFormat::S16_LE;
    case SND_PCM_FORMAT_S24_3LE:
      return PcmFormat::S24_3LE;
    case SND_PCM_FORMAT_S32_LE:
      return PcmFormat::S32_LE;
    case SND_PCM_FORMAT_FLOAT_LE:
      return PcmFormat::FLOAT32_LE;
    default:
      throw std::runtime_error("Unsupported negotiated ALSA sample format");
  }
}
}  // namespace
#endif

AlsaPcmDevice::~AlsaPcmDevice()
{
  close();
}

void AlsaPcmDevice::openCapture(const app::DeviceConfig& config)
{
  configure(config, true);
}

void AlsaPcmDevice::openPlayback(const app::DeviceConfig& config)
{
  configure(config, false);
}

void AlsaPcmDevice::close()
{
#if defined(__linux__)
  if (pcm_ != nullptr)
  {
    snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
    pcm_ = nullptr;
  }
#endif
}

int AlsaPcmDevice::recoverXrun(const int error_code) const
{
#if defined(__linux__)
  if (pcm_ == nullptr)
  {
    return error_code;
  }
  return snd_pcm_recover(static_cast<snd_pcm_t*>(pcm_), error_code, 1);
#else
  return error_code;
#endif
}

std::int64_t AlsaPcmDevice::readInterleaved(std::uint8_t* dst, const std::uint32_t frames) const
{
#if defined(__linux__)
  if (pcm_ == nullptr)
  {
    return -1;
  }
  return snd_pcm_readi(static_cast<snd_pcm_t*>(pcm_), dst, frames);
#else
  (void)dst;
  (void)frames;
  return -1;
#endif
}

std::int64_t AlsaPcmDevice::writeInterleaved(const std::uint8_t* src, const std::uint32_t frames) const
{
#if defined(__linux__)
  if (pcm_ == nullptr)
  {
    return -1;
  }
  return snd_pcm_writei(static_cast<snd_pcm_t*>(pcm_), src, frames);
#else
  (void)src;
  (void)frames;
  return -1;
#endif
}

std::int64_t AlsaPcmDevice::availFrames() const
{
#if defined(__linux__)
  if (pcm_ == nullptr)
  {
    return -1;
  }
  return snd_pcm_avail(static_cast<snd_pcm_t*>(pcm_));
#else
  return -1;
#endif
}

std::size_t AlsaPcmDevice::playbackQueuedFrames() const
{
  const std::int64_t avail = availFrames();
  if (avail < 0)
  {
    return 0;
  }
  const std::int64_t queued = static_cast<std::int64_t>(negotiated_.buffer_frames) - avail;
  if (queued <= 0)
  {
    return 0;
  }
  return static_cast<std::size_t>(queued);
}

void AlsaPcmDevice::dropStream() const
{
#if defined(__linux__)
  if (pcm_ != nullptr)
  {
    (void)snd_pcm_drop(static_cast<snd_pcm_t*>(pcm_));
  }
#endif
}

void AlsaPcmDevice::configure(const app::DeviceConfig& config, const bool is_capture)
{
#if defined(__linux__)
  close();
  snd_pcm_t* pcm = nullptr;
  const auto stream = is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  const int open_rc = snd_pcm_open(&pcm, config.alsa_device.c_str(), stream, 0);
  if (open_rc < 0)
  {
    throw std::runtime_error(
        "Failed to open ALSA device: " + config.alsa_device + ": " + snd_strerror(open_rc));
  }
  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm_guard(pcm, &snd_pcm_close);
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  RequireAlsa(snd_pcm_hw_params_any(pcm, hw), "snd_pcm_hw_params_any");
  RequireAlsa(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "set_access");
  RequireAlsa(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE), "set_format");
  unsigned int rate = config.sample_rate_hz;
  RequireAlsa(snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr), "set_rate_near");
  unsigned int channels = is_capture ? 8U : 2U;
  const int channel_result = is_capture ? snd_pcm_hw_params_set_channels_near(pcm, hw, &channels)
                                        : snd_pcm_hw_params_set_channels(pcm, hw, channels);
  if (channel_result < 0)
  {
    throw std::runtime_error(std::string(is_capture ? "Capture device channel negotiation failed: "
                                                    : "Playback device channel configuration failed: ") +
                             snd_strerror(channel_result));
  }
  if (!is_capture && channels != 2U)
  {
    throw std::runtime_error("Playback device does not support required stereo output");
  }
  snd_pcm_uframes_t period = config.period_frames;
  RequireAlsa(snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr), "set_period_size_near");
  snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(config.period_frames * config.periods);
  RequireAlsa(snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer), "set_buffer_size_near");
  RequireAlsa(snd_pcm_hw_params(pcm, hw), "snd_pcm_hw_params");

  snd_pcm_format_t applied_format = SND_PCM_FORMAT_UNKNOWN;
  RequireAlsa(snd_pcm_hw_params_get_format(hw, &applied_format), "get_format");
  unsigned int applied_rate = 0;
  int rate_dir = 0;
  RequireAlsa(snd_pcm_hw_params_get_rate(hw, &applied_rate, &rate_dir), "get_rate");
  unsigned int applied_channels = 0;
  RequireAlsa(snd_pcm_hw_params_get_channels(hw, &applied_channels), "get_channels");
  snd_pcm_uframes_t applied_period = 0;
  int period_dir = 0;
  RequireAlsa(snd_pcm_hw_params_get_period_size(hw, &applied_period, &period_dir), "get_period_size");
  snd_pcm_uframes_t applied_buffer = 0;
  RequireAlsa(snd_pcm_hw_params_get_buffer_size(hw, &applied_buffer), "get_buffer_size");

  if (!is_capture && applied_channels != 2U)
  {
    throw std::runtime_error("Playback device negotiated non-stereo channel count");
  }
  if (applied_period == 0 || applied_buffer == 0)
  {
    throw std::runtime_error("ALSA negotiated zero period or buffer size");
  }

  negotiated_.sample_rate_hz = applied_rate;
  negotiated_.channels = applied_channels;
  negotiated_.period_frames = static_cast<std::uint32_t>(applied_period);
  negotiated_.buffer_frames = static_cast<std::uint32_t>(applied_buffer);
  negotiated_.format = ToPcmFormat(applied_format);
  pcm_ = pcm_guard.release();
#else
  (void)config;
  (void)is_capture;
  throw std::runtime_error("ALSA is not available on this platform");
#endif
}
}  // namespace sonitude::audio::alsa
