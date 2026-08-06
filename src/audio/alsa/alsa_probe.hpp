#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/config.hpp"

namespace sonitude::audio::alsa
{
struct ProbeRateSupport
{
  std::uint32_t rate_hz = 0;
  bool supported = false;
};

struct DeviceProbeResult
{
  std::string pcm_name;
  std::string card_name;
  std::uint32_t channels_min = 0;
  std::uint32_t channels_max = 0;
  std::vector<std::string> formats;
  std::vector<ProbeRateSupport> rates;
};

std::vector<DeviceProbeResult> ProbeAllCapturePcms();
DeviceProbeResult ProbeSingleCaptureDevice(const app::DeviceConfig& config);
DeviceProbeResult ProbeSinglePlaybackDevice(const app::DeviceConfig& config);
}  // namespace sonitude::audio::alsa
