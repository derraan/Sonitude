#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/limiter.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestStereoLimiterLeftOnlyOverload()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.8F, .release_ms = 40.0F}, 48000);
  std::vector<float> left = {0.2F, 1.3F, 0.2F};
  std::vector<float> right = {0.1F, 0.1F, 0.1F};
  limiter.process(left, right);
  Require(std::fabs(left[1]) <= 0.801F, "left-only overload must be clamped");
  Require(std::fabs(right[1]) <= 0.801F, "right must share linked gain on left overload");
  Require(std::fabs((left[1] / right[1]) - (1.3F / 0.1F)) < 1.0e-4F,
          "linked gain must preserve L/R ratio on left overload");
}

void TestStereoLimiterRightOnlyOverload()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.75F, .release_ms = 40.0F}, 48000);
  std::vector<float> left = {0.1F, 0.1F, 0.1F};
  std::vector<float> right = {0.2F, 1.4F, 0.2F};
  limiter.process(left, right);
  Require(std::fabs(right[1]) <= 0.751F, "right-only overload must be clamped");
  Require(std::fabs(left[1]) <= 0.751F, "left must share linked gain on right overload");
  Require(std::fabs((left[1] / right[1]) - (0.1F / 1.4F)) < 1.0e-4F,
          "linked gain must preserve L/R ratio on right overload");
}

void TestStereoLimiterBothEarOverload()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.8F, .release_ms = 40.0F}, 48000);
  std::vector<float> left = {0.2F, 1.1F, 0.2F};
  std::vector<float> right = {0.3F, 1.3F, 0.2F};
  limiter.process(left, right);
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    Require(std::fabs(left[i]) <= 0.801F, "left channel exceeds ceiling on dual overload");
    Require(std::fabs(right[i]) <= 0.801F, "right channel exceeds ceiling on dual overload");
  }
}

void TestStereoLimiterLinkedGainEquality()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.85F, .release_ms = 20.0F}, 48000);
  std::vector<float> left = {0.5F, 2.0F};
  std::vector<float> right = {0.25F, 0.5F};
  const float ratio_before = left[1] / right[1];
  limiter.process(left, right);
  Require(left[0] != 0.0F && right[0] != 0.0F, "non-overload samples must remain non-zero");
  const float ratio_after = left[1] / right[1];
  Require(std::fabs(ratio_before - ratio_after) < 1.0e-4F,
          "linked stereo sample-peak limiter must preserve L/R ratio");
}

void TestStereoLimiterRelease()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.9F, .release_ms = 5.0F}, 1000);
  std::vector<float> left = {2.0F};
  std::vector<float> right = {0.1F};
  limiter.process(left, right);
  const float dipped_gain = limiter.currentGain();
  Require(dipped_gain < 1.0F, "linked limiter should reduce gain during overload");
  left = {0.1F};
  right = {0.1F};
  for (int i = 0; i < 200; ++i)
  {
    limiter.process(left, right);
  }
  Require(limiter.currentGain() > dipped_gain, "linked limiter should release toward unity");
}

void TestStereoLimiterSanitizesNonFiniteInput()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.9F, .release_ms = 20.0F}, 16000);
  std::vector<float> left = {std::numeric_limits<float>::quiet_NaN(), 0.2F};
  std::vector<float> right = {std::numeric_limits<float>::infinity(), -0.2F};
  limiter.process(left, right);
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    Require(std::isfinite(left[i]), "left must remain finite");
    Require(std::isfinite(right[i]), "right must remain finite");
  }
}
}  // namespace

void RunStereoLimiterTests()
{
  TestStereoLimiterLeftOnlyOverload();
  TestStereoLimiterRightOnlyOverload();
  TestStereoLimiterBothEarOverload();
  TestStereoLimiterLinkedGainEquality();
  TestStereoLimiterRelease();
  TestStereoLimiterSanitizesNonFiniteInput();
}
