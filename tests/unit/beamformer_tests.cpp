#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/beamformer.hpp"
#include "tests/support/synth_signals.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

sonitude::app::GeometryConfig BuildGeometry()
{
  sonitude::app::GeometryConfig g;
  g.profile_name = "unit_test_geometry";
  g.microphones = {
      {"M0", -0.038, 0.168, 0.0}, {"M1", 0.038, 0.168, 0.0}, {"M2", -0.090, 0.050, 0.0},
      {"M3", 0.090, 0.050, 0.0},  {"M4", -0.060, 0.000, 0.0}, {"M5", 0.060, 0.000, 0.0},
  };
  return g;
}

sonitude::app::CalibrationConfig BuildCalibration(const std::vector<float>& delays = {})
{
  sonitude::app::CalibrationConfig c;
  c.sample_rate_hz = 16000;
  c.channels.resize(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < c.channels.size(); ++i)
  {
    c.channels[i].id = "M" + std::to_string(i);
    c.channels[i].polarity = 1;
    c.channels[i].gain_linear = 1.0F;
    c.channels[i].delay_samples = delays.empty() ? 0.0F : delays[i];
  }
  return c;
}

sonitude::app::SteeringConfig BuildSteering()
{
  sonitude::app::SteeringConfig s;
  s.speed_of_sound_mps = 343.0F;
  s.reference_mic_index = 0;
  s.steering_ramp_ms = 100.0F;
  s.ambient_floor_linear = 0.25F;
  return s;
}

void TestAlignmentBeatsOffAxis()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 4096;
  const auto geometry = BuildGeometry();
  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 850.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 25.0F, 0.0F, 343.0F);

  sonitude::dsp::DelaySumBeamformer on_axis;
  on_axis.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  on_axis.setTarget({25.0F, 0.0F});
  std::vector<float> on(kFrames, 0.0F);
  on_axis.process(mic, on);

  sonitude::dsp::DelaySumBeamformer off_axis;
  off_axis.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  off_axis.setTarget({-65.0F, 0.0F});
  std::vector<float> off(kFrames, 0.0F);
  off_axis.process(mic, off);

  const double on_rms = sonitude::tests::support::ComputeRms(on, 512);
  const double off_rms = sonitude::tests::support::ComputeRms(off, 512);
  const double source_rms = sonitude::tests::support::ComputeRms(source, 512);
  Require(on_rms > off_rms * 1.2, "on-axis beam energy must exceed off-axis case");
  Require(std::fabs(on_rms - source_rms) < (source_rms * 0.15),
          "on-axis coherent beam output should stay close to source RMS");
}

void TestClickFreeRetarget()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 5000;
  const auto geometry = BuildGeometry();
  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 600.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);

  sonitude::dsp::DelaySumBeamformer beam;
  beam.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  beam.setTarget({-70.0F, 0.0F});

  std::vector<float> out(kFrames, 0.0F);
  for (std::size_t i = 0; i < kFrames; i += 100)
  {
    if (i == 1500)
    {
      beam.setTarget({30.0F, 0.0F});
    }
    if (i == 3000)
    {
      beam.setTarget({80.0F, 0.0F});
    }
    const std::size_t count = std::min<std::size_t>(100, kFrames - i);
    beam.process(std::span<const sonitude::audio::MicFrame>(mic.data() + i, count),
                 std::span<float>(out.data() + i, count));
  }

  const double jump = sonitude::tests::support::MaxSecondDifference(out);
  Require(jump < 0.8, "retargeting introduced click-like discontinuity");
}

void TestCalibrationDelayClosure()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 4096;
  const auto geometry = BuildGeometry();
  std::vector<float> source(kFrames, 0.0F);
  for (std::size_t i = 0; i < kFrames; ++i)
  {
    source[i] = static_cast<float>(0.55 * std::sin(2.0 * sonitude::spatial::kPi * 430.0 *
                                                   static_cast<double>(i) / static_cast<double>(kFs)) +
                                   0.35 * std::sin(2.0 * sonitude::spatial::kPi * 910.0 *
                                                   static_cast<double>(i) / static_cast<double>(kFs)) +
                                   0.10 * std::sin(2.0 * sonitude::spatial::kPi * 1370.0 *
                                                   static_cast<double>(i) / static_cast<double>(kFs)));
  }
  const std::vector<float> mismatch = {0.0F, 1.25F, -0.8F, 0.5F, -1.1F, 0.7F};

  auto misaligned = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);
  for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
  {
    std::vector<float> channel_signal(kFrames, 0.0F);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
      channel_signal[i] = misaligned[i][ch];
    }
    for (std::size_t i = 0; i < kFrames; ++i)
    {
      misaligned[i][ch] = sonitude::tests::support::DelayReadLinear(channel_signal, i, mismatch[ch]);
    }
  }

  sonitude::dsp::DelaySumBeamformer no_cal;
  no_cal.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  no_cal.setTarget({0.0F, 0.0F});
  std::vector<float> out_no_cal(kFrames, 0.0F);
  no_cal.process(misaligned, out_no_cal);

  std::vector<float> correction(sonitude::audio::kMicChannels, 0.0F);
  for (std::size_t i = 0; i < correction.size(); ++i)
  {
    correction[i] = -mismatch[i];
  }
  sonitude::dsp::DelaySumBeamformer with_cal;
  with_cal.configure(geometry, BuildSteering(), BuildCalibration(correction), kFs, kFrames);
  with_cal.setTarget({0.0F, 0.0F});
  std::vector<float> out_with_cal(kFrames, 0.0F);
  with_cal.process(misaligned, out_with_cal);

  const auto aligned = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);
  sonitude::dsp::DelaySumBeamformer ideal_beam;
  ideal_beam.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  ideal_beam.setTarget({0.0F, 0.0F});
  std::vector<float> out_ideal(kFrames, 0.0F);
  ideal_beam.process(aligned, out_ideal);

  const double no_cal_rms = sonitude::tests::support::ComputeRms(out_no_cal, 512);
  const double with_cal_rms = sonitude::tests::support::ComputeRms(out_with_cal, 512);
  const double ideal_rms = sonitude::tests::support::ComputeRms(out_ideal, 512);
  const double no_cal_err = std::fabs(no_cal_rms - ideal_rms);
  const double with_cal_err = std::fabs(with_cal_rms - ideal_rms);
  Require(with_cal_err < no_cal_err,
          "calibration delay correction should move beam output toward ideal alignment");
}
}  // namespace

void RunBeamformerTests()
{
  TestAlignmentBeatsOffAxis();
  TestClickFreeRetarget();
  TestCalibrationDelayClosure();
}
