#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
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
#include "dsp/binaural_renderer.hpp"
#include "dsp/hrtf_table.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/limiter.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/resampler.hpp"
#include "dsp/suppression_stage.hpp"
#include "rt/param_snapshot.hpp"
#include "rt/spsc_ring.hpp"
#include "rt/telemetry.hpp"
#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_provider.hpp"

namespace
{
#if defined(__linux__)
std::atomic<bool> g_running{true};

void SignalStop(const int) { g_running.store(false, std::memory_order_relaxed); }

struct PlaybackBlockRef
{
  std::size_t slot = 0;
  std::size_t frames = 0;
};

sonitude::dsp::BinauralBackend ParseBinauralBackend(const std::string& backend)
{
  if (backend == "itd_ild")
  {
    return sonitude::dsp::BinauralBackend::ItdIld;
  }
  if (backend == "compact_hrtf")
  {
    return sonitude::dsp::BinauralBackend::CompactHrtf;
  }
  if (backend == "full_hrtf_reference")
  {
    return sonitude::dsp::BinauralBackend::FullHrtfReference;
  }
  if (backend == "array_downmix")
  {
    return sonitude::dsp::BinauralBackend::ArrayDownmix;
  }
  return sonitude::dsp::BinauralBackend::MonoReference;
}

std::string SiblingFile(const std::string& path, const std::string& filename)
{
  const auto pos = path.find_last_of("/\\");
  if (pos == std::string::npos)
  {
    return filename;
  }
  return path.substr(0, pos + 1U) + filename;
}

sonitude::dsp::ArrayDownmixWeights ArrayDownmixForGeometry(
    const sonitude::app::GeometryConfig& geometry)
{
  std::vector<double> mic_x;
  mic_x.reserve(geometry.microphones.size());
  for (const auto& mic : geometry.microphones)
  {
    mic_x.push_back(mic.x);
  }
  return sonitude::dsp::MakeArrayDownmixWeights(mic_x);
}

std::unique_ptr<sonitude::dsp::HrtfTable> TryLoadHrtfTable(const std::string& path)
{
  if (path.empty())
  {
    return {};
  }
  try
  {
    return std::make_unique<sonitude::dsp::HrtfTable>(sonitude::dsp::LoadHrtfTableFromFile(path));
  }
  catch (const std::exception& ex)
  {
    std::cerr << "realtime: HRTF table not loaded from " << path << ": " << ex.what() << '\n';
    return {};
  }
}

const sonitude::dsp::HrtfTable* TableFor(const sonitude::dsp::HrtfTable* compact,
                                         const sonitude::dsp::HrtfTable* reference,
                                         const sonitude::dsp::BinauralBackend backend)
{
  if (backend == sonitude::dsp::BinauralBackend::CompactHrtf)
  {
    return compact;
  }
  if (backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
  {
    return reference;
  }
  return nullptr;
}
#endif

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
    std::signal(SIGINT, SignalStop);
    std::signal(SIGTERM, SignalStop);
    constexpr std::size_t kPrefillBlocks = 2;
    constexpr std::size_t kMaxConsecutivePlaybackFailures = 16;

    sonitude::audio::alsa::AlsaPcmDevice cap;
    sonitude::audio::alsa::AlsaPcmDevice pb;
    cap.openCapture(runtime_config.capture);
    pb.openPlayback(runtime_config.playback);
    const auto cap_params = cap.negotiated();
    const auto pb_params = pb.negotiated();
    const std::size_t desired_software_queue_frames =
        cap_params.period_frames * (kPrefillBlocks - 1U);
    sonitude::app::ValidateRuntimeAudioContract(
        runtime_config,
        {.capture_sample_rate_hz = cap_params.sample_rate_hz,
         .playback_sample_rate_hz = pb_params.sample_rate_hz,
         .capture_channels = cap_params.channels,
         .playback_buffer_frames = pb_params.buffer_frames,
         .software_queue_frames = desired_software_queue_frames,
         .minimum_asrc_headroom_frames = cap_params.period_frames});
    const std::uint32_t dsp_sample_rate_hz = cap_params.sample_rate_hz;

    sonitude::rt::TelemetryCounters counters;
    sonitude::audio::alsa::CaptureWorker cap_worker(&cap, &runtime_config, &counters);
    auto resampler = sonitude::dsp::CreateSrcResampler();
    sonitude::dsp::AsrcController ctl({
        .min_ratio = runtime_config.asrc.min_ratio,
        .max_ratio = runtime_config.asrc.max_ratio,
        .kp = runtime_config.asrc.pi_kp,
        .ki = runtime_config.asrc.pi_ki,
        .target_buffer_frames = static_cast<double>(runtime_config.asrc.target_buffer_frames),
        .max_ratio_step = 0.0001,
    });
    const bool asrc_enabled =
        runtime_config.asrc.enabled &&
        !(runtime_config.asrc.allow_bypass_for_locked_bench && mode == "passthrough");
    sonitude::audio::alsa::PlaybackWorker pb_worker(
        &pb, resampler.get(), &ctl, &counters, asrc_enabled);

    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }
    const auto calibration = sonitude::app::LoadCalibrationFromFile(runtime_config.calibration_path);
    sonitude::app::ValidateCalibrationConfig(
        calibration, geometry_ids, dsp_sample_rate_hz);
    sonitude::dsp::CalibrationApplier calibration_applier(calibration.channels,
                                                          geometry_ids,
                                                          dsp_sample_rate_hz,
                                                          runtime_config.calibration_dc_block_hz);

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
        geometry, runtime_config.steering, calibration, dsp_sample_rate_hz, 4096);
    sonitude::dsp::SuppressionStage suppressor;
    const auto suppression_backend = sonitude::dsp::ResolveEnabledBackend(
        runtime_config.suppression.enabled, runtime_config.suppression.backend);
    suppressor.configure(
        {.backend = suppression_backend,
         .sample_rate_hz = dsp_sample_rate_hz,
         .maximum_block_frames = cap_worker.periodFrames(),
         .conservative = {.ambient_floor_linear = runtime_config.steering.ambient_floor_linear,
                          .fade_ms = runtime_config.suppression.fade_ms,
                          .activity_threshold = runtime_config.suppression.activity_threshold,
                          .confidence_threshold = runtime_config.suppression.confidence_threshold},
         .spectral = {.enabled = true,
                      .fft_size = runtime_config.suppression.spectral.fft_size,
                      .hop_size = runtime_config.suppression.spectral.hop_size,
                      .gain_floor_db = runtime_config.suppression.spectral.gain_floor_db,
                      .confidence_threshold = runtime_config.suppression.confidence_threshold}});
    sonitude::dsp::PeakLimiter limiter;
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, dsp_sample_rate_hz);

    constexpr std::size_t kPlaybackRingSlots = 16;
    const std::size_t period_frames = cap_worker.periodFrames();

    const bool binaural_enabled = runtime_config.binaural.enabled;
    const auto binaural_backend = ParseBinauralBackend(runtime_config.binaural.backend);
    std::unique_ptr<sonitude::dsp::HrtfTable> compact_hrtf;
    std::unique_ptr<sonitude::dsp::HrtfTable> reference_hrtf;
    if (binaural_enabled &&
        (binaural_backend == sonitude::dsp::BinauralBackend::CompactHrtf ||
         binaural_backend == sonitude::dsp::BinauralBackend::FullHrtfReference))
    {
      compact_hrtf = TryLoadHrtfTable(runtime_config.binaural.profile.table_path);
      if (!runtime_config.binaural.profile.table_path.empty())
      {
        reference_hrtf =
            TryLoadHrtfTable(SiblingFile(runtime_config.binaural.profile.table_path, "reference.shrf"));
      }
    }
    sonitude::dsp::BinauralRenderer binaural_renderer;
    sonitude::dsp::StereoPeakLimiter stereo_limiter;
    bool binaural_renderer_ready = false;
    if (binaural_enabled)
    {
      const sonitude::dsp::HrtfTable* table =
          TableFor(compact_hrtf.get(), reference_hrtf.get(), binaural_backend);
      if ((binaural_backend == sonitude::dsp::BinauralBackend::CompactHrtf ||
           binaural_backend == sonitude::dsp::BinauralBackend::FullHrtfReference) &&
          (table == nullptr || table->empty()))
      {
        std::cerr << "realtime: binaural backend requires HRTF table; falling back to mono L=R\n";
      }
      else
      {
        try
        {
          binaural_renderer.configure({.sample_rate_hz = dsp_sample_rate_hz,
                                       .backend = binaural_backend,
                                       .transition_ms = runtime_config.binaural.transition.duration_ms,
                                       .max_block_frames = period_frames,
                                       .itd_ild = {.head_radius_m = runtime_config.binaural.model.head_radius_m,
                                                   .max_ild_db = runtime_config.binaural.model.max_ild_db},
                                       .table = table,
                                       .array_downmix = ArrayDownmixForGeometry(geometry)});
          stereo_limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, dsp_sample_rate_hz);
          binaural_renderer_ready = true;
          std::cout << "Binaural renderer enabled: " << runtime_config.binaural.backend << '\n';
        }
        catch (const std::exception& ex)
        {
          std::cerr << "realtime: binaural configure failed: " << ex.what() << '\n';
        }
      }
    }

    std::vector<sonitude::audio::MicFrame> mic_frames(period_frames);
    std::vector<sonitude::audio::MicFrame> calibrated_frames(period_frames);
    std::vector<float> mono(period_frames, 0.0F);
    std::array<std::vector<float>, sonitude::dsp::kGuardLooks> guard_looks{
        std::vector<float>(period_frames), std::vector<float>(period_frames), std::vector<float>(period_frames)};
    std::vector<float> binaural_left(period_frames, 0.0F);
    std::vector<float> binaural_right(period_frames, 0.0F);
    std::vector<sonitude::dsp::StereoSample> stereo(period_frames);
    std::vector<std::vector<sonitude::dsp::StereoSample>> playback_blocks(
        kPlaybackRingSlots, std::vector<sonitude::dsp::StereoSample>(period_frames));
    sonitude::rt::SpscRing<PlaybackBlockRef> playback_ring(32);
    std::size_t write_slot = 0;
    std::size_t software_queued_frames = 0;
    bool playback_started = false;
    std::size_t consecutive_playback_write_failures = 0;

    std::cout << "Running " << mode << " mode. Press Ctrl+C to stop.\n";
    sonitude::audio::BeamformerSteering last_target{};
    bool have_target = false;
    const auto start_tp = std::chrono::steady_clock::now();
    std::jthread control_thread;
    if (mode == "beamform")
    {
      control_thread = std::jthread([&]
      {
        try
        {
          while (g_running.load(std::memory_order_relaxed))
          {
            const auto now_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start_tp)
                    .count());
            control_loop.tick(now_ns);
            counters.control_state.store(static_cast<std::uint8_t>(conversation.state()),
                                         std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        catch (const std::exception& ex)
        {
          std::cerr << "Control thread failed: " << ex.what() << '\n';
          g_running.store(false, std::memory_order_relaxed);
        }
      });
    }
    std::jthread telemetry_thread([&]
    {
      const auto period = std::chrono::milliseconds(runtime_config.telemetry.stats_period_ms);
      while (g_running.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(period);
        if (!g_running.load(std::memory_order_relaxed))
        {
          break;
        }
        std::cout << "telemetry: cap_xruns=" << counters.capture_xruns.load(std::memory_order_relaxed)
                  << " pb_xruns=" << counters.playback_xruns.load(std::memory_order_relaxed)
                  << " pb_write_fail=" << counters.playback_write_failures.load(std::memory_order_relaxed)
                  << " ring_overruns=" << counters.ring_overruns.load(std::memory_order_relaxed)
                  << " ring_underruns=" << counters.ring_underruns.load(std::memory_order_relaxed)
                  << " asrc_ppm=" << counters.asrc_ratio_ppm.load(std::memory_order_relaxed)
                  << " occupancy=" << counters.ring_occupancy_frames.load(std::memory_order_relaxed)
                  << " sup_gain_milli=" << counters.suppressor_gain_milli.load(std::memory_order_relaxed)
                  << " state=" << static_cast<int>(counters.control_state.load(std::memory_order_relaxed))
                  << '\n';
      }
    });
    bool hold_estimator_after_xrun = false;
    while (g_running.load(std::memory_order_relaxed))
    {
      std::size_t frame_count = 0;
      if (!cap_worker.readBlock(std::span<sonitude::audio::MicFrame>(mic_frames), &frame_count))
      {
        beamformer.resetStream();
        suppressor.reset();
        if (binaural_renderer_ready)
        {
          binaural_renderer.reset();
        }
        hold_estimator_after_xrun = true;
        continue;
      }

      for (std::size_t i = 0; i < frame_count; ++i)
      {
        calibrated_frames[i] = calibration_applier.process(mic_frames[i]);
      }

      if (mode == "passthrough")
      {
        for (std::size_t i = 0; i < frame_count; ++i)
        {
          stereo[i].left = calibrated_frames[i][4];
          stereo[i].right = calibrated_frames[i][5];
        }
      }
      else
      {
        const auto snapshot = steering_reader.acquire();
        if (!have_target || std::fabs(snapshot.target.azimuth_deg - last_target.azimuth_deg) > 0.01F ||
            std::fabs(snapshot.target.elevation_deg - last_target.elevation_deg) > 0.01F)
        {
          beamformer.setTarget(snapshot.target);
          last_target = snapshot.target;
          have_target = true;
        }
        std::fill(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(frame_count), 0.0F);
        sonitude::dsp::GuardLookSpans guards{};
        if (suppression_backend == sonitude::dsp::SuppressionBackend::Spectral)
        {
          guards = sonitude::dsp::BindGuardLooks(guard_looks, frame_count);
          beamformer.process(
              std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count),
              std::span<float>(mono.data(), frame_count),
              guards);
        }
        else
        {
          beamformer.process(
              std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count),
              std::span<float>(mono.data(), frame_count));
        }
        if (suppression_backend != sonitude::dsp::SuppressionBackend::Off)
        {
          const bool focus_active = !snapshot.failsafe;
          const float confidence = focus_active ? snapshot.confidence : 0.0F;
          suppressor.setEstimatorHold(hold_estimator_after_xrun);
          hold_estimator_after_xrun = false;
          suppressor.setControl(focus_active, confidence);
          if (suppression_backend == sonitude::dsp::SuppressionBackend::Spectral)
          {
            suppressor.process(std::span<float>(mono.data(), frame_count),
                               sonitude::dsp::ConstGuardLooks(guards));
          }
          else
          {
            suppressor.process(std::span<float>(mono.data(), frame_count));
          }
        }
        limiter.process(std::span<float>(mono.data(), frame_count));
        counters.suppressor_gain_milli.store(
            static_cast<std::int64_t>(std::llround(suppressor.currentGain() * 1000.0F)),
            std::memory_order_relaxed);
        if (binaural_renderer_ready)
        {
          const sonitude::audio::BeamformerSteering binaural_dir =
              runtime_config.binaural.direction.follow_steering
                  ? snapshot.target
                  : sonitude::audio::BeamformerSteering{
                        runtime_config.binaural.direction.azimuth_deg,
                        runtime_config.binaural.direction.elevation_deg};
          binaural_renderer.setDirection(binaural_dir);
          binaural_renderer.process(std::span<const float>(mono.data(), frame_count),
                                    std::span<float>(binaural_left.data(), frame_count),
                                    std::span<float>(binaural_right.data(), frame_count));
          stereo_limiter.process(std::span<float>(binaural_left.data(), frame_count),
                                 std::span<float>(binaural_right.data(), frame_count));
          for (std::size_t i = 0; i < frame_count; ++i)
          {
            stereo[i].left = binaural_left[i];
            stereo[i].right = binaural_right[i];
          }
        }
        else
        {
          for (std::size_t i = 0; i < frame_count; ++i)
          {
            stereo[i].left = mono[i];
            stereo[i].right = mono[i];
          }
        }
      }

      std::copy(stereo.begin(),
                stereo.begin() + static_cast<std::ptrdiff_t>(frame_count),
                playback_blocks[write_slot].begin());
      const std::size_t written_slot = write_slot;
      write_slot = (write_slot + 1U) % playback_blocks.size();
      if (!playback_ring.push({written_slot, frame_count}))
      {
        counters.ring_overruns.fetch_add(1, std::memory_order_relaxed);
      }
      else
      {
        software_queued_frames += frame_count;
      }

      if (!playback_started && playback_ring.size() >= kPrefillBlocks)
      {
        playback_started = true;
      }

      if (playback_started)
      {
        PlaybackBlockRef block;
        if (playback_ring.pop(block))
        {
          software_queued_frames -= block.frames;
          const std::size_t device_queued = pb.playbackQueuedFrames();
          const std::size_t occupancy = software_queued_frames + device_queued;
          if (!pb_worker.writeStereo(
              std::span<const sonitude::dsp::StereoSample>(playback_blocks[block.slot].data(), block.frames),
              occupancy))
          {
            counters.playback_write_failures.fetch_add(1, std::memory_order_relaxed);
            ++consecutive_playback_write_failures;
            if (consecutive_playback_write_failures >= kMaxConsecutivePlaybackFailures)
            {
              std::cerr << "Playback write failed repeatedly; stopping audio loop.\n";
              g_running.store(false, std::memory_order_relaxed);
            }
          }
          else
          {
            consecutive_playback_write_failures = 0;
          }
        }
        else
        {
          counters.ring_underruns.fetch_add(1, std::memory_order_relaxed);
        }
      }

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
