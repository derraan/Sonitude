#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "app/config.hpp"
#include "audio/audio_types.hpp"

void RunAudioSupportTests();
void RunRtPrimitiveTests();
void RunAsrcSimulationTests();
void RunCalibrationTests();
void RunBeamformerTests();
void RunLimiterTests();
void RunStereoLimiterTests();
void RunSnapshotTests();
void RunOdasParserTests();
void RunSourceTrackerTests();
void RunControlLoopTests();
void RunZoneTests();
void RunStateMachineTests();
void RunSuppressorTests();
void RunLifecycleTests();
void RunThreadSafetyTests();
void RunWavReplayTests();
void RunCalibrationEstimateTests();
void RunStftTests();
void RunSpectralPostfilterTests();
void RunBinauralTests();

namespace
{
std::string FixturePath(const char* rel)
{
  return std::string(SONITUDE_SOURCE_DIR) + "/" + rel;
}

void Require(bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void RequireThrowsContains(const std::string& expected, const std::function<void()>& fn,
                           const std::string& message)
{
  bool threw = false;
  try
  {
    fn();
  }
  catch (const std::exception& ex)
  {
    threw = true;
    if (ex.what() == nullptr || std::string(ex.what()).find(expected) == std::string::npos)
    {
      throw std::runtime_error(message + ": unexpected error '" + std::string(ex.what()) + "'");
    }
  }
  if (!threw)
  {
    throw std::runtime_error(message + ": expected an exception");
  }
}

void TestRuntimeConfigValid()
{
  const auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("tests/fixtures/runtime_valid.yaml"));
  Require(config.active_channel_map.size() == sonitude::audio::kMicChannels,
          "valid runtime config did not load six channels");
  Require(config.suppression.fade_ms > 0.0F, "suppression config should parse from runtime YAML");
  Require(config.suppression.backend == "conservative",
          "omitted suppression.backend must default to conservative");
  Require(config.calibration_dc_block_hz > 0.0F,
          "calibration_dc_block_hz should parse from runtime YAML");
  Require(config.realtime.capture_priority > 0, "realtime config should parse from runtime YAML");
  Require(!config.realtime.require_memory_lock,
          "missing require_memory_lock must retain the backward-compatible false default");
  Require(config.zones.front().policy == sonitude::app::ZonePolicy::Focus,
          "zone policy should parse from runtime YAML");
}

void TestProductionRealtimeContract()
{
  const auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("config/production_pi.yaml"));
  Require(config.realtime.require_realtime,
          "the production Pi configuration must require realtime scheduling");
  Require(config.realtime.enable_mlockall && config.realtime.require_memory_lock,
          "the production Pi configuration must require memory locking");

  auto invalid = config;
  invalid.realtime.enable_mlockall = false;
  bool threw = false;
  try
  {
    sonitude::app::ValidateRuntimeConfig(invalid);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "require_memory_lock without enable_mlockall must be rejected");
}

void TestProductionPiProfileMatchesValidatedHardware()
{
  const auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("config/production_pi.yaml"));
  Require(config.capture.alsa_device == "hw:active,0",
          "production capture ALSA device must match Pi-tested identity");
  Require(config.playback.alsa_device == "hw:X1,0",
          "production playback ALSA device must match Pi-tested identity");
  Require(config.active_channel_map == std::vector<std::size_t>({5, 4, 3, 2, 1, 0}),
          "production active_channel_map must match the Pi-tested reverse map");
  Require(config.geometry_path.find("geometry_soundbubble_xyz_v1.yaml") != std::string::npos,
          "production geometry path must use the corrected XYZ profile");
  Require(config.calibration_path.find("calibration_example.yaml") != std::string::npos,
          "production calibration path must use the unity fixture");
  Require(!config.odas.enabled, "production profile must keep ODAS disabled for baseline gates");
  Require(!config.suppression.enabled,
          "production profile must keep suppression disabled for baseline gates");
  Require(config.realtime.require_realtime,
          "production profile must fail closed when realtime policy cannot be obtained");
  Require(config.realtime.enable_mlockall && config.realtime.require_memory_lock,
          "production profile must fail closed when memory lock is unavailable");
}

void TestRuntimeConfigDuplicateChannelFails()
{
  bool threw = false;
  try
  {
    (void)sonitude::app::LoadRuntimeConfigFromFile(
        FixturePath("tests/fixtures/runtime_invalid_duplicate_channel.yaml"));
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "duplicate channel map should throw");
}

void TestRuntimeConfigUnknownSuppressionBackendFails()
{
  bool threw = false;
  try
  {
    (void)sonitude::app::LoadRuntimeConfigFromFile(
        FixturePath("tests/fixtures/runtime_invalid_suppression_backend.yaml"));
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "unknown suppression backend should throw");
}

void TestRuntimeConfigUnknownBinauralBackendFails()
{
  bool threw = false;
  try
  {
    (void)sonitude::app::LoadRuntimeConfigFromFile(
        FixturePath("tests/fixtures/runtime_invalid_binaural_backend.yaml"));
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "unknown binaural backend should throw");
}

void TestRuntimeAudioContract()
{
  auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("tests/fixtures/runtime_valid.yaml"));
  config.asrc.target_buffer_frames = 128;
  const sonitude::app::RuntimeAudioContract valid{
      .capture_sample_rate_hz = config.capture.sample_rate_hz,
      .playback_sample_rate_hz = config.playback.sample_rate_hz,
      .capture_channels = 8,
      .playback_buffer_frames = 192,
      .software_queue_frames = 64,
      .minimum_asrc_headroom_frames = 64,
      .capture_period_frames = 64,
      .playback_period_frames = 64,
      .asrc_max_ratio = config.asrc.max_ratio,
      .required_playback_scratch_frames = 256,
      .negotiated_playback_scratch_frames = 256,
  };
  sonitude::app::ValidateRuntimeAudioContract(config, valid);

  bool rate_rejected = false;
  try
  {
    auto mismatch = valid;
    mismatch.playback_sample_rate_hz = 48000;
    sonitude::app::ValidateRuntimeAudioContract(config, mismatch);
  }
  catch (const std::exception&)
  {
    rate_rejected = true;
  }
  Require(rate_rejected, "material negotiated playback-rate mismatch should throw");

  bool capture_rate_rejected = false;
  try
  {
    auto mismatch = valid;
    mismatch.capture_sample_rate_hz = 48000;
    sonitude::app::ValidateRuntimeAudioContract(config, mismatch);
  }
  catch (const std::exception&)
  {
    capture_rate_rejected = true;
  }
  Require(capture_rate_rejected, "material negotiated capture-rate mismatch should throw");

  bool target_rejected = false;
  try
  {
    config.asrc.target_buffer_frames = 256;
    sonitude::app::ValidateRuntimeAudioContract(config, valid);
  }
  catch (const std::exception&)
  {
    target_rejected = true;
  }
  Require(target_rejected, "ASRC target at usable capacity should throw");
}

void TestRuntimeAudioContractHeadroom()
{
  auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("tests/fixtures/runtime_valid.yaml"));
  config.asrc.target_buffer_frames = 128;

  const sonitude::app::RuntimeAudioContract base{
      .capture_sample_rate_hz = config.capture.sample_rate_hz,
      .playback_sample_rate_hz = config.playback.sample_rate_hz,
      .capture_channels = 8,
      .playback_buffer_frames = 192,
      .software_queue_frames = 64,
      .minimum_asrc_headroom_frames = 64,
      .capture_period_frames = 64,
      .playback_period_frames = 64,
      .asrc_max_ratio = config.asrc.max_ratio,
      .required_playback_scratch_frames = 256,
      .negotiated_playback_scratch_frames = 256,
  };

  sonitude::app::ValidateRuntimeAudioContract(config, base);

  config.asrc.target_buffer_frames = 192;
  sonitude::app::ValidateRuntimeAudioContract(config, base);

  auto target_too_high = base;
  bool threw = false;
  try
  {
    config.asrc.target_buffer_frames = 193;
    sonitude::app::ValidateRuntimeAudioContract(config, target_too_high);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "ASRC target without upper-side headroom should throw");

  threw = false;
  try
  {
    config.asrc.target_buffer_frames = 80;
    sonitude::app::ValidateRuntimeAudioContract(config, base);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "ASRC target below software-floor plus headroom should throw");

  threw = false;
  try
  {
    config.asrc.target_buffer_frames = 64;
    sonitude::app::ValidateRuntimeAudioContract(config, base);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "ASRC target at software-floor should throw");

  threw = false;
  try
  {
    auto negotiated_period_128 = base;
    negotiated_period_128.software_queue_frames = 128;
    negotiated_period_128.minimum_asrc_headroom_frames = 128;
    negotiated_period_128.playback_buffer_frames = 384;
    config.asrc.target_buffer_frames = 128;
    sonitude::app::ValidateRuntimeAudioContract(config, negotiated_period_128);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "negotiated 128-frame period with target at software floor should throw");

  threw = false;
  try
  {
    auto insufficient_channels = base;
    insufficient_channels.capture_channels = 4;
    config.asrc.target_buffer_frames = 128;
    sonitude::app::ValidateRuntimeAudioContract(config, insufficient_channels);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "active map that exceeds negotiated capture channels should throw");

  auto sufficient_channels = base;
  sufficient_channels.capture_channels = 6;
  config.asrc.target_buffer_frames = 128;
  sonitude::app::ValidateRuntimeAudioContract(config, sufficient_channels);

  threw = false;
  try
  {
    auto smaller_playback_period = base;
    smaller_playback_period.playback_period_frames = 32;
    smaller_playback_period.required_playback_scratch_frames = 256;
    smaller_playback_period.negotiated_playback_scratch_frames = 192;
    sonitude::app::ValidateRuntimeAudioContract(config, smaller_playback_period);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "playback period smaller than capture period should fail when scratch capacity is "
                 "insufficient");

  threw = false;
  try
  {
    auto ratio_stress = base;
    ratio_stress.asrc_max_ratio = config.asrc.max_ratio;
    ratio_stress.required_playback_scratch_frames = 320;
    ratio_stress.negotiated_playback_scratch_frames = 256;
    sonitude::app::ValidateRuntimeAudioContract(config, ratio_stress);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "high ASRC ratio scratch demand beyond negotiated capacity should throw");
}

void TestRuntimeConfigOdasContradictionFails()
{
  RequireThrowsContains(
      "contradictory provider settings",
      [&]()
      {
        (void)sonitude::app::LoadRuntimeConfigFromFile(
            FixturePath("tests/fixtures/runtime_invalid_odas_disabled_real.yaml"));
      },
      "odas.enabled=false with use_mock_provider=false should throw a specific validation error");
}

void TestRuntimeConfigRejectsNonFiniteAndZeroInvalidFields()
{
  RequireThrowsContains(
      "asrc.min_ratio must be finite",
      [&]()
      {
        (void)sonitude::app::LoadRuntimeConfigFromFile(
            FixturePath("tests/fixtures/runtime_invalid_asrc_nan.yaml"));
      },
      "runtime config should reject non-finite ASRC ratio bounds");
  RequireThrowsContains(
      "steering.speed_of_sound_mps must be in (100, 500]",
      [&]()
      {
        (void)sonitude::app::LoadRuntimeConfigFromFile(
            FixturePath("tests/fixtures/runtime_invalid_steering_speed_zero.yaml"));
      },
      "runtime config should reject non-physical steering speed");
  RequireThrowsContains(
      "telemetry.stats_period_ms must be non-zero",
      [&]()
      {
        (void)sonitude::app::LoadRuntimeConfigFromFile(
            FixturePath("tests/fixtures/runtime_invalid_telemetry_period_zero.yaml"));
      },
      "runtime config should reject zero telemetry period");
}

void TestGeometryValid()
{
  const auto geometry =
      sonitude::app::LoadGeometryFromFile(FixturePath("tests/fixtures/geometry_valid.yaml"));
  Require(geometry.microphones.size() == sonitude::audio::kMicChannels,
          "valid geometry did not load six microphones");
}

void TestProductionGeometryFrame()
{
  const auto geometry =
      sonitude::app::LoadGeometryFromFile(FixturePath("config/geometry_soundbubble_xyz_v1.yaml"));
  Require(geometry.microphones.size() == 6U, "production geometry must contain six microphones");
  Require(geometry.microphones.front().id == "M0_upper_inner_left",
          "production geometry IDs must remain in expected order");
  std::unordered_set<std::string> ids;
  bool saw_left = false;
  bool saw_right = false;
  for (const auto& mic : geometry.microphones)
  {
    Require(std::isfinite(mic.x) && std::isfinite(mic.y) && std::isfinite(mic.z),
            "production geometry coordinates must be finite");
    Require(ids.insert(mic.id).second, "production geometry IDs must remain unique");
    if (mic.id.find("left") != std::string::npos)
    {
      Require(mic.y < 0.0, "left microphones must lie on negative Y");
      saw_left = true;
    }
    if (mic.id.find("right") != std::string::npos)
    {
      Require(mic.y > 0.0, "right microphones must lie on positive Y");
      saw_right = true;
    }
  }
  Require(geometry.microphones[0].x > 0.0 && geometry.microphones[1].x > 0.0,
          "front upper microphones must remain at positive X");
  Require(saw_left && saw_right, "production geometry must contain both left and right IDs");
}

void TestGeometryInvalidCountFails()
{
  bool threw = false;
  try
  {
    (void)sonitude::app::LoadGeometryFromFile(
        FixturePath("tests/fixtures/geometry_invalid_count.yaml"));
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "invalid microphone count should throw");
}

void TestGeometryInvalidNanFails()
{
  RequireThrowsContains(
      "geometry.x must be finite",
      [&]()
      {
        (void)sonitude::app::LoadGeometryFromFile(
            FixturePath("tests/fixtures/geometry_invalid_nan.yaml"));
      },
      "geometry parser should reject non-finite coordinates");
}

void TestAudioTypeInvariants()
{
  sonitude::audio::MicFrame frame{};
  Require(frame.size() == sonitude::audio::kMicChannels, "MicFrame width must match channel count");
}
} // namespace

int main()
{
  try
  {
    TestRuntimeConfigValid();
    TestProductionRealtimeContract();
    TestProductionPiProfileMatchesValidatedHardware();
    TestRuntimeConfigDuplicateChannelFails();
    TestRuntimeConfigUnknownBinauralBackendFails();
    TestRuntimeConfigUnknownSuppressionBackendFails();
    TestRuntimeAudioContract();
    TestRuntimeAudioContractHeadroom();
    TestRuntimeConfigOdasContradictionFails();
    TestRuntimeConfigRejectsNonFiniteAndZeroInvalidFields();
    TestGeometryValid();
    TestProductionGeometryFrame();
    TestGeometryInvalidCountFails();
    TestGeometryInvalidNanFails();
    TestAudioTypeInvariants();
    RunAudioSupportTests();
    RunRtPrimitiveTests();
    RunAsrcSimulationTests();
    RunCalibrationTests();
    RunBeamformerTests();
    RunSuppressorTests();
    RunStftTests();
    RunSpectralPostfilterTests();
    RunLimiterTests();
    RunStereoLimiterTests();
    RunBinauralTests();
    RunSnapshotTests();
    RunOdasParserTests();
    RunSourceTrackerTests();
    RunControlLoopTests();
    RunZoneTests();
    RunStateMachineTests();
    RunLifecycleTests();
    RunThreadSafetyTests();
    RunWavReplayTests();
    RunCalibrationEstimateTests();
    std::cout << "All unit tests passed.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Unit test failure: " << ex.what() << '\n';
    return 1;
  }
}
