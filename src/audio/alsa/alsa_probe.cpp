#include "audio/alsa/alsa_probe.hpp"

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

#include <cstdio>
#include <stdexcept>

namespace sonitude::audio::alsa
{
#if defined(__linux__)
namespace
{
DeviceProbeResult ProbePcm(const std::string& pcm_name, const snd_pcm_stream_t stream)
{
  snd_pcm_t* pcm = nullptr;
  if (snd_pcm_open(&pcm, pcm_name.c_str(), stream, SND_PCM_NONBLOCK) < 0)
  {
    throw std::runtime_error("Failed to open ALSA PCM: " + pcm_name);
  }
  snd_pcm_hw_params_t* params = nullptr;
  snd_pcm_hw_params_alloca(&params);
  snd_pcm_hw_params_any(pcm, params);

  unsigned int min_channels = 0;
  unsigned int max_channels = 0;
  snd_pcm_hw_params_get_channels_min(params, &min_channels);
  snd_pcm_hw_params_get_channels_max(params, &max_channels);

  DeviceProbeResult out;
  out.pcm_name = pcm_name;
  out.channels_min = min_channels;
  out.channels_max = max_channels;
  out.formats = {"S16_LE", "S24_3LE", "S32_LE"};
  for (const std::uint32_t rate : {16000U, 24000U, 44100U, 48000U, 96000U})
  {
    const int rc = snd_pcm_hw_params_test_rate(pcm, params, rate, 0);
    out.rates.push_back(ProbeRateSupport{.rate_hz = rate, .supported = (rc == 0)});
  }
  snd_pcm_close(pcm);
  return out;
}
}  // namespace
#endif

std::vector<DeviceProbeResult> ProbeAllCapturePcms()
{
#if defined(__linux__)
  std::vector<DeviceProbeResult> out;
  int card = -1;
  if (snd_card_next(&card) < 0)
  {
    return out;
  }
  while (card >= 0)
  {
    char card_name[32];
    std::snprintf(card_name, sizeof(card_name), "hw:%d,0", card);
    try
    {
      out.push_back(ProbePcm(card_name, SND_PCM_STREAM_CAPTURE));
    }
    catch (const std::exception&)
    {
    }
    if (snd_card_next(&card) < 0)
    {
      break;
    }
  }
  return out;
#else
  return {};
#endif
}

DeviceProbeResult ProbeSingleCaptureDevice(const app::DeviceConfig& config)
{
#if defined(__linux__)
  return ProbePcm(config.alsa_device, SND_PCM_STREAM_CAPTURE);
#else
  (void)config;
  return {};
#endif
}

DeviceProbeResult ProbeSinglePlaybackDevice(const app::DeviceConfig& config)
{
#if defined(__linux__)
  return ProbePcm(config.alsa_device, SND_PCM_STREAM_PLAYBACK);
#else
  (void)config;
  return {};
#endif
}
}  // namespace sonitude::audio::alsa
