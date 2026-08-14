#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/calibration_writer.hpp"
#include "app/config.hpp"
#include "audio/wav_io.hpp"
#include "tools/calibration_estimate_support.hpp"

namespace
{
struct Options
{
  std::string input_path = "build/calibration_capture.hardware.wav";
  std::string output_path = "build/calibration_estimate.hardware.yaml";
  std::string config_path = "config/default.yaml";
  std::string geometry_path;
  std::optional<float> min_correlation;
  bool synthetic = false;
};

void PrintUsage()
{
  std::cout
      << "Usage:\n"
      << "  sonitude_calibration_estimate --input <capture.wav> --output <calibration.yaml>\n"
      << "      --min-correlation <0..1> [--config <runtime.yaml>] [--geometry <geometry.yaml>]\n"
      << "      [--synthetic]\n";
}

Options ParseArgs(const int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--input" && i + 1 < argc)
    {
      options.input_path = argv[++i];
    }
    else if (arg == "--output" && i + 1 < argc)
    {
      options.output_path = argv[++i];
    }
    else if (arg == "--config" && i + 1 < argc)
    {
      options.config_path = argv[++i];
    }
    else if (arg == "--geometry" && i + 1 < argc)
    {
      options.geometry_path = argv[++i];
    }
    else if (arg == "--min-correlation" && i + 1 < argc)
    {
      options.min_correlation = sonitude::tools::calibration::ParseMinimumCorrelation(argv[++i]);
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
  if (!options.min_correlation.has_value())
  {
    throw std::runtime_error(
        "--min-correlation <0..1> is required for hardware and synthetic calibration estimation");
  }
  return options;
}

std::string EvidenceOutputPath(const Options& options)
{
  if (!options.synthetic)
  {
    return options.output_path;
  }

  std::filesystem::path path(options.output_path);
  if (path.filename().string().find("synthetic") == std::string::npos)
  {
    path = path.parent_path() / (path.stem().string() + ".synthetic" + path.extension().string());
  }
  return path.string();
}

} // namespace

int main(int argc, char** argv)
{
  try
  {
    const Options options = ParseArgs(argc, argv);
    const std::string output_path = EvidenceOutputPath(options);
    if (options.synthetic)
    {
      std::cout << "WARNING: --synthetic selected. Output is NOT HARDWARE EVIDENCE and will be "
                   "written to "
                << output_path << "\n";
    }
    const auto runtime = sonitude::app::LoadRuntimeConfigFromFile(options.config_path);
    const std::string geometry_path =
        options.geometry_path.empty() ? runtime.geometry_path : options.geometry_path;
    const auto geometry = sonitude::app::LoadGeometryFromFile(geometry_path);
    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }

    const auto wav = sonitude::audio::ReadWavFile(options.input_path);
    if (wav.channels != geometry_ids.size())
    {
      throw std::runtime_error("calibration_estimate expects one channel per geometry microphone");
    }
    sonitude::app::CalibrationConfig cal;
    cal.sample_rate_hz = wav.sample_rate_hz;
    cal.channels.resize(geometry_ids.size());

    const std::size_t frames = wav.interleaved.size() / wav.channels;
    if (frames < 32)
    {
      throw std::runtime_error("calibration_estimate requires at least 32 frames");
    }

    std::vector<std::vector<float>> channels(wav.channels);
    for (auto& ch : channels)
    {
      ch.reserve(frames);
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
      for (std::size_t ch = 0; ch < wav.channels; ++ch)
      {
        channels[ch].push_back(wav.interleaved[(frame * wav.channels) + ch]);
      }
    }

    const auto& reference = channels[0];
    const auto reference_moments = sonitude::tools::calibration::ComputeChannelMoments(reference);
    constexpr int kMaxLagSamples = 256;
    for (std::size_t ch = 0; ch < wav.channels; ++ch)
    {
      const auto& sig = channels[ch];
      const auto moments = sonitude::tools::calibration::ComputeChannelMoments(sig);
      const auto corr = (ch == 0U) ? sonitude::tools::calibration::DelayPolarityEstimate{}
                                   : sonitude::tools::calibration::EstimateDelayAndPolarity(
                                         reference, sig, kMaxLagSamples);
      auto& out = cal.channels[ch];
      out.id = geometry_ids[ch];
      out.polarity = (ch == 0U) ? 1 : corr.polarity;
      out.gain_linear = (ch == 0U) ? 1.0F : (reference_moments.rms / moments.rms);
      out.delay_samples = (ch == 0U) ? 0.0F : corr.delay_samples;
      out.dc_offset = moments.mean;
      std::cout << out.id << " dc=" << moments.mean << " rms=" << moments.rms
                << " delay=" << out.delay_samples << " polarity=" << out.polarity
                << " gain=" << out.gain_linear << " corr=" << corr.peak_correlation << "\n";
      if (ch != 0U)
      {
        sonitude::tools::calibration::RequireMinimumCorrelation(out.id, corr.peak_correlation,
                                                                *options.min_correlation);
      }
    }
    sonitude::app::ValidateCalibrationConfig(cal, geometry_ids, runtime.capture.sample_rate_hz);
    sonitude::app::WriteCalibrationYamlBackupSafe(output_path, cal, true);
    std::cout << "Wrote calibration YAML: " << output_path << "\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "calibration_estimate failed: " << ex.what() << "\n";
    return 1;
  }
}
