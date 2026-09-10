#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/steering_lut.hpp"
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
  s.model = "near_field";
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
void TestCovarianceAdaptsDuringLongSteeringTransition()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr float kTransitionMs = 400.0F;
  constexpr std::size_t kHopSize = 32;
  const std::size_t ramp =
      std::max<std::size_t>(1U, static_cast<std::size_t>((kTransitionMs * 0.001F) * kFs));
  const std::size_t warmup = 512;
  const std::size_t frames = warmup + ramp + 256;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering();
  steering.steering_ramp_ms = kTransitionMs;
  const auto source = sonitude::tests::support::GenerateSine(frames, kFs, 720.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);

  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, steering, BuildCalibration(), kFs, 256);
  bf.setTarget({0.0F, 0.0F});
  std::vector<float> out(frames, 0.0F);
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data(), warmup),
             std::span<float>(out.data(), warmup));

  bf.resetCovarianceDiagnosticsForTest();
  bf.setTarget({35.0F, 0.0F});
  Require(bf.crossfadingForTest(), "setTarget must enter steering crossfade");
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + warmup, ramp),
             std::span<float>(out.data() + warmup, ramp));

  Require(bf.covarianceUpdateHopsForTest() >= (ramp / kHopSize) / 2U,
          "covariance must keep adapting through a long steering crossfade");
}

void TestRepeatedTargetUpdatesDoNotStarveAdaptation()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr float kTransitionMs = 300.0F;
  constexpr std::size_t kRetargetInterval = 64;
  const std::size_t frames = 8192;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering();
  steering.steering_ramp_ms = kTransitionMs;
  const auto source = sonitude::tests::support::GenerateSine(frames, kFs, 680.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);

  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, steering, BuildCalibration(), kFs, 256);
  bf.setTarget({0.0F, 0.0F});
  std::vector<float> out(frames, 0.0F);
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data(), 512),
             std::span<float>(out.data(), 512));

  bf.resetCovarianceDiagnosticsForTest();
  float azimuth = 10.0F;
  for (std::size_t start = 512; start < frames; start += kRetargetInterval)
  {
    bf.setTarget({azimuth, 0.0F});
    azimuth += 12.0F;
    const std::size_t count = std::min(kRetargetInterval, frames - start);
    bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + start, count),
               std::span<float>(out.data() + start, count));
  }

  Require(bf.covarianceUpdateHopsForTest() >= (frames - 512) / 64U,
          "repeated steering retargets must not freeze covariance adaptation");
}

void TestMovingNoiseStatisticsUpdateDuringCrossfade()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 6144;
  constexpr float kTransitionMs = 250.0F;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering();
  steering.steering_ramp_ms = kTransitionMs;
  sonitude::dsp::MvdrTuningParams tuning{};
  tuning.cov_tau_sec = 0.020F;

  const auto target_src = sonitude::tests::support::GenerateSine(kFrames, kFs, 700.0);
  const auto early_noise = sonitude::tests::support::GenerateSine(kFrames, kFs, 950.0);
  const auto late_noise = sonitude::tests::support::GenerateSine(kFrames, kFs, 1250.0);
  auto mic = sonitude::tests::support::GeneratePlaneWave(
      target_src, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);
  const auto early_interf = sonitude::tests::support::GeneratePlaneWave(
      early_noise, geometry, kFs, 0, 90.0F, 0.0F, 343.0F);
  const auto late_interf = sonitude::tests::support::GeneratePlaneWave(
      late_noise, geometry, kFs, 0, -75.0F, 0.0F, 343.0F);
  const std::size_t scene_switch = 2048;
  for (std::size_t i = 0; i < kFrames; ++i)
  {
    const auto& noise = (i < scene_switch) ? early_interf : late_interf;
    for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
    {
      mic[i][ch] += noise[i][ch];
    }
  }

  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, steering, BuildCalibration(), kFs, 256);
  bf.setTuning(tuning);
  bf.setTarget({0.0F, 0.0F});
  std::vector<float> out(kFrames, 0.0F);
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data(), scene_switch),
             std::span<float>(out.data(), scene_switch));

  bf.resetCovarianceDiagnosticsForTest();
  bf.setTarget({40.0F, 0.0F});
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + scene_switch, kFrames - scene_switch),
             std::span<float>(out.data() + scene_switch, kFrames - scene_switch));

  Require(bf.covarianceUpdateHopsForTest() > 0U,
          "moving interferer statistics must update while output steering crossfades");

  const double out_rms = sonitude::tests::support::ComputeRms(out, 1024);
  const double target_rms = sonitude::tests::support::ComputeRms(target_src, 1024);
  const double late_noise_rms = sonitude::tests::support::ComputeRms(late_noise, 1024);
  Require(out_rms < (target_rms + late_noise_rms) * 0.90,
          "MVDR should track scene changes that occur during steering transition");
  Require(out_rms > target_rms * 0.35, "MVDR should retain on-axis target energy");
}

void TestNearFieldElevationSteering()
{
  constexpr std::uint32_t kFs = 16000;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering();
  steering.model = "near_field";
  steering.source_distance_m = 0.45F;

  sonitude::dsp::KemarSteeringLut model;
  model.configure(geometry, steering, kFs);
  const auto elevated =
      model.computeNearFieldDelays({30.0F, 17.5F}, steering.reference_mic_index);
  const auto horizontal =
      model.computeNearFieldDelays({30.0F, 0.0F}, steering.reference_mic_index);
  bool differs = false;
  for (std::size_t m = 0; m < sonitude::audio::kMicChannels; ++m)
  {
    if (std::fabs(elevated[m] - horizontal[m]) > 1.0e-6)
    {
      differs = true;
      break;
    }
  }
  Require(differs, "near-field steering must use requested elevation in delay model");

  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, steering, BuildCalibration(), kFs, 256);
  bf.setTarget({30.0F, 17.5F});
  Require(std::fabs(bf.pendingTargetForTest().elevation_deg - 17.5F) < 1.0e-3F,
          "beamformer must preserve requested elevation target");
}


void TestKemarLutFlagDoesNotAffectArraySteering()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 4096;
  const auto geometry = BuildGeometry();
  auto steering_off = BuildSteering();
  steering_off.model = "near_field";
  steering_off.source_distance_m = 0.45F;
  steering_off.kemar_lut.enabled = false;
  auto steering_on = steering_off;
  steering_on.kemar_lut.enabled = true;
  steering_on.kemar_lut.table_path = "unused.shrf";
  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 850.0);
  const auto mic = sonitude::tests::support::GenerateSphericalPointSource(
      source, geometry, kFs, 0, 30.0F, 17.5F, steering_off.source_distance_m, steering_off.speed_of_sound_mps);
  sonitude::dsp::MvdrBeamformer without_lut;
  without_lut.configure(geometry, steering_off, BuildCalibration(), kFs, kFrames);
  without_lut.setTarget({30.0F, 17.5F});
  std::vector<float> out_off(kFrames, 0.0F);
  without_lut.process(mic, out_off);
  sonitude::dsp::MvdrBeamformer with_lut_flag;
  with_lut_flag.configure(geometry, steering_on, BuildCalibration(), kFs, kFrames);
  with_lut_flag.setTarget({30.0F, 17.5F});
  std::vector<float> out_on(kFrames, 0.0F);
  with_lut_flag.process(mic, out_on);
  double max_diff = 0.0;
  for (std::size_t i = 512; i < kFrames; ++i)
  {
    max_diff = std::max(max_diff, std::fabs(static_cast<double>(out_off[i] - out_on[i])));
  }
  Require(max_diff < 1.0e-5, "kemar_lut flag must not change analytic array steering output");
}

void TestSteeringTransitionStateMachine()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr float kTransitionMs = 200.0F;
  const std::size_t ramp =
      std::max<std::size_t>(1U, static_cast<std::size_t>((kTransitionMs * 0.001F) * kFs));
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering();
  steering.steering_ramp_ms = kTransitionMs;
  const auto source = sonitude::tests::support::GenerateSine(ramp * 3U, kFs, 620.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);

  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, steering, BuildCalibration(), kFs, 256);
  bf.setTarget({0.0F, 0.0F});
  std::vector<float> out(ramp * 3U, 0.0F);
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data(), ramp / 2U),
             std::span<float>(out.data(), ramp / 2U));
  Require(!bf.crossfadingForTest(), "identical initial target must not start a crossfade");

  bf.setTarget({40.0F, 0.0F});
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + ramp / 2U, ramp / 4U),
             std::span<float>(out.data() + ramp / 2U, ramp / 4U));
  Require(bf.crossfadingForTest(), "new target must start crossfade");
  const std::size_t cursor_mid = bf.fadeCursorForTest();
  Require(cursor_mid > 0U && cursor_mid < ramp, "fade cursor must advance during transition");

  bf.setTarget({40.05F, 0.0F});
  Require(bf.fadeCursorForTest() == cursor_mid,
          "sub-deadband jitter must not restart the steering crossfade");

  bf.setTarget({40.0F, 0.0F});
  Require(bf.fadeCursorForTest() == cursor_mid,
          "repeated identical pending target must not restart the crossfade");

  const std::size_t retarget_at = ramp / 2U + ramp / 4U;
  const float sample_before = out[retarget_at > 0U ? retarget_at - 1U : 0U];
  const std::size_t cursor_before_retarget = bf.fadeCursorForTest();
  bf.setTarget({-25.0F, 0.0F});
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + retarget_at, 1U),
             std::span<float>(out.data() + retarget_at, 1U));
  Require(bf.fadeCursorForTest() > 0U && bf.fadeCursorForTest() < ramp,
          "mid-fade retarget must preserve the inverted crossfade progress");
  Require(bf.fadeCursorForTest() != cursor_before_retarget,
          "mid-fade retarget must pivot crossfade progress");
  Require(std::fabs(bf.activeTargetForTest().azimuth_deg - 40.0F) < 1.0e-3F,
          "active target must track the audible blend start after pivot");
  Require(std::fabs(bf.pendingTargetForTest().azimuth_deg + 25.0F) < 1.0e-3F,
          "pending target must reflect the latest command");
  Require(std::fabs(out[retarget_at] - sample_before) < 0.35F,
          "mid-fade retarget must continue from the current acoustic state");

  const std::array<float, 4> rapid_az = {10.0F, 28.0F, 52.0F, 70.0F};
  std::size_t cursor = 0;
  for (const float az : rapid_az)
  {
    bf.setTarget({az, 0.0F});
    const std::size_t chunk = std::min<std::size_t>(ramp / 8U, out.size() - cursor);
    bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + cursor, chunk),
               std::span<float>(out.data() + cursor, chunk));
    cursor += chunk;
  }
  Require(sonitude::tests::support::MaxSecondDifference(out) < 0.8,
          "rapid steering changes must stay click-free");

  bf.setTarget({179.8F, 0.0F});
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data(), ramp / 4U),
             std::span<float>(out.data(), ramp / 4U));
  const std::size_t wrap_cursor = bf.fadeCursorForTest();
  bf.setTarget({-179.9F, 0.0F});
  Require(bf.fadeCursorForTest() == wrap_cursor,
          "180 wraparound within deadband must not restart crossfade");

  bf.setTarget({179.8F, 12.0F});
  bf.process(std::span<const sonitude::audio::MicFrame>(mic.data() + ramp / 4U, ramp / 4U),
             std::span<float>(out.data() + ramp / 4U, ramp / 4U));
  Require(std::fabs(bf.pendingTargetForTest().elevation_deg - 12.0F) < 1.0e-3F,
          "elevation changes must update the pending steering target");
  Require(bf.crossfadingForTest(), "elevation retarget must keep the crossfade active");
}

void TestResetRestoresCovarianceFloor()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 2048;
  const auto geometry = BuildGeometry();
  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 700.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);

  sonitude::dsp::MvdrBeamformer fresh;
  fresh.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  std::vector<float> a(kFrames, 0.0F);
  fresh.process(mic, a);

  sonitude::dsp::MvdrBeamformer warmed;
  warmed.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  std::vector<float> discard(kFrames, 0.0F);
  warmed.process(mic, discard);
  warmed.resetStream();
  std::vector<float> b(kFrames, 0.0F);
  warmed.process(mic, b);
  double err = 0.0;
  for (std::size_t i = 256; i < kFrames; ++i)
  {
    err += static_cast<double>(a[i] - b[i]) * static_cast<double>(a[i] - b[i]);
  }
  err = std::sqrt(err / static_cast<double>(kFrames - 256));
  Require(err < 1.0e-5, "reset must restore covariance floor and match a fresh configure");
}

void TestSingleFactorizationPerBin()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 1024;
  const auto geometry = BuildGeometry();
  const auto source = sonitude::tests::support::GenerateSine(kFrames, kFs, 900.0);
  const auto mic = sonitude::tests::support::GeneratePlaneWave(
      source, geometry, kFs, 0, 15.0F, 0.0F, 343.0F);
  sonitude::dsp::MvdrBeamformer bf;
  bf.configure(geometry, BuildSteering(), BuildCalibration(), kFs, kFrames);
  bf.resetCovarianceDiagnosticsForTest();
  std::vector<float> out(kFrames, 0.0F);
  bf.process(mic, out);
  const std::uint64_t hops = bf.covarianceUpdateHopsForTest();
  Require(hops > 0, "processing must update covariance");
  Require(bf.factorizationCountForTest() == hops * 63U,
          "adaptive path must factor once per interior bin per hop");
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
  TestCovarianceAdaptsDuringLongSteeringTransition();
  TestRepeatedTargetUpdatesDoNotStarveAdaptation();
  TestMovingNoiseStatisticsUpdateDuringCrossfade();
  TestNearFieldElevationSteering();
  TestKemarLutFlagDoesNotAffectArraySteering();
  TestSteeringTransitionStateMachine();
  TestResetRestoresCovarianceFloor();
  TestSingleFactorizationPerBin();
}
