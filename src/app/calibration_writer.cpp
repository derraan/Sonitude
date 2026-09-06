#include "app/calibration_writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace sonitude::app
{
namespace
{
std::string TimestampSuffix()
{
  const auto now = std::chrono::system_clock::now();
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  return std::to_string(epoch);
}
}  // namespace

void WriteCalibrationYamlBackupSafe(const std::string& path,
                                    const CalibrationConfig& calibration,
                                    const bool force_overwrite)
{
  const std::filesystem::path dst(path);
  const std::filesystem::path tmp = dst.string() + ".tmp";
  if (std::filesystem::exists(dst) && !force_overwrite)
  {
    throw std::runtime_error("destination exists; pass force_overwrite=true to replace");
  }

  YAML::Node root;
  root["schema_version"] = calibration.schema_version;
  root["sample_rate_hz"] = calibration.sample_rate_hz;

  if (!calibration.reference.microphone_id.empty())
  {
    root["reference"]["microphone_id"] = calibration.reference.microphone_id;
  }

  if (!calibration.identity.geometry_id.empty() || !calibration.identity.created_utc.empty() ||
      !calibration.identity.calibration_sequence.empty() || !calibration.identity.device_id.empty())
  {
    if (!calibration.identity.geometry_id.empty())
    {
      root["identity"]["geometry_id"] = calibration.identity.geometry_id;
    }
    if (!calibration.identity.device_id.empty())
    {
      root["identity"]["device_id"] = calibration.identity.device_id;
    }
    if (!calibration.identity.calibration_sequence.empty())
    {
      root["identity"]["calibration_sequence"] = calibration.identity.calibration_sequence;
    }
    if (!calibration.identity.created_utc.empty())
    {
      root["identity"]["created_utc"] = calibration.identity.created_utc;
    }
  }

  if (calibration.capture.sample_rate_hz != 0 || calibration.capture.channel_count != 0)
  {
    root["capture"]["sample_rate_hz"] =
        calibration.capture.sample_rate_hz != 0 ? calibration.capture.sample_rate_hz
                                              : calibration.sample_rate_hz;
    root["capture"]["channel_count"] =
        calibration.capture.channel_count != 0 ? calibration.capture.channel_count : 6U;
  }

  for (const auto& channel : calibration.channels)
  {
    YAML::Node ch;
    ch["id"] = channel.id;
    ch["polarity"] = channel.polarity;
    ch["gain_linear"] = channel.gain_linear;
    ch["delay_samples"] = channel.delay_samples;
    ch["dc_offset"] = channel.dc_offset;
    root["channels"].push_back(ch);
  }

  root["quality"]["valid"] = calibration.quality.valid;
  root["quality"]["hardware_evidence"] = calibration.quality.hardware_evidence;
  for (const auto& warning : calibration.quality.warnings)
  {
    root["quality"]["warnings"].push_back(warning);
  }

  std::ofstream out(tmp, std::ios::trunc);
  if (!out)
  {
    throw std::runtime_error("failed to create calibration temp file");
  }
  out << root;
  out.close();

  if (std::filesystem::exists(dst))
  {
    const std::filesystem::path backup = dst.string() + ".bak." + TimestampSuffix();
    std::filesystem::rename(dst, backup);
  }
  std::filesystem::rename(tmp, dst);
}

void WriteCalibrationReport(const std::string& path, const CalibrationEstimateReport& report)
{
  std::ofstream out(path, std::ios::trunc);
  if (!out)
  {
    throw std::runtime_error("failed to create calibration report file");
  }

  out << "Sonitude calibration report\n";
  out << "=========================\n";
  out << "overall_status: " << CalibrationQualityStatusToString(report.overall_status) << "\n";
  out << "hardware_evidence: " << (report.hardware_evidence ? "true" : "false") << "\n";
  out << "sample_rate_hz: " << report.sample_rate_hz << "\n";
  out << "reference_microphone_id: " << report.reference_microphone_id << "\n";
  out << "reference_channel_index: " << report.reference_channel_index << "\n";
  out << "silence_frames: " << report.silence_frame_count << "\n";
  out << "signal_frames: " << report.signal_frame_count << "\n";
  out << "\n";

  out << std::fixed << std::setprecision(6);
  for (const auto& ch : report.channels)
  {
    out << "channel: " << ch.id << "\n";
    out << "  status: " << CalibrationQualityStatusToString(ch.status) << "\n";
    out << "  polarity: " << ch.polarity;
    if (ch.polarity_unresolved)
    {
      out << " (UNRESOLVED)";
    }
    out << "\n";
    out << "  dc_offset: " << ch.dc_offset << "\n";
    out << "  gain_linear: " << ch.gain_linear << "\n";
    out << "  relative_gain_db: " << ch.relative_gain_db << "\n";
    out << "  delay_samples: " << ch.delay_samples << "\n";
    out << "  delay_us: " << ch.delay_us << "\n";
    out << "  correlation_peak: " << ch.correlation_peak << "\n";
    out << "  delay_confidence: " << ch.delay_confidence << "\n";
    out << "  rms_level: " << ch.rms_level << "\n";
    out << "  clipping: " << (ch.clipping ? "true" : "false") << "\n";
    for (const auto& w : ch.warnings)
    {
      out << "  warning: " << w << "\n";
    }
    out << "\n";
  }

  for (const auto& w : report.warnings)
  {
    out << "warning: " << w << "\n";
  }
}
}  // namespace sonitude::app
