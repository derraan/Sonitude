#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sonitude::dsp
{
inline constexpr std::size_t kMaxInterferenceReferences = 2U;
inline constexpr std::size_t kMaxMultiReferenceTaps = 32U;

enum class AdaptationState : std::uint8_t
{
  Adapt = 0,
  Hold = 1,
  Decay = 2,
  Bypass = 3
};

struct MultiReferenceSuppressorConfig
{
  std::size_t taps = 0;
  float step_size = 0.0F;
  float leakage = 0.0F;
  float coefficient_limit = 0.0F;
  float max_attenuation_db = 0.0F;
};

struct MultiReferenceControl
{
  // Index 0: own voice. Index 1: strongest external distractor.
  std::array<AdaptationState, kMaxInterferenceReferences> state{
      AdaptationState::Bypass, AdaptationState::Bypass};
  std::array<float, kMaxInterferenceReferences> cancellation_mix{0.0F, 0.0F};
};

// Bounded two-reference normalized adaptive canceller. configure() is slow-path;
// process() is allocation-free, lock-free, and exception-free.
//
// Target protection is explicit: control must choose ADAPT/HOLD/DECAY/BYPASS
// from OVD freshness, own-reference quality, target activity/correlation, and
// external-distractor state. Uncertainty must map to HOLD or BYPASS.
class MultiReferenceSuppressor
{
public:
  void configure(const MultiReferenceSuppressorConfig& config);
  bool process(std::span<float> target_reference, std::span<const float> own_reference,
               std::span<const float> distractor_reference,
               const MultiReferenceControl& control) noexcept;

  std::array<float, kMaxInterferenceReferences> coefficientEnergy() const noexcept;

private:
  struct AdaptiveReference
  {
    std::array<float, kMaxMultiReferenceTaps> weights{};
    std::array<float, kMaxMultiReferenceTaps> history{};
    std::size_t cursor = 0;
  };

  float pushAndPredict(AdaptiveReference& reference, float sample) noexcept;
  float historyAt(const AdaptiveReference& reference, std::size_t tap) const noexcept;
  void update(AdaptiveReference& reference, float error, float norm) noexcept;
  void decay(AdaptiveReference& reference) noexcept;

  std::array<AdaptiveReference, kMaxInterferenceReferences> references_{};
  MultiReferenceSuppressorConfig config_{};
  float output_floor_gain_ = 1.0F;
  bool configured_ = false;
};
} // namespace sonitude::dsp
