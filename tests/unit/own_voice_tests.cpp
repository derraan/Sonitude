#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/multi_reference_suppressor.hpp"
#include "dsp/own_voice_reference.hpp"
#include "ovd/own_voice_channel.hpp"
#include "ovd/own_voice_detector.hpp"
#include "ovd/own_voice_features.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
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

sonitude::ovd::StatisticalOvtfModel ModelFor(
    const sonitude::ovd::OwnVoiceObservation& observation)
{
  sonitude::ovd::StatisticalOvtfModel model{};
  model.feature_mean = observation.features;
  model.feature_inverse_variance.fill(1.0F);
  model.feature_weight.fill(1.0F);
  model.calibrated = true;
  model.calibration_revision = 1;
  return model;
}

void TestDetectorHysteresisAndStaleFailsafe()
{
  sonitude::ovd::OwnVoiceObservation own{};
  own.valid = true;
  own.observed_ns = 100;
  own.sequence = 1;

  sonitude::ovd::OwnVoiceDetector detector;
  detector.configure({.enabled = true,
                      .activation_probability = 0.8F,
                      .release_probability = 0.2F,
                      .activation_hold_ns = 10,
                      .release_hold_ns = 10,
                      .stale_timeout_ns = 50},
                     ModelFor(own));

  auto state = detector.evaluate(own, 100);
  Require(!state.active && state.health == sonitude::ovd::OwnVoiceHealth::Healthy,
          "first matching observation should start, not skip, activation hold");
  own.observed_ns = 110;
  state = detector.evaluate(own, 110);
  Require(state.active, "matching observations should activate after the configured hold");

  sonitude::ovd::OwnVoiceObservation external = own;
  external.features.fill(10.0F);
  external.observed_ns = 120;
  state = detector.evaluate(external, 120);
  Require(state.active, "first mismatch should start, not skip, release hangover");
  external.observed_ns = 130;
  state = detector.evaluate(external, 130);
  Require(!state.active, "mismatch should release after the configured hangover");

  state = detector.stateForTime(181);
  Require(!state.active && state.health == sonitude::ovd::OwnVoiceHealth::Stale,
          "stale detector state must fail safe to inactive");
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
  sonitude::ovd::OwnVoiceState latest{};
  Require(states.drainLatest(latest) && latest.generation == 3,
          "audio consumer must drain to the newest complete OVD state");
}

void TestMultiReferenceBypassAndAdaptationStates()
{
  sonitude::dsp::MultiReferenceSuppressor suppressor;
  suppressor.configure({.taps = 4,
                        .step_size = 0.2F,
                        .leakage = 0.001F,
                        .coefficient_limit = 2.0F,
                        .max_attenuation_db = 12.0F});

  std::vector<float> primary(512, 0.7F);
  const std::vector<float> original = primary;
  std::vector<float> own(512, 0.5F);
  std::vector<float> distractor(512, 0.0F);

  sonitude::dsp::MultiReferenceControl bypass{};
  Require(suppressor.process(primary, own, distractor, bypass), "bypass processing failed");
  Require(primary == original, "BYPASS must preserve the target path bit-for-bit");

  sonitude::dsp::MultiReferenceControl adapt{};
  adapt.state[0] = sonitude::dsp::AdaptationState::Adapt;
  adapt.cancellation_mix[0] = 1.0F;
  primary = original;
  Require(suppressor.process(primary, own, distractor, adapt), "own-reference adaptation failed");
  const float tail_mean =
      std::accumulate(primary.end() - 64, primary.end(), 0.0F) / 64.0F;
  Require(tail_mean < 0.50F, "calibrated correlated reference should reduce interference");
  Require(tail_mean > 0.17F, "attenuation safety floor should prevent destructive nulling");

  const auto adapted_energy = suppressor.coefficientEnergy();
  sonitude::dsp::MultiReferenceControl hold = adapt;
  hold.state[0] = sonitude::dsp::AdaptationState::Hold;
  primary = original;
  Require(suppressor.process(primary, own, distractor, hold), "HOLD processing failed");
  const auto held_energy = suppressor.coefficientEnergy();
  Require(std::fabs(held_energy[0] - adapted_energy[0]) < 1.0e-7F,
          "HOLD must freeze own-reference coefficients");

  sonitude::dsp::MultiReferenceControl decay = adapt;
  decay.state[0] = sonitude::dsp::AdaptationState::Decay;
  primary = original;
  Require(suppressor.process(primary, own, distractor, decay), "DECAY processing failed");
  Require(suppressor.coefficientEnergy()[0] < held_energy[0],
          "DECAY must reduce retained coefficient energy");
}
} // namespace

int main()
{
  try
  {
    TestContinuousReferenceAndFeatures();
    TestDetectorHysteresisAndStaleFailsafe();
    TestBoundedChannelsDropInsteadOfBlocking();
    TestMultiReferenceBypassAndAdaptationStates();
    std::cout << "Own-voice architecture tests passed.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Own-voice test failure: " << ex.what() << '\n';
    return 1;
  }
}
