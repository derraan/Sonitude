#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#include "app/calibration_estimator.hpp"
#include "app/calibration_writer.hpp"
#include "app/config.hpp"
#include "audio/wav_io.hpp"

namespace
{
void PrintUsage()
{
  std::cout << "Usage: sonitude_calibration_estimate <input.wav> <output.yaml> [options]\n"
            << "Options:\n"
            << "  --geometry <path>           geometry YAML for microphone IDs (default: config/geometry)\n"
            << "  --reference-index <N>       reference channel index (default: 2)\n"
            << "  --report <path>             write human-readable quality report\n"
            << "  --silence-frames <N>        silence region length (default: auto)\n"
            << "  --signal-start <N>          signal region start frame (default: after silence)\n"
            << "  --signal-frames <N>         signal region length (default: remainder)\n"
            << "  --min-correlation <V>       minimum delay/polarity confidence [0,1]\n"
            << "  --hardware-evidence         mark output as hardware-evidence-backed\n";
}

std::vector<std::string> LoadGeometryIds(const std::string& geometry_path)
{
  const auto geometry = sonitude::app::LoadGeometryFromFile(geometry_path);
  std::vector<std::string> ids;
  ids.reserve(geometry.microphones.size());
  for (const auto& mic : geometry.microphones)
  {
    ids.push_back(mic.id);
  }
  return ids;
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    PrintUsage();
    return 1;
  }

  std::string in_path = argv[1];
  std::string out_path = argv[2];
  std::string geometry_path = "config/geometry_soundbubble_initial.yaml";
  std::string report_path;
  std::size_t reference_index = 2;
  std::size_t silence_frames = 0;
  std::size_t signal_start = 0;
  std::size_t signal_frames = 0;
  float min_correlation = -1.0F;
  bool hardware_evidence = false;

  for (int i = 3; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--geometry" && i + 1 < argc)
    {
      geometry_path = argv[++i];
    }
    else if (arg == "--reference-index" && i + 1 < argc)
    {
      reference_index = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--report" && i + 1 < argc)
    {
      report_path = argv[++i];
    }
    else if (arg == "--silence-frames" && i + 1 < argc)
    {
      silence_frames = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--signal-start" && i + 1 < argc)
    {
      signal_start = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--signal-frames" && i + 1 < argc)
    {
      signal_frames = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--hardware-evidence")
    {
      hardware_evidence = true;
    }
    else if (arg == "--min-correlation" && i + 1 < argc)
    {
      char* parse_end = nullptr;
      min_correlation = std::strtof(argv[++i], &parse_end);
      if (parse_end == argv[i] || (parse_end != nullptr && *parse_end != '\0'))
      {
        std::cerr << "--min-correlation must be a float literal\n";
        return 1;
      }
      if (!std::isfinite(min_correlation) || min_correlation < 0.0F || min_correlation > 1.0F)
      {
        std::cerr << "--min-correlation must be in [0,1]\n";
        return 1;
      }
    }
    else if (arg == "--help" || arg == "-h")
    {
      PrintUsage();
      return 0;
    }
    else
    {
      std::cerr << "Unknown argument: " << arg << "\n";
      PrintUsage();
      return 1;
    }
  }

  try
  {
    const auto wav = sonitude::audio::ReadWavFile(in_path);
    if (wav.channels != 6)
    {
      throw std::runtime_error("calibration_estimate expects a 6-channel WAV");
    }

    const auto geometry = sonitude::app::LoadGeometryFromFile(geometry_path);
    const auto channel_ids = LoadGeometryIds(geometry_path);

    sonitude::app::CalibrationEstimateOptions options;
    options.channel_ids = channel_ids;
    options.reference_channel_index = reference_index;
    options.sample_rate_hz = wav.sample_rate_hz;
    options.silence_frame_count = silence_frames;
    options.signal_start_frame = signal_start;
    options.signal_frame_count = signal_frames;
    options.geometry_id = geometry.profile_name;
    options.hardware_evidence = hardware_evidence;
    if (min_correlation >= 0.0F)
    {
      options.delay_confidence_threshold = min_correlation;
      options.polarity_correlation_threshold = min_correlation;
    }

    const auto estimate = sonitude::app::EstimateCalibrationFromCapture(
        std::span<const float>(wav.interleaved.data(), wav.interleaved.size()), wav.channels, options);

    const bool failed_closed =
        estimate.report.overall_status == sonitude::app::CalibrationQualityStatus::Invalid ||
        estimate.report.overall_status == sonitude::app::CalibrationQualityStatus::Unresolved;

    std::cout << "overall_status: "
              << sonitude::app::CalibrationQualityStatusToString(estimate.report.overall_status)
              << "\n";

    if (!report_path.empty())
    {
      sonitude::app::WriteCalibrationReport(report_path, estimate.report);
      std::cout << "Wrote calibration report: " << report_path << "\n";
    }

    const std::filesystem::path out_dir = std::filesystem::path(out_path).parent_path();
    if (!out_dir.empty())
    {
      std::filesystem::create_directories(out_dir);
    }
    if (failed_closed)
    {
      std::cout << "Calibration is not usable; refusing to write YAML output.\n";
      return 2;
    }

    sonitude::app::WriteCalibrationYamlBackupSafe(out_path, estimate.calibration, true);
    std::cout << "Wrote calibration YAML: " << out_path << "\n";

    for (const auto& ch : estimate.report.channels)
    {
      std::cout << ch.id << " dc=" << ch.dc_offset << " gain=" << ch.gain_linear
                << " delay=" << ch.delay_samples << " polarity=" << ch.polarity;
      if (ch.polarity_unresolved)
      {
        std::cout << "(unresolved)";
      }
      std::cout << " conf=" << ch.delay_confidence << "\n";
    }

    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "calibration_estimate failed: " << ex.what() << "\n";
    return 1;
  }
}
