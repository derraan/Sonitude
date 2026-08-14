#include <iomanip>
#include <iostream>
#include <string>

#include "app/config.hpp"
#include "audio/alsa/alsa_probe.hpp"

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  if (argc > 2 && std::string(argv[1]) == "--config")
  {
    config_path = argv[2];
  }

  try
  {
    const auto config = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    std::cout << "Sonitude M1 probe using config: " << config_path << "\n";
#if SONITUDE_HAS_ALSA
    const auto cap = sonitude::audio::alsa::ProbeSingleCaptureDevice(config.capture);
    const auto pb = sonitude::audio::alsa::ProbeSinglePlaybackDevice(config.playback);
    auto print_device = [](const sonitude::audio::alsa::DeviceProbeResult& d, const std::string& label) {
      std::cout << "\n[" << label << "] " << d.pcm_name << "\n";
      std::cout << "  channels: " << d.channels_min << " .. " << d.channels_max << "\n";
      std::cout << "  rates:\n";
      for (const auto& r : d.rates)
      {
        std::cout << "    " << std::setw(6) << r.rate_hz << " Hz : " << (r.supported ? "yes" : "no") << "\n";
      }
    };
    print_device(cap, "capture");
    print_device(pb, "playback");
    std::cout << "\nExpected active map: [";
    for (std::size_t i = 0; i < config.active_channel_map.size(); ++i)
    {
      std::cout << config.active_channel_map[i] << (i + 1 < config.active_channel_map.size() ? ", " : "");
    }
    std::cout << "]\n";
    return 0;
#else
    std::cout << "ALSA probing is unavailable in this build (SONITUDE_HAS_ALSA=0).\n";
    (void)config;
    return 0;
#endif
  }
  catch (const std::exception& ex)
  {
    std::cerr << "device_probe failed: " << ex.what() << '\n';
    return 1;
  }
}
