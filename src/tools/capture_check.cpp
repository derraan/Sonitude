#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/alsa/capture_worker.hpp"
#include "audio/wav_io.hpp"
#include "rt/telemetry.hpp"

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  if (argc > 2 && std::string(argv[1]) == "--config")
  {
    config_path = argv[2];
  }

#if !defined(__linux__)
  std::cout << "capture_check is Linux-only (ALSA).\n";
  return 0;
#else
  try
  {
    const auto config = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    sonitude::audio::alsa::AlsaPcmDevice dev;
    dev.openCapture(config.capture);
    sonitude::rt::TelemetryCounters counters;
    sonitude::audio::alsa::CaptureWorker worker(&dev, &config, &counters);

    std::vector<float> interleaved;
    std::vector<double> sum_abs(8, 0.0);
    std::vector<double> max_abs(8, 0.0);
    constexpr std::size_t iterations = 100;
    for (std::size_t n = 0; n < iterations; ++n)
    {
      std::vector<sonitude::audio::MicFrame> frames;
      if (!worker.readBlock(frames))
      {
        continue;
      }
      for (const auto& frame : frames)
      {
        for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
        {
          const double v = std::fabs(frame[ch]);
          sum_abs[ch] += v;
          max_abs[ch] = std::max(max_abs[ch], v);
          interleaved.push_back(frame[ch]);
        }
      }
    }

    std::cout << "Capture check summary (" << counters.capture_frames.load() << " frames):\n";
    for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
    {
      const double avg = sum_abs[ch] / std::max<std::uint64_t>(1, counters.capture_frames.load());
      std::cout << "  ch" << ch << " avg_abs=" << avg << " max_abs=" << max_abs[ch] << "\n";
    }
    sonitude::audio::WavData wav;
    wav.sample_rate_hz = dev.negotiated().sample_rate_hz;
    wav.channels = sonitude::audio::kMicChannels;
    wav.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    wav.interleaved = interleaved;
    sonitude::audio::WriteWavFile("build/capture_check.wav", wav);
    std::cout << "Wrote build/capture_check.wav\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "capture_check failed: " << ex.what() << '\n';
    return 1;
  }
#endif
}
