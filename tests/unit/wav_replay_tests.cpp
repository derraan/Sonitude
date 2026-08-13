#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/audio_types.hpp"
#include "audio/wav_io.hpp"
#include "tools/wav_replay_support.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestMalformedCsvRejected()
{
  const std::string path = "unit_wav_replay_bad_script.csv";
  {
    std::ofstream out(path);
    out << "time_s,azimuth_deg,elevation_deg\n";
    out << "0.01,,5.0\n";
  }

  bool threw = false;
  try
  {
    (void)sonitude::tools::wav_replay::LoadSteeringScript(path, 44100);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  (void)std::remove(path.c_str());
  Require(threw, "malformed steering CSV should throw");
}

void TestFirstEventDelayedUsesNeutralTarget()
{
  const std::vector<sonitude::tools::wav_replay::SteeringEvent> events = {
      {100, {30.0F, 0.0F}},
  };
  const auto segments = sonitude::tools::wav_replay::BuildReplaySegments(
      256, 256, events, sonitude::audio::BeamformerSteering{0.0F, 0.0F});
  Require(segments.size() == 2, "delayed first event should split block into two segments");
  Require(segments[0].start_frame == 0 && segments[0].frame_count == 100,
          "first neutral segment bounds incorrect");
  Require(segments[0].target.azimuth_deg == 0.0F, "first segment should use neutral azimuth");
  Require(segments[1].start_frame == 100 && segments[1].frame_count == 156,
          "second segment bounds incorrect");
  Require(segments[1].target.azimuth_deg == 30.0F, "second segment should apply event azimuth");
}

void TestEventInsideBlockSplitsProcessing()
{
  const std::vector<sonitude::tools::wav_replay::SteeringEvent> events = {
      {0, {-20.0F, 0.0F}},
      {128, {10.0F, 0.0F}},
      {200, {40.0F, 0.0F}},
  };
  const auto segments = sonitude::tools::wav_replay::BuildReplaySegments(
      300, 256, events, sonitude::audio::BeamformerSteering{});
  Require(segments.size() == 4, "within-block event boundaries should create split segments");
  Require(segments[0].start_frame == 0 && segments[0].frame_count == 128 &&
              segments[0].target.azimuth_deg == -20.0F,
          "segment 0 mismatch");
  Require(segments[1].start_frame == 128 && segments[1].frame_count == 72 &&
              segments[1].target.azimuth_deg == 10.0F,
          "segment 1 mismatch");
  Require(segments[2].start_frame == 200 && segments[2].frame_count == 56 &&
              segments[2].target.azimuth_deg == 40.0F,
          "segment 2 mismatch");
  Require(segments[3].start_frame == 256 && segments[3].frame_count == 44 &&
              segments[3].target.azimuth_deg == 40.0F,
          "segment 3 mismatch");
}

void TestNonIdentityChannelMap()
{
  sonitude::audio::WavData wav;
  wav.sample_rate_hz = 44100;
  wav.channels = 8;
  wav.format = sonitude::audio::PcmFormat::FLOAT32_LE;
  wav.interleaved = {
      0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F,
      10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F, 17.0F,
  };
  const std::vector<std::size_t> map = {5, 4, 3, 2, 1, 0};
  const auto frames = sonitude::tools::wav_replay::ExtractMappedMicFrames(wav, map);
  Require(frames.size() == 2, "mapped output should contain two frames");
  Require(frames[0][0] == 5.0F && frames[0][5] == 0.0F, "frame 0 channel-map mismatch");
  Require(frames[1][0] == 15.0F && frames[1][5] == 10.0F, "frame 1 channel-map mismatch");

  bool threw = false;
  try
  {
    (void)sonitude::tools::wav_replay::ExtractMappedMicFrames(wav, {0, 1, 2, 3, 4, 8});
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "out-of-range active channel map should throw");
}
}  // namespace

void RunWavReplayTests()
{
  TestMalformedCsvRejected();
  TestFirstEventDelayedUsesNeutralTarget();
  TestEventInsideBlockSplitsProcessing();
  TestNonIdentityChannelMap();
}
