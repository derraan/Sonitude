#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <exception>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
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
#include "audio/playback_block_pool.hpp"
#include "control/control_loop.hpp"
#include "control/conversation_state_machine.hpp"
#include "control/zones.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/limiter.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/resampler.hpp"
#include "dsp/selective_suppressor.hpp"
#include "dsp/suppressor.hpp"
#include "rt/param_snapshot.hpp"
#include "rt/rt_thread.hpp"
#include "rt/telemetry.hpp"
#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_provider.hpp"

namespace
{
#if defined(__linux__)
std::atomic<bool> g_running{true};

enum class RuntimeFailure : std::uint8_t
{
  None,
  PlaybackWrite,
  PlaybackBlockRelease,
  PlaybackQueuePublish,
};

void SignalStop(const int) { g_running.store(false, std::memory_order_relaxed); }
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
    // TODO(sonitude-geometry): Keep load-time geometry validation explicit until a dedicated
    // startup diagnostics path reports geometry health separately from runtime startup.

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
    const std::size_t required_scratch_frames =
        sonitude::audio::alsa::PlaybackWorker::CalculateRequiredScratchFrames(
            cap_params.period_frames, pb_params.period_frames, runtime_config.asrc.max_ratio);
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
        &pb,
        resampler.get(),
        &ctl,
        &counters,
        asrc_enabled,
        cap_params.period_frames,
        runtime_config.asrc.max_ratio);
    sonitude::app::ValidateRuntimeAudioContract(
        runtime_config,
        {.capture_sample_rate_hz = cap_params.sample_rate_hz,
         .playback_sample_rate_hz = pb_params.sample_rate_hz,
         .capture_channels = cap_params.channels,
         .playback_buffer_frames = pb_params.buffer_frames,
         .software_queue_frames = desired_software_queue_frames,
         .minimum_asrc_headroom_frames = cap_params.period_frames,
         .capture_period_frames = cap_params.period_frames,
         .playback_period_frames = pb_params.period_frames,
         .asrc_max_ratio = runtime_config.asrc.max_ratio,
         .required_playback_scratch_frames = required_scratch_frames,
         .negotiated_playback_scratch_frames = pb_worker.scratchCapacityFrames()});

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
    // TODO(sonitude-control): Replace scripted mock DOA events with scenario fixtures once
    // ODAS-provider integration tests exercise deterministic event playback.
    mock_events.push_back({0, {1, -30.0F, 0.0F, 0.8F, 0}});
    mock_events.push_back({8'000'000'000ULL, {1, 35.0F, 0.0F, 0.85F, 8'000'000'000ULL}});
    if (!runtime_config.odas.enabled)
    {
      provider = std::make_unique<sonitude::spatial::MockDoaProvider>(
          std::vector<sonitude::spatial::MockDoaEvent>{});
    }
    else if (runtime_config.odas.use_mock_provider)
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
    sonitude::dsp::OutputGainGate output_gain_gate;
    output_gain_gate.configure(
        {.ambient_floor_linear = runtime_config.steering.ambient_floor_linear,
         .fade_ms = runtime_config.suppression.fade_ms,
         .activity_threshold = runtime_config.suppression.activity_threshold,
         .confidence_threshold = runtime_config.suppression.confidence_threshold},
        dsp_sample_rate_hz);
    sonitude::dsp::SelectiveSuppressor selective_suppressor;
    selective_suppressor.configure({}, dsp_sample_rate_hz);
    sonitude::dsp::PeakLimiter limiter;
    // TODO(sonitude-limiter): Promote limiter defaults into runtime config when limiter tuning
    // is calibrated against target hardware and benchmark fixtures.
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, dsp_sample_rate_hz);

    constexpr std::size_t kPlaybackRingSlots = 16;
    const std::size_t period_frames = cap_worker.periodFrames();
    std::vector<sonitude::audio::MicFrame> mic_frames(period_frames);
    std::vector<sonitude::audio::MicFrame> calibrated_frames(period_frames);
    std::vector<float> mono(period_frames, 0.0F);
    std::vector<float> distractor_reference(period_frames, 0.0F);
    std::vector<sonitude::dsp::StereoSample> stereo(period_frames);
    sonitude::audio::PlaybackBlockPool playback_pool(kPlaybackRingSlots, period_frames);

    std::cout << "Running " << mode << " mode. Press Ctrl+C to stop.\n";
    sonitude::audio::BeamformerSteering last_target{};
    bool have_target = false;
    const auto start_tp = std::chrono::steady_clock::now();
    if (runtime_config.realtime.enable_mlockall &&
        !sonitude::rt::TryEnableMemoryLocking())
    {
      std::cerr << "Warning: mlockall failed; realtime memory locking is unavailable.\n";
    }
    std::jthread control_thread;
    if (mode == "beamform")
    {
      control_thread = std::jthread([&](std::stop_token stop_token)
      {
        try
        {
          while (g_running.load(std::memory_order_relaxed) && !stop_token.stop_requested())
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
      if (!sonitude::rt::TryConfigureOtherScheduling(control_thread))
      {
        std::cerr << "Warning: control thread SCHED_OTHER pinning was not applied.\n";
      }
    }

    std::condition_variable_any telemetry_cv;
    std::mutex telemetry_mutex;
    std::atomic<RuntimeFailure> runtime_failure{RuntimeFailure::None};
    std::jthread telemetry_thread([&](std::stop_token stop_token)
    {
      const auto period = std::chrono::milliseconds(runtime_config.telemetry.stats_period_ms);
      std::unique_lock<std::mutex> lock(telemetry_mutex);
      while (!stop_token.stop_requested())
      {
        const bool stop_now = telemetry_cv.wait_for(
            lock, stop_token, period, [&] { return !g_running.load(std::memory_order_relaxed); });
        if (stop_now || !g_running.load(std::memory_order_relaxed))
        {
          switch (runtime_failure.load(std::memory_order_acquire))
          {
            case RuntimeFailure::PlaybackWrite:
              std::cerr << "Playback write failed repeatedly; audio loop stopped.\n";
              break;
            case RuntimeFailure::PlaybackBlockRelease:
              std::cerr << "Playback block ownership invariant failed; audio loop stopped.\n";
              break;
            case RuntimeFailure::PlaybackQueuePublish:
              std::cerr << "Playback filled queue invariant failed; audio loop stopped.\n";
              break;
            case RuntimeFailure::None:
              break;
          }
          break;
        }
        lock.unlock();
        std::cout << "telemetry: cap_xruns=" << counters.capture_xruns.load(std::memory_order_relaxed)
                  << " pb_xruns=" << counters.playback_xruns.load(std::memory_order_relaxed)
                  << " pb_write_fail=" << counters.playback_write_failures.load(std::memory_order_relaxed)
                  << " ring_overruns=" << counters.ring_overruns.load(std::memory_order_relaxed)
                  << " ring_underruns=" << counters.ring_underruns.load(std::memory_order_relaxed)
                  << " asrc_ppm=" << counters.asrc_ratio_ppm.load(std::memory_order_relaxed)
                  << " occupancy=" << counters.ring_occupancy_frames.load(std::memory_order_relaxed)
                  << " gate_gain_milli=" << counters.suppressor_gain_milli.load(std::memory_order_relaxed)
                  << " confidence_milli="
                  << counters.steering_confidence_milli.load(std::memory_order_relaxed)
                  << " speech_prob_milli="
                  << counters.speech_probability_milli.load(std::memory_order_relaxed)
                  << " state=" << static_cast<int>(counters.control_state.load(std::memory_order_relaxed))
                  << '\n';
        lock.lock();
      }
    });
    if (!sonitude::rt::TryConfigureOtherScheduling(telemetry_thread))
    {
      std::cerr << "Warning: telemetry thread SCHED_OTHER pinning was not applied.\n";
    }

    std::jthread playback_thread([&](std::stop_token stop_token)
    {
      bool playback_started = false;
      std::size_t consecutive_playback_write_failures = 0;
      while (!stop_token.stop_requested() &&
             (g_running.load(std::memory_order_relaxed) || playback_pool.filledCount() > 0U))
      {
        if (!playback_started)
        {
          if (playback_pool.filledCount() < kPrefillBlocks)
          {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
          }
          playback_started = true;
        }

        sonitude::audio::PlaybackBlockRef block;
        if (!playback_pool.consume(block))
        {
          counters.ring_underruns.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }

        const std::size_t device_queued = pb.playbackQueuedFrames();
        const std::size_t occupancy =
            playback_pool.queuedFrames() + block.frames + device_queued;
        if (!pb_worker.writeStereo(playback_pool.readable(block), occupancy))
        {
          counters.playback_write_failures.fetch_add(1, std::memory_order_relaxed);
          ++consecutive_playback_write_failures;
          if (consecutive_playback_write_failures >= kMaxConsecutivePlaybackFailures)
          {
            runtime_failure.store(RuntimeFailure::PlaybackWrite, std::memory_order_release);
            g_running.store(false, std::memory_order_relaxed);
            telemetry_cv.notify_all();
          }
        }
        else
        {
          consecutive_playback_write_failures = 0;
        }
        if (!playback_pool.release(block.slot))
        {
          runtime_failure.store(RuntimeFailure::PlaybackBlockRelease, std::memory_order_release);
          g_running.store(false, std::memory_order_relaxed);
          telemetry_cv.notify_all();
        }
      }
    });
    if (!sonitude::rt::TryConfigureRtScheduling(playback_thread, runtime_config.realtime.playback_priority))
    {
      std::cerr << "Warning: playback thread realtime scheduling was not applied.\n";
    }

    std::jthread capture_unblock_thread([&](std::stop_token stop_token)
    {
      while (g_running.load(std::memory_order_relaxed) && !stop_token.stop_requested())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!stop_token.stop_requested())
      {
        cap.dropStream();
      }
    });
    if (!sonitude::rt::TryConfigureOtherScheduling(capture_unblock_thread))
    {
      std::cerr << "Warning: capture-unblock thread SCHED_OTHER pinning was not applied.\n";
    }
    if (!sonitude::rt::TryConfigureCurrentThreadRtScheduling(runtime_config.realtime.capture_priority))
    {
      std::cerr << "Warning: capture/DSP thread realtime scheduling was not applied.\n";
    }

    while (g_running.load(std::memory_order_relaxed))
    {
      std::size_t frame_count = 0;
      if (!cap_worker.readBlock(std::span<sonitude::audio::MicFrame>(mic_frames), &frame_count))
      {
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
        if (runtime_config.suppression.enabled && snapshot.has_distractor)
        {
          beamformer.processWithReference(
              std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count),
              std::span<float>(mono.data(), frame_count),
              snapshot.distractor,
              std::span<float>(distractor_reference.data(), frame_count));
        }
        else
        {
          beamformer.process(std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count),
                             std::span<float>(mono.data(), frame_count));
          std::fill(distractor_reference.begin(),
                    distractor_reference.begin() + static_cast<std::ptrdiff_t>(frame_count),
                    0.0F);
        }
        if (runtime_config.suppression.enabled)
        {
          const bool focus_active = !snapshot.failsafe;
          selective_suppressor.setControl(focus_active, snapshot.has_distractor);
          selective_suppressor.process(std::span<float>(mono.data(), frame_count),
                                       std::span<const float>(distractor_reference.data(), frame_count));
          output_gain_gate.setControl(snapshot.failsafe, snapshot.confidence);
          output_gain_gate.process(std::span<float>(mono.data(), frame_count));
        }
        else
        {
          selective_suppressor.setControl(false, false);
          output_gain_gate.setControl(false, 0.0F);
        }
        limiter.process(std::span<float>(mono.data(), frame_count));
        counters.suppressor_gain_milli.store(
            static_cast<std::int64_t>(std::llround(output_gain_gate.currentGain() * 1000.0F)),
            std::memory_order_relaxed);
        counters.steering_confidence_milli.store(
            static_cast<std::int64_t>(std::llround(snapshot.confidence * 1000.0F)),
            std::memory_order_relaxed);
        counters.speech_probability_milli.store(
            static_cast<std::int64_t>(std::llround(snapshot.speech_probability * 1000.0F)),
            std::memory_order_relaxed);
        for (std::size_t i = 0; i < frame_count; ++i)
        {
          stereo[i].left = mono[i];
          stereo[i].right = mono[i];
        }
      }

      std::size_t write_slot = 0;
      if (!playback_pool.acquire(write_slot))
      {
        counters.ring_overruns.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      const auto output_block = playback_pool.writable(write_slot);
      std::copy(stereo.begin(),
                stereo.begin() + static_cast<std::ptrdiff_t>(frame_count),
                output_block.begin());
      if (!playback_pool.publish(write_slot, frame_count))
      {
        runtime_failure.store(RuntimeFailure::PlaybackQueuePublish, std::memory_order_release);
        g_running.store(false, std::memory_order_relaxed);
        telemetry_cv.notify_all();
      }

    }
    g_running.store(false, std::memory_order_relaxed);
    telemetry_cv.notify_all();
    control_thread.request_stop();
    telemetry_thread.request_stop();
    playback_thread.request_stop();
    capture_unblock_thread.request_stop();
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
