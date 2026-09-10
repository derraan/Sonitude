#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/audio_types.hpp"

namespace sonitude::dsp
{
constexpr char kArrayProfileMagic[4] = {'S', 'M', 'V', '3'};
constexpr std::uint16_t kArrayProfileFormatVersion = 1;
constexpr std::size_t kArrayProfileHeaderBytes = 128;
constexpr std::size_t kPositiveBinCount128 = 65;

enum class ArrayNoiseModel : std::uint8_t
{
  AngularAtf = 0,
  MeasuredCovariance = 1
};

struct ArrayProfileIdentity
{
  std::string geometry_id;
  std::array<std::uint8_t, 16> conditioning_hash{};
  std::array<std::uint8_t, 8> content_fnv64{};
  bool synthetic = true;
  ArrayNoiseModel noise_model = ArrayNoiseModel::AngularAtf;
};

struct ArrayProfile
{
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t fft_size = 128;
  std::uint16_t hop_size = 32;
  std::uint8_t mic_count = 6;
  std::uint8_t bin_count = 65;
  std::uint16_t direction_count = 0;
  std::uint8_t reference_mic = 0;
  std::uint8_t left_ear_mic = 0;
  std::uint8_t right_ear_mic = 5;
  std::uint8_t transform_sign = 0;  // 0 = forward e^{-j}
  float reference_gain = 1.0F;
  ArrayProfileIdentity identity{};
  std::vector<float> azimuth_deg;
  // Layout: [dir][bin][mic] packed as interleaved re,im float32
  std::vector<float> weights_ri;
  std::vector<float> steering_ri;
  std::vector<std::uint8_t> valid;  // dir * bin
  std::vector<float> dominance_scale;  // bin

  [[nodiscard]] std::size_t binCount() const noexcept { return bin_count; }
  [[nodiscard]] bool empty() const noexcept { return direction_count == 0 || weights_ri.empty(); }
};

struct ArrayProfileView
{
  const ArrayProfile* profile = nullptr;
  [[nodiscard]] bool valid() const noexcept { return profile != nullptr && !profile->empty(); }
};

struct AzimuthBracket
{
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  float blend = 0.0F;  // 0 => left only, 1 => right only
};

ArrayProfile LoadArrayProfileFromBytes(std::span<const std::uint8_t> bytes);
ArrayProfile LoadArrayProfileFromFile(const std::string& path);
std::vector<std::uint8_t> SerializeArrayProfile(const ArrayProfile& profile);
void ValidateArrayProfile(const ArrayProfile& profile,
                          std::uint32_t sample_rate_hz,
                          std::uint16_t fft_size,
                          std::uint16_t hop_size);
std::size_t NearestAzimuthIndex(const ArrayProfile& profile, float azimuth_deg);
AzimuthBracket BracketAzimuth(const ArrayProfile& profile, float azimuth_deg);
}  // namespace sonitude::dsp
