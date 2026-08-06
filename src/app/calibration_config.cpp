#include "app/calibration_config.hpp"

#include <cmath>
#include <stdexcept>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace sonitude::app
{
namespace
{
template <typename T>
T RequireScalar(const YAML::Node& node, const char* key)
{
  if (!node[key])
  {
    throw std::runtime_error(std::string("Missing required key: ") + key);
  }
  return node[key].as<T>();
}
}  // namespace

CalibrationConfig LoadCalibrationFromFile(const std::string& path)
{
  const YAML::Node root = YAML::LoadFile(path);
  CalibrationConfig calibration;
  calibration.sample_rate_hz = RequireScalar<std::uint32_t>(root, "sample_rate_hz");
  const YAML::Node channels = root["channels"];
  if (!channels || !channels.IsSequence())
  {
    throw std::runtime_error("channels must be a sequence");
  }

  calibration.channels.reserve(channels.size());
  for (const YAML::Node& item : channels)
  {
    CalibrationChannel channel;
    channel.id = RequireScalar<std::string>(item, "id");
    channel.polarity = RequireScalar<int>(item, "polarity");
    channel.gain_linear = RequireScalar<float>(item, "gain_linear");
    channel.delay_samples = RequireScalar<float>(item, "delay_samples");
    if (item["dc_offset"])
    {
      channel.dc_offset = item["dc_offset"].as<float>();
    }
    calibration.channels.push_back(channel);
  }
  return calibration;
}

void ValidateCalibrationConfig(const CalibrationConfig& calibration,
                               const std::vector<std::string>& geometry_ids,
                               const std::uint32_t expected_sample_rate_hz)
{
  if (calibration.sample_rate_hz != expected_sample_rate_hz)
  {
    throw std::runtime_error("calibration sample_rate_hz does not match capture rate");
  }
  if (calibration.channels.size() != 6)
  {
    throw std::runtime_error("calibration must have six channels");
  }

  std::unordered_set<std::string> id_set(geometry_ids.begin(), geometry_ids.end());
  for (const auto& channel : calibration.channels)
  {
    if (id_set.find(channel.id) == id_set.end())
    {
      throw std::runtime_error("calibration channel id does not exist in geometry");
    }
    if (!(channel.polarity == 1 || channel.polarity == -1))
    {
      throw std::runtime_error("calibration polarity must be +1 or -1");
    }
    if (!(channel.gain_linear > 0.0F && channel.gain_linear <= 8.0F))
    {
      throw std::runtime_error("calibration gain out of range");
    }
    if (std::fabs(channel.delay_samples) > 256.0F)
    {
      throw std::runtime_error("calibration delay_samples out of range");
    }
  }
}
}  // namespace sonitude::app
