#include <exception>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "app/logging.hpp"
#include "audio/alsa/alsa_device.hpp"
#include "audio/alsa/capture_worker.hpp"
#include "audio/alsa/playback_worker.hpp"
#include "control/control_loop.hpp"
#include "control/conversation_state_machine.hpp"
#include "control/zones.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/resampler.hpp"
#include "rt/param_snapshot.hpp"
#include "rt/telemetry.hpp"
#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_provider.hpp"

namespace
{
void PrintUsage()
{
  std::cout
      << "sonitude_realtime Milestone 0 scaffold\n"
      << "Usage:\n"
      << "  sonitude_realtime [--config <path>] [--validate-config] [--mode passthrough|beamform]\n"
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

    if (mode != "passthrough" && mode != "beamform")
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

    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }
    const auto calibration = sonitude::app::LoadCalibrationFromFile(runtime_config.calibration_path);
    sonitude::app::ValidateCalibrationConfig(
        calibration, geometry_ids, runtime_config.capture.sample_rate_hz);

    std::unique_ptr<sonitude::spatial::IDoaProvider> provider;
    std::vector<sonitude::spatial::MockDoaEvent> mock_events;
    mock_events.push_back({0, {1, -30.0F, 0.0F, 0.8F, 0}});
    mock_events.push_back({8'000'000'000ULL, {1, 35.0F, 0.0F, 0.85F, 8'000'000'000ULL}});
    if (runtime_config.odas.use_mock_provider)
    {
      provider = std::make_unique<sonitude::spatial::MockDoaProvider>(std::move(mock_events));
    }
    else
    {
      provider = sonitude::spatial::CreateOdasProvider(runtime_config.odas.endpoint);
    }

    sonitude::rt::SnapshotBuffer<sonitude::control::SteeringSnapshot> steering_buffer({});
    sonitude::rt::SnapshotPublisher<sonitude::control::SteeringSnapshot> steering_writer(&steering_buffer);
    sonitude::rt::SnapshotReader<sonitude::control::SteeringSnapshot> steering_reader(&steering_buffer);

    sonitude::control::ConversationStateMachine conversation(
        runtime_config.state_machine,
        runtime_config.steering.ambient_floor_linear,
        sonitude::control::ZoneMap(runtime_config.zones));
    sonitude::control::ControlLoop control_loop(
        provider.get(),
        steering_writer,
        {.failsafe_timeout_ns =
             static_cast<std::uint64_t>(runtime_config.state_machine.release_hold_ms) * 1'000'000ULL,
         .ambient_floor_linear = runtime_config.steering.ambient_floor_linear},
        &conversation);

    sonitude::dsp::DelaySumBeamformer beamformer;
    beamformer.configure(
        geometry, runtime_config.steering, calibration, runtime_config.capture.sample_rate_hz, 4096);

    std::cout << "Running " << mode << " mode for 30 seconds...\n";
    sonitude::audio::BeamformerSteering last_target{};
    bool have_target = false;
    const auto start_tp = std::chrono::steady_clock::now();
    for (int sec = 0; sec < 30; ++sec)
    {
      std::vector<sonitude::audio::MicFrame> mic_frames;
      if (cap_worker.readBlock(mic_frames))
      {
        std::vector<sonitude::dsp::StereoSample> stereo(mic_frames.size());
        if (mode == "passthrough")
        {
          for (std::size_t i = 0; i < mic_frames.size(); ++i)
          {
            stereo[i].left = mic_frames[i][4];
            stereo[i].right = mic_frames[i][5];
          }
        }
        else
        {
          const auto now_tp = std::chrono::steady_clock::now();
          const auto now_ns = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(now_tp - start_tp).count());
          control_loop.tick(now_ns);
          const auto snapshot = steering_reader.acquire();
          if (!have_target || std::fabs(snapshot.target.azimuth_deg - last_target.azimuth_deg) > 0.01F ||
              std::fabs(snapshot.target.elevation_deg - last_target.elevation_deg) > 0.01F)
          {
            beamformer.setTarget(snapshot.target);
            last_target = snapshot.target;
            have_target = true;
          }
          std::vector<float> mono(mic_frames.size(), 0.0F);
          beamformer.process(mic_frames, mono);
          for (std::size_t i = 0; i < mic_frames.size(); ++i)
          {
            stereo[i].left = mono[i];
            stereo[i].right = mono[i];
          }
        }
        (void)pb_worker.writeStereo(stereo, mic_frames.size());
      }
      if ((sec % 5) == 0)
      {
        std::cout << "telemetry: cap_xruns=" << counters.capture_xruns.load()
                  << " pb_xruns=" << counters.playback_xruns.load()
                  << " asrc_ppm=" << counters.asrc_ratio_ppm.load()
                  << " state=" << static_cast<int>(conversation.state()) << '\n';
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
