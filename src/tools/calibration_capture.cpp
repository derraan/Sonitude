#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "audio/audio_types.hpp"
#if SONITUDE_HAS_ALSA
#include "audio/alsa/alsa_device.hpp"
#include "audio/alsa/capture_worker.hpp"
#include "rt/telemetry.hpp"
#endif
#include "audio/wav_io.hpp"

namespace
{
struct Options
{
  std::string config_path = "config/default.yaml";
  std::string output_path = "build/calibration_capture.hardware.wav";
  double seconds = 5.0;
  bool synthetic = false;
};

void PrintUsage()
{
  std::cout
      << "Usage:\n"
      << "  sonitude_calibration_capture [--config <runtime_yaml>] [--seconds <duration>] \\\n"
      << "      [--output <wav>] [--synthetic]\n";
}

Options ParseArgs(const int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--config" && i + 1 < argc)
    {
      options.config_path = argv[++i];
    }
    else if (arg == "--output" && i + 1 < argc)
    {
      options.output_path = argv[++i];
    }
    else if (arg == "--seconds" && i + 1 < argc)
    {
      options.seconds = std::stod(argv[++i]);
    }
    else if (arg == "--synthetic")
    {
      options.synthetic = true;
    }
    else if (arg == "--help")
    {
      PrintUsage();
      std::exit(0);
    }
    else
    {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  if (options.seconds <= 0.0)
  {
    throw std::runtime_error("--seconds must be greater than zero");
  }
  return options;
}

sonitude::audio::WavData GenerateSyntheticCapture(const std::uint32_t sample_rate_hz,
                                                  const std::size_t frames)
{
  sonitude::audio::WavData wav;
  wav.sample_rate_hz = sample_rate_hz;
  wav.channels = sonitude::audio::kMicChannels;
  wav.format = sonitude::audio::PcmFormat::FLOAT32_LE;
  wav.interleaved.resize(frames * wav.channels, 0.0F);

  for (std::size_t i = 0; i < frames; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate_hz);
    for (std::size_t ch = 0; ch < wav.channels; ++ch)
    {
      const float base_hz = 400.0F + (70.0F * static_cast<float>(ch));
      wav.interleaved[(i * wav.channels) + ch] =
          0.08F * std::sin(2.0F * 3.1415926535F * base_hz * t);
    }
  }
  return wav;
}
} // namespace

int main(int argc, char** argv)
{
  try
  {
    const Options options = ParseArgs(argc, argv);
    if (options.synthetic)
    {
      std::cout
          << "WARNING: --synthetic selected. Output is deterministic synthetic audio and is NOT "
             "valid calibration evidence.\n";
      const std::uint32_t sample_rate_hz = 44100;
      const std::size_t frames = static_cast<std::size_t>(options.seconds * sample_rate_hz);
      auto wav = GenerateSyntheticCapture(sample_rate_hz, frames);
      std::filesystem::path synthetic_path(options.output_path);
      if (synthetic_path.filename().string().find("synthetic") == std::string::npos)
      {
        synthetic_path =
            synthetic_path.parent_path() /
            (synthetic_path.stem().string() + ".synthetic" + synthetic_path.extension().string());
      }
      sonitude::audio::WriteWavFile(synthetic_path.string(), wav);
      std::cout << "Wrote synthetic capture WAV (NOT HARDWARE EVIDENCE): "
                << synthetic_path.string() << '\n';
      return 0;
    }

#if !SONITUDE_HAS_ALSA
    std::cerr << "calibration_capture failed: built without ALSA support; use --synthetic on this "
                 "build.\n";
    return 1;
#else
    const auto config = sonitude::app::LoadRuntimeConfigFromFile(options.config_path);
    sonitude::audio::alsa::AlsaPcmDevice device;
    device.openCapture(config.capture);
    const auto actual = device.negotiated();
    if (actual.sample_rate_hz != config.capture.sample_rate_hz)
    {
      throw std::runtime_error("Negotiated capture sample_rate_hz (" +
                               std::to_string(actual.sample_rate_hz) +
                               ") does not match configured capture sample_rate_hz (" +
                               std::to_string(config.capture.sample_rate_hz) +
                               "); refusing to record mislabeled calibration evidence");
    }
    sonitude::rt::TelemetryCounters counters;
    sonitude::audio::alsa::CaptureWorker worker(&device, &config, &counters);

    const std::size_t target_frames =
        static_cast<std::size_t>(options.seconds * static_cast<double>(actual.sample_rate_hz));
    std::vector<float> interleaved;
    interleaved.reserve(target_frames * sonitude::audio::kMicChannels);

    while ((interleaved.size() / sonitude::audio::kMicChannels) < target_frames)
    {
      std::vector<sonitude::audio::MicFrame> block;
      if (!worker.readBlock(block))
      {
        continue;
      }
      for (const auto& frame : block)
      {
        if ((interleaved.size() / sonitude::audio::kMicChannels) >= target_frames)
        {
          break;
        }
        for (float sample : frame)
        {
          interleaved.push_back(sample);
        }
      }
    }

    sonitude::audio::WavData wav;
    wav.sample_rate_hz = actual.sample_rate_hz;
    wav.channels = sonitude::audio::kMicChannels;
    wav.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    wav.interleaved = std::move(interleaved);
    sonitude::audio::WriteWavFile(options.output_path, wav);
    std::cout << "Captured " << target_frames << " frames at " << actual.sample_rate_hz << " Hz to "
              << options.output_path << '\n';
    return 0;
#endif
  }
  catch (const std::exception& ex)
  {
    std::cerr << "calibration_capture failed: " << ex.what() << '\n';
    return 1;
  }
}
