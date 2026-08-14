#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "app/config.hpp"
#include "audio/audio_types.hpp"

void RunAudioSupportTests();
void RunRtPrimitiveTests();
void RunAsrcSimulationTests();
void RunCalibrationTests();
void RunBeamformerTests();
void RunLimiterTests();
void RunSnapshotTests();
void RunOdasParserTests();
void RunControlLoopTests();
void RunZoneTests();
void RunStateMachineTests();
void RunSuppressorTests();

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

void TestRuntimeConfigValid()
{
  const auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("tests/fixtures/runtime_valid.yaml"));
  Require(config.active_channel_map.size() == sonitude::audio::kMicChannels,
          "valid runtime config did not load six channels");
  Require(config.suppression.fade_ms > 0.0F, "suppression config should parse from runtime YAML");
  Require(config.calibration_dc_block_hz > 0.0F, "calibration_dc_block_hz should parse from runtime YAML");
  Require(config.realtime.capture_priority > 0, "realtime config should parse from runtime YAML");
  Require(config.zones.front().policy == sonitude::app::ZonePolicy::Focus,
          "zone policy should parse from runtime YAML");
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
  Require(
      threw,
      "playback period smaller than capture period should fail when scratch capacity is insufficient");

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
  auto config =
      sonitude::app::LoadRuntimeConfigFromFile(FixturePath("tests/fixtures/runtime_valid.yaml"));
  config.odas.enabled = false;
  config.odas.use_mock_provider = false;
  bool threw = false;
  try
  {
    sonitude::app::ValidateRuntimeConfig(config);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "odas.enabled=false with use_mock_provider=false should throw");
}

void TestGeometryValid()
{
  const auto geometry =
      sonitude::app::LoadGeometryFromFile(FixturePath("tests/fixtures/geometry_valid.yaml"));
  Require(geometry.microphones.size() == sonitude::audio::kMicChannels,
          "valid geometry did not load six microphones");
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

void TestAudioTypeInvariants()
{
  sonitude::audio::MicFrame frame{};
  Require(frame.size() == sonitude::audio::kMicChannels, "MicFrame width must match channel count");
}
}  // namespace

int main()
{
  try
  {
    TestRuntimeConfigValid();
    TestRuntimeConfigDuplicateChannelFails();
    TestRuntimeAudioContract();
    TestRuntimeAudioContractHeadroom();
    TestRuntimeConfigOdasContradictionFails();
    TestGeometryValid();
    TestGeometryInvalidCountFails();
    TestAudioTypeInvariants();
    RunAudioSupportTests();
    RunRtPrimitiveTests();
    RunAsrcSimulationTests();
    RunCalibrationTests();
    RunBeamformerTests();
    RunSuppressorTests();
    RunLimiterTests();
    RunSnapshotTests();
    RunOdasParserTests();
    RunControlLoopTests();
    RunZoneTests();
    RunStateMachineTests();
    std::cout << "All unit tests passed.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Unit test failure: " << ex.what() << '\n';
    return 1;
  }
}
