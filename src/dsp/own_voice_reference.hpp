#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "audio/audio_types.hpp"

namespace sonitude::dsp
{
struct OwnVoiceReferenceModel
{
  std::array<float, audio::kMicChannels> weights{};
  std::uint32_t calibration_revision = 0;
  bool calibrated = false;
};

// A sample-synchronous, fixed spatial filter for the wearer's voice.
//
// Ownership: configured by the slow thread before audio starts, then owned and
// mutated only by the audio thread. process() allocates nothing, takes no lock,
// and produces own_ref continuously for every input period.
class OwnVoiceReferenceExtractor
{
public:
  // Non-RT setup. A calibrated model is mandatory; no unmeasured fallback
  // weights are invented by this class.
  void configure(const OwnVoiceReferenceModel& model);

  // RT path. Returns false and zeros output on a contract violation.
  bool process(std::span<const audio::MicFrame> calibrated_input,
               std::span<float> own_reference) const noexcept;

  bool configured() const noexcept
  {
    return configured_;
  }

private:
  std::array<float, audio::kMicChannels> weights_{};
  bool configured_ = false;
};
} // namespace sonitude::dsp
