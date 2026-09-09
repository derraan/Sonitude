#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/format_convert.hpp"

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  if (argc > 2 && std::string(argv[1]) == "--config")
  {
    config_path = argv[2];
  }

#if !defined(__linux__)
  std::cout << "playback_check is Linux-only (ALSA).\n";
  return 0;
#else
  try
  {
    const auto config = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    sonitude::audio::alsa::AlsaPcmDevice dev;
    dev.openPlayback(config.playback);
    const auto negotiated = dev.negotiated();

    const std::size_t frames = negotiated.period_frames * 100U;
    std::vector<float> stereo(frames * 2U, 0.0F);
    for (std::size_t i = 0; i < frames; ++i)
    {
      const float t = static_cast<float>(i) / static_cast<float>(negotiated.sample_rate_hz);
      const float s = 0.2F * std::sin(2.0F * 3.1415926535F * 1000.0F * t);
      stereo[(i * 2U) + 0U] = s;
      stereo[(i * 2U) + 1U] = s;
    }
    const auto bytes = sonitude::audio::FloatToInterleaved(stereo, negotiated.format);
    const std::size_t frame_bytes = 2U * sonitude::audio::BytesPerSample(negotiated.format);
    std::size_t written_total = 0;
    std::size_t stall_count = 0;
    std::size_t recoveries = 0;
    while (written_total < frames)
    {
      const std::size_t remaining = frames - written_total;
      const auto* cursor = bytes.data() + (written_total * frame_bytes);
      const std::int64_t written = dev.writeInterleaved(
          cursor, static_cast<std::uint32_t>(std::min<std::size_t>(remaining, negotiated.period_frames)));
      if (written < 0)
      {
        const int recovered = dev.recoverXrun(static_cast<int>(written));
        ++recoveries;
        if (recovered < 0)
        {
          std::cerr << "Playback write failed and could not recover xrun: " << recovered << '\n';
          return 1;
        }
        continue;
      }
      if (written == 0)
      {
        ++stall_count;
        if (stall_count > 50U)
        {
          std::cerr << "Playback device did not accept the full buffer after repeated retries.\n";
          return 1;
        }
        continue;
      }
      stall_count = 0;
      written_total += static_cast<std::size_t>(written);
    }
    std::cout << "Playback check wrote " << written_total << " frames (recoveries=" << recoveries << ").\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "playback_check failed: " << ex.what() << '\n';
    return 1;
  }
#endif
}
