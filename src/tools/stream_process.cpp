// sonitude_stream_process — portable block-streaming adapter around the existing
// calibration -> beamformer -> suppressor -> limiter chain (the same objects used by
// sonitude_realtime and sonitude_wav_replay). It performs no device I/O of its own:
// it reads framed 6-channel PCM blocks from stdin and writes framed stereo PCM blocks
// to stdout, so any caller (e.g. the Python test bench driving a microphone/speaker via
// sounddevice) can push live audio through the unmodified DSP chain without linking
// against it directly or reimplementing any of the algorithm.
//
// Wire protocol (all fields little-endian, binary stdin/stdout):
//
//   Input block (host -> this process), repeated until a frame_count == 0 block:
//     u32 magic            = kInputMagic
//     u32 frame_count
//     f32 azimuth_deg
//     f32 elevation_deg
//     f32 width_deg          (0..kMaxWidthDeg; see below)
//     u8  suppression_focus_active (0 or 1)
//     u8  reserved[3]
//     f32 pcm[frame_count * kMicChannels]   // interleaved, range [-1, 1]
//
//   Output block (this process -> host), one per input block:
//     u32 magic            = kOutputMagic
//     u32 frame_count
//     f32 pcm[frame_count * 2]              // interleaved stereo L/R
//
// A frame_count of 0 in an input block is a clean shutdown request; this process exits 0.
// Any framing/magic mismatch is treated as a fatal protocol error.
//
// width_deg: DelaySumBeamformer has no native "beam width" parameter (a
// fixed delay-and-sum array has a fixed spatial response). This tool defines
// width as a directivity blend applied to its own beamformer output: the
// mono beam is linearly blended toward a simple omnidirectional average of
// the six calibrated mic channels, in proportion to width_deg / kMaxWidthDeg.
// 0 = fully directional (unchanged from before width existed), kMaxWidthDeg
// = fully omnidirectional. See testbench/README.md, "Steering width
// definition" for the rationale; sonitude_wav_replay applies the identical
// blend for batch mode.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/binaural_renderer.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/hrtf_table.hpp"
#include "dsp/limiter.hpp"
#include "dsp/suppressor.hpp"

namespace
{
constexpr std::uint32_t kInputMagic = 0x31424253U;   // "SBB1" little-endian on disk
constexpr std::uint32_t kInputMagicV2 = 0x32424253U; // "SBB2" little-endian on disk
constexpr std::uint32_t kOutputMagic = 0x314F4253U;  // "SBO1" little-endian on disk
constexpr float kMaxWidthDeg = 180.0F;               // matches sonitude_wav_replay's kMaxWidthDeg

sonitude::dsp::BinauralBackend ParseBinauralBackendByte(const std::uint8_t value)
{
  switch (value)
  {
    case 0:
      return sonitude::dsp::BinauralBackend::MonoReference;
    case 1:
      return sonitude::dsp::BinauralBackend::ItdIld;
    case 2:
      return sonitude::dsp::BinauralBackend::CompactHrtf;
    case 3:
      return sonitude::dsp::BinauralBackend::FullHrtfReference;
    default:
      throw std::runtime_error("Unknown binaural backend byte");
  }
}

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_stream_process --config <runtime_yaml>\n"
            << "                          [--sample-rate <hz>] [--max-block-frames <n>]\n"
            << "                          [--enable-suppression] [--disable-limiter]\n"
            << "\n"
            << "  Reads framed 6-channel PCM blocks from stdin, runs them through the\n"
            << "  existing calibration/beamformer/suppressor/limiter chain, and writes\n"
            << "  framed stereo PCM blocks to stdout. See file header for wire format,\n"
            << "  including the per-block width_deg directivity-blend parameter.\n";
}

bool ReadExact(std::istream& in, void* dest, const std::size_t bytes)
{
  in.read(reinterpret_cast<char*>(dest), static_cast<std::streamsize>(bytes));
  return static_cast<bool>(in);
}

void WriteExact(std::ostream& out, const void* src, const std::size_t bytes)
{
  out.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(bytes));
}
}  // namespace

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  std::uint32_t sample_rate_override = 0;
  std::size_t max_block_frames = 8192;
  bool enable_suppression = false;
  bool disable_limiter = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--config" && i + 1 < argc)
    {
      config_path = argv[++i];
    }
    else if (arg == "--sample-rate" && i + 1 < argc)
    {
      sample_rate_override = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--max-block-frames" && i + 1 < argc)
    {
      max_block_frames = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--enable-suppression")
    {
      enable_suppression = true;
    }
    else if (arg == "--disable-limiter")
    {
      disable_limiter = true;
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else
    {
      std::cerr << "Unknown or incomplete argument: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }

#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  try
  {
    const auto runtime = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    const auto geometry = sonitude::app::LoadGeometryFromFile(runtime.geometry_path);
    const auto calibration = sonitude::app::LoadCalibrationFromFile(runtime.calibration_path);
    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }

    const std::uint32_t sample_rate_hz =
        sample_rate_override != 0 ? sample_rate_override : runtime.capture.sample_rate_hz;
    sonitude::app::ValidateCalibrationConfig(calibration, geometry_ids, sample_rate_hz);

    sonitude::dsp::CalibrationApplier calibration_applier(
        calibration.channels, geometry_ids, sample_rate_hz, runtime.calibration_dc_block_hz);

    sonitude::dsp::DelaySumBeamformer beamformer;
    beamformer.configure(geometry, runtime.steering, calibration, sample_rate_hz, max_block_frames);

    const bool suppression_enabled = enable_suppression || runtime.suppression.enabled;
    sonitude::dsp::ConservativeSuppressor suppressor;
    suppressor.configure(
        {.ambient_floor_linear = runtime.steering.ambient_floor_linear,
         .fade_ms = runtime.suppression.fade_ms,
         .activity_threshold = runtime.suppression.activity_threshold,
         .confidence_threshold = runtime.suppression.confidence_threshold},
        sample_rate_hz);

    sonitude::dsp::PeakLimiter limiter;
    sonitude::dsp::StereoPeakLimiter stereo_limiter;
    const bool runtime_binaural_enabled = runtime.binaural.enabled;
    std::unique_ptr<sonitude::dsp::HrtfTable> runtime_hrtf_table;
    sonitude::dsp::BinauralRenderer runtime_binaural;
    if (runtime_binaural_enabled)
    {
      sonitude::dsp::BinauralBackend backend = sonitude::dsp::BinauralBackend::MonoReference;
      if (runtime.binaural.backend == "mono_reference")
      {
        backend = sonitude::dsp::BinauralBackend::MonoReference;
      }
      else if (runtime.binaural.backend == "itd_ild")
      {
        backend = sonitude::dsp::BinauralBackend::ItdIld;
      }
      else if (runtime.binaural.backend == "compact_hrtf")
      {
        backend = sonitude::dsp::BinauralBackend::CompactHrtf;
      }
      else if (runtime.binaural.backend == "full_hrtf_reference")
      {
        backend = sonitude::dsp::BinauralBackend::FullHrtfReference;
      }
      else
      {
        throw std::runtime_error("Unknown binaural backend: " + runtime.binaural.backend);
      }
      if (backend == sonitude::dsp::BinauralBackend::CompactHrtf ||
          backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
      {
        if (runtime.binaural.profile.table_path.empty())
        {
          throw std::runtime_error("binaural.profile.table_path is required for HRTF backend");
        }
        runtime_hrtf_table = std::make_unique<sonitude::dsp::HrtfTable>(
            sonitude::dsp::LoadHrtfTableFromFile(runtime.binaural.profile.table_path));
      }
      runtime_binaural.configure({.sample_rate_hz = sample_rate_hz,
                                  .backend = backend,
                                  .transition_ms = runtime.binaural.transition.duration_ms,
                                  .max_block_frames = max_block_frames,
                                  .itd_ild = {.head_radius_m = runtime.binaural.model.head_radius_m,
                                              .max_ild_db = runtime.binaural.model.max_ild_db},
                                  .table = runtime_hrtf_table.get()});
      runtime_binaural.setDirection({runtime.binaural.direction.azimuth_deg,
                                     runtime.binaural.direction.elevation_deg});
      stereo_limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, sample_rate_hz);
    }
    else
    {
      limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, sample_rate_hz);
    }

    std::cerr << "sonitude_stream_process ready: sample_rate_hz=" << sample_rate_hz
              << " suppression=" << (suppression_enabled ? "on" : "off")
              << " limiter=" << (disable_limiter ? "off" : "on") << '\n';

    std::vector<sonitude::audio::MicFrame> mic_frames;
    std::vector<sonitude::audio::MicFrame> calibrated_frames;
    std::vector<float> mono;
    std::vector<float> input_pcm;
    std::vector<float> output_pcm;
    std::vector<float> left;
    std::vector<float> right;
    bool have_target = false;
    sonitude::audio::BeamformerSteering last_target{};

    while (true)
    {
      std::uint32_t magic = 0;
      if (!ReadExact(std::cin, &magic, sizeof(magic)))
      {
        break;  // stdin closed: treat as shutdown.
      }
      if (magic != kInputMagic && magic != kInputMagicV2)
      {
        std::cerr << "stream_process: bad input magic, aborting\n";
        return 1;
      }
      const bool use_v2 = (magic == kInputMagicV2);

      std::uint32_t frame_count = 0;
      float azimuth_deg = 0.0F;
      float elevation_deg = 0.0F;
      float width_deg = 0.0F;
      std::uint8_t suppression_focus_active = 0;
      std::uint8_t reserved[3] = {0, 0, 0};
      std::uint8_t binaural_enabled = 0;
      std::uint8_t binaural_backend = 0;
      std::uint8_t binaural_follow_steering = 1;
      std::uint8_t binaural_reserved = 0;
      float binaural_azimuth_deg = 0.0F;
      float binaural_elevation_deg = 0.0F;
      if (!ReadExact(std::cin, &frame_count, sizeof(frame_count)) ||
          !ReadExact(std::cin, &azimuth_deg, sizeof(azimuth_deg)) ||
          !ReadExact(std::cin, &elevation_deg, sizeof(elevation_deg)) ||
          !ReadExact(std::cin, &width_deg, sizeof(width_deg)) ||
          !ReadExact(std::cin, &suppression_focus_active, sizeof(suppression_focus_active)) ||
          !ReadExact(std::cin, reserved, sizeof(reserved)))
      {
        std::cerr << "stream_process: truncated block header, aborting\n";
        return 1;
      }
      if (use_v2)
      {
        if (!ReadExact(std::cin, &binaural_enabled, sizeof(binaural_enabled)) ||
            !ReadExact(std::cin, &binaural_backend, sizeof(binaural_backend)) ||
            !ReadExact(std::cin, &binaural_follow_steering, sizeof(binaural_follow_steering)) ||
            !ReadExact(std::cin, &binaural_reserved, sizeof(binaural_reserved)) ||
            !ReadExact(std::cin, &binaural_azimuth_deg, sizeof(binaural_azimuth_deg)) ||
            !ReadExact(std::cin, &binaural_elevation_deg, sizeof(binaural_elevation_deg)))
        {
          std::cerr << "stream_process: truncated v2 extension header, aborting\n";
          return 1;
        }
      }

      if (frame_count == 0)
      {
        break;  // clean shutdown request.
      }
      if (frame_count > max_block_frames)
      {
        std::cerr << "stream_process: frame_count exceeds --max-block-frames, aborting\n";
        return 1;
      }

      const std::size_t frames = frame_count;
      input_pcm.assign(frames * sonitude::audio::kMicChannels, 0.0F);
      if (!ReadExact(std::cin, input_pcm.data(), input_pcm.size() * sizeof(float)))
      {
        std::cerr << "stream_process: truncated PCM payload, aborting\n";
        return 1;
      }

      mic_frames.assign(frames, sonitude::audio::MicFrame{});
      for (std::size_t i = 0; i < frames; ++i)
      {
        for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
        {
          mic_frames[i][ch] = input_pcm[(i * sonitude::audio::kMicChannels) + ch];
        }
      }

      calibrated_frames.assign(frames, sonitude::audio::MicFrame{});
      calibration_applier.processBlock(
          std::span<const sonitude::audio::MicFrame>(mic_frames.data(), frames),
          std::span<sonitude::audio::MicFrame>(calibrated_frames.data(), frames));

      const sonitude::audio::BeamformerSteering target{azimuth_deg, elevation_deg};
      if (!have_target || std::fabs(target.azimuth_deg - last_target.azimuth_deg) > 0.01F ||
          std::fabs(target.elevation_deg - last_target.elevation_deg) > 0.01F)
      {
        beamformer.setTarget(target);
        last_target = target;
        have_target = true;
      }

      mono.assign(frames, 0.0F);
      beamformer.process(std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frames),
                         std::span<float>(mono.data(), frames));

      const float clamped_width_deg = std::clamp(width_deg, 0.0F, kMaxWidthDeg);
      if (clamped_width_deg > 0.0F)
      {
        const float blend = clamped_width_deg / kMaxWidthDeg;
        for (std::size_t i = 0; i < frames; ++i)
        {
          float omni = 0.0F;
          for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
          {
            omni += calibrated_frames[i][ch];
          }
          omni /= static_cast<float>(sonitude::audio::kMicChannels);
          mono[i] = ((1.0F - blend) * mono[i]) + (blend * omni);
        }
      }

      if (suppression_enabled)
      {
        suppressor.setControl(suppression_focus_active != 0, suppression_focus_active != 0 ? 1.0F : 0.0F);
        suppressor.process(std::span<float>(mono.data(), frames));
      }
      const bool effective_binaural_enabled = use_v2 ? (binaural_enabled != 0) : runtime_binaural_enabled;
      left.assign(frames, 0.0F);
      right.assign(frames, 0.0F);
      if (effective_binaural_enabled)
      {
        if (!runtime_binaural_enabled)
        {
          std::cerr << "stream_process: binaural requested but runtime config has binaural disabled\n";
          return 1;
        }
        sonitude::dsp::BinauralBackend active_backend =
            runtime_binaural_enabled ? runtime_binaural.resolvedBackend()
                                     : sonitude::dsp::BinauralBackend::MonoReference;
        if (use_v2 && runtime_binaural_enabled)
        {
          const auto requested_backend = ParseBinauralBackendByte(binaural_backend);
          if (requested_backend != runtime_binaural.resolvedBackend())
          {
            std::cerr << "stream_process: requested backend not available, using runtime backend\n";
          }
        }
        const sonitude::audio::BeamformerSteering binaural_target =
            (use_v2 && binaural_follow_steering == 0)
                ? sonitude::audio::BeamformerSteering{binaural_azimuth_deg, binaural_elevation_deg}
                : target;
        runtime_binaural.setDirection(binaural_target);
        runtime_binaural.process(
            std::span<const float>(mono.data(), frames), std::span<float>(left.data(), frames), std::span<float>(right.data(), frames));
        if (!disable_limiter)
        {
          stereo_limiter.process(std::span<float>(left.data(), frames),
                                 std::span<float>(right.data(), frames));
        }
        std::cerr << "binaural backend=" << static_cast<int>(active_backend)
                  << " az=" << binaural_target.azimuth_deg << " el=" << binaural_target.elevation_deg
                  << '\n';
      }
      else
      {
        if (!disable_limiter)
        {
          limiter.process(std::span<float>(mono.data(), frames));
        }
        for (std::size_t i = 0; i < frames; ++i)
        {
          left[i] = mono[i];
          right[i] = mono[i];
        }
      }

      output_pcm.assign(frames * 2, 0.0F);
      for (std::size_t i = 0; i < frames; ++i)
      {
        output_pcm[(i * 2) + 0] = left[i];
        output_pcm[(i * 2) + 1] = right[i];
      }

      WriteExact(std::cout, &kOutputMagic, sizeof(kOutputMagic));
      WriteExact(std::cout, &frame_count, sizeof(frame_count));
      WriteExact(std::cout, output_pcm.data(), output_pcm.size() * sizeof(float));
      std::cout.flush();
    }

    std::cerr << "sonitude_stream_process exiting cleanly\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "stream_process failed: " << ex.what() << '\n';
    return 1;
  }
}
