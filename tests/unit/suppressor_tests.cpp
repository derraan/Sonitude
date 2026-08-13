#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/selective_suppressor.hpp"
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

double MeanSquaredError(const std::vector<float>& a, const std::vector<float>& b)
{
  if (a.size() != b.size())
  {
    throw std::runtime_error("MSE requires equal-sized vectors");
  }
  double sum = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    const double diff = static_cast<double>(a[i] - b[i]);
    sum += diff * diff;
  }
  return sum / static_cast<double>(a.size());
}

void TestOutputGainGateAmbientFloor()
{
  sonitude::dsp::OutputGainGate gate;
  gate.configure(
      {.ambient_floor_linear = 0.25F,
       .fade_ms = 10.0F,
       .activity_threshold = 0.01F,
       .confidence_threshold = 0.6F},
      1000);
  gate.setControl(true, 1.0F);
  auto mono = ConstantBlock(80, 1.0F);
  gate.process(mono);

  const float gain = gate.currentGain();
  Require(gain <= 0.30F, "output gain gate should converge near configured floor");
  Require(gain >= 0.25F, "output gain gate must not dip below floor");
  const float min_sample = *std::min_element(mono.begin(), mono.end());
  Require(min_sample >= 0.24F, "output samples should respect ambient floor");
}

void TestSelectiveSuppressorAttenuatesDistractor()
{
  constexpr std::size_t kFrames = 512;
  std::vector<float> focus(kFrames, 0.0F);
  std::vector<float> reference(kFrames, 0.0F);
  std::vector<float> mixture(kFrames, 0.0F);
  for (std::size_t i = 0; i < kFrames; ++i)
  {
    const float t = static_cast<float>(i) / 16000.0F;
    focus[i] = std::sin(2.0F * 3.14159265F * 400.0F * t);
    reference[i] = 0.8F * std::sin(2.0F * 3.14159265F * 1100.0F * t);
    mixture[i] = focus[i] + reference[i];
  }
  const std::vector<float> before = mixture;

  sonitude::dsp::SelectiveSuppressor suppressor;
  suppressor.configure({}, 16000);
  suppressor.setControl(true, true);
  suppressor.process(mixture, reference);

  const double mse_before = MeanSquaredError(before, focus);
  const double mse_after = MeanSquaredError(mixture, focus);
  Require(mse_after < mse_before, "selective suppressor should move output closer to focus source");
}

void TestSelectiveSuppressorFrozenWithoutDistractor()
{
  auto focus = ConstantBlock(128, 0.4F);
  auto reference = ConstantBlock(128, 0.0F);
  const auto before = focus;
  sonitude::dsp::SelectiveSuppressor suppressor;
  suppressor.configure({}, 16000);
  suppressor.setControl(true, false);
  suppressor.process(focus, reference);
  Require(MeanSquaredError(focus, before) < 1e-6, "suppression should stay frozen without distractor");
}
}  // namespace

void RunSuppressorTests()
{
  TestOutputGainGateAmbientFloor();
  TestSelectiveSuppressorAttenuatesDistractor();
  TestSelectiveSuppressorFrozenWithoutDistractor();
}
