#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/limiter.hpp"
#include "dsp/resampler.hpp"

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
  const sonitude::dsp::LimiterTelemetry telemetry = limiter.process(mono);

  for (const float sample : mono)
  {
    Require(std::fabs(sample) <= 0.8001F, "limiter must clamp output peaks to ceiling");
  }
  Require(telemetry.input_over_ceiling_events >= 2U,
          "mono telemetry must count samples over the limiter ceiling");
  Require(telemetry.output_saturation_events == 0U,
          "mono limiter output should stay bounded when ceiling is below full-scale");
}

void TestLimiterRecoversAfterTransient()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.90F, .release_ms = 10.0F}, 1000);

  std::vector<float> impulse = {1.8F};
  (void)limiter.process(impulse);
  const float dipped_gain = limiter.currentGain();
  Require(dipped_gain < 1.0F, "limiter should reduce gain during overload");

  std::vector<float> tail(30, 0.2F);
  (void)limiter.process(tail);
  Require(limiter.currentGain() > dipped_gain, "limiter should release toward unity after overload");
}

void TestLimiterResetRestoresUnity()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.85F, .release_ms = 40.0F}, 16000);
  std::vector<float> block(10, 1.2F);
  (void)limiter.process(block);
  Require(limiter.currentGain() < 1.0F, "limiter should have reduced gain before reset");
  limiter.reset();
  Require(std::fabs(limiter.currentGain() - 1.0F) < 1e-6F, "reset should restore unity gain");
}

void TestLinkedStereoLimiterTracksHigherChannel()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.75F, .release_ms = 40.0F}, 48000);
  std::vector<sonitude::dsp::StereoSample> stereo = {
      {.left = 0.10F, .right = 0.20F},
      {.left = 1.30F, .right = 0.30F},
      {.left = 0.30F, .right = -1.10F},
      {.left = -0.50F, .right = 0.50F}};
  const sonitude::dsp::LimiterTelemetry telemetry = limiter.processLinkedStereo(stereo);

  for (const sonitude::dsp::StereoSample& sample : stereo)
  {
    Require(std::fabs(sample.left) <= 0.7501F, "linked limiter must bound left channel");
    Require(std::fabs(sample.right) <= 0.7501F, "linked limiter must bound right channel");
  }
  Require(telemetry.input_over_ceiling_events >= 2U,
          "linked limiter must count over-ceiling stereo frames");
  Require(std::fabs(stereo[1].left) > std::fabs(stereo[1].right),
          "linked limiter must preserve relative channel balance");
}

void TestLinkedStereoLimiterHandlesSustainedOverload()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.70F, .release_ms = 200.0F}, 48000);
  std::vector<sonitude::dsp::StereoSample> overload(512, {.left = 2.4F, .right = -1.9F});
  const sonitude::dsp::LimiterTelemetry telemetry = limiter.processLinkedStereo(overload);

  for (const sonitude::dsp::StereoSample& sample : overload)
  {
    Require(std::fabs(sample.left) <= 0.7010F, "sustained overload left channel must be bounded");
    Require(std::fabs(sample.right) <= 0.7010F, "sustained overload right channel must be bounded");
  }
  Require(telemetry.input_over_ceiling_events == overload.size(),
          "every overloaded stereo frame must be counted");
  Require(telemetry.output_saturation_events == 0U,
          "linked limiter should prevent post-limiter saturation");
}

void TestLinkedStereoLimiterReleaseAfterOverload()
{
  sonitude::dsp::PeakLimiter limiter;
  limiter.configure({.ceiling_linear = 0.85F, .release_ms = 5.0F}, 1000);

  std::vector<sonitude::dsp::StereoSample> hot = {{.left = 2.0F, .right = 1.8F}};
  (void)limiter.processLinkedStereo(hot);
  const float dipped_gain = limiter.currentGain();
  Require(dipped_gain < 1.0F, "linked limiter should drop gain after overload");

  std::vector<sonitude::dsp::StereoSample> tail(40, {.left = 0.1F, .right = -0.1F});
  (void)limiter.processLinkedStereo(tail);
  Require(limiter.currentGain() > dipped_gain,
          "linked limiter should release toward unity after overload");
}
}  // namespace

void RunLimiterTests()
{
  TestLimiterClampsPeaks();
  TestLimiterRecoversAfterTransient();
  TestLimiterResetRestoresUnity();
  TestLinkedStereoLimiterTracksHigherChannel();
  TestLinkedStereoLimiterHandlesSustainedOverload();
  TestLinkedStereoLimiterReleaseAfterOverload();
}
