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

void TestStereoLimiterClampsLinkedPeaks()
{
  sonitude::dsp::StereoPeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.8F, .release_ms = 40.0F}, 48000);
  std::vector<float> left = {0.2F, 1.3F, 0.2F};
  std::vector<float> right = {0.3F, 0.1F, 0.2F};
  limiter.process(left, right);
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    Require(std::fabs(left[i]) <= 0.801F, "left channel exceeds ceiling");
    Require(std::fabs(right[i]) <= 0.801F, "right channel exceeds ceiling");
  }
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
  TestStereoLimiterClampsLinkedPeaks();
  TestStereoLimiterSanitizesNonFiniteInput();
}
