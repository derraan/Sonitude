#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/calibration_estimator.hpp"
#include "app/calibration_writer.hpp"
#include "audio/audio_types.hpp"
#include "dsp/calibration_applier.hpp"
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

std::vector<std::string> GeometryIds()
{
  return {"M0", "M1", "M2", "M3", "M4", "M5"};
}

void TestCalibrationApply()
{
  std::vector<sonitude::app::CalibrationChannel> channels(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < channels.size(); ++i)
  {
    channels[i].id = "M" + std::to_string(i);
    channels[i].polarity = (i == 1) ? -1 : 1;
    channels[i].gain_linear = 1.1F;
    channels[i].dc_offset = 0.01F;
  }
  sonitude::dsp::CalibrationApplier applier(channels, GeometryIds(), 44100, 20.0F);
  sonitude::audio::MicFrame in{};
  in.fill(0.1F);
  const auto out = applier.process(in);
  Require(out[0] > 0.08F, "calibration gain/polarity failed");
  Require(out[1] < -0.08F, "calibration polarity inversion failed");
}

void TestCalibrationMapsById()
{
  const auto ids = GeometryIds();
  std::vector<sonitude::app::CalibrationChannel> channels(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < channels.size(); ++i)
  {
    channels[i].id = ids[channels.size() - 1U - i];
    channels[i].polarity = 1;
    channels[i].gain_linear = static_cast<float>(channels.size() - i);
  }
  sonitude::dsp::CalibrationApplier applier(channels, ids, 44100, 20.0F);
  sonitude::audio::MicFrame in{};
  in.fill(0.1F);
  const auto out = applier.process(in);
  Require(out[0] < out[5], "calibration must map gains by microphone ID, not YAML order");
}

void TestCalibrationRejectsDuplicateIds()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 44100;
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    calibration.channels.push_back({.id = id});
  }
  calibration.channels.back().id = calibration.channels.front().id;
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "calibration validation must reject duplicate microphone IDs");
}

void TestCalibrationRejectsSampleRateMismatch()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 48000;
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    calibration.channels.push_back({.id = id});
  }
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "calibration validation must reject sample-rate mismatch");
}

void TestCalibrationRejectsInvalidPolarity()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 44100;
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    calibration.channels.push_back({.id = id, .polarity = 1});
  }
  calibration.channels[0].polarity = 0;
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "calibration validation must reject invalid polarity");
}

void TestCalibrationRejectsNonFiniteGain()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 44100;
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    calibration.channels.push_back({.id = id, .gain_linear = 1.0F});
  }
  calibration.channels[0].gain_linear = std::numeric_limits<float>::quiet_NaN();
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "calibration validation must reject non-finite gain");
}

void TestCalibrationRejectsUnknownChannel()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 44100;
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    calibration.channels.push_back({.id = id});
  }
  calibration.channels[0].id = "UNKNOWN";
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "calibration validation must reject unknown channel id");
}

void TestCalibrationRejectsBadReference()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 44100;
  calibration.reference.microphone_id = "MISSING";
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    calibration.channels.push_back({.id = id});
  }
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "calibration validation must reject missing reference microphone");
}

void TestDcBlockerCutoff()
{
  std::vector<sonitude::app::CalibrationChannel> channels(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < channels.size(); ++i)
  {
    channels[i].id = "M" + std::to_string(i);
    channels[i].polarity = 1;
    channels[i].gain_linear = 1.0F;
    channels[i].dc_offset = 0.0F;
  }
  sonitude::dsp::CalibrationApplier applier(channels, GeometryIds(), 44100, 20.0F);
  std::vector<sonitude::audio::MicFrame> in(4410);
  std::vector<sonitude::audio::MicFrame> out(4410);
  for (auto& frame : in)
  {
    frame.fill(1.0F);
  }
  applier.processBlock(std::span<const sonitude::audio::MicFrame>(in.data(), in.size()),
                       std::span<sonitude::audio::MicFrame>(out.data(), out.size()));
  Require(std::fabs(out.back()[0]) < 0.1F, "dc blocker should attenuate sustained DC");
}

void TestWriter()
{
  sonitude::app::CalibrationConfig cal;
  cal.schema_version = sonitude::app::kCalibrationSchemaVersion;
  cal.sample_rate_hz = 44100;
  cal.reference.microphone_id = "M0";
  cal.quality.valid = false;
  cal.quality.hardware_evidence = false;
  cal.quality.warnings.push_back("unit test");
  for (std::size_t i = 0; i < sonitude::audio::kMicChannels; ++i)
  {
    sonitude::app::CalibrationChannel c;
    c.id = "M" + std::to_string(i);
    cal.channels.push_back(c);
  }
  sonitude::app::WriteCalibrationYamlBackupSafe("calibration_writer_test.yaml", cal, true);
  const auto loaded = sonitude::app::LoadCalibrationFromFile("calibration_writer_test.yaml");
  Require(loaded.schema_version == sonitude::app::kCalibrationSchemaVersion, "schema version round-trip");
  Require(loaded.reference.microphone_id == "M0", "reference round-trip");
  Require(loaded.quality.warnings.size() == 1U, "quality warnings round-trip");
}

void TestEstimatorUnity()
{
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kSilence = kFs / 2U;
  constexpr std::size_t kSignal = kFs * 2U;
  const std::size_t total = kSilence + kSignal;
  std::vector<float> interleaved(total * 6U, 0.0F);
  for (std::size_t i = 0; i < kSignal; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(kFs);
    const float s = 0.3F * std::sin(2.0F * 3.1415926535F * 900.0F * t);
    for (std::size_t ch = 0; ch < 6U; ++ch)
    {
      interleaved[(kSilence + i) * 6U + ch] = s;
    }
  }

  sonitude::app::CalibrationEstimateOptions options;
  options.channel_ids = GeometryIds();
  options.reference_channel_index = 0;
  options.sample_rate_hz = kFs;
  options.silence_frame_count = kSilence;
  options.signal_start_frame = kSilence;
  options.signal_frame_count = kSignal;

  const auto out = sonitude::app::EstimateCalibrationFromCapture(interleaved, 6, options);
  for (const auto& ch : out.report.channels)
  {
    Require(std::fabs(ch.delay_samples) < 0.5F,
            "unity capture should estimate near-zero relative delay");
    Require(std::fabs(ch.gain_linear - 1.0F) < 0.15F,
            "unity capture should estimate near-unity gain");
  }
}

void TestEstimatorKnownMismatch()
{
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kSilence = kFs / 2U;
  constexpr std::size_t kSignal = kFs * 2U;
  const std::vector<float> known_delays = {0.0F, 1.25F, -0.8F, 0.5F, -1.1F, 0.7F};
  const std::vector<float> known_gains = {1.0F, 0.85F, 1.0F, 1.12F, 0.93F, 1.05F};
  const std::vector<int> known_polarity = {1, -1, 1, 1, 1, 1};

  std::vector<float> mono(kSignal, 0.0F);
  for (std::size_t i = 0; i < kSignal; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(kFs);
    mono[i] = 0.35F * std::sin(2.0F * 3.1415926535F * 700.0F * t) +
              0.25F * std::sin(2.0F * 3.1415926535F * 1200.0F * t);
  }

  const std::size_t total = kSilence + kSignal;
  std::vector<float> interleaved(total * 6U, 0.0F);
  for (std::size_t ch = 0; ch < 6U; ++ch)
  {
    for (std::size_t i = 0; i < kSignal; ++i)
    {
      const float sample = static_cast<float>(known_polarity[ch]) * known_gains[ch] *
                           sonitude::tests::support::DelayReadLinear(mono, i, known_delays[ch]);
      interleaved[(kSilence + i) * 6U + ch] = sample;
    }
  }

  sonitude::app::CalibrationEstimateOptions options;
  options.channel_ids = GeometryIds();
  options.reference_channel_index = 0;
  options.sample_rate_hz = kFs;
  options.silence_frame_count = kSilence;
  options.signal_start_frame = kSilence;
  options.signal_frame_count = kSignal;

  const auto out = sonitude::app::EstimateCalibrationFromCapture(interleaved, 6, options);
  for (std::size_t ch = 1; ch < 6U; ++ch)
  {
    const float expected_delay = -known_delays[ch];
    Require(std::fabs(out.report.channels[ch].delay_samples - expected_delay) < 0.35F,
            "fractional delay should be recovered within tolerance");
    const float expected_gain = 1.0F / known_gains[ch];
    Require(std::fabs(out.report.channels[ch].gain_linear - expected_gain) < 0.2F,
            "relative gain should be recovered within tolerance");
  }
  Require(out.report.channels[1].polarity == -1, "inverted channel polarity should be detected");
}

void TestEstimatorLowSignalIsUnresolved()
{
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kFrames = 4096;
  std::vector<float> interleaved(kFrames * 6U, 0.0F);

  sonitude::app::CalibrationEstimateOptions options;
  options.channel_ids = GeometryIds();
  options.reference_channel_index = 0;
  options.sample_rate_hz = kFs;
  options.signal_start_frame = 0;
  options.signal_frame_count = kFrames;
  options.min_signal_rms = 1.0e-4F;

  const auto out = sonitude::app::EstimateCalibrationFromCapture(interleaved, 6, options);
  Require(out.report.overall_status == sonitude::app::CalibrationQualityStatus::Unresolved,
          "silent capture should fail closed as unresolved");
}

void TestCalibrationEqValidation()
{
  sonitude::app::CalibrationConfig calibration;
  calibration.sample_rate_hz = 44100;
  const auto ids = GeometryIds();
  for (const auto& id : ids)
  {
    sonitude::app::CalibrationChannel ch;
    ch.id = id;
    sonitude::app::CalibrationChannel::EqSection sec;
    sec.type = "PK";
    sec.freq_hz = 2000.0F;
    sec.gain_db = 2.0F;
    sec.q = 1.2F;
    ch.eq.enabled = true;
    ch.eq.sections.push_back(sec);
    calibration.channels.push_back(ch);
  }
  sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);

  calibration.channels[0].eq.sections[0].type = "BAD";
  bool rejected = false;
  try
  {
    sonitude::app::ValidateCalibrationConfig(calibration, ids, 44100);
  }
  catch (const std::runtime_error&)
  {
    rejected = true;
  }
  Require(rejected, "unknown EQ type must be rejected");
}
}  // namespace

void RunCalibrationTests()
{
  TestCalibrationApply();
  TestCalibrationMapsById();
  TestCalibrationRejectsDuplicateIds();
  TestCalibrationRejectsSampleRateMismatch();
  TestCalibrationRejectsInvalidPolarity();
  TestCalibrationRejectsNonFiniteGain();
  TestCalibrationRejectsUnknownChannel();
  TestCalibrationRejectsBadReference();
  TestDcBlockerCutoff();
  TestWriter();
  TestCalibrationEqValidation();
  TestEstimatorUnity();
  TestEstimatorKnownMismatch();
  TestEstimatorLowSignalIsUnresolved();
}
