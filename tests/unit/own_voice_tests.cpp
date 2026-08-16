#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/multi_reference_suppressor.hpp"
#include "dsp/own_voice_reference.hpp"
#include "ovd/own_voice_channel.hpp"
#include "ovd/own_voice_detector.hpp"
#include "ovd/own_voice_features.hpp"
#include "ovd/own_voice_state.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

bool NearlyEqual(const float lhs, const float rhs, const float tolerance = 1.0e-6F)
{
  return std::fabs(lhs - rhs) <= tolerance;
}

sonitude::ovd::OwnVoiceObservation MakeObservation(const float feature_value,
                                                   const std::uint64_t sequence,
                                                   const std::uint64_t observed_ns)
{
  sonitude::ovd::OwnVoiceObservation observation{};
  observation.features.fill(feature_value);
  observation.sequence = sequence;
  observation.observed_ns = observed_ns;
  observation.valid = true;
  return observation;
}

sonitude::ovd::StatisticalOvtfModel ModelFor(const sonitude::ovd::OwnVoiceObservation& observation)
{
  sonitude::ovd::StatisticalOvtfModel model{};
  model.feature_mean = observation.features;
  model.feature_inverse_variance.fill(1.0F);
  model.feature_weight.fill(1.0F);
  model.calibrated = true;
  model.calibration_revision = 1;
  return model;
}

sonitude::ovd::OwnVoiceDetectorConfig DefaultDetectorConfig()
{
  return sonitude::ovd::OwnVoiceDetectorConfig{.enabled = true,
                                               .activation_probability = 0.8F,
                                               .release_probability = 0.2F,
                                               .activation_hold_ns = 10,
                                               .release_hold_ns = 10,
                                               .stale_timeout_ns = 50};
}

void TestContinuousReferenceAndFeatures()
{
  sonitude::dsp::OwnVoiceReferenceExtractor extractor;
  sonitude::dsp::OwnVoiceReferenceModel model{};
  model.weights.fill(1.0F);
  model.calibrated = true;
  model.calibration_revision = 1;
  extractor.configure(model);

  std::vector<sonitude::audio::MicFrame> input(32);
  for (auto& frame : input)
  {
    frame.fill(0.25F);
  }
  std::vector<float> own_reference(input.size());
  Require(extractor.process(input, own_reference), "continuous own reference extraction failed");
  Require(std::all_of(own_reference.begin(), own_reference.end(),
                      [](const float value) { return std::fabs(value - 0.25F) < 1.0e-6F; }),
          "unit-L1 fixed filter should preserve identical channels");

  sonitude::ovd::OwnVoiceFeatureExtractor features(0);
  const auto observation = features.extract(input, own_reference, 7, 1000);
  Require(observation.valid, "finite aligned period should produce a valid observation");
  for (std::size_t index = 1; index <= sonitude::ovd::kRelativeLevelFeatureCount; ++index)
  {
    Require(std::fabs(observation.features[index]) < 1.0e-5F,
            "equal channels should have zero log-level ratios");
  }
  for (std::size_t index = 1 + sonitude::ovd::kRelativeLevelFeatureCount;
       index < sonitude::ovd::kOwnVoiceFeatureCount; ++index)
  {
    Require(observation.features[index] > 0.999F,
            "equal channels should have near-unit normalized correlation");
  }
}

void TestReferenceExtractorInputFailuresAreAtomic()
{
  sonitude::dsp::OwnVoiceReferenceExtractor extractor;
  sonitude::dsp::OwnVoiceReferenceModel model{};
  model.weights.fill(1.0F);
  model.calibrated = true;
  extractor.configure(model);

  std::vector<sonitude::audio::MicFrame> input(4);
  for (auto& frame : input)
  {
    frame.fill(0.5F);
  }
  std::vector<float> own_reference(input.size(), 99.0F);
  input[2][1] = std::numeric_limits<float>::quiet_NaN();
  Require(!extractor.process(input, own_reference), "non-finite input should fail extraction");
  Require(std::all_of(own_reference.begin(), own_reference.end(),
                      [](const float sample) { return sample == 0.0F; }),
          "failed extraction must zero the entire own-reference output");

  const std::vector<sonitude::audio::MicFrame> short_input(2);
  std::vector<float> mismatched_output(3, 42.0F);
  Require(!extractor.process(short_input, mismatched_output), "span mismatch must fail extraction");
  Require(std::all_of(mismatched_output.begin(), mismatched_output.end(),
                      [](const float sample) { return sample == 0.0F; }),
          "span mismatch must zero the entire own-reference output");
}

void TestDetectorActivationAtTimestampZero()
{
  sonitude::ovd::OwnVoiceDetector detector;
  sonitude::ovd::OwnVoiceObservation own = MakeObservation(0.0F, 1, 0);
  detector.configure(DefaultDetectorConfig(), ModelFor(own));

  auto state = detector.evaluate(own, 0);
  Require(!state.active && state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "first timestamp-zero observation should start activation hold");

  own.sequence = 2;
  own.observed_ns = 10;
  state = detector.evaluate(own, 10);
  Require(state.active && state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "activation hold started at timestamp zero must release correctly");
}

void TestDetectorHysteresisStaleAndRecovery()
{
  sonitude::ovd::OwnVoiceObservation own = MakeObservation(0.0F, 1, 100);
  sonitude::ovd::OwnVoiceDetector detector;
  detector.configure(DefaultDetectorConfig(), ModelFor(own));

  auto state = detector.evaluate(own, 100);
  Require(!state.active && state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "first matching observation should start, not skip, activation hold");

  own.sequence = 2;
  own.observed_ns = 110;
  state = detector.evaluate(own, 110);
  Require(state.active, "matching observations should activate after the configured hold");

  sonitude::ovd::OwnVoiceObservation external = own;
  external.features.fill(10.0F);
  external.sequence = 3;
  external.observed_ns = 120;
  state = detector.evaluate(external, 120);
  Require(state.active, "first mismatch should start, not skip, release hangover");

  external.sequence = 4;
  external.observed_ns = 130;
  state = detector.evaluate(external, 130);
  Require(!state.active, "mismatch should release after the configured hangover");

  state = detector.stateForTime(181);
  Require(!state.active && state.health == sonitude::ovd::OwnVoiceHealth::Stale,
          "stale detector state must fail safe to inactive");

  own.sequence = 5;
  own.observed_ns = 190;
  state = detector.evaluate(own, 190);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "fresh valid observation must recover from stale state");
}

void TestDetectorHealthStatesStayDistinct()
{
  sonitude::ovd::OwnVoiceObservation own = MakeObservation(0.0F, 1, 10);
  const auto model = ModelFor(own);
  const auto config = DefaultDetectorConfig();

  sonitude::ovd::OwnVoiceDetector unconfigured;
  auto state = unconfigured.evaluate(own, 10);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::ModelUnavailable,
          "unconfigured detector should report model unavailable");

  sonitude::ovd::OwnVoiceDetector configured;
  configured.configure(config, model);
  state = configured.stateForTime(0);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::ModelUnavailable,
          "pre-observation state must not be rewritten to stale");

  sonitude::ovd::OwnVoiceObservation invalid = own;
  invalid.valid = false;
  state = configured.evaluate(invalid, 10);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::InvalidObservation,
          "invalid observation must remain distinguishable from stale");

  own.sequence = 2;
  own.observed_ns = 100;
  state = configured.evaluate(own, 100);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "valid observation should recover from invalid-observation state");
}

void TestDetectorRejectsDuplicateOrBackwardObservations()
{
  sonitude::ovd::OwnVoiceObservation own = MakeObservation(0.0F, 1, 100);
  sonitude::ovd::OwnVoiceDetector detector;
  detector.configure(DefaultDetectorConfig(), ModelFor(own));
  auto state = detector.evaluate(own, 100);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "baseline observation should be healthy");

  sonitude::ovd::OwnVoiceObservation duplicate = own;
  state = detector.evaluate(duplicate, 100);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::InvalidObservation,
          "duplicate observation must be rejected as invalid");

  sonitude::ovd::OwnVoiceObservation backward = own;
  backward.sequence = 2;
  backward.observed_ns = 99;
  state = detector.evaluate(backward, 99);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::InvalidObservation,
          "backward timestamp must be rejected as invalid");
}

void TestDetectorModelValidation()
{
  sonitude::ovd::OwnVoiceObservation observation = MakeObservation(0.0F, 1, 100);
  auto model = ModelFor(observation);
  const auto config = DefaultDetectorConfig();

  model.feature_weight[0] = 0.0F;
  model.feature_inverse_variance[0] = 0.0F;
  sonitude::ovd::OwnVoiceDetector detector;
  detector.configure(config, model);
  auto state = detector.evaluate(observation, 100);
  Require(state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "disabled feature weight should not require a positive inverse variance");

  model = ModelFor(observation);
  model.feature_inverse_variance[0] = 0.0F;
  bool threw = false;
  try
  {
    detector.configure(config, model);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "enabled feature should still require positive inverse variance");
}

void TestBoundedChannelsDropInsteadOfBlocking()
{
  sonitude::ovd::OwnVoiceObservationChannel observations(4);
  sonitude::ovd::OwnVoiceObservation observation{};
  observation.valid = true;
  Require(observations.publish(observation), "queue should accept first observation");
  Require(observations.publish(observation), "queue should accept second observation");
  Require(observations.publish(observation), "queue should accept usable capacity");
  Require(!observations.publish(observation), "full OVD queue must drop instead of blocking");
  Require(observations.drops() == 1, "OVD observation drop must be observable");

  sonitude::ovd::OwnVoiceStateChannel states(4);
  for (std::uint64_t generation = 1; generation <= 3; ++generation)
  {
    sonitude::ovd::OwnVoiceState state{};
    state.generation = generation;
    Require(states.publish(state), "state channel should accept complete state");
  }
  sonitude::ovd::OwnVoiceState overflow{};
  overflow.generation = 4;
  Require(!states.publish(overflow), "full state queue should drop newest publication");

  sonitude::ovd::OwnVoiceState latest{};
  Require(states.drainLatest(latest) && latest.generation == 3,
          "audio consumer must drain to the newest complete queued OVD state");
}

void TestMultiReferenceInvalidInputIsAtomic()
{
  sonitude::dsp::MultiReferenceSuppressor suppressor;
  suppressor.configure({.taps = 4,
                        .step_size = 0.2F,
                        .leakage = 0.001F,
                        .coefficient_limit = 2.0F,
                        .max_attenuation_db = 12.0F});

  std::vector<float> primary(16, 0.5F);
  std::vector<float> own(16, 0.3F);
  std::vector<float> distractor(16, 0.1F);
  sonitude::dsp::MultiReferenceControl control{};
  control.state[0] = sonitude::dsp::AdaptationState::Adapt;
  control.cancellation_mix[0] = 1.0F;

  const std::vector<float> original = primary;
  const auto energy_before = suppressor.coefficientEnergy();

  std::vector<float> short_own(8, 0.3F);
  Require(!suppressor.process(primary, short_own, distractor, control),
          "mismatched spans should fail");
  Require(primary == original, "mismatched spans must not mutate target path");
  Require(suppressor.coefficientEnergy() == energy_before,
          "mismatched spans must not mutate adaptive state");

  own[3] = std::numeric_limits<float>::quiet_NaN();
  Require(!suppressor.process(primary, own, distractor, control), "non-finite input should fail");
  Require(primary == original, "non-finite period must not partially mutate target path");
  Require(suppressor.coefficientEnergy() == energy_before,
          "non-finite period must not mutate adaptive state");
}

void TestMultiReferenceStateAndMixSemantics()
{
  sonitude::dsp::MultiReferenceSuppressor suppressor;
  suppressor.configure({.taps = 8,
                        .step_size = 0.2F,
                        .leakage = 0.001F,
                        .coefficient_limit = 2.0F,
                        .max_attenuation_db = 12.0F});

  std::vector<float> primary(512, 0.7F);
  const std::vector<float> original = primary;
  std::vector<float> own(512, 0.5F);
  std::vector<float> distractor(512, 0.0F);

  sonitude::dsp::MultiReferenceControl adapt_zero_mix{};
  adapt_zero_mix.state[0] = sonitude::dsp::AdaptationState::Adapt;
  adapt_zero_mix.cancellation_mix[0] = 0.0F;
  Require(suppressor.process(primary, own, distractor, adapt_zero_mix),
          "zero-mix adaptation processing failed");
  Require(primary == original, "zero cancellation mix should preserve target output");
  const auto zero_mix_energy = suppressor.coefficientEnergy();
  Require(zero_mix_energy[0] > 0.0F, "zero cancellation mix should still allow adaptation");

  sonitude::dsp::MultiReferenceControl adapt{};
  adapt.state[0] = sonitude::dsp::AdaptationState::Adapt;
  adapt.cancellation_mix[0] = 1.0F;
  primary = original;
  Require(suppressor.process(primary, own, distractor, adapt), "own-reference adaptation failed");
  const float tail_mean = std::accumulate(primary.end() - 64, primary.end(), 0.0F) / 64.0F;
  Require(tail_mean < 0.50F, "correlated own-reference should reduce interference");
  Require(tail_mean > 0.17F, "attenuation safety floor should prevent destructive nulling");

  const auto adapted_energy = suppressor.coefficientEnergy();
  sonitude::dsp::MultiReferenceControl hold = adapt;
  hold.state[0] = sonitude::dsp::AdaptationState::Hold;
  primary = original;
  Require(suppressor.process(primary, own, distractor, hold), "HOLD processing failed");
  const auto held_energy = suppressor.coefficientEnergy();
  Require(NearlyEqual(held_energy[0], adapted_energy[0], 1.0e-6F),
          "HOLD must freeze own-reference coefficients");

  sonitude::dsp::MultiReferenceControl bypass = adapt;
  bypass.state[0] = sonitude::dsp::AdaptationState::Bypass;
  primary = original;
  Require(suppressor.process(primary, own, distractor, bypass), "BYPASS processing failed");
  Require(primary == original, "BYPASS must preserve target path bit-for-bit");
  const auto bypass_energy = suppressor.coefficientEnergy();
  Require(bypass_energy[0] < held_energy[0],
          "BYPASS should decay retained coefficients while cancellation is bypassed");

  sonitude::dsp::MultiReferenceControl decay = adapt;
  decay.state[0] = sonitude::dsp::AdaptationState::Decay;
  primary = original;
  Require(suppressor.process(primary, own, distractor, decay), "DECAY processing failed");
  Require(suppressor.coefficientEnergy()[0] < bypass_energy[0],
          "DECAY must continue reducing retained coefficient energy");
}

void TestMultiReferenceCorrelatedInputsRemainBounded()
{
  sonitude::dsp::MultiReferenceSuppressor suppressor;
  constexpr float kCoefficientLimit = 0.25F;
  constexpr std::size_t kTaps = 6;
  suppressor.configure({.taps = kTaps,
                        .step_size = 0.3F,
                        .leakage = 0.001F,
                        .coefficient_limit = kCoefficientLimit,
                        .max_attenuation_db = 6.0F});

  std::vector<float> primary(1024, 0.4F);
  std::vector<float> own(1024);
  std::vector<float> distractor(1024);
  for (std::size_t i = 0; i < own.size(); ++i)
  {
    const float sample = std::sin(static_cast<float>(i) * 0.1F);
    own[i] = sample;
    distractor[i] = sample;
  }

  sonitude::dsp::MultiReferenceControl control{};
  control.state[0] = sonitude::dsp::AdaptationState::Adapt;
  control.state[1] = sonitude::dsp::AdaptationState::Adapt;
  control.cancellation_mix[0] = 1.0F;
  control.cancellation_mix[1] = 1.0F;
  Require(suppressor.process(primary, own, distractor, control),
          "correlated-reference adaptation should complete");

  const auto energy = suppressor.coefficientEnergy();
  const float max_energy = static_cast<float>(kTaps) * kCoefficientLimit * kCoefficientLimit;
  Require(std::isfinite(energy[0]) && std::isfinite(energy[1]),
          "coefficient energies must remain finite");
  Require(energy[0] <= max_energy + 1.0e-5F && energy[1] <= max_energy + 1.0e-5F,
          "coefficient clamp should keep correlated adaptation bounded");
}

void TestSuppressorReconfigureResetsAdaptiveState()
{
  sonitude::dsp::MultiReferenceSuppressor suppressor;
  const sonitude::dsp::MultiReferenceSuppressorConfig config{.taps = 4,
                                                             .step_size = 0.2F,
                                                             .leakage = 0.001F,
                                                             .coefficient_limit = 1.0F,
                                                             .max_attenuation_db = 12.0F};
  suppressor.configure(config);

  std::vector<float> primary(128, 0.7F);
  std::vector<float> own(128, 0.4F);
  std::vector<float> distractor(128, 0.0F);
  sonitude::dsp::MultiReferenceControl control{};
  control.state[0] = sonitude::dsp::AdaptationState::Adapt;
  control.cancellation_mix[0] = 1.0F;
  Require(suppressor.process(primary, own, distractor, control), "adaptation should run");
  Require(suppressor.coefficientEnergy()[0] > 0.0F,
          "adaptation should produce non-zero coefficients");

  suppressor.configure(config);
  const auto reset_energy = suppressor.coefficientEnergy();
  Require(reset_energy[0] == 0.0F && reset_energy[1] == 0.0F,
          "reconfigure should reset adaptive state deterministically");
}
} // namespace

int main()
{
  try
  {
    static_assert(std::is_trivially_copyable_v<sonitude::ovd::OwnVoiceObservation>);
    static_assert(std::is_trivially_copyable_v<sonitude::ovd::OwnVoiceState>);

    TestContinuousReferenceAndFeatures();
    TestReferenceExtractorInputFailuresAreAtomic();
    TestDetectorActivationAtTimestampZero();
    TestDetectorHysteresisStaleAndRecovery();
    TestDetectorHealthStatesStayDistinct();
    TestDetectorRejectsDuplicateOrBackwardObservations();
    TestDetectorModelValidation();
    TestBoundedChannelsDropInsteadOfBlocking();
    TestMultiReferenceInvalidInputIsAtomic();
    TestMultiReferenceStateAndMixSemantics();
    TestMultiReferenceCorrelatedInputsRemainBounded();
    TestSuppressorReconfigureResetsAdaptiveState();
    std::cout << "Own-voice architecture tests passed.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Own-voice test failure: " << ex.what() << '\n';
    return 1;
  }
}
