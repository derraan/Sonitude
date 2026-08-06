#include "app/calibration_writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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
  root["sample_rate_hz"] = calibration.sample_rate_hz;
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
}  // namespace sonitude::app
