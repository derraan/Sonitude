#include <algorithm>
#include <cmath>
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

void TestLimiterClampsPeaks()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.80F, .release_ms = 50.0F}, 48000);
  std::vector<float> mono = {0.2F, 0.4F, 1.2F, -1.1F, 0.3F};
  limiter.process(mono);

  for (const float sample : mono)
  {
    Require(std::fabs(sample) <= 0.8001F, "limiter must clamp output peaks to ceiling");
  }
}

void TestLimiterRecoversAfterTransient()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.90F, .release_ms = 10.0F}, 1000);

  std::vector<float> impulse = {1.8F};
  limiter.process(impulse);
  const float dipped_gain = limiter.currentGain();
  Require(dipped_gain < 1.0F, "limiter should reduce gain during overload");

  std::vector<float> tail(30, 0.2F);
  limiter.process(tail);
  Require(limiter.currentGain() > dipped_gain, "limiter should release toward unity after overload");
}

void TestLimiterResetRestoresUnity()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.85F, .release_ms = 40.0F}, 16000);
  std::vector<float> block(10, 1.2F);
  limiter.process(block);
  Require(limiter.currentGain() < 1.0F, "limiter should have reduced gain before reset");
  limiter.reset();
  Require(std::fabs(limiter.currentGain() - 1.0F) < 1e-6F, "reset should restore unity gain");
}
}  // namespace

void RunLimiterTests()
{
  TestLimiterClampsPeaks();
  TestLimiterRecoversAfterTransient();
  TestLimiterResetRestoresUnity();
}
