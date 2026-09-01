#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/spectral_postfilter.hpp"
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
  s.model = "far_field";
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

  sonitude::dsp::MvdrBeamformer on_axis;
  on_axis.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  on_axis.setTarget({25.0F, 0.0F});
  std::vector<float> on(kFrames, 0.0F);
  on_axis.process(mic, on);

  sonitude::dsp::MvdrBeamformer off_axis;
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

  sonitude::dsp::MvdrBeamformer beam;
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

void TestRepeatedIdenticalSetTargetSettles()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr float kTransitionMs = 150.0F;
  const std::size_t ramp =
      std::max<std::size_t>(1U, static_cast<std::size_t>((kTransitionMs * 0.001F) * kFs));
  const std::size_t frames = ramp + 512;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering();
  steering.steering_ramp_ms = kTransitionMs;
  const auto source = sonitude::tests::support::GenerateSine(frames, kFs, 600.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);

  sonitude::dsp::MvdrBeamformer settled;
  settled.configure(geometry, steering, BuildCalibration(), kFs, 256);
  settled.setTarget({45.0F, 0.0F});
  std::vector<float> settled_out(frames, 0.0F);
  settled.process(std::span<const sonitude::audio::MicFrame>(mic.data(), frames),
                  std::span<float>(settled_out.data(), frames));

  sonitude::dsp::MvdrBeamformer live;
  live.configure(geometry, steering, BuildCalibration(), kFs, 256);
  std::vector<float> live_out(frames, 0.0F);
  for (std::size_t start = 0; start < frames; start += 256)
  {
    live.setTarget({45.0F, 0.0F});
    const std::size_t count = std::min<std::size_t>(256, frames - start);
    live.process(std::span<const sonitude::audio::MicFrame>(mic.data() + start, count),
                 std::span<float>(live_out.data() + start, count));
  }
  for (std::size_t i = ramp; i < frames; ++i)
  {
    Require(std::fabs(live_out[i] - settled_out[i]) < 1e-5F,
            "repeated identical setTarget must not restart the beamformer crossfade");
  }
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

  sonitude::dsp::MvdrBeamformer no_cal;
  no_cal.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  no_cal.setTarget({0.0F, 0.0F});
  std::vector<float> out_no_cal(kFrames, 0.0F);
  no_cal.process(misaligned, out_no_cal);

  std::vector<float> correction(sonitude::audio::kMicChannels, 0.0F);
  for (std::size_t i = 0; i < correction.size(); ++i)
  {
    correction[i] = -mismatch[i];
  }
  sonitude::dsp::MvdrBeamformer with_cal;
  with_cal.configure(geometry, BuildSteering(), BuildCalibration(correction), kFs, kFrames);
  with_cal.setTarget({0.0F, 0.0F});
  std::vector<float> out_with_cal(kFrames, 0.0F);
  with_cal.process(misaligned, out_with_cal);

  const auto aligned = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);
  sonitude::dsp::MvdrBeamformer ideal_beam;
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

void TestLeftRightAzimuthConvention()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 4096;
  const auto geometry = BuildGeometry();
  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 700.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, -90.0F, 0.0F, 343.0F);

  // In the head frame, azimuth -90 deg is listener-left and should arrive at
  // the left-side microphone before the right-side microphone.
  double lead_sum = 0.0;
  for (std::size_t i = 1; i < kFrames; ++i)
  {
    lead_sum += static_cast<double>(mic[i][4]) * static_cast<double>(mic[i - 1][5]);
    lead_sum -= static_cast<double>(mic[i][5]) * static_cast<double>(mic[i - 1][4]);
  }
  Require(lead_sum > 0.0, "left-side source should lead at left ear channel");

  sonitude::dsp::MvdrBeamformer left_steer;
  left_steer.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  left_steer.setTarget({-90.0F, 0.0F});
  std::vector<float> out_left(kFrames, 0.0F);
  left_steer.process(mic, out_left);

  sonitude::dsp::MvdrBeamformer right_steer;
  right_steer.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  right_steer.setTarget({90.0F, 0.0F});
  std::vector<float> out_right(kFrames, 0.0F);
  right_steer.process(mic, out_right);

  const double left_rms = sonitude::tests::support::ComputeRms(out_left, 512);
  const double right_rms = sonitude::tests::support::ComputeRms(out_right, 512);
  Require(left_rms > right_rms * 1.2, "listener-left steering should beat listener-right steering");
}

void TestSpectralSharesSingleStftDelay()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 2048;
  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(BuildGeometry(), BuildSteering(), BuildCalibration(), kFs, kFrames);
  Require(bf.algorithmicDelaySamples() == 127, "MVDR first-arrival is one 128/32 STFT");

  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(static_cast<double>(kFs), kFrames, {.enabled = true, .gain_floor_db = -12.0F}),
          "shared-hop postfilter prepare");
  bf.setSpectralPostfilter(&pf);
  Require(bf.algorithmicDelaySamples() == 127,
          "attaching spectral NS must not add a second STFT delay");

  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 700.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, BuildGeometry(), kFs, 0, 0.0F, 0.0F, 343.0F);
  std::vector<float> out(kFrames, 0.0F);
  bf.setTarget({0.0F, 0.0F});
  pf.setControl(true, 1.0F);
  bf.process(mic, out);
  Require(std::all_of(out.begin(), out.end(), [](const float v) { return std::isfinite(v); }),
          "shared-hop spectral MVDR output must stay finite");
}

void TestMvdrNullsOffAxisInterferer()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 8192;
  const auto geometry = BuildGeometry();
  const auto target_src = sonitude::tests::support::GenerateSine(kFrames, kFs, 700.0);
  const auto interf_src = sonitude::tests::support::GenerateSine(kFrames, kFs, 1100.0);
  auto mic = sonitude::tests::support::GeneratePlaneWave(
      target_src, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);
  const auto interf = sonitude::tests::support::GeneratePlaneWave(
      interf_src, geometry, kFs, 0, 90.0F, 0.0F, 343.0F);
  for (std::size_t i = 0; i < kFrames; ++i)
  {
    for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
    {
      mic[i][ch] += interf[i][ch];
    }
  }

  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  bf.setTarget({0.0F, 0.0F});
  std::vector<float> out(kFrames, 0.0F);
  bf.process(mic, out);

  const double out_rms = sonitude::tests::support::ComputeRms(out, 1024);
  const double target_rms = sonitude::tests::support::ComputeRms(target_src, 1024);
  const double mix_ref = sonitude::tests::support::ComputeRms(interf_src, 1024);
  Require(out_rms < (target_rms + mix_ref) * 0.85,
          "MVDR target look should suppress some off-axis interferer energy");
  Require(out_rms > target_rms * 0.4, "MVDR should not cancel the look direction");
}
}  // namespace

void RunBeamformerTests()
{
  TestAlignmentBeatsOffAxis();
  TestClickFreeRetarget();
  TestRepeatedIdenticalSetTargetSettles();
  TestCalibrationDelayClosure();
  TestLeftRightAzimuthConvention();
  TestSpectralSharesSingleStftDelay();
  TestMvdrNullsOffAxisInterferer();
}
