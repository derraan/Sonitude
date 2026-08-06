#include <exception>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "app/config.hpp"
#include "app/logging.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/alsa/capture_worker.hpp"
#include "audio/alsa/playback_worker.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/resampler.hpp"
#include "rt/telemetry.hpp"

namespace
{
void PrintUsage()
{
  std::cout
      << "sonitude_realtime Milestone 0 scaffold\n"
      << "Usage:\n"
      << "  sonitude_realtime [--config <path>] [--validate-config] [--mode passthrough]\n"
      << "  sonitude_realtime --version\n"
      << "  sonitude_realtime --help\n";
}
}  // namespace

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  bool validate_only = false;
  std::string mode = "scaffold";

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--config")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "Missing value for --config\n";
        return 2;
      }
      config_path = argv[++i];
    }
    else if (arg == "--validate-config")
    {
      validate_only = true;
    }
    else if (arg == "--version")
    {
      std::cout << SONITUDE_VERSION << '\n';
      return 0;
    }
    else if (arg == "--mode")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "Missing value for --mode\n";
        return 2;
      }
      mode = argv[++i];
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else
    {
      std::cerr << "Unknown argument: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }

  try
  {
    sonitude::app::InitializeLogging("info");
    const auto runtime_config = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    const auto geometry = sonitude::app::LoadGeometryFromFile(runtime_config.geometry_path);
    (void)geometry;

    std::cout << "Config validation passed: " << config_path << '\n';
    if (validate_only)
    {
      return 0;
    }

    if (mode != "passthrough")
    {
      std::cout << "Milestone scaffold mode: passthrough not enabled.\n";
      return 0;
    }

#if defined(__linux__)
    sonitude::audio::alsa::AlsaPcmDevice cap;
    sonitude::audio::alsa::AlsaPcmDevice pb;
    cap.openCapture(runtime_config.capture);
    pb.openPlayback(runtime_config.playback);
    sonitude::rt::TelemetryCounters counters;
    sonitude::audio::alsa::CaptureWorker cap_worker(&cap, &runtime_config, &counters);
    auto resampler = sonitude::dsp::CreateSrcResampler();
    sonitude::dsp::AsrcController ctl({
        .min_ratio = runtime_config.asrc.min_ratio,
        .max_ratio = runtime_config.asrc.max_ratio,
        .kp = runtime_config.asrc.pi_kp,
        .ki = runtime_config.asrc.pi_ki,
        .target_buffer_frames = static_cast<double>(runtime_config.asrc.target_buffer_frames),
        .max_ratio_step = 0.00005,
    });
    sonitude::audio::alsa::PlaybackWorker pb_worker(&pb, resampler.get(), &ctl, &counters);
    std::cout << "Running passthrough mode for 30 seconds...\n";
    for (int sec = 0; sec < 30; ++sec)
    {
      std::vector<sonitude::audio::MicFrame> mic_frames;
      if (cap_worker.readBlock(mic_frames))
      {
        std::vector<sonitude::dsp::StereoSample> stereo(mic_frames.size());
        for (std::size_t i = 0; i < mic_frames.size(); ++i)
        {
          stereo[i].left = mic_frames[i][4];
          stereo[i].right = mic_frames[i][5];
        }
        (void)pb_worker.writeStereo(stereo, mic_frames.size());
      }
      if ((sec % 5) == 0)
      {
        std::cout << "telemetry: cap_xruns=" << counters.capture_xruns.load()
                  << " pb_xruns=" << counters.playback_xruns.load()
                  << " asrc_ppm=" << counters.asrc_ratio_ppm.load() << '\n';
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
#else
    std::cout << "Passthrough mode is Linux-only (ALSA).\n";
    return 0;
#endif
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Startup failed: " << ex.what() << '\n';
    return 1;
  }
}
