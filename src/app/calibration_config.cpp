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

bool IsKnownEqType(const std::string& type)
{
  return type == "PK" || type == "LS" || type == "HS" || type == "LP" || type == "HP";
}

bool IsFiniteFloat(const float value)
{
  return std::isfinite(value);
}
}  // namespace

const char* CalibrationQualityStatusToString(const CalibrationQualityStatus status) noexcept
{
  switch (status)
  {
    case CalibrationQualityStatus::Pass:
      return "PASS";
    case CalibrationQualityStatus::Warning:
      return "WARNING";
    case CalibrationQualityStatus::Unresolved:
      return "UNRESOLVED";
    case CalibrationQualityStatus::Invalid:
      return "INVALID";
  }
  return "INVALID";
}

CalibrationConfig LoadCalibrationFromFile(const std::string& path)
{
  const YAML::Node root = YAML::LoadFile(path);
  CalibrationConfig calibration;
  if (root["schema_version"])
  {
    calibration.schema_version = root["schema_version"].as<int>();
  }
  calibration.sample_rate_hz = RequireScalar<std::uint32_t>(root, "sample_rate_hz");

  if (root["reference"])
  {
    const YAML::Node ref = root["reference"];
    if (ref["microphone_id"])
    {
      calibration.reference.microphone_id = ref["microphone_id"].as<std::string>();
    }
  }

  if (root["identity"])
  {
    const YAML::Node id = root["identity"];
    if (id["geometry_id"])
    {
      calibration.identity.geometry_id = id["geometry_id"].as<std::string>();
    }
    if (id["device_id"])
    {
      calibration.identity.device_id = id["device_id"].as<std::string>();
    }
    if (id["calibration_sequence"])
    {
      calibration.identity.calibration_sequence = id["calibration_sequence"].as<std::string>();
    }
    if (id["created_utc"])
    {
      calibration.identity.created_utc = id["created_utc"].as<std::string>();
    }
  }

  if (root["capture"])
  {
    const YAML::Node cap = root["capture"];
    if (cap["sample_rate_hz"])
    {
      calibration.capture.sample_rate_hz = cap["sample_rate_hz"].as<std::uint32_t>();
    }
    if (cap["channel_count"])
    {
      calibration.capture.channel_count = cap["channel_count"].as<std::size_t>();
    }
  }

  if (root["quality"])
  {
    const YAML::Node q = root["quality"];
    if (q["valid"])
    {
      calibration.quality.valid = q["valid"].as<bool>();
    }
    if (q["hardware_evidence"])
    {
      calibration.quality.hardware_evidence = q["hardware_evidence"].as<bool>();
    }
    if (q["warnings"] && q["warnings"].IsSequence())
    {
      for (const YAML::Node& w : q["warnings"])
      {
        calibration.quality.warnings.push_back(w.as<std::string>());
      }
    }
  }

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
    if (item["eq"])
    {
      const YAML::Node eq = item["eq"];
      if (eq["enabled"])
      {
        channel.eq.enabled = eq["enabled"].as<bool>();
      }
      if (eq["sections"])
      {
        const YAML::Node sections = eq["sections"];
        if (!sections.IsSequence())
        {
          throw std::runtime_error("calibration eq.sections must be a sequence");
        }
        for (const YAML::Node& s : sections)
        {
          CalibrationChannel::EqSection sec;
          sec.type = RequireScalar<std::string>(s, "type");
          sec.freq_hz = RequireScalar<float>(s, "freq_hz");
          if (s["gain_db"])
          {
            sec.gain_db = s["gain_db"].as<float>();
          }
          sec.q = RequireScalar<float>(s, "q");
          channel.eq.sections.push_back(sec);
        }
      }
    }
    calibration.channels.push_back(channel);
  }
  return calibration;
}

void ValidateCalibrationConfig(const CalibrationConfig& calibration,
                               const std::vector<std::string>& geometry_ids,
                               const std::uint32_t expected_sample_rate_hz)
{
  if (calibration.schema_version < 1 || calibration.schema_version > kCalibrationSchemaVersion)
  {
    throw std::runtime_error("calibration schema_version is not supported");
  }
  if (calibration.sample_rate_hz != expected_sample_rate_hz)
  {
    throw std::runtime_error("calibration sample_rate_hz does not match capture rate");
  }
  if (calibration.channels.size() != 6)
  {
    throw std::runtime_error("calibration must have six channels");
  }
  if (calibration.capture.channel_count != 0 && calibration.capture.channel_count != 6)
  {
    throw std::runtime_error("calibration capture channel_count must be six when present");
  }
  if (calibration.capture.sample_rate_hz != 0 &&
      calibration.capture.sample_rate_hz != calibration.sample_rate_hz)
  {
    throw std::runtime_error("calibration capture sample_rate_hz does not match sample_rate_hz");
  }

  const std::unordered_set<std::string> geometry_id_set(geometry_ids.begin(), geometry_ids.end());
  std::unordered_set<std::string> calibration_id_set;
  for (const auto& channel : calibration.channels)
  {
    if (geometry_id_set.find(channel.id) == geometry_id_set.end())
    {
      throw std::runtime_error("calibration channel id does not exist in geometry");
    }
    if (!calibration_id_set.insert(channel.id).second)
    {
      throw std::runtime_error("calibration channel ids must be unique");
    }
    if (!(channel.polarity == 1 || channel.polarity == -1))
    {
      throw std::runtime_error("calibration polarity must be +1 or -1");
    }
    if (!(channel.gain_linear > 0.0F && channel.gain_linear <= 8.0F))
    {
      throw std::runtime_error("calibration gain out of range");
    }
    if (!IsFiniteFloat(channel.gain_linear))
    {
      throw std::runtime_error("calibration gain_linear must be finite");
    }
    if (!IsFiniteFloat(channel.delay_samples))
    {
      throw std::runtime_error("calibration delay_samples must be finite");
    }
    if (!IsFiniteFloat(channel.dc_offset))
    {
      throw std::runtime_error("calibration dc_offset must be finite");
    }
    // Engineering guardrail: fractional delays beyond ~6 ms at 44.1 kHz are suspect.
    if (std::fabs(channel.delay_samples) > 256.0F)
    {
      throw std::runtime_error("calibration delay_samples out of range");
    }
    if (channel.eq.sections.size() > 24U)
    {
      throw std::runtime_error("calibration eq exceeds max 24 sections");
    }
    for (const auto& section : channel.eq.sections)
    {
      if (!IsKnownEqType(section.type))
      {
        throw std::runtime_error("calibration eq type must be one of PK/LS/HS/LP/HP");
      }
      if (!(section.freq_hz > 0.0F && section.freq_hz < (0.5F * static_cast<float>(expected_sample_rate_hz))))
      {
        throw std::runtime_error("calibration eq freq_hz out of range");
      }
      if (!(section.q > 0.0F && section.q <= 20.0F))
      {
        throw std::runtime_error("calibration eq q must be in (0, 20]");
      }
      if (std::fabs(section.gain_db) > 24.0F)
      {
        throw std::runtime_error("calibration eq gain_db out of range");
      }
    }
  }
  if (calibration_id_set.size() != geometry_id_set.size())
  {
    throw std::runtime_error("calibration must contain exactly one channel for each geometry microphone");
  }
  if (!calibration.reference.microphone_id.empty() &&
      calibration_id_set.find(calibration.reference.microphone_id) == calibration_id_set.end())
  {
    throw std::runtime_error("calibration reference microphone_id does not exist in channels");
  }
}
}  // namespace sonitude::app
