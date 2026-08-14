#include <chrono>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "app/config.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/alsa/capture_worker.hpp"
#include "audio/alsa/playback_worker.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/resampler.hpp"
#include "rt/telemetry.hpp"

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  if (argc > 2 && std::string(argv[1]) == "--config")
  {
    config_path = argv[2];
  }

#if !defined(__linux__)
  std::cout << "loopback_diag is Linux-only (ALSA).\n";
  return 0;
#else
  try
  {
    const auto config = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    std::cout << "DIAGNOSTIC ONLY: pre-ASRC loopback is not production-stable.\n";

    sonitude::audio::alsa::AlsaPcmDevice cap;
    sonitude::audio::alsa::AlsaPcmDevice pb;
    cap.openCapture(config.capture);
    pb.openPlayback(config.playback);
    const auto cap_params = cap.negotiated();
    const auto pb_params = pb.negotiated();
    sonitude::app::ValidateRuntimeAudioContract(
        config,
        {.capture_sample_rate_hz = cap_params.sample_rate_hz,
         .playback_sample_rate_hz = pb_params.sample_rate_hz,
         .capture_channels = cap_params.channels,
         .playback_buffer_frames = pb_params.buffer_frames,
         .software_queue_frames = 0,
         .minimum_asrc_headroom_frames = cap_params.period_frames});

    sonitude::rt::TelemetryCounters counters;
    sonitude::audio::alsa::CaptureWorker cap_worker(&cap, &config, &counters);
    auto resampler = sonitude::dsp::CreateSrcResampler();
    sonitude::dsp::AsrcController ctl({
        .min_ratio = config.asrc.min_ratio,
        .max_ratio = config.asrc.max_ratio,
        .kp = config.asrc.pi_kp,
        .ki = config.asrc.pi_ki,
        .target_buffer_frames = static_cast<double>(config.asrc.target_buffer_frames),
        .max_ratio_step = 0.00005,
    });
    const bool asrc_enabled =
        config.asrc.enabled && !config.asrc.allow_bypass_for_locked_bench;
    sonitude::audio::alsa::PlaybackWorker pb_worker(
        &pb,
        resampler.get(),
        &ctl,
        &counters,
        asrc_enabled,
        cap_params.period_frames,
        config.asrc.max_ratio);

    const std::size_t period_frames = cap_worker.periodFrames();
    std::vector<sonitude::audio::MicFrame> mic_frames(period_frames);
    std::vector<sonitude::dsp::StereoSample> stereo(period_frames);
    std::size_t playback_write_failures = 0;

    const auto t0 = std::chrono::steady_clock::now();
    while (true)
    {
      const auto elapsed = std::chrono::steady_clock::now() - t0;
      if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 60)
      {
        break;
      }
      std::size_t frames_read = 0;
      if (!cap_worker.readBlock(std::span<sonitude::audio::MicFrame>(mic_frames), &frames_read))
      {
        continue;
      }
      for (std::size_t i = 0; i < frames_read; ++i)
      {
        stereo[i].left = mic_frames[i][4];
        stereo[i].right = mic_frames[i][5];
      }
      const std::size_t occupancy = pb.playbackQueuedFrames();
      if (!pb_worker.writeStereo(
              std::span<const sonitude::dsp::StereoSample>(stereo.data(), frames_read), occupancy))
      {
        ++playback_write_failures;
      }
    }

    std::cout << "Loopback diagnostic complete: "
              << "capture_xruns=" << counters.capture_xruns.load()
              << " playback_xruns=" << counters.playback_xruns.load()
              << " playback_write_failures=" << playback_write_failures
              << " asrc_ppm=" << counters.asrc_ratio_ppm.load() << "\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "loopback_diag failed: " << ex.what() << '\n';
    return 1;
  }
#endif
}
