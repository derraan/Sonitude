#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/suppression_stage.hpp"
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

void TestSuppressionStageOffIsExactCopy()
{
  sonitude::dsp::SuppressionStage stage;
  stage.configure({.backend = sonitude::dsp::SuppressionBackend::Off,
                   .sample_rate_hz = 44100,
                   .maximum_block_frames = 64});
  Require(stage.algorithmicDelaySamples() == 0, "off backend must not add spectral delay");
  auto mono = ConstantBlock(64, 0.37F);
  const auto original = mono;
  stage.process(mono);
  Require(mono == original, "off backend must preserve samples exactly");
}

void TestConservativeStageHasNoSpectralDelay()
{
  sonitude::dsp::SuppressionStage stage;
  stage.configure({.backend = sonitude::dsp::SuppressionBackend::Conservative,
                   .sample_rate_hz = 1000,
                   .maximum_block_frames = 80,
                   .conservative = {.ambient_floor_linear = 0.25F,
                                    .fade_ms = 10.0F,
                                    .activity_threshold = 0.01F,
                                    .confidence_threshold = 0.6F}});
  Require(stage.algorithmicDelaySamples() == 0, "conservative delay must remain 0");
  Require(stage.spectralFilter() == nullptr, "conservative must not expose a spectral hop filter");
}

void TestSpectralStageWiresSharedStftWithoutPcmProcess()
{
  sonitude::dsp::SuppressionStage stage;
  stage.configure({.backend = sonitude::dsp::SuppressionBackend::Spectral,
                   .sample_rate_hz = 44100,
                   .maximum_block_frames = 64,
                   .spectral = {.enabled = true, .fft_size = 128, .hop_size = 32, .gain_floor_db = -12.0F}});
  Require(stage.algorithmicDelaySamples() == 0, "spectral adds no extra delay beyond the MVDR STFT");
  Require(stage.spectralFilter() != nullptr, "spectral backend must expose the shared-hop filter");
  auto mono = ConstantBlock(64, 0.37F);
  const auto original = mono;
  stage.process(mono);
  Require(mono == original, "spectral stage must not process PCM; the MVDR hop owns the gains");
}

void RunSuppressorTests()
{
  TestFloorClampAndAttenuation();
  TestFallbackRampToUnity();
  TestLowConfidenceBypassesSuppression();
  TestSuppressionStageOffIsExactCopy();
  TestConservativeStageHasNoSpectralDelay();
  TestSpectralStageWiresSharedStftWithoutPcmProcess();
}
