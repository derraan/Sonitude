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
