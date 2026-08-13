#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "audio/wav_io.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/limiter.hpp"
#include "dsp/suppressor.hpp"
#include "tools/wav_replay_support.hpp"

namespace
{
void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_wav_replay --input <six_channel_wav> --config <runtime_yaml>\n"
            << "                      --script <steering_csv> --output <mono_wav>\n"
            << "                      [--enable-suppression] [--disable-limiter]\n";
}

}  // namespace

int main(int argc, char** argv)
{
  std::string input_path;
  std::string config_path = "config/default.yaml";
  std::string script_path;
  std::string output_path;
  bool enable_suppression = false;
  bool disable_limiter = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--input" && i + 1 < argc)
    {
      input_path = argv[++i];
    }
    else if (arg == "--config" && i + 1 < argc)
    {
      config_path = argv[++i];
    }
    else if (arg == "--script" && i + 1 < argc)
    {
      script_path = argv[++i];
    }
    else if (arg == "--output" && i + 1 < argc)
    {
      output_path = argv[++i];
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else if (arg == "--enable-suppression")
    {
      enable_suppression = true;
    }
    else if (arg == "--disable-limiter")
    {
      disable_limiter = true;
    }
    else
    {
      std::cerr << "Unknown or incomplete argument: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }

  if (input_path.empty() || script_path.empty() || output_path.empty())
  {
    PrintUsage();
    return 2;
  }

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
    sonitude::app::ValidateCalibrationConfig(calibration, geometry_ids, runtime.capture.sample_rate_hz);

    const auto input_wav = sonitude::audio::ReadWavFile(input_path);
    if (input_wav.channels < sonitude::audio::kMicChannels)
    {
      throw std::runtime_error("Input WAV must contain at least six channels");
    }
    if (input_wav.sample_rate_hz != runtime.capture.sample_rate_hz)
    {
      throw std::runtime_error("Input WAV sample rate must match runtime capture sample_rate_hz");
    }

    const std::size_t frames = input_wav.interleaved.size() / input_wav.channels;
    std::vector<sonitude::audio::MicFrame> mic =
        sonitude::tools::wav_replay::ExtractMappedMicFrames(input_wav, runtime.active_channel_map);
    std::vector<sonitude::audio::MicFrame> calibrated(frames);
    sonitude::dsp::CalibrationApplier calibration_applier(
        calibration.channels, geometry_ids, runtime.capture.sample_rate_hz, runtime.calibration_dc_block_hz);
    calibration_applier.processBlock(
        std::span<const sonitude::audio::MicFrame>(mic.data(), mic.size()),
        std::span<sonitude::audio::MicFrame>(calibrated.data(), calibrated.size()));

    const auto events = sonitude::tools::wav_replay::LoadSteeringScript(
        script_path, input_wav.sample_rate_hz);
    sonitude::dsp::DelaySumBeamformer beamformer;
    beamformer.configure(
        geometry, runtime.steering, calibration, input_wav.sample_rate_hz, runtime.capture.period_frames);
    constexpr sonitude::audio::BeamformerSteering kNeutralTarget{};
    beamformer.setTarget(kNeutralTarget);
    sonitude::dsp::ConservativeSuppressor suppressor;
    suppressor.configure(
        {.ambient_floor_linear = runtime.steering.ambient_floor_linear,
         .fade_ms = runtime.suppression.fade_ms,
         .activity_threshold = runtime.suppression.activity_threshold,
         .confidence_threshold = runtime.suppression.confidence_threshold},
        input_wav.sample_rate_hz);
    sonitude::dsp::PeakLimiter limiter;
    // TODO(sonitude-limiter): Promote limiter defaults into replay/runtime config once
    // calibration-backed limiter tuning is available across target devices.
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, input_wav.sample_rate_hz);

    std::vector<float> mono(frames, 0.0F);
    constexpr std::size_t kBlock = 256;
    const auto segments =
        sonitude::tools::wav_replay::BuildReplaySegments(frames, kBlock, events, kNeutralTarget);
    for (const auto& segment : segments)
    {
      beamformer.setTarget(segment.target);
      beamformer.process(
          std::span<const sonitude::audio::MicFrame>(calibrated.data() + segment.start_frame,
                                                     segment.frame_count),
          std::span<float>(mono.data() + segment.start_frame, segment.frame_count));
      if (enable_suppression)
      {
        suppressor.setControl(true, 1.0F);
        suppressor.process(
            std::span<float>(mono.data() + segment.start_frame, segment.frame_count));
      }
      if (!disable_limiter)
      {
        limiter.process(std::span<float>(mono.data() + segment.start_frame, segment.frame_count));
      }
    }

    sonitude::audio::WavData out;
    out.sample_rate_hz = input_wav.sample_rate_hz;
    out.channels = 1;
    out.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    out.interleaved = std::move(mono);
    sonitude::audio::WriteWavFile(output_path, out);
    std::cout << "Rendered beamformed WAV to " << output_path << '\n';
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "wav_replay failed: " << ex.what() << '\n';
    return 1;
  }
}
