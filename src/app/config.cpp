#include "app/config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

DeviceConfig ParseDevice(const YAML::Node& node, const char* parent_key)
{
  if (!node || !node.IsMap())
  {
    throw std::runtime_error(std::string("Expected map for ") + parent_key);
  }

  DeviceConfig out;
  out.alsa_device = RequireScalar<std::string>(node, "alsa_device");
  out.sample_rate_hz = RequireScalar<std::uint32_t>(node, "sample_rate_hz");
  out.period_frames = RequireScalar<std::uint32_t>(node, "period_frames");
  out.periods = RequireScalar<std::uint32_t>(node, "periods");
  return out;
}

std::vector<std::size_t> ParseChannelMap(const YAML::Node& node)
{
  if (!node || !node.IsSequence())
  {
    throw std::runtime_error("active_channel_map must be a sequence");
  }

  std::vector<std::size_t> out;
  out.reserve(node.size());
  for (const YAML::Node& value : node)
  {
    out.push_back(value.as<std::size_t>());
  }
  return out;
}

std::vector<ZoneConfig> ParseZones(const YAML::Node& node)
{
  if (!node || !node.IsMap())
  {
    throw std::runtime_error("zones must be a map");
  }

  std::vector<ZoneConfig> out;
  out.reserve(node.size());
  for (const auto& it : node)
  {
    ZoneConfig zone;
    zone.name = it.first.as<std::string>();
    zone.azimuth_min_deg = RequireScalar<float>(it.second, "azimuth_min_deg");
    zone.azimuth_max_deg = RequireScalar<float>(it.second, "azimuth_max_deg");
    if (it.second["policy"])
    {
      std::string policy = it.second["policy"].as<std::string>();
      std::transform(policy.begin(), policy.end(), policy.begin(), [](const unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
      if (policy == "focus")
      {
        zone.policy = ZonePolicy::Focus;
      }
      else if (policy == "assist")
      {
        zone.policy = ZonePolicy::Assist;
      }
      else if (policy == "ambient")
      {
        zone.policy = ZonePolicy::Ambient;
      }
      else
      {
        throw std::runtime_error("zone policy must be one of: focus, assist, ambient");
      }
    }
    out.push_back(zone);
  }
  return out;
}

std::string ResolvePath(const std::string& base_file, const std::string& candidate)
{
  const std::filesystem::path candidate_path(candidate);
  if (candidate_path.is_absolute())
  {
    return candidate_path.string();
  }

  const std::filesystem::path base_path(base_file);
  return (base_path.parent_path() / candidate_path).lexically_normal().string();
}
}  // namespace

RuntimeConfig LoadRuntimeConfigFromFile(const std::string& path)
{
  const YAML::Node root = YAML::LoadFile(path);

  RuntimeConfig config;
  config.capture = ParseDevice(root["capture"], "capture");
  config.playback = ParseDevice(root["playback"], "playback");
  config.active_channel_map = ParseChannelMap(root["active_channel_map"]);

  config.geometry_path = ResolvePath(path, RequireScalar<std::string>(root, "geometry_path"));
  config.calibration_path = ResolvePath(path, RequireScalar<std::string>(root, "calibration_path"));
  config.calibration_dc_block_hz = RequireScalar<float>(root, "calibration_dc_block_hz");

  const YAML::Node asrc = root["asrc"];
  config.asrc.enabled = RequireScalar<bool>(asrc, "enabled");
  config.asrc.allow_bypass_for_locked_bench =
      RequireScalar<bool>(asrc, "allow_bypass_for_locked_bench");
  config.asrc.min_ratio = RequireScalar<double>(asrc, "min_ratio");
  config.asrc.max_ratio = RequireScalar<double>(asrc, "max_ratio");
  config.asrc.target_buffer_frames = RequireScalar<std::uint32_t>(asrc, "target_buffer_frames");
  config.asrc.pi_kp = RequireScalar<double>(asrc, "pi_kp");
  config.asrc.pi_ki = RequireScalar<double>(asrc, "pi_ki");

  const YAML::Node steering = root["steering"];
  config.steering.speed_of_sound_mps = RequireScalar<float>(steering, "speed_of_sound_mps");
  config.steering.reference_mic_index =
      RequireScalar<std::size_t>(steering, "reference_mic_index");
  config.steering.steering_ramp_ms = RequireScalar<float>(steering, "steering_ramp_ms");
  config.steering.ambient_floor_linear = RequireScalar<float>(steering, "ambient_floor_linear");

  const YAML::Node suppression = root["suppression"];
  config.suppression.enabled = RequireScalar<bool>(suppression, "enabled");
  config.suppression.fade_ms = RequireScalar<float>(suppression, "fade_ms");
  config.suppression.activity_threshold = RequireScalar<float>(suppression, "activity_threshold");
  config.suppression.confidence_threshold =
      RequireScalar<float>(suppression, "confidence_threshold");

  const YAML::Node sm = root["state_machine"];
  config.state_machine.activation_hold_ms = RequireScalar<std::uint32_t>(sm, "activation_hold_ms");
  config.state_machine.confirmation_hold_ms =
      RequireScalar<std::uint32_t>(sm, "confirmation_hold_ms");
  config.state_machine.release_hold_ms = RequireScalar<std::uint32_t>(sm, "release_hold_ms");
  config.state_machine.hold_direction_ms = RequireScalar<std::uint32_t>(sm, "hold_direction_ms");
  config.state_machine.zone_direction_stability_deg =
      RequireScalar<float>(sm, "zone_direction_stability_deg");

  const YAML::Node odas = root["odas"];
  config.odas.enabled = RequireScalar<bool>(odas, "enabled");
  config.odas.use_mock_provider = RequireScalar<bool>(odas, "use_mock_provider");
  config.odas.endpoint = RequireScalar<std::string>(odas, "endpoint");

  const YAML::Node telemetry = root["telemetry"];
  config.telemetry.log_human_readable = RequireScalar<bool>(telemetry, "log_human_readable");
  config.telemetry.emit_csv = RequireScalar<bool>(telemetry, "emit_csv");
  config.telemetry.emit_json = RequireScalar<bool>(telemetry, "emit_json");
  config.telemetry.stats_period_ms = RequireScalar<std::uint32_t>(telemetry, "stats_period_ms");

  config.zones = ParseZones(root["zones"]);

  ValidateRuntimeConfig(config);
  return config;
}

GeometryConfig LoadGeometryFromFile(const std::string& path)
{
  const YAML::Node root = YAML::LoadFile(path);
  GeometryConfig geometry;
  geometry.profile_name = RequireScalar<std::string>(root, "profile_name");

  const YAML::Node mics = root["microphones"];
  if (!mics || !mics.IsSequence())
  {
    throw std::runtime_error("microphones must be a sequence");
  }

  geometry.microphones.reserve(mics.size());
  for (const YAML::Node& mic_node : mics)
  {
    GeometryMic mic;
    mic.id = RequireScalar<std::string>(mic_node, "id");
    mic.x = RequireScalar<double>(mic_node, "x");
    mic.y = RequireScalar<double>(mic_node, "y");
    mic.z = RequireScalar<double>(mic_node, "z");
    geometry.microphones.push_back(mic);
  }

  ValidateGeometryConfig(geometry);
  return geometry;
}

void ValidateRuntimeConfig(const RuntimeConfig& config)
{
  if (config.capture.sample_rate_hz < 16000 || config.capture.sample_rate_hz > 96000)
  {
    throw std::runtime_error("capture sample_rate_hz is out of expected bounds");
  }
  if (config.playback.sample_rate_hz < 16000 || config.playback.sample_rate_hz > 96000)
  {
    throw std::runtime_error("playback sample_rate_hz is out of expected bounds");
  }
  if (config.capture.sample_rate_hz != config.playback.sample_rate_hz)
  {
    throw std::runtime_error("capture and playback nominal sample rates must match");
  }

  if (config.capture.period_frames == 0 || config.capture.periods < 2)
  {
    throw std::runtime_error("capture period settings are invalid");
  }

  if (config.playback.period_frames == 0 || config.playback.periods < 2)
  {
    throw std::runtime_error("playback period settings are invalid");
  }

  if (config.active_channel_map.size() != 6)
  {
    throw std::runtime_error("active_channel_map must contain exactly six channels");
  }

  std::unordered_set<std::size_t> seen;
  for (const std::size_t channel : config.active_channel_map)
  {
    if (channel > 15)
    {
      throw std::runtime_error("active_channel_map channel index out of bounds");
    }
    if (!seen.insert(channel).second)
    {
      throw std::runtime_error("active_channel_map contains duplicate channel indices");
    }
  }

  if (config.asrc.min_ratio <= 0.0 || config.asrc.max_ratio <= 0.0 ||
      config.asrc.min_ratio >= config.asrc.max_ratio)
  {
    throw std::runtime_error("ASRC ratio bounds are invalid");
  }

  if (config.asrc.target_buffer_frames == 0)
  {
    throw std::runtime_error("ASRC target buffer must be non-zero");
  }

  if (config.calibration_dc_block_hz <= 0.0F || config.calibration_dc_block_hz > 500.0F)
  {
    throw std::runtime_error("calibration_dc_block_hz must be in (0, 500]");
  }

  if (config.steering.reference_mic_index >= config.active_channel_map.size())
  {
    throw std::runtime_error("reference_mic_index is out of active channel map range");
  }

  if (config.steering.steering_ramp_ms < 10.0F || config.steering.steering_ramp_ms > 500.0F)
  {
    throw std::runtime_error("steering_ramp_ms is outside safe bounds");
  }

  if (config.steering.ambient_floor_linear < 0.0F || config.steering.ambient_floor_linear > 1.0F)
  {
    throw std::runtime_error("ambient_floor_linear must be in [0, 1]");
  }

  if (config.suppression.fade_ms < 1.0F || config.suppression.fade_ms > 1000.0F)
  {
    throw std::runtime_error("suppression.fade_ms must be in [1, 1000]");
  }

  if (config.suppression.activity_threshold < 0.0F || config.suppression.activity_threshold > 1.0F)
  {
    throw std::runtime_error("suppression.activity_threshold must be in [0, 1]");
  }

  if (config.suppression.confidence_threshold < 0.0F ||
      config.suppression.confidence_threshold > 1.0F)
  {
    throw std::runtime_error("suppression.confidence_threshold must be in [0, 1]");
  }

  if (config.state_machine.activation_hold_ms == 0 || config.state_machine.confirmation_hold_ms == 0)
  {
    throw std::runtime_error("state machine activation and confirmation holds must be non-zero");
  }

  if (config.state_machine.zone_direction_stability_deg <= 0.0F ||
      config.state_machine.zone_direction_stability_deg > 180.0F)
  {
    throw std::runtime_error("zone_direction_stability_deg must be in (0, 180]");
  }

  if (config.zones.empty())
  {
    throw std::runtime_error("at least one zone must be configured");
  }

  for (const ZoneConfig& zone : config.zones)
  {
    if (zone.name.empty())
    {
      throw std::runtime_error("zone name cannot be empty");
    }
    if (zone.azimuth_min_deg < -180.0F || zone.azimuth_min_deg > 360.0F ||
        zone.azimuth_max_deg < -180.0F || zone.azimuth_max_deg > 360.0F)
    {
      throw std::runtime_error("zone azimuth bounds must be within [-180, 360]");
    }
  }
}

void ValidateRuntimeAudioContract(const RuntimeConfig& config, const RuntimeAudioContract& contract)
{
  if (contract.capture_sample_rate_hz != config.capture.sample_rate_hz)
  {
    throw std::runtime_error("negotiated capture sample rate does not match the configured DSP rate");
  }
  if (contract.playback_sample_rate_hz != config.playback.sample_rate_hz)
  {
    throw std::runtime_error("negotiated playback sample rate does not match the configured DSP rate");
  }
  if (contract.capture_channels == 0)
  {
    throw std::runtime_error("negotiated capture channel count must be non-zero");
  }
  for (const std::size_t channel : config.active_channel_map)
  {
    if (channel >= contract.capture_channels)
    {
      throw std::runtime_error(
          "active channel map index exceeds negotiated capture channel count");
    }
  }
  if (!config.asrc.enabled)
  {
    return;
  }

  const std::size_t min_occupancy = contract.software_queue_frames;
  const std::size_t max_occupancy =
      contract.software_queue_frames + contract.playback_buffer_frames;
  const std::size_t target = config.asrc.target_buffer_frames;
  const std::size_t headroom = contract.minimum_asrc_headroom_frames;
  if (contract.playback_buffer_frames == 0 || max_occupancy < min_occupancy || headroom == 0 ||
      target < min_occupancy || target > max_occupancy ||
      (target - min_occupancy) < headroom || (max_occupancy - target) < headroom)
  {
    throw std::runtime_error(
        "ASRC target_buffer_frames must sit at least one negotiated block above the retained "
        "software-queue floor and one block below negotiated playback capacity");
  }
}

void ValidateGeometryConfig(const GeometryConfig& geometry)
{
  if (geometry.profile_name.empty())
  {
    throw std::runtime_error("geometry profile_name cannot be empty");
  }

  if (geometry.microphones.size() != 6)
  {
    throw std::runtime_error("geometry must contain exactly six microphones for v1");
  }

  std::unordered_set<std::string> ids;
  for (const GeometryMic& mic : geometry.microphones)
  {
    if (mic.id.empty())
    {
      throw std::runtime_error("microphone id cannot be empty");
    }
    if (!ids.insert(mic.id).second)
    {
      throw std::runtime_error("geometry microphone ids must be unique");
    }
  }
}
}  // namespace sonitude::app
