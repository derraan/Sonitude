#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "tools/calibration_estimate_support.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

float DeterministicNoise(const std::size_t i)
{
  std::uint32_t x = static_cast<std::uint32_t>((i * 1103515245ULL + 12345ULL) & 0x7fffffffULL);
  return (static_cast<float>(x) / 1073741824.0F) - 1.0F;
}

std::vector<float> MakeReferenceSignal(const std::size_t frames, const float sample_rate_hz)
{
  std::vector<float> out(frames, 0.0F);
  for (std::size_t i = 0; i < frames; ++i)
  {
    const float t = static_cast<float>(i) / sample_rate_hz;
    out[i] = (0.15F * std::sin(2.0F * 3.1415926535F * 300.0F * t)) +
             (0.10F * std::sin(2.0F * 3.1415926535F * 931.0F * t)) +
             (0.75F * DeterministicNoise(i));
  }
  return out;
}

float SampleLinear(const std::vector<float>& signal, const float index)
{
  const float n = static_cast<float>(signal.size());
  float wrapped = std::fmod(index, n);
  if (wrapped < 0.0F)
  {
    wrapped += n;
  }
  const std::size_t i0 = static_cast<std::size_t>(wrapped);
  const std::size_t i1 = (i0 + 1U) % signal.size();
  const float frac = wrapped - static_cast<float>(i0);
  return ((1.0F - frac) * signal[i0]) + (frac * signal[i1]);
}

std::vector<float> MakeDerivedChannel(const std::vector<float>& reference,
                                      const float delay_samples, const int polarity,
                                      const float gain, const float dc_offset,
                                      const float noise_gain)
{
  std::vector<float> out(reference.size(), 0.0F);
  for (std::size_t i = 0; i < reference.size(); ++i)
  {
    const float delayed = SampleLinear(reference, static_cast<float>(i) + delay_samples);
    out[i] = static_cast<float>(polarity) * gain * delayed + dc_offset +
             (noise_gain * DeterministicNoise(i + 7U));
  }
  return out;
}

void TestPositiveDelayWithDcAndGain()
{
  const auto ref = MakeReferenceSignal(4096, 44100.0F);
  const auto ch = MakeDerivedChannel(ref, 12.50F, 1, 0.35F, 0.12F, 0.005F);

  const auto estimate = sonitude::tools::calibration::EstimateDelayAndPolarity(ref, ch, 128);
  Require(std::fabs(estimate.delay_samples - 12.50F) < 0.75F,
          "positive delay estimate outside tolerance (got " +
              std::to_string(estimate.delay_samples) + ")");
  Require(estimate.polarity == 1, "polarity should remain positive");
}

void TestRelativeGainRecovery()
{
  std::vector<float> ref(2048, 0.0F);
  std::vector<float> ch(2048, 0.0F);
  for (std::size_t i = 0; i < ref.size(); ++i)
  {
    const float t = static_cast<float>(i) / 44100.0F;
    ref[i] = std::sin(2.0F * 3.1415926535F * 440.0F * t);
    ch[i] = (0.5F * ref[i]) + 0.2F;
  }
  const auto ref_moments = sonitude::tools::calibration::ComputeChannelMoments(ref);
  const auto ch_moments = sonitude::tools::calibration::ComputeChannelMoments(ch);
  const float recovered_gain = ref_moments.rms / ch_moments.rms;
  Require(std::fabs(recovered_gain - 2.0F) < 0.05F,
          "relative gain recovery outside tolerance (got " + std::to_string(recovered_gain) + ")");
}

void TestNegativeDelayAndPolarityInversion()
{
  const auto ref = MakeReferenceSignal(4096, 44100.0F);
  const auto ch = MakeDerivedChannel(ref, -9.25F, -1, 0.80F, -0.07F, 0.004F);

  const auto estimate = sonitude::tools::calibration::EstimateDelayAndPolarity(ref, ch, 128);
  Require(std::fabs(estimate.delay_samples - (-9.25F)) < 0.85F,
          "negative delay estimate outside tolerance (got " +
              std::to_string(estimate.delay_samples) + ")");
  Require(estimate.polarity == -1, "polarity inversion should be detected");
}

void TestNearZeroEnergyRejected()
{
  std::vector<float> silence(2048, 1e-10F);
  bool threw = false;
  try
  {
    (void)sonitude::tools::calibration::EstimateDelayAndPolarity(silence, silence, 16);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "near-zero energy input should be rejected");
}

void TestMinimumCorrelationPassingAndBoundary()
{
  sonitude::tools::calibration::RequireMinimumCorrelation("M1", 0.91F, 0.90F);
  sonitude::tools::calibration::RequireMinimumCorrelation("M2", 0.75F, 0.75F);
  Require(sonitude::tools::calibration::ParseMinimumCorrelation("0") == 0.0F,
          "zero correlation threshold should be accepted");
  Require(sonitude::tools::calibration::ParseMinimumCorrelation("1") == 1.0F,
          "one correlation threshold should be accepted");
}

void TestMinimumCorrelationRejectionReportsValues()
{
  bool threw = false;
  try
  {
    sonitude::tools::calibration::RequireMinimumCorrelation("M3", 0.49F, 0.50F);
  }
  catch (const std::exception& ex)
  {
    const std::string message(ex.what());
    threw = message.find("channel=M3") != std::string::npos &&
            message.find("measured=0.49") != std::string::npos &&
            message.find("required=0.5") != std::string::npos;
  }
  Require(threw, "correlation rejection should report channel, measured, and required values");
}

void TestInvalidMinimumCorrelationsRejected()
{
  const std::vector<std::string> invalid = {"-0.01", "1.01", "nan",         "NaN",
                                            "inf",   "-inf", "0.5trailing", ""};
  for (const auto& value : invalid)
  {
    bool threw = false;
    try
    {
      (void)sonitude::tools::calibration::ParseMinimumCorrelation(value);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    Require(threw, "invalid minimum correlation should be rejected: " + value);
  }

  bool threw = false;
  try
  {
    sonitude::tools::calibration::RequireMinimumCorrelation(
        "M4", std::numeric_limits<float>::quiet_NaN(), 0.5F);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "non-finite measured correlation should be rejected");
}
} // namespace

void RunCalibrationEstimateTests()
{
  TestPositiveDelayWithDcAndGain();
  TestRelativeGainRecovery();
  TestNegativeDelayAndPolarityInversion();
  TestNearZeroEnergyRejected();
  TestMinimumCorrelationPassingAndBoundary();
  TestMinimumCorrelationRejectionReportsValues();
  TestInvalidMinimumCorrelationsRejected();
}
