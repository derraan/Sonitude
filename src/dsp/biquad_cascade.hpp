#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/audio_types.hpp"

namespace sonitude::dsp
{
enum class BiquadType
{
  Peak,
  LowShelf,
  HighShelf,
  LowPass,
  HighPass,
};

struct BiquadSectionSpec
{
  BiquadType type = BiquadType::Peak;
  float freq_hz = 1000.0F;
  float gain_db = 0.0F;
  float q = 0.707F;
  bool enabled = true;
};

class BiquadCascade
{
 public:
  static constexpr std::size_t kMaxSections = 24;
  static constexpr std::size_t kMaxChannels = audio::kMicChannels;

  void configure(std::uint32_t sample_rate_hz,
                 std::span<const BiquadSectionSpec> sections,
                 std::size_t channels,
                 bool enabled);
  void reset();

  float processSample(std::size_t channel, float x);
  void processMono(std::span<float> mono);
  void processMicFrame(audio::MicFrame& frame);

  bool enabled() const { return enabled_ && section_count_ > 0U; }
  std::size_t sectionCount() const { return section_count_; }
  std::size_t channels() const { return channels_; }

  static BiquadType ParseType(const std::string& value);

 private:
  struct Coeff
  {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
  };
  struct State
  {
    double z1 = 0.0;
    double z2 = 0.0;
  };

  static Coeff Design(const BiquadSectionSpec& spec, std::uint32_t sample_rate_hz);

  bool configured_ = false;
  bool enabled_ = false;
  std::size_t channels_ = 1U;
  std::size_t section_count_ = 0U;
  std::array<Coeff, kMaxSections> coeffs_{};
  std::array<std::array<State, kMaxChannels>, kMaxSections> states_{};
};
}  // namespace sonitude::dsp
