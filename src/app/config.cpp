#include "app/config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

bool IsKnownEqType(const std::string& type)
{
  return type == "PK" || type == "LS" || type == "HS" || type == "LP" || type == "HP";
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
  if (steering["model"])
  {
    config.steering.model = RequireScalar<std::string>(steering, "model");
  }
  if (steering["source_distance_m"])
  {
    config.steering.source_distance_m = RequireScalar<float>(steering, "source_distance_m");
  }
  if (steering["experimental_dual_reference_mvdr"])
  {
    config.steering.experimental_dual_reference_mvdr =
        RequireScalar<bool>(steering, "experimental_dual_reference_mvdr");
  }
  else if (steering["binaural_output"])
  {
    config.steering.experimental_dual_reference_mvdr =
        RequireScalar<bool>(steering, "binaural_output");
  }
  if (steering["left_ear_mic_index"])
  {
    config.steering.left_ear_mic_index = RequireScalar<std::size_t>(steering, "left_ear_mic_index");
  }
  if (steering["right_ear_mic_index"])
  {
    config.steering.right_ear_mic_index = RequireScalar<std::size_t>(steering, "right_ear_mic_index");
  }
  if (steering["kemar_lut"])
  {
    const YAML::Node kemar = steering["kemar_lut"];
    if (kemar["enabled"])
    {
      config.steering.kemar_lut.enabled = kemar["enabled"].as<bool>();
    }
    if (kemar["table_path"])
    {
      config.steering.kemar_lut.table_path =
          ResolvePath(path, RequireScalar<std::string>(kemar, "table_path"));
    }
  }

  const YAML::Node spatial = root["spatial"];
  if (spatial)
  {
    if (spatial["backend"])
    {
      config.spatial.backend = RequireScalar<std::string>(spatial, "backend");
    }
    if (spatial["profile_path"])
    {
      config.spatial.profile_path =
          ResolvePath(path, RequireScalar<std::string>(spatial, "profile_path"));
    }
    if (spatial["mask_enabled"])
    {
      config.spatial.mask_enabled = RequireScalar<bool>(spatial, "mask_enabled");
    }
    if (spatial["eta_low_db"])
    {
      config.spatial.eta_low_db = RequireScalar<float>(spatial, "eta_low_db");
    }
    if (spatial["eta_high_db"])
    {
      config.spatial.eta_high_db = RequireScalar<float>(spatial, "eta_high_db");
    }
    if (spatial["mask_smooth_sec"])
    {
      config.spatial.mask_smooth_sec = RequireScalar<float>(spatial, "mask_smooth_sec");
    }
  }

  const YAML::Node suppression = root["suppression"];
  config.suppression.enabled = RequireScalar<bool>(suppression, "enabled");
  config.suppression.fade_ms = RequireScalar<float>(suppression, "fade_ms");
  config.suppression.activity_threshold = RequireScalar<float>(suppression, "activity_threshold");
  config.suppression.confidence_threshold =
      RequireScalar<float>(suppression, "confidence_threshold");
  if (suppression["backend"])
  {
    config.suppression.backend = RequireScalar<std::string>(suppression, "backend");
  }
  if (suppression["spectral"])
  {
    const YAML::Node spectral = suppression["spectral"];
    if (spectral["fft_size"])
    {
      config.suppression.spectral.fft_size = RequireScalar<std::size_t>(spectral, "fft_size");
    }
    if (spectral["hop_size"])
    {
      config.suppression.spectral.hop_size = RequireScalar<std::size_t>(spectral, "hop_size");
    }
    if (spectral["gain_floor_db"])
    {
      config.suppression.spectral.gain_floor_db = RequireScalar<float>(spectral, "gain_floor_db");
    }
  }

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

  const YAML::Node realtime = root["realtime"];
  config.realtime.capture_priority = RequireScalar<std::int32_t>(realtime, "capture_priority");
  config.realtime.playback_priority = RequireScalar<std::int32_t>(realtime, "playback_priority");
  config.realtime.enable_mlockall = RequireScalar<bool>(realtime, "enable_mlockall");

  if (root["binaural"])
  {
    const YAML::Node binaural = root["binaural"];
    config.binaural.enabled = RequireScalar<bool>(binaural, "enabled");
    config.binaural.backend = RequireScalar<std::string>(binaural, "backend");
    if (binaural["direction"])
    {
      const YAML::Node direction = binaural["direction"];
      config.binaural.direction.follow_steering = RequireScalar<bool>(direction, "follow_steering");
      if (direction["azimuth_deg"])
      {
        config.binaural.direction.azimuth_deg = RequireScalar<float>(direction, "azimuth_deg");
      }
      if (direction["elevation_deg"])
      {
        config.binaural.direction.elevation_deg = RequireScalar<float>(direction, "elevation_deg");
      }
    }
    if (binaural["transition"])
    {
      const YAML::Node transition = binaural["transition"];
      config.binaural.transition.duration_ms = RequireScalar<float>(transition, "duration_ms");
    }
    if (binaural["profile"])
    {
      const YAML::Node profile = binaural["profile"];
      config.binaural.profile.id = RequireScalar<std::string>(profile, "id");
      if (profile["table_path"])
      {
        config.binaural.profile.table_path =
            ResolvePath(path, RequireScalar<std::string>(profile, "table_path"));
      }
    }
    if (binaural["model"])
    {
      const YAML::Node model = binaural["model"];
      config.binaural.model.head_radius_m = RequireScalar<float>(model, "head_radius_m");
      config.binaural.model.max_ild_db = RequireScalar<float>(model, "max_ild_db");
    }
  }

  if (root["common_eq"])
  {
    const YAML::Node eq = root["common_eq"];
    if (eq["enabled"])
    {
      config.common_eq.enabled = RequireScalar<bool>(eq, "enabled");
    }
    if (eq["sections"])
    {
      const YAML::Node sections = eq["sections"];
      if (!sections.IsSequence())
      {
        throw std::runtime_error("common_eq.sections must be a sequence");
      }
      for (const YAML::Node& s : sections)
      {
        EqSectionConfig sec;
        sec.type = RequireScalar<std::string>(s, "type");
        sec.freq_hz = RequireScalar<float>(s, "freq_hz");
        if (s["gain_db"])
        {
          sec.gain_db = s["gain_db"].as<float>();
        }
        sec.q = RequireScalar<float>(s, "q");
        config.common_eq.sections.push_back(sec);
      }
    }
  }

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
  if (!std::isfinite(config.asrc.min_ratio) || !std::isfinite(config.asrc.max_ratio) ||
      !std::isfinite(config.asrc.pi_kp) || !std::isfinite(config.asrc.pi_ki))
  {
    throw std::runtime_error("ASRC scalar values must be finite");
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
  if (!std::isfinite(config.steering.steering_ramp_ms))
  {
    throw std::runtime_error("steering_ramp_ms must be finite");
  }

  if (config.steering.ambient_floor_linear < 0.0F || config.steering.ambient_floor_linear > 1.0F)
  {
    throw std::runtime_error("ambient_floor_linear must be in [0, 1]");
  }
  if (config.steering.model != "near_field" && config.steering.model != "far_field")
  {
    throw std::runtime_error("steering.model must be near_field or far_field");
  }
  if (config.steering.source_distance_m <= 0.0F || config.steering.source_distance_m > 5.0F)
  {
    throw std::runtime_error("steering.source_distance_m is outside engineering guardrails");
  }
  if (config.spatial.backend != "adaptive_geometric" && config.spatial.backend != "fixed_measured")
  {
    throw std::runtime_error("spatial.backend must be adaptive_geometric or fixed_measured");
  }
  if (config.spatial.backend == "fixed_measured")
  {
    if (config.spatial.profile_path.empty())
    {
      throw std::runtime_error("spatial.backend=fixed_measured requires spatial.profile_path");
    }
    if (config.binaural.enabled)
    {
      throw std::runtime_error(
          "fixed_measured backend already emits stereo; disable binaural.enabled to avoid a second HRTF renderer");
    }
    if (config.steering.experimental_dual_reference_mvdr)
    {
      throw std::runtime_error(
          "fixed_measured cannot be combined with experimental_dual_reference_mvdr");
    }
  }
  if (config.steering.left_ear_mic_index >= 6 || config.steering.right_ear_mic_index >= 6)
  {
    throw std::runtime_error("steering ear mic index out of range");
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

  if (config.suppression.backend != "conservative" && config.suppression.backend != "spectral")
  {
    throw std::runtime_error(
        "suppression.backend must be conservative or spectral (unknown value is not remapped)");
  }

  const bool spectral_pair_ok =
      (config.suppression.spectral.fft_size == 128 && config.suppression.spectral.hop_size == 32) ||
      (config.suppression.spectral.fft_size == 256 && config.suppression.spectral.hop_size == 64);
  if (config.suppression.backend == "spectral" && !spectral_pair_ok)
  {
    throw std::runtime_error("suppression.spectral fft/hop must be 128/32 or 256/64");
  }
  if (config.suppression.spectral.gain_floor_db > 0.0F ||
      config.suppression.spectral.gain_floor_db < -80.0F)
  {
    throw std::runtime_error("suppression.spectral.gain_floor_db must be in [-80, 0]");
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

  if (!config.odas.enabled && !config.odas.use_mock_provider)
  {
    throw std::runtime_error(
        "odas.enabled=false requires odas.use_mock_provider=true to avoid contradictory provider settings");
  }
  if (config.telemetry.stats_period_ms == 0)
  {
    throw std::runtime_error("telemetry.stats_period_ms must be non-zero");
  }

  if (config.realtime.capture_priority < 1 || config.realtime.capture_priority > 99)
  {
    throw std::runtime_error("realtime.capture_priority must be in [1, 99]");
  }
  if (config.realtime.playback_priority < 1 || config.realtime.playback_priority > 99)
  {
    throw std::runtime_error("realtime.playback_priority must be in [1, 99]");
  }

  static const std::array<const char*, 5> kKnownBinauralBackends = {
      "mono_reference", "itd_ild", "compact_hrtf", "full_hrtf_reference", "array_downmix"};
  const bool known_backend = std::any_of(
      kKnownBinauralBackends.begin(),
      kKnownBinauralBackends.end(),
      [&](const char* value) { return config.binaural.backend == value; });
  if (!known_backend)
  {
    throw std::runtime_error("Unknown binaural backend: " + config.binaural.backend);
  }
  if (config.binaural.transition.duration_ms < 0.0F || config.binaural.transition.duration_ms > 500.0F)
  {
    throw std::runtime_error("binaural.transition.duration_ms must be in [0, 500]");
  }
  if (config.binaural.model.head_radius_m <= 0.0F || config.binaural.model.head_radius_m > 0.25F)
  {
    throw std::runtime_error("binaural.model.head_radius_m must be in (0, 0.25]");
  }
  if (config.binaural.profile.id.empty())
  {
    throw std::runtime_error("binaural.profile.id cannot be empty");
  }

  if (config.common_eq.sections.size() > 24U)
  {
    throw std::runtime_error("common_eq supports at most 24 sections");
  }
  for (const auto& sec : config.common_eq.sections)
  {
    if (!IsKnownEqType(sec.type))
    {
      throw std::runtime_error("common_eq section type must be PK/LS/HS/LP/HP");
    }
    if (!(sec.freq_hz > 0.0F && sec.freq_hz < (0.5F * static_cast<float>(config.capture.sample_rate_hz))))
    {
      throw std::runtime_error("common_eq.freq_hz out of range");
    }
    if (!(sec.q > 0.0F && sec.q <= 20.0F))
    {
      throw std::runtime_error("common_eq.q must be in (0, 20]");
    }
    if (std::fabs(sec.gain_db) > 24.0F)
    {
      throw std::runtime_error("common_eq.gain_db out of range");
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
  if (contract.capture_period_frames == 0 || contract.playback_period_frames == 0)
  {
    throw std::runtime_error("negotiated capture/playback periods must be non-zero");
  }
  if (contract.asrc_max_ratio <= 0.0)
  {
    throw std::runtime_error("negotiated ASRC max ratio must be positive");
  }
  if (contract.asrc_max_ratio > config.asrc.max_ratio)
  {
    throw std::runtime_error("negotiated ASRC max ratio exceeds configured ASRC max ratio");
  }
  if (contract.required_playback_scratch_frames > contract.negotiated_playback_scratch_frames)
  {
    throw std::runtime_error(
        "playback scratch capacity is below the negotiated minimum for capture/playback periods");
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
