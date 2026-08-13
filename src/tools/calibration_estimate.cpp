#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/calibration_writer.hpp"
#include "app/config.hpp"
#include "audio/wav_io.hpp"

namespace
{
struct Options
{
  std::string input_path = "build/calibration_capture.wav";
  std::string output_path = "build/calibration_estimate.yaml";
  std::string config_path = "config/default.yaml";
  std::string geometry_path;
};

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_calibration_estimate --input <capture.wav> --output <calibration.yaml>\n"
            << "      [--config <runtime.yaml>] [--geometry <geometry.yaml>]\n";
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
  return options;
}

float Mean(const std::vector<float>& v)
{
  if (v.empty())
  {
    return 0.0F;
  }
  double sum = 0.0;
  for (const float x : v)
  {
    sum += x;
  }
  return static_cast<float>(sum / static_cast<double>(v.size()));
}

struct CorrelationEstimate
{
  float delay_samples = 0.0F;
  int polarity = 1;
};

CorrelationEstimate EstimateDelayAndPolarity(const std::vector<float>& reference,
                                             const std::vector<float>& channel,
                                             const int max_lag)
{
  if (reference.size() != channel.size() || reference.empty())
  {
    throw std::runtime_error("cross-correlation requires non-empty equal-length channels");
  }

  const std::size_t n = reference.size();
  std::vector<double> corr(static_cast<std::size_t>((2 * max_lag) + 1), 0.0);
  for (int lag = -max_lag; lag <= max_lag; ++lag)
  {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
      const std::int64_t shifted = static_cast<std::int64_t>(i) + lag;
      if (shifted < 0 || shifted >= static_cast<std::int64_t>(n))
      {
        continue;
      }
      sum += static_cast<double>(reference[i]) * static_cast<double>(channel[static_cast<std::size_t>(shifted)]);
    }
    corr[static_cast<std::size_t>(lag + max_lag)] = sum;
  }

  int best_lag = 0;
  double best_abs = -1.0;
  double best_value = 0.0;
  for (int lag = -max_lag; lag <= max_lag; ++lag)
  {
    const double value = corr[static_cast<std::size_t>(lag + max_lag)];
    const double magnitude = std::fabs(value);
    if (magnitude > best_abs)
    {
      best_abs = magnitude;
      best_value = value;
      best_lag = lag;
    }
  }

  double fractional_offset = 0.0;
  if (best_lag > -max_lag && best_lag < max_lag)
  {
    const double c_prev = corr[static_cast<std::size_t>((best_lag - 1) + max_lag)];
    const double c_peak = corr[static_cast<std::size_t>(best_lag + max_lag)];
    const double c_next = corr[static_cast<std::size_t>((best_lag + 1) + max_lag)];
    const double denom = (c_prev - (2.0 * c_peak) + c_next);
    if (std::fabs(denom) > std::numeric_limits<double>::epsilon())
    {
      fractional_offset = 0.5 * (c_prev - c_next) / denom;
      fractional_offset = std::clamp(fractional_offset, -1.0, 1.0);
    }
  }

  CorrelationEstimate estimate;
  estimate.delay_samples = static_cast<float>(-static_cast<double>(best_lag) - fractional_offset);
  estimate.polarity = (best_value >= 0.0) ? 1 : -1;
  return estimate;
}
}  // namespace

int main(int argc, char** argv)
{
  try
  {
    const Options options = ParseArgs(argc, argv);
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
    constexpr int kMaxLagSamples = 256;
    for (std::size_t ch = 0; ch < wav.channels; ++ch)
    {
      const auto& sig = channels[ch];
      const float dc = Mean(sig);
      double rms_sum = 0.0;
      double max_abs = 0.0;
      for (const float s : sig)
      {
        const float c = s - dc;
        rms_sum += static_cast<double>(c) * static_cast<double>(c);
        max_abs = std::max(max_abs, static_cast<double>(std::fabs(c)));
      }
      const float rms = static_cast<float>(std::sqrt(rms_sum / static_cast<double>(sig.size())));
      const CorrelationEstimate corr =
          (ch == 0U) ? CorrelationEstimate{} : EstimateDelayAndPolarity(reference, sig, kMaxLagSamples);
      auto& out = cal.channels[ch];
      out.id = geometry_ids[ch];
      out.polarity = (ch == 0U) ? 1 : corr.polarity;
      out.gain_linear = (rms > 1e-6F) ? (0.1F / rms) : 1.0F;
      out.delay_samples = (ch == 0U) ? 0.0F : corr.delay_samples;
      out.dc_offset = dc;
      std::cout << out.id << " dc=" << dc << " rms=" << rms << " peak=" << max_abs
                << " delay=" << out.delay_samples << " polarity=" << out.polarity << "\n";
    }
    sonitude::app::ValidateCalibrationConfig(cal, geometry_ids, runtime.capture.sample_rate_hz);
    sonitude::app::WriteCalibrationYamlBackupSafe(options.output_path, cal, true);
    std::cout << "Wrote calibration YAML: " << options.output_path << "\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "calibration_estimate failed: " << ex.what() << "\n";
    return 1;
  }
}
