#include "audio/alsa/alsa_device.hpp"

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

#include <stdexcept>

namespace sonitude::audio::alsa
{
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

void AlsaPcmDevice::configure(const app::DeviceConfig& config, const bool is_capture)
{
#if defined(__linux__)
  close();
  snd_pcm_t* pcm = nullptr;
  const auto stream = is_capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
  if (snd_pcm_open(&pcm, config.alsa_device.c_str(), stream, 0) < 0)
  {
    throw std::runtime_error("Failed to open ALSA device: " + config.alsa_device);
  }
  snd_pcm_hw_params_t* hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);
  snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
  unsigned int rate = config.sample_rate_hz;
  snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
  unsigned int channels = 8;
  snd_pcm_hw_params_set_channels_near(pcm, hw, &channels);
  snd_pcm_uframes_t period = config.period_frames;
  snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
  snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(config.period_frames * config.periods);
  snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
  if (snd_pcm_hw_params(pcm, hw) < 0)
  {
    snd_pcm_close(pcm);
    throw std::runtime_error("Failed to set ALSA hardware parameters");
  }

  negotiated_.sample_rate_hz = rate;
  negotiated_.channels = channels;
  negotiated_.period_frames = static_cast<std::uint32_t>(period);
  negotiated_.buffer_frames = static_cast<std::uint32_t>(buffer);
  negotiated_.format = PcmFormat::S16_LE;
  pcm_ = pcm;
#else
  (void)config;
  (void)is_capture;
  throw std::runtime_error("ALSA is not available on this platform");
#endif
}
}  // namespace sonitude::audio::alsa
