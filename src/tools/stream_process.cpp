// sonitude_stream_process — framed stdin/stdout adapter around the existing
// calibration -> beamformer -> suppressor -> limiter chain, plus optional
// binaural rendering.
//
// Protocol v2 (little-endian). See testbench/app/processing/protocol.py.
//
// Input header (48 bytes):
//   u32 magic              = 0x32424253 ("SBB2")
//   u16 protocol_version   = 2
//   u16 message_type       = AUDIO_BLOCK(1) | SHUTDOWN(2)
//   u32 sequence
//   u32 frame_count
//   u32 flags
//   u32 payload_length     = frame_count * 6 * sizeof(float) for AUDIO_BLOCK
//   f32 azimuth_deg
//   f32 elevation_deg
//   f32 directivity_blend_deg   // 0..180 mix toward 6-mic average; NOT HPBW
//   f32 binaural_azimuth_deg
//   f32 binaural_elevation_deg
//   u8  binaural_backend
//   u8  reserved[3]
//   f32 pcm[frame_count * 6]    // only if payload_length > 0
//
// Output header (24 bytes):
//   u32 magic              = 0x324F4253 ("SBO2")
//   u16 protocol_version   = 2
//   u16 message_type
//   u32 sequence           // echoes the request
//   u32 frame_count
//   u32 flags
//   u32 payload_length
//   f32 pcm[frame_count * 2]
//
// Input flags: bit0 suppression_focus, bit1 binaural_enabled, bit2 follow_steering
// Output flags: bit0 suppression_applied, bit1 binaural_applied,
//               bit2 binaural_unavailable, bit3 mono_reference
//
// Backends: 0 none, 1 mono_reference, 2 itd_ild, 3 compact_hrtf, 4 full_hrtf_reference.
// Requesting an unimplemented or unloadable backend sets BINAURAL_UNAVAILABLE
// and emits L=R of directional mono (protocol-defined fallback).

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
constexpr std::uint32_t kInputMagic = 0x32424253U;
constexpr std::uint32_t kOutputMagic = 0x324F4253U;
constexpr std::uint16_t kProtocolVersion = 2;
constexpr std::uint16_t kMsgAudioBlock = 1;
constexpr std::uint16_t kMsgShutdown = 2;
constexpr std::uint16_t kMsgError = 3;
constexpr float kMaxBlendDeg = 180.0F;
constexpr std::uint32_t kFlagSuppressionFocus = 1U << 0;
constexpr std::uint32_t kFlagBinauralEnabled = 1U << 1;
constexpr std::uint32_t kFlagBinauralFollowSteering = 1U << 2;
constexpr std::uint32_t kOutSuppressionApplied = 1U << 0;
constexpr std::uint32_t kOutBinauralApplied = 1U << 1;
constexpr std::uint32_t kOutBinauralUnavailable = 1U << 2;
constexpr std::uint32_t kOutMonoReference = 1U << 3;
constexpr std::uint8_t kBackendNone = 0;
constexpr std::uint8_t kBackendMonoReference = 1;
constexpr std::uint8_t kBackendItdIld = 2;
constexpr std::uint8_t kBackendCompactHrtf = 3;
constexpr std::uint8_t kBackendFullHrtfReference = 4;

enum class SuppressionMode
{
  Auto,
  On,
  Off
};

struct BinauralRuntime
{
  std::unique_ptr<sonitude::dsp::HrtfTable> compact_table;
  std::unique_ptr<sonitude::dsp::HrtfTable> reference_table;
  sonitude::dsp::BinauralRenderer renderer;
  sonitude::dsp::StereoPeakLimiter stereo_limiter;
  bool renderer_configured = false;
  bool stereo_limiter_configured = false;
  sonitude::dsp::BinauralBackend configured_backend = sonitude::dsp::BinauralBackend::MonoReference;
};

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_stream_process --config <runtime_yaml>\n"
            << "                          [--sample-rate <hz>] [--max-block-frames <n>]\n"
            << "                          [--suppression auto|on|off]\n"
            << "                          [--enable-suppression] [--disable-suppression]\n"
            << "                          [--disable-limiter]\n"
            << "                          [--capabilities]\n";
}

void PrintCapabilities()
{
  std::cout << "{"
            << "\"protocol_version\":2,"
            << "\"suppression\":{\"modes\":[\"auto\",\"on\",\"off\"]},"
            << "\"taps\":[\"processed\"],"
            << "\"binaural\":{"
            << "\"available\":true,"
            << "\"backends\":[\"mono_reference\",\"itd_ild\",\"compact_hrtf\",\"full_hrtf_reference\"],"
            << "\"unavailable_backends\":[],"
            << "\"note\":\"ITD/ILD and SADIE II D2 HRTF tables are implemented. "
               "Unavailable at runtime only if the requested HRTF table cannot be loaded.\""
            << "}"
            << "}\n";
}

bool ReadExact(std::istream& in, void* dest, const std::size_t bytes)
{
  in.read(reinterpret_cast<char*>(dest), static_cast<std::streamsize>(bytes));
  return static_cast<bool>(in) && in.gcount() == static_cast<std::streamsize>(bytes);
}

void WriteExact(std::ostream& out, const void* src, const std::size_t bytes)
{
  out.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(bytes));
}

SuppressionMode ParseSuppressionMode(const std::string& value)
{
  if (value == "on" || value == "enable" || value == "enabled")
  {
    return SuppressionMode::On;
  }
  if (value == "off" || value == "disable" || value == "disabled")
  {
    return SuppressionMode::Off;
  }
  return SuppressionMode::Auto;
}

bool ResolveSuppression(const SuppressionMode mode, const bool yaml_enabled)
{
  if (mode == SuppressionMode::On)
  {
    return true;
  }
  if (mode == SuppressionMode::Off)
  {
    return false;
  }
  return yaml_enabled;
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
    std::cerr << "stream_process: HRTF table not loaded from " << path << ": " << ex.what() << '\n';
    return {};
  }
}

bool BackendFromByte(const std::uint8_t value, sonitude::dsp::BinauralBackend& backend)
{
  switch (value)
  {
    case kBackendNone:
    case kBackendMonoReference:
      backend = sonitude::dsp::BinauralBackend::MonoReference;
      return true;
    case kBackendItdIld:
      backend = sonitude::dsp::BinauralBackend::ItdIld;
      return true;
    case kBackendCompactHrtf:
      backend = sonitude::dsp::BinauralBackend::CompactHrtf;
      return true;
    case kBackendFullHrtfReference:
      backend = sonitude::dsp::BinauralBackend::FullHrtfReference;
      return true;
    default:
      return false;
  }
}

const sonitude::dsp::HrtfTable* TableFor(const BinauralRuntime& runtime,
                                         const sonitude::dsp::BinauralBackend backend)
{
  if (backend == sonitude::dsp::BinauralBackend::CompactHrtf)
  {
    return runtime.compact_table.get();
  }
  if (backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
  {
    return runtime.reference_table.get();
  }
  return nullptr;
}

bool BackendReady(const BinauralRuntime& runtime, const sonitude::dsp::BinauralBackend backend)
{
  if (backend == sonitude::dsp::BinauralBackend::CompactHrtf)
  {
    return runtime.compact_table && !runtime.compact_table->empty();
  }
  if (backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
  {
    return runtime.reference_table && !runtime.reference_table->empty();
  }
  return true;
}

std::string AvailableBackendsJson(const BinauralRuntime& runtime)
{
  std::string json = "[\"mono_reference\",\"itd_ild\"";
  if (runtime.compact_table && !runtime.compact_table->empty())
  {
    json += ",\"compact_hrtf\"";
  }
  if (runtime.reference_table && !runtime.reference_table->empty())
  {
    json += ",\"full_hrtf_reference\"";
  }
  json += "]";
  return json;
}
}  // namespace

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  std::uint32_t sample_rate_override = 0;
  std::size_t max_block_frames = 8192;
  SuppressionMode suppression_mode = SuppressionMode::Auto;
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
    else if (arg == "--suppression" && i + 1 < argc)
    {
      suppression_mode = ParseSuppressionMode(argv[++i]);
    }
    else if (arg == "--enable-suppression")
    {
      suppression_mode = SuppressionMode::On;
    }
    else if (arg == "--disable-suppression")
    {
      suppression_mode = SuppressionMode::Off;
    }
    else if (arg == "--disable-limiter")
    {
      disable_limiter = true;
    }
    else if (arg == "--capabilities")
    {
      PrintCapabilities();
      return 0;
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

    const bool suppression_enabled = ResolveSuppression(suppression_mode, runtime.suppression.enabled);
    sonitude::dsp::ConservativeSuppressor suppressor;
    suppressor.configure(
        {.ambient_floor_linear = runtime.steering.ambient_floor_linear,
         .fade_ms = runtime.suppression.fade_ms,
         .activity_threshold = runtime.suppression.activity_threshold,
         .confidence_threshold = runtime.suppression.confidence_threshold},
        sample_rate_hz);

    sonitude::dsp::PeakLimiter limiter;
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, sample_rate_hz);

    BinauralRuntime binaural_runtime;
    binaural_runtime.compact_table = TryLoadHrtfTable(runtime.binaural.profile.table_path);
    if (!runtime.binaural.profile.table_path.empty())
    {
      binaural_runtime.reference_table =
          TryLoadHrtfTable(SiblingFile(runtime.binaural.profile.table_path, "reference.shrf"));
    }

    const char* requested = suppression_mode == SuppressionMode::On
                                ? "on"
                                : (suppression_mode == SuppressionMode::Off ? "off" : "auto");
    std::cerr << "sonitude_resolved {\"protocol_version\":2,\"suppression_requested\":\"" << requested
              << "\",\"suppression_resolved\":" << (suppression_enabled ? "true" : "false")
              << ",\"limiter_disabled\":" << (disable_limiter ? "true" : "false")
              << ",\"binaural_backends\":" << AvailableBackendsJson(binaural_runtime) << "}\n";
    std::cerr << "sonitude_stream_process ready: sample_rate_hz=" << sample_rate_hz
              << " suppression=" << (suppression_enabled ? "on" : "off")
              << " limiter=" << (disable_limiter ? "off" : "on") << '\n';

    std::vector<sonitude::audio::MicFrame> mic_frames;
    std::vector<sonitude::audio::MicFrame> calibrated_frames;
    std::vector<float> mono;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> input_pcm;
    std::vector<float> output_pcm;
    bool have_target = false;
    sonitude::audio::BeamformerSteering last_target{};
    std::uint32_t expected_sequence = 0;
    bool have_sequence = false;

    while (true)
    {
      std::uint32_t magic = 0;
      if (!ReadExact(std::cin, &magic, sizeof(magic)))
      {
        break;
      }
      if (magic != kInputMagic)
      {
        std::cerr << "stream_process: invalid magic, aborting\n";
        return 1;
      }

      std::uint16_t version = 0;
      std::uint16_t message_type = 0;
      std::uint32_t sequence = 0;
      std::uint32_t frame_count = 0;
      std::uint32_t flags = 0;
      std::uint32_t payload_length = 0;
      float azimuth_deg = 0.0F;
      float elevation_deg = 0.0F;
      float blend_deg = 0.0F;
      float binaural_az = 0.0F;
      float binaural_el = 0.0F;
      std::uint8_t binaural_backend = 0;
      std::uint8_t reserved[3] = {0, 0, 0};
      if (!ReadExact(std::cin, &version, sizeof(version)) ||
          !ReadExact(std::cin, &message_type, sizeof(message_type)) ||
          !ReadExact(std::cin, &sequence, sizeof(sequence)) ||
          !ReadExact(std::cin, &frame_count, sizeof(frame_count)) ||
          !ReadExact(std::cin, &flags, sizeof(flags)) ||
          !ReadExact(std::cin, &payload_length, sizeof(payload_length)) ||
          !ReadExact(std::cin, &azimuth_deg, sizeof(azimuth_deg)) ||
          !ReadExact(std::cin, &elevation_deg, sizeof(elevation_deg)) ||
          !ReadExact(std::cin, &blend_deg, sizeof(blend_deg)) ||
          !ReadExact(std::cin, &binaural_az, sizeof(binaural_az)) ||
          !ReadExact(std::cin, &binaural_el, sizeof(binaural_el)) ||
          !ReadExact(std::cin, &binaural_backend, sizeof(binaural_backend)) ||
          !ReadExact(std::cin, reserved, sizeof(reserved)))
      {
        std::cerr << "stream_process: truncated header, aborting\n";
        return 1;
      }

      if (version != kProtocolVersion)
      {
        std::cerr << "stream_process: unsupported protocol version " << version << "\n";
        return 1;
      }
      if (message_type == kMsgShutdown || frame_count == 0)
      {
        break;
      }
      if (message_type != kMsgAudioBlock)
      {
        std::cerr << "stream_process: unknown message type " << message_type << "\n";
        return 1;
      }
      if (frame_count > max_block_frames)
      {
        std::cerr << "stream_process: invalid frame_count, aborting\n";
        return 1;
      }
      const std::uint32_t expected_payload =
          frame_count * static_cast<std::uint32_t>(sonitude::audio::kMicChannels) * sizeof(float);
      if (payload_length != expected_payload)
      {
        std::cerr << "stream_process: invalid payload_length, aborting\n";
        return 1;
      }
      if (have_sequence && sequence != expected_sequence)
      {
        std::cerr << "stream_process: unexpected sequence " << sequence << " expected " << expected_sequence
                  << "\n";
        return 1;
      }
      have_sequence = true;
      expected_sequence = sequence + 1;

      input_pcm.assign(frame_count * sonitude::audio::kMicChannels, 0.0F);
      if (!ReadExact(std::cin, input_pcm.data(), payload_length))
      {
        std::cerr << "stream_process: truncated PCM payload, aborting\n";
        return 1;
      }

      mic_frames.assign(frame_count, sonitude::audio::MicFrame{});
      for (std::size_t i = 0; i < frame_count; ++i)
      {
        for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
        {
          mic_frames[i][ch] = input_pcm[(i * sonitude::audio::kMicChannels) + ch];
        }
      }

      calibrated_frames.assign(frame_count, sonitude::audio::MicFrame{});
      calibration_applier.processBlock(
          std::span<const sonitude::audio::MicFrame>(mic_frames.data(), frame_count),
          std::span<sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count));

      const sonitude::audio::BeamformerSteering target{azimuth_deg, elevation_deg};
      if (!have_target || std::fabs(target.azimuth_deg - last_target.azimuth_deg) > 0.01F ||
          std::fabs(target.elevation_deg - last_target.elevation_deg) > 0.01F)
      {
        beamformer.setTarget(target);
        last_target = target;
        have_target = true;
      }

      mono.assign(frame_count, 0.0F);
      beamformer.process(std::span<const sonitude::audio::MicFrame>(calibrated_frames.data(), frame_count),
                         std::span<float>(mono.data(), frame_count));

      const float clamped_blend = std::clamp(blend_deg, 0.0F, kMaxBlendDeg);
      if (clamped_blend > 0.0F)
      {
        const float mix = clamped_blend / kMaxBlendDeg;
        for (std::size_t i = 0; i < frame_count; ++i)
        {
          float omni = 0.0F;
          for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
          {
            omni += calibrated_frames[i][ch];
          }
          omni /= static_cast<float>(sonitude::audio::kMicChannels);
          mono[i] = ((1.0F - mix) * mono[i]) + (mix * omni);
        }
      }

      std::uint32_t out_flags = 0;
      if (suppression_enabled)
      {
        const bool focus_active = (flags & kFlagSuppressionFocus) != 0;
        suppressor.setControl(focus_active, focus_active ? 1.0F : 0.0F);
        suppressor.process(std::span<float>(mono.data(), frame_count));
        out_flags |= kOutSuppressionApplied;
      }

      const bool binaural_requested = (flags & kFlagBinauralEnabled) != 0;
      const bool follow_steering = (flags & kFlagBinauralFollowSteering) != 0;
      sonitude::dsp::BinauralBackend backend = sonitude::dsp::BinauralBackend::MonoReference;
      bool unavailable = false;
      if (binaural_requested)
      {
        if (!BackendFromByte(binaural_backend, backend) || !BackendReady(binaural_runtime, backend))
        {
          unavailable = true;
          backend = sonitude::dsp::BinauralBackend::MonoReference;
        }
      }

      if (binaural_requested && !unavailable)
      {
        if (!binaural_runtime.renderer_configured || binaural_runtime.configured_backend != backend)
        {
          try
          {
            binaural_runtime.renderer.configure(
                {.sample_rate_hz = sample_rate_hz,
                 .backend = backend,
                 .transition_ms = runtime.binaural.transition.duration_ms,
                 .max_block_frames = max_block_frames,
                 .itd_ild = {.head_radius_m = runtime.binaural.model.head_radius_m,
                             .max_ild_db = runtime.binaural.model.max_ild_db},
                 .table = TableFor(binaural_runtime, backend)});
            binaural_runtime.configured_backend = backend;
            binaural_runtime.renderer_configured = true;
          }
          catch (const std::exception& ex)
          {
            std::cerr << "stream_process: binaural configure failed: " << ex.what() << '\n';
            unavailable = true;
          }
        }
      }

      left.assign(frame_count, 0.0F);
      right.assign(frame_count, 0.0F);
      if (binaural_requested && !unavailable)
      {
        const sonitude::audio::BeamformerSteering binaural_dir =
            follow_steering ? target : sonitude::audio::BeamformerSteering{binaural_az, binaural_el};
        binaural_runtime.renderer.setDirection(binaural_dir);
        binaural_runtime.renderer.process(std::span<const float>(mono.data(), frame_count),
                                          std::span<float>(left.data(), frame_count),
                                          std::span<float>(right.data(), frame_count));
        if (!disable_limiter)
        {
          if (!binaural_runtime.stereo_limiter_configured)
          {
            binaural_runtime.stereo_limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F},
                                                      sample_rate_hz);
            binaural_runtime.stereo_limiter_configured = true;
          }
          binaural_runtime.stereo_limiter.process(std::span<float>(left.data(), frame_count),
                                                  std::span<float>(right.data(), frame_count));
        }
        out_flags |= kOutBinauralApplied;
        if (backend == sonitude::dsp::BinauralBackend::MonoReference)
        {
          out_flags |= kOutMonoReference;
        }
      }
      else
      {
        if (!disable_limiter)
        {
          limiter.process(std::span<float>(mono.data(), frame_count));
        }
        for (std::size_t i = 0; i < frame_count; ++i)
        {
          left[i] = mono[i];
          right[i] = mono[i];
        }
        out_flags |= kOutMonoReference;
        if (unavailable)
        {
          out_flags |= kOutBinauralUnavailable;
        }
        if (binaural_requested)
        {
          out_flags |= kOutBinauralApplied;
        }
      }

      output_pcm.assign(frame_count * 2, 0.0F);
      for (std::size_t i = 0; i < frame_count; ++i)
      {
        output_pcm[(i * 2) + 0] = left[i];
        output_pcm[(i * 2) + 1] = right[i];
      }
      const std::uint32_t out_payload =
          static_cast<std::uint32_t>(output_pcm.size() * sizeof(float));

      WriteExact(std::cout, &kOutputMagic, sizeof(kOutputMagic));
      WriteExact(std::cout, &kProtocolVersion, sizeof(kProtocolVersion));
      WriteExact(std::cout, &kMsgAudioBlock, sizeof(kMsgAudioBlock));
      WriteExact(std::cout, &sequence, sizeof(sequence));
      WriteExact(std::cout, &frame_count, sizeof(frame_count));
      WriteExact(std::cout, &out_flags, sizeof(out_flags));
      WriteExact(std::cout, &out_payload, sizeof(out_payload));
      WriteExact(std::cout, output_pcm.data(), out_payload);
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
