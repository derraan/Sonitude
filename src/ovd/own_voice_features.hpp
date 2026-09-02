#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "audio/audio_types.hpp"

namespace sonitude::ovd
{
inline constexpr std::size_t kRelativeLevelFeatureCount = audio::kMicChannels - 1U;
inline constexpr std::size_t kCorrelationFeatureCount = audio::kMicChannels - 1U;
inline constexpr std::size_t kOwnVoiceFeatureCount =
    1U + kRelativeLevelFeatureCount + kCorrelationFeatureCount;

struct OwnVoiceObservation
{
  std::array<float, kOwnVoiceFeatureCount> features{};
  std::uint64_t sequence = 0;
  std::uint64_t observed_ns = 0;
  bool valid = false;
};

// Cheap bounded period features for the RT -> OVD queue. This deliberately
// stops short of claiming a production OVTF/RATF representation.
class OwnVoiceFeatureExtractor
{
public:
  explicit OwnVoiceFeatureExtractor(std::size_t reference_mic_index = 0);

  // Feature order:
  //   [0] own_ref RMS / mean channel RMS
  //   [1..5] log RMS ratios to the configured reference microphone
  //   [6..10] normalized correlations to that microphone
  //
  // TODO(ovd-level1): Replace or augment these period summaries with the
  // frequency-binned statistical RATF features selected from Sonitude headset
  // measurements. Keep OwnVoiceObservation fixed-size and trivially copyable.
  OwnVoiceObservation extract(std::span<const audio::MicFrame> calibrated_input,
                              std::span<const float> own_reference, std::uint64_t sequence,
                              std::uint64_t observed_ns) const noexcept;

private:
  std::size_t reference_mic_index_ = 0;
};

static_assert(std::is_trivially_copyable_v<OwnVoiceObservation>,
              "OwnVoiceObservation crosses an RT boundary and must be "
              "trivially copyable");
} // namespace sonitude::ovd
