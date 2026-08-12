#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/suppressor.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

std::vector<float> ConstantBlock(const std::size_t frames, const float value)
{
  return std::vector<float>(frames, value);
}

void TestFloorClampAndAttenuation()
{
  sonitude::dsp::ConservativeSuppressor suppressor;
  suppressor.configure(
      {.ambient_floor_linear = 0.25F,
       .fade_ms = 10.0F,
       .activity_threshold = 0.01F,
       .confidence_threshold = 0.6F},
      1000);
  suppressor.setControl(true, 1.0F);
  auto mono = ConstantBlock(80, 1.0F);
  suppressor.process(mono);

  const float gain = suppressor.currentGain();
  Require(gain <= 0.30F, "suppressor should converge near configured floor");
  Require(gain >= 0.25F, "suppressor gain must not dip below floor");
  const float min_sample = *std::min_element(mono.begin(), mono.end());
  Require(min_sample >= 0.24F, "output samples should respect ambient floor");
}

void TestFallbackRampToUnity()
{
  sonitude::dsp::ConservativeSuppressor suppressor;
  suppressor.configure(
      {.ambient_floor_linear = 0.25F,
       .fade_ms = 20.0F,
       .activity_threshold = 0.01F,
       .confidence_threshold = 0.6F},
      1000);

  suppressor.setControl(true, 1.0F);
  auto focused = ConstantBlock(100, 0.9F);
  suppressor.process(focused);
  Require(suppressor.currentGain() < 0.50F, "focused state should attenuate");

  suppressor.setControl(false, 0.0F);
  auto fallback = ConstantBlock(120, 0.9F);
  suppressor.process(fallback);
  Require(suppressor.currentGain() > 0.95F, "failsafe fallback should ramp gain to unity");
}

void TestLowConfidenceBypassesSuppression()
{
  sonitude::dsp::ConservativeSuppressor suppressor;
  suppressor.configure(
      {.ambient_floor_linear = 0.30F,
       .fade_ms = 10.0F,
       .activity_threshold = 0.01F,
       .confidence_threshold = 0.8F},
      1000);

  suppressor.setControl(true, 0.2F);
  auto mono = ConstantBlock(80, 0.8F);
  suppressor.process(mono);
  Require(suppressor.currentGain() > 0.95F, "low-confidence steering should bypass suppression");
}
}  // namespace

void RunSuppressorTests()
{
  TestFloorClampAndAttenuation();
  TestFallbackRampToUnity();
  TestLowConfidenceBypassesSuppression();
}
