#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonitude::app
{
enum class ZonePolicy : std::uint8_t
{
  Focus = 0,
  Assist = 1,
  Ambient = 2
};

struct ZoneConfig
{
  std::string name;
  float azimuth_min_deg = 0.0F;
  float azimuth_max_deg = 0.0F;
  ZonePolicy policy = ZonePolicy::Focus;
};

struct DeviceConfig
{
  std::string alsa_device;
  std::uint32_t sample_rate_hz = 0;
  std::uint32_t period_frames = 0;
  std::uint32_t periods = 0;
};

struct AsrcConfig
{
  bool enabled = true;
  bool allow_bypass_for_locked_bench = false;
  double min_ratio = 0.995;
  double max_ratio = 1.005;
  std::uint32_t target_buffer_frames = 128;
  double pi_kp = 0.0002;
  double pi_ki = 0.00001;
};

struct GeometryMic
{
  std::string id;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct GeometryConfig
{
  std::string profile_name;
  std::vector<GeometryMic> microphones;
};

struct SpatialConfig
{
  std::string backend = "adaptive_geometric";
  std::string profile_path;
  bool mask_enabled = true;
  float eta_low_db = -3.0F;
  float eta_high_db = 3.0F;
  float mask_smooth_sec = 0.020F;
};

struct SteeringConfig
{
  float speed_of_sound_mps = 343.0F;
  std::size_t reference_mic_index = 0;
  float steering_ramp_ms = 150.0F;
  float ambient_floor_linear = 0.25F;
  std::string model = "near_field";
  float source_distance_m = 0.45F;
  bool experimental_dual_reference_mvdr = false;
  bool binaural_output = false;
  std::size_t left_ear_mic_index = 0;
  std::size_t right_ear_mic_index = 5;
  struct KemarLutConfig
  {
    bool enabled = false;
    std::string table_path;
  } kemar_lut;
};

struct SpectralSuppressionConfig
{
  std::size_t fft_size = 128;
  std::size_t hop_size = 32;
  float gain_floor_db = -12.0F;
};

struct SuppressionConfig
{
  bool enabled = false;
  std::string backend = "conservative";
  float fade_ms = 120.0F;
  float activity_threshold = 0.03F;
  float confidence_threshold = 0.6F;
  SpectralSuppressionConfig spectral{};
};

struct StateMachineConfig
{
  std::uint32_t activation_hold_ms = 400;
  std::uint32_t confirmation_hold_ms = 250;
  std::uint32_t release_hold_ms = 1500;
  std::uint32_t hold_direction_ms = 1000;
  float zone_direction_stability_deg = 15.0F;
};

struct OdasConfig
{
  bool enabled = false;
  bool use_mock_provider = true;
  std::string endpoint;
};

struct TelemetryConfig
{
  bool log_human_readable = true;
  bool emit_csv = false;
  bool emit_json = false;
  std::uint32_t stats_period_ms = 1000;
};

struct BinauralDirectionConfig
{
  bool follow_steering = false;
  float azimuth_deg = 0.0F;
  float elevation_deg = 0.0F;
};

struct BinauralTransitionConfig
{
  float duration_ms = 150.0F;
};

struct BinauralProfileConfig
{
  std::string id = "generic";
  std::string table_path;
};

struct BinauralModelConfig
{
  float head_radius_m = 0.0875F;
  float max_ild_db = 6.0F;
};

struct BinauralConfig
{
  bool enabled = false;
  std::string backend = "mono_reference";
  BinauralDirectionConfig direction;
  BinauralTransitionConfig transition;
  BinauralProfileConfig profile;
  BinauralModelConfig model;
};

struct RealtimeConfig
{
  std::int32_t capture_priority = 80;
  std::int32_t playback_priority = 78;
  bool enable_mlockall = true;
};

struct RuntimeConfig
{
  DeviceConfig capture;
  DeviceConfig playback;
  std::vector<std::size_t> active_channel_map;
  std::string geometry_path;
  std::string calibration_path;
  float calibration_dc_block_hz = 20.0F;
  AsrcConfig asrc;
  SteeringConfig steering;
  SpatialConfig spatial;
  SuppressionConfig suppression;
  StateMachineConfig state_machine;
  OdasConfig odas;
  TelemetryConfig telemetry;
  BinauralConfig binaural;
  RealtimeConfig realtime;
  std::vector<ZoneConfig> zones;
};

struct RuntimeAudioContract
{
  std::uint32_t capture_sample_rate_hz = 0;
  std::uint32_t playback_sample_rate_hz = 0;
  std::size_t capture_channels = 0;
  std::size_t playback_buffer_frames = 0;
  std::size_t software_queue_frames = 0;
  std::size_t minimum_asrc_headroom_frames = 0;
  std::size_t capture_period_frames = 0;
  std::size_t playback_period_frames = 0;
  double asrc_max_ratio = 0.0;
  std::size_t required_playback_scratch_frames = 0;
  std::size_t negotiated_playback_scratch_frames = 0;
};

RuntimeConfig LoadRuntimeConfigFromFile(const std::string& path);
GeometryConfig LoadGeometryFromFile(const std::string& path);
void ValidateRuntimeConfig(const RuntimeConfig& config);
void ValidateRuntimeAudioContract(const RuntimeConfig& config, const RuntimeAudioContract& contract);
void ValidateGeometryConfig(const GeometryConfig& geometry);
}  // namespace sonitude::app
