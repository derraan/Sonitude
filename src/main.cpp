#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "app/logging.hpp"
#if SONITUDE_HAS_ALSA
#include "audio/alsa/alsa_device.hpp"
#include "audio/alsa/capture_worker.hpp"
#include "audio/alsa/playback_worker.hpp"
#endif
#include "control/control_loop.hpp"
#include "control/conversation_state_machine.hpp"
#include "control/rt_steering_snapshot.hpp"
#include "control/steering_channel.hpp"
#include "control/zones.hpp"
#include "dsp/asrc_controller.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/binaural_renderer.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/hrtf_table.hpp"
#include "dsp/limiter.hpp"
#include "dsp/resampler.hpp"
#include "dsp/suppression_stage.hpp"
#include "rt/block_channel.hpp"
#include "rt/lifecycle.hpp"
#include "rt/managed_thread.hpp"
#include "rt/scheduling.hpp"
#include "rt/telemetry.hpp"
#include "rt/wake_event.hpp"
#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_provider.hpp"

namespace
{
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

void PrintUsage()
{
  std::cout
      << "sonitude_realtime realtime audio runtime\n"
      << "Usage:\n"
      << "  sonitude_realtime [--config <path>] [--validate-config] [--mode passthrough|beamform]\n"
      << "  sonitude_realtime --version\n"
      << "  sonitude_realtime --help\n";
}

#if SONITUDE_HAS_ALSA
constexpr std::size_t kPrefillBlocks = 2;
constexpr std::size_t kPlaybackBlockSlots = 16;
constexpr std::size_t kMaxConsecutivePlaybackFailures = 16;

using Clock = std::chrono::steady_clock;

std::uint64_t ElapsedNs(const Clock::time_point start) noexcept
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

// Describes one thread for the startup report and for the post-gate validation.
struct ThreadRole
{
  const char* role = "";
  sonitude::rt::ManagedThread* thread = nullptr;
  sonitude::rt::SchedClass expected_class = sonitude::rt::SchedClass::Normal;
  std::int32_t expected_priority = 0;
};

void PrintSchedulingTable(const std::vector<ThreadRole>& roles)
{
  std::cout << "thread scheduling:\n";
  for (const ThreadRole& role : roles)
  {
    if (role.thread == nullptr || !role.thread->joinable())
    {
      continue;
    }
    const sonitude::rt::ThreadStatus& status = role.thread->status();
    std::cout << "  " << role.role << " name=" << status.name << " tid=" << status.observed.tid
              << " requested=" << sonitude::rt::SchedClassName(role.expected_class) << '/'
              << role.expected_priority << " actual="
              << (status.observed.valid ? (status.observed.realtime ? "SCHED_FIFO" : "SCHED_OTHER")
                                        : "unknown")
              << '/' << status.observed.priority << (status.degraded ? " DEGRADED" : "") << '\n';
  }
}

// Confirms that what the kernel gave each thread is what the design requires.
// A slow thread that came up realtime, or a realtime thread that came up
// SCHED_OTHER, is reported rather than tolerated silently.
bool ValidateScheduling(const std::vector<ThreadRole>& roles, const bool require_realtime)
{
  bool acceptable = true;
  for (const ThreadRole& role : roles)
  {
    if (role.thread == nullptr || !role.thread->joinable())
    {
      continue;
    }
    const sonitude::rt::ThreadStatus& status = role.thread->status();
    if (!status.observed.valid)
    {
      std::cerr << "Scheduling check failed: " << role.role
                << " could not report its policy (error " << status.observed.error << ")\n";
      acceptable = false;
      continue;
    }
    const bool wants_rt = role.expected_class == sonitude::rt::SchedClass::Realtime;
    if (!wants_rt && status.observed.realtime)
    {
      // This is the failure mode that scheduler inheritance used to produce.
      std::cerr << "Scheduling check failed: slow thread " << role.role
                << " is running with a realtime policy (priority " << status.observed.priority
                << ")\n";
      acceptable = false;
      continue;
    }
    if (wants_rt && !status.observed.realtime)
    {
      std::cerr << "Warning: " << role.role
                << " could not obtain SCHED_FIFO and is running SCHED_OTHER (error "
                << status.create_error << "). Audio timing is not guaranteed.\n";
      if (require_realtime)
      {
        acceptable = false;
      }
      continue;
    }
    if (wants_rt && status.observed.priority != role.expected_priority)
    {
      std::cerr << "Scheduling check failed: " << role.role << " expected priority "
                << role.expected_priority << " but is running at " << status.observed.priority
                << '\n';
      acceptable = false;
    }
  }
  return acceptable;
}
#endif
} // namespace

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  bool validate_only = false;
  std::string mode = "passthrough";

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
    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }
    const auto calibration =
        sonitude::app::LoadCalibrationFromFile(runtime_config.calibration_path);
    sonitude::app::ValidateCalibrationConfig(calibration, geometry_ids,
                                             runtime_config.capture.sample_rate_hz);
    // TODO(sonitude-geometry): Keep load-time geometry validation explicit until a dedicated
    // startup diagnostics path reports geometry health separately from runtime startup.

    std::cout << "Config validation passed: " << config_path << '\n';
    if (validate_only)
    {
      return 0;
    }

    if (mode != "passthrough" && mode != "beamform")
    {
      std::cerr << "Unsupported mode: " << mode << " (expected passthrough or beamform).\n";
      return 2;
    }

#if SONITUDE_HAS_ALSA
    // ------------------------------------------------------------------
    // Phase 1: initialisation. The supervisor thread stays SCHED_OTHER for
    // all of it, so nothing here can be inherited as a realtime policy, and
    // every allocation happens before any thread has a deadline.
    // ------------------------------------------------------------------
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
    sonitude::audio::alsa::PlaybackWorker pb_worker(&pb, resampler.get(), &ctl, &counters,
                                                    asrc_enabled, cap_params.period_frames,
                                                    runtime_config.asrc.max_ratio);
    sonitude::app::ValidateRuntimeAudioContract(
        runtime_config, {.capture_sample_rate_hz = cap_params.sample_rate_hz,
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

    sonitude::app::ValidateCalibrationConfig(calibration, geometry_ids, dsp_sample_rate_hz);
    sonitude::dsp::CalibrationApplier calibration_applier(calibration.channels, geometry_ids,
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

    // Control -> audio. Bounded, trivially copyable payload, latest-wins.
    sonitude::control::SteeringChannel steering_channel;

    sonitude::control::ConversationStateMachine conversation(
        runtime_config.state_machine, runtime_config.steering.ambient_floor_linear,
        sonitude::control::ZoneMap(runtime_config.zones));
    sonitude::control::ControlLoop control_loop(
        provider.get(), &steering_channel,
        {.failsafe_timeout_ns =
             static_cast<std::uint64_t>(runtime_config.state_machine.release_hold_ms) *
             1'000'000ULL,
         .ambient_floor_linear = runtime_config.steering.ambient_floor_linear},
        &conversation);

    sonitude::dsp::MvdrBeamformer beamformer;
    beamformer.configure(geometry, runtime_config.steering, calibration, dsp_sample_rate_hz, 4096);
    sonitude::dsp::SuppressionStage suppressor;
    const auto suppression_backend =
        sonitude::dsp::ParseSuppressionBackend(runtime_config.suppression.backend);
    suppressor.configure(
        {.enabled = runtime_config.suppression.enabled,
         .backend = suppression_backend,
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
    beamformer.setSpectralPostfilter(suppressor.spectralFilter());
    sonitude::dsp::PeakLimiter limiter;
    // TODO(sonitude-limiter): Promote limiter defaults into runtime config when limiter tuning
    // is calibrated against target hardware and benchmark fixtures.
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, dsp_sample_rate_hz);

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
    std::vector<float> binaural_left(period_frames, 0.0F);
    std::vector<float> binaural_right(period_frames, 0.0F);
    std::vector<sonitude::dsp::StereoSample> stereo(period_frames);

    // Capture/DSP -> playback. Every slot has exactly one owner at all times and
    // is only reused after playback returns it.
    static_assert(kPlaybackBlockSlots > kPrefillBlocks,
                  "the block pool must be able to hold the prefill plus work in flight");
    sonitude::rt::BlockChannel<sonitude::dsp::StereoSample> blocks(kPlaybackBlockSlots,
                                                                   period_frames);

    // Nominal period, used only for instrumentation thresholds and for bounding
    // the event waits. It is not a pacing timer.
    const double period_seconds = cap_params.sample_rate_hz > 0U
                                      ? static_cast<double>(cap_params.period_frames) /
                                            static_cast<double>(cap_params.sample_rate_hz)
                                      : 0.0;
    const std::uint64_t nominal_period_ns =
        static_cast<std::uint64_t>(period_seconds * 1'000'000'000.0);
    const int wait_timeout_ms =
        std::max(10, static_cast<int>(std::lround(period_seconds * 1000.0 * 8.0)));

    // ------------------------------------------------------------------
    // Phase 2: lifecycle and memory locking, still on the supervisor.
    // ------------------------------------------------------------------
    sonitude::rt::Lifecycle lifecycle;
    sonitude::rt::SignalHandlerInstallation signal_handlers(lifecycle);
    if (!signal_handlers.installed())
    {
      std::cerr << "Warning: could not install SIGINT/SIGTERM handlers.\n";
    }
    if (runtime_config.realtime.enable_mlockall)
    {
      int lock_error = 0;
      if (!sonitude::rt::TryEnableMemoryLocking(&lock_error))
      {
        std::cerr << "Warning: mlockall(MCL_CURRENT|MCL_FUTURE) failed with errno " << lock_error
                  << ". Grant CAP_IPC_LOCK or raise RLIMIT_MEMLOCK "
                  << "(see scripts/set_realtime_environment.sh); page faults may cause audio "
                  << "dropouts.\n";
        if (runtime_config.realtime.require_memory_lock)
        {
          std::cerr << "Startup failed: production configuration requires successful memory "
                       "locking.\n";
          return 1;
        }
      }
      else if (runtime_config.realtime.require_memory_lock)
      {
        std::cout << "memory_lock=ok mode=required\n";
      }
    }

    // Signalled by capture/DSP when a block becomes available, so playback is
    // woken by work arriving rather than by a fixed-interval timer.
    sonitude::rt::WakeEvent playback_wake;

    // Resolved once: the realtime loop must not evaluate string comparisons per
    // period.
    const bool passthrough_mode = mode == "passthrough";
    const bool control_enabled = mode == "beamform";
    const std::size_t expected_threads = control_enabled ? 4U : 3U;
    sonitude::rt::StartGate gate(expected_threads);

    sonitude::rt::ManagedThread capture_thread;
    sonitude::rt::ManagedThread playback_thread;
    sonitude::rt::ManagedThread control_thread;
    sonitude::rt::ManagedThread telemetry_thread;

    const Clock::time_point start_tp = Clock::now();
    const std::size_t rt_stack_bytes =
        static_cast<std::size_t>(runtime_config.realtime.rt_stack_kib) * 1024U;
    const std::size_t rt_prefault_bytes =
        static_cast<std::size_t>(runtime_config.realtime.rt_prefault_kib) * 1024U;

    // ---------------- capture / DSP (realtime) ----------------
    auto capture_body = [&]()
    {
      sonitude::audio::BeamformerSteering last_target{};
      bool have_target = false;
      sonitude::control::RtSteeringSnapshot steering{};
      std::uint64_t previous_start_ns = 0;
      bool hold_estimator_after_xrun = false;

      const bool can_wait = cap_worker.waitingSupported();
      while (!lifecycle.stopRequested())
      {
        if (can_wait)
        {
          const sonitude::audio::alsa::CaptureWait wait_result =
              cap_worker.waitForData(lifecycle.wakeFd(), wait_timeout_ms);
          if (wait_result == sonitude::audio::alsa::CaptureWait::Interrupted)
          {
            break;
          }
          if (wait_result == sonitude::audio::alsa::CaptureWait::Timeout)
          {
            counters.capture_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          if (wait_result == sonitude::audio::alsa::CaptureWait::Error)
          {
            counters.capture_wait_errors.fetch_add(1, std::memory_order_relaxed);
            lifecycle.requestStop(sonitude::rt::StopReason::CaptureFailure);
            break;
          }
        }

        const std::uint64_t period_start_ns = ElapsedNs(start_tp);
        if (previous_start_ns != 0U)
        {
          const std::uint64_t delta_us = (period_start_ns - previous_start_ns) / 1000U;
          counters.capture_period_last_us.store(delta_us, std::memory_order_relaxed);
          sonitude::rt::StoreMaxRelaxed(counters.capture_period_max_us, delta_us);
        }
        previous_start_ns = period_start_ns;

        std::size_t frame_count = 0;
        if (!cap_worker.readBlock(std::span<sonitude::audio::MicFrame>(mic_frames), &frame_count) ||
            frame_count == 0U)
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

        if (passthrough_mode)
        {
          for (std::size_t i = 0; i < frame_count; ++i)
          {
            stereo[i].left = calibrated_frames[i][4];
            stereo[i].right = calibrated_frames[i][5];
          }
        }
        else
        {
          // Take every control message queued since the last period and keep the
          // newest. When none arrived, `steering` keeps its previous value.
          if (steering_channel.drainLatest(steering))
          {
            counters.control_updates_applied.fetch_add(1, std::memory_order_relaxed);
          }
          counters.control_snapshot_age_us.store(
              (period_start_ns > steering.published_ns
                   ? (period_start_ns - steering.published_ns) / 1000U
                   : 0U),
              std::memory_order_relaxed);

          if (!have_target ||
              std::fabs(steering.target.azimuth_deg - last_target.azimuth_deg) > 0.01F ||
              std::fabs(steering.target.elevation_deg - last_target.elevation_deg) > 0.01F)
          {
            beamformer.setTarget(steering.target);
            last_target = steering.target;
            have_target = true;
          }
          std::fill(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(frame_count), 0.0F);
          const bool focus_active = !steering.failsafe;
          const float confidence = focus_active ? steering.confidence : 0.0F;
          suppressor.setEstimatorHold(hold_estimator_after_xrun);
          hold_estimator_after_xrun = false;
          suppressor.setControl(focus_active, confidence);
          beamformer.process(
              std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count),
              std::span<float>(mono.data(), frame_count));
          suppressor.process(std::span<float>(mono.data(), frame_count));
          limiter.process(std::span<float>(mono.data(), frame_count));
          counters.suppressor_gain_milli.store(
              static_cast<std::int64_t>(std::llround(suppressor.currentGain() * 1000.0F)),
              std::memory_order_relaxed);
          counters.steering_confidence_milli.store(
              static_cast<std::int64_t>(std::llround(steering.confidence * 1000.0F)),
              std::memory_order_relaxed);
          counters.speech_probability_milli.store(
              static_cast<std::int64_t>(std::llround(steering.speech_probability * 1000.0F)),
              std::memory_order_relaxed);
          if (binaural_renderer_ready)
          {
            const sonitude::audio::BeamformerSteering binaural_dir =
                runtime_config.binaural.direction.follow_steering
                    ? steering.target
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

        if (passthrough_mode)
        {
          const sonitude::dsp::LimiterTelemetry limiter_telemetry = limiter.processLinkedStereo(
              std::span<sonitude::dsp::StereoSample>(stereo.data(), frame_count));
          counters.limiter_input_over_ceiling_events.fetch_add(
              limiter_telemetry.input_over_ceiling_events, std::memory_order_relaxed);
          counters.limiter_output_saturation_events.fetch_add(
              limiter_telemetry.output_saturation_events, std::memory_order_relaxed);
        }

        // Ownership handoff: take a free slot, fill it, publish it. If playback
        // has not returned a slot yet this block is dropped and counted; the
        // pool is deliberately not grown to paper over that.
        std::uint32_t slot = 0;
        if (!blocks.acquire(slot))
        {
        }
        else
        {
          const std::span<sonitude::dsp::StereoSample> destination = blocks.writable(slot);
          std::copy(stereo.begin(), stereo.begin() + static_cast<std::ptrdiff_t>(frame_count),
                    destination.begin());
          if (!blocks.commit(slot, frame_count))
          {
            (void)blocks.abandon(slot);
          }
          else
          {
            playback_wake.signal();
          }
        }

        const std::uint64_t work_us = (ElapsedNs(start_tp) - period_start_ns) / 1000U;
        sonitude::rt::StoreMaxRelaxed(counters.capture_work_max_us, work_us);
        if (nominal_period_ns > 0U && (work_us * 1000U) > nominal_period_ns)
        {
          counters.capture_deadline_misses.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // Capture ending means the pipeline is finished, whatever the cause.
      lifecycle.requestStop(sonitude::rt::StopReason::CaptureStreamEnded);
      playback_wake.signal();
    };

    // ---------------- playback (realtime) ----------------
    auto playback_body = [&]()
    {
      bool playback_started = false;
      std::size_t consecutive_playback_write_failures = 0;
      while (!lifecycle.stopRequested())
      {
        if (!playback_started)
        {
          if (blocks.readyCount() < kPrefillBlocks)
          {
            if (sonitude::rt::WaitAnyOf(playback_wake, lifecycle.wakeEvent(),
                                        std::chrono::milliseconds(wait_timeout_ms)) ==
                sonitude::rt::WaitOutcome::Second)
            {
              break;
            }
            playback_wake.consume();
            continue;
          }
          playback_started = true;
        }

        sonitude::rt::BlockRef block{};
        if (!blocks.take(block))
        {
          // Clear any stale token, re-check, then block until work or shutdown.
          playback_wake.consume();
          if (blocks.readyCount() == 0U &&
              sonitude::rt::WaitAnyOf(playback_wake, lifecycle.wakeEvent(),
                                      std::chrono::milliseconds(wait_timeout_ms)) ==
                  sonitude::rt::WaitOutcome::Second)
          {
            break;
          }
          continue;
        }

        // Occupancy is derived from the block channel's own accounting, which is
        // published on the same release/acquire edge as the block itself, so it
        // cannot disagree with the queue or underflow.
        const std::size_t device_queued = pb.playbackQueuedFrames();
        const std::size_t occupancy =
            static_cast<std::size_t>(blocks.pendingFrames()) + device_queued;

        const std::uint64_t write_start_ns = ElapsedNs(start_tp);
        const bool written = pb_worker.writeStereo(blocks.readable(block), occupancy);
        const std::uint64_t write_us = (ElapsedNs(start_tp) - write_start_ns) / 1000U;
        counters.playback_write_last_us.store(write_us, std::memory_order_relaxed);
        sonitude::rt::StoreMaxRelaxed(counters.playback_write_max_us, write_us);

        // The slot goes back to the producer whether or not the write succeeded:
        // ownership is not conditional on success.
        (void)blocks.release(block);

        if (!written)
        {
          counters.playback_write_failures.fetch_add(1, std::memory_order_relaxed);
          ++consecutive_playback_write_failures;
          if (consecutive_playback_write_failures >= kMaxConsecutivePlaybackFailures)
          {
            // Report via the lifecycle; the supervisor prints. A realtime thread
            // does not write to stderr.
            lifecycle.requestStop(sonitude::rt::StopReason::PlaybackFailure);
            break;
          }
        }
        else
        {
          consecutive_playback_write_failures = 0;
        }
      }
    };

    // ---------------- control (slow) ----------------
    auto control_body = [&]()
    {
      try
      {
        while (!lifecycle.stopRequested())
        {
          control_loop.tick(ElapsedNs(start_tp));
          counters.control_ticks.fetch_add(1, std::memory_order_relaxed);
          counters.control_state.store(static_cast<std::uint8_t>(conversation.state()),
                                       std::memory_order_relaxed);
          if (lifecycle.waitForStopOr(std::chrono::milliseconds(10)))
          {
            break;
          }
        }
        // Leave the audio thread with a safe steering state rather than the last
        // live one, so a stopped control loop cannot keep a beam pointed.
        control_loop.publishFailsafe(ElapsedNs(start_tp));
      }
      catch (const std::exception& ex)
      {
        control_loop.publishFailsafe(ElapsedNs(start_tp));
        std::cerr << "Control thread failed: " << ex.what() << '\n';
        lifecycle.requestStop(sonitude::rt::StopReason::ControlFailure);
      }
    };

    // ---------------- telemetry (slow) ----------------
    std::vector<ThreadRole> roles;
    auto telemetry_body = [&]()
    {
      const auto period = std::chrono::milliseconds(runtime_config.telemetry.stats_period_ms);
      while (!lifecycle.waitForStopOr(period))
      {
        std::cout
            << "telemetry:" << " cap_frames="
            << counters.capture_frames.load(std::memory_order_relaxed)
            << " pb_frames=" << counters.playback_frames.load(std::memory_order_relaxed)
            << " cap_xruns=" << counters.capture_xruns.load(std::memory_order_relaxed)
            << " pb_xruns=" << counters.playback_xruns.load(std::memory_order_relaxed)
            << " pb_write_fail=" << counters.playback_write_failures.load(std::memory_order_relaxed)
            << " cap_wait_timeouts="
            << counters.capture_wait_timeouts.load(std::memory_order_relaxed)
            << " cap_wait_errors=" << counters.capture_wait_errors.load(std::memory_order_relaxed)
            << " cap_overflow_refusals="
            << counters.capture_overflow_refusals.load(std::memory_order_relaxed)
            << " block_commit_fail=" << blocks.commitFailureCount()
            << " pool_exhausted=" << blocks.poolExhaustedCount()
            << " playback_empty_waits=" << blocks.playbackEmptyWaitCount()
            << " ready_high_water=" << blocks.readyHighWater()
            << " free_slots=" << blocks.freeCount()
            << " occupancy=" << counters.ring_occupancy_frames.load(std::memory_order_relaxed)
            << " asrc_ppm=" << counters.asrc_ratio_ppm.load(std::memory_order_relaxed)
            << " asrc_ppm_min=" << counters.asrc_ratio_ppm_min.load(std::memory_order_relaxed)
            << " asrc_ppm_max=" << counters.asrc_ratio_ppm_max.load(std::memory_order_relaxed)
            << " cap_period_us_max="
            << counters.capture_period_max_us.load(std::memory_order_relaxed)
            << " cap_work_us_max=" << counters.capture_work_max_us.load(std::memory_order_relaxed)
            << " cap_deadline_misses="
            << counters.capture_deadline_misses.load(std::memory_order_relaxed)
            << " pb_write_us_max=" << counters.playback_write_max_us.load(std::memory_order_relaxed)
            << " limiter_over_ceiling="
            << counters.limiter_input_over_ceiling_events.load(std::memory_order_relaxed)
            << " limiter_output_saturation="
            << counters.limiter_output_saturation_events.load(std::memory_order_relaxed)
            << " ctl_ticks=" << counters.control_ticks.load(std::memory_order_relaxed)
            << " ctl_applied=" << counters.control_updates_applied.load(std::memory_order_relaxed)
            << " ctl_snapshot_age_us="
            << counters.control_snapshot_age_us.load(std::memory_order_relaxed)
            << " ctl_publish_drops=" << steering_channel.publishDrops()
            << " ctl_superseded=" << steering_channel.superseded()
            << " gate_gain_milli=" << counters.suppressor_gain_milli.load(std::memory_order_relaxed)
            << " confidence_milli="
            << counters.steering_confidence_milli.load(std::memory_order_relaxed)
            << " state=" << static_cast<int>(counters.control_state.load(std::memory_order_relaxed))
            << '\n'
            << std::flush;
      }
    };

    // ------------------------------------------------------------------
    // Phase 3: start every thread with its final policy already applied,
    // verify what the kernel actually gave us, and only then release the
    // gate so any workload runs.
    // ------------------------------------------------------------------
    const sonitude::rt::ThreadSpec capture_spec{
        .name = "snd-capture",
        .sched_class = sonitude::rt::SchedClass::Realtime,
        .priority = runtime_config.realtime.capture_priority,
        .require_realtime = runtime_config.realtime.require_realtime,
        .stack_bytes = rt_stack_bytes,
        .prefault_bytes = rt_prefault_bytes};
    const sonitude::rt::ThreadSpec playback_spec{
        .name = "snd-playback",
        .sched_class = sonitude::rt::SchedClass::Realtime,
        .priority = runtime_config.realtime.playback_priority,
        .require_realtime = runtime_config.realtime.require_realtime,
        .stack_bytes = rt_stack_bytes,
        .prefault_bytes = rt_prefault_bytes};
    const sonitude::rt::ThreadSpec control_spec{.name = "snd-control"};
    const sonitude::rt::ThreadSpec telemetry_spec{.name = "snd-telemetry"};

    roles.push_back({"capture/DSP", &capture_thread, sonitude::rt::SchedClass::Realtime,
                     runtime_config.realtime.capture_priority});
    roles.push_back({"playback", &playback_thread, sonitude::rt::SchedClass::Realtime,
                     runtime_config.realtime.playback_priority});
    roles.push_back({"telemetry", &telemetry_thread, sonitude::rt::SchedClass::Normal, 0});
    if (control_enabled)
    {
      roles.push_back({"control", &control_thread, sonitude::rt::SchedClass::Normal, 0});
    }

    const bool capture_wait_supported = cap_worker.prepareWaiting();
    if (!capture_wait_supported)
    {
      std::cerr << "Warning: capture device cannot be polled; falling back to blocking reads. "
                << "Shutdown will interrupt capture by dropping the stream instead.\n";
    }

    bool started_all = capture_thread.start(capture_spec, &gate, capture_body) &&
                       playback_thread.start(playback_spec, &gate, playback_body) &&
                       telemetry_thread.start(telemetry_spec, &gate, telemetry_body);
    if (started_all && control_enabled)
    {
      started_all = control_thread.start(control_spec, &gate, control_body);
    }

    if (!started_all)
    {
      std::cerr << "Startup failed: could not create runtime threads with the required "
                << "scheduling policy.\n";
      for (const ThreadRole& role : roles)
      {
        if (role.thread != nullptr && !role.thread->joinable() &&
            role.thread->status().create_error != 0)
        {
          std::cerr << "  " << role.role << ": error " << role.thread->status().create_error
                    << '\n';
        }
      }
      gate.abort();
      return 1;
    }

    if (!gate.waitForAll(std::chrono::milliseconds(runtime_config.realtime.startup_timeout_ms)))
    {
      std::cerr << "Startup failed: only " << gate.arrived() << " of " << expected_threads
                << " threads reached the start gate.\n";
      gate.abort();
      return 1;
    }

    PrintSchedulingTable(roles);
    if (!ValidateScheduling(roles, runtime_config.realtime.require_realtime))
    {
      std::cerr << "Startup failed: thread scheduling does not satisfy the realtime contract.\n";
      gate.abort();
      return 1;
    }

    std::cout << "Running " << mode << " mode. Press Ctrl+C to stop.\n";
    gate.release();

    // ------------------------------------------------------------------
    // Phase 4: supervise. The supervisor owns the only stop authority and
    // performs the teardown in a fixed order.
    // ------------------------------------------------------------------
    while (!lifecycle.waitForStopOr(std::chrono::milliseconds(200)))
    {
    }

    const sonitude::rt::StopReason reason = lifecycle.reason();
    std::cout << "Shutting down (" << sonitude::rt::StopReasonName(reason) << ").\n";

    // Wake anything that could still be blocked, then join in pipeline order.
    // The lifecycle wake descriptor already released every poll-based wait; the
    // one case it cannot reach is a capture device that could not be polled, so
    // that read is interrupted by dropping the stream. This is done by the
    // supervisor, once, instead of by a millisecond-polling helper thread.
    playback_wake.signal();
    if (!capture_wait_supported)
    {
      cap.dropStream();
    }
    capture_thread.join();
    playback_thread.join();
    control_thread.join();
    telemetry_thread.join();

    const std::int64_t asrc_ppm_min =
        counters.asrc_ratio_ppm_has_sample.load(std::memory_order_relaxed)
            ? counters.asrc_ratio_ppm_min.load(std::memory_order_relaxed)
            : 0;
    const std::int64_t asrc_ppm_max =
        counters.asrc_ratio_ppm_has_sample.load(std::memory_order_relaxed)
            ? counters.asrc_ratio_ppm_max.load(std::memory_order_relaxed)
            : 0;
    std::cout
        << "final_counters:" << " stop_reason=" << sonitude::rt::StopReasonName(reason)
        << " cap_xruns=" << counters.capture_xruns.load(std::memory_order_relaxed)
        << " pb_xruns=" << counters.playback_xruns.load(std::memory_order_relaxed)
        << " pb_write_fail=" << counters.playback_write_failures.load(std::memory_order_relaxed)
        << " block_commit_fail=" << blocks.commitFailureCount()
        << " pool_exhausted=" << blocks.poolExhaustedCount()
        << " playback_empty_waits=" << blocks.playbackEmptyWaitCount() << " cap_deadline_misses="
        << counters.capture_deadline_misses.load(std::memory_order_relaxed)
        << " cap_period_us_max=" << counters.capture_period_max_us.load(std::memory_order_relaxed)
        << " cap_work_us_max=" << counters.capture_work_max_us.load(std::memory_order_relaxed)
        << " pb_write_us_max=" << counters.playback_write_max_us.load(std::memory_order_relaxed)
        << " asrc_ppm_current=" << counters.asrc_ratio_ppm.load(std::memory_order_relaxed)
        << " asrc_ppm_min=" << asrc_ppm_min << " asrc_ppm_max=" << asrc_ppm_max
        << " limiter_over_ceiling="
        << counters.limiter_input_over_ceiling_events.load(std::memory_order_relaxed)
        << " limiter_output_saturation="
        << counters.limiter_output_saturation_events.load(std::memory_order_relaxed) << '\n';

    // Only now is it safe to touch the devices: every thread that could have
    // used them has been joined.
    cap.dropStream();
    pb.dropStream();
    cap.close();
    pb.close();

    if (reason == sonitude::rt::StopReason::PlaybackFailure)
    {
      std::cerr << "Playback write failed repeatedly; audio pipeline stopped.\n";
      return 1;
    }
    if (reason == sonitude::rt::StopReason::CaptureFailure ||
        reason == sonitude::rt::StopReason::ControlFailure)
    {
      return 1;
    }
    return 0;
#else
    std::cout << "Passthrough mode is unavailable because this build has no ALSA support.\n";
    return 0;
#endif
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Startup failed: " << ex.what() << '\n';
    return 1;
  }
}
