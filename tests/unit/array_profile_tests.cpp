#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/array_profile.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

sonitude::dsp::ArrayProfile MakeDasProfile()
{
  sonitude::dsp::ArrayProfile p;
  p.sample_rate_hz = 44100;
  p.fft_size = 128;
  p.hop_size = 32;
  p.mic_count = 6;
  p.bin_count = 65;
  p.direction_count = 2;
  p.reference_mic = 0;
  p.left_ear_mic = 0;
  p.right_ear_mic = 5;
  p.reference_gain = 1.0F;
  p.identity.geometry_id = "synthetic";
  p.identity.synthetic = true;
  p.azimuth_deg = {0.0F, 10.0F};
  const std::size_t n = static_cast<std::size_t>(p.direction_count) * p.bin_count * p.mic_count * 2U;
  p.weights_ri.assign(n, 0.0F);
  p.steering_ri.assign(n, 0.0F);
  p.valid.assign(static_cast<std::size_t>(p.direction_count) * p.bin_count, 1);
  p.dominance_scale.assign(p.bin_count, 1.0F);
  for (std::size_t dir = 0; dir < p.direction_count; ++dir)
  {
    for (std::size_t b = 0; b < p.bin_count; ++b)
    {
      for (std::size_t ch = 0; ch < p.mic_count; ++ch)
      {
        const std::size_t i = ((((dir * p.bin_count) + b) * p.mic_count) + ch) * 2U;
        p.steering_ri[i] = 1.0F;
        p.weights_ri[i] = 1.0F / static_cast<float>(p.mic_count);
      }
    }
  }
  return p;
}
}  // namespace

void RunArrayProfileTests()
{
  const auto profile = MakeDasProfile();
  const auto bytes = sonitude::dsp::SerializeArrayProfile(profile);
  const auto loaded = sonitude::dsp::LoadArrayProfileFromBytes(bytes);
  Require(loaded.direction_count == 2, "round-trip direction count");
  Require(loaded.bin_count == 65, "round-trip bin count");
  Require(std::fabs(loaded.weights_ri[0] - (1.0F / 6.0F)) < 1.0e-6F, "weight round trip");
  sonitude::dsp::ValidateArrayProfile(loaded, 44100, 128, 32);
  Require(sonitude::dsp::NearestAzimuthIndex(loaded, 9.0F) == 1, "nearest azimuth");
  const auto bracket = sonitude::dsp::BracketAzimuth(loaded, 5.0F);
  Require(bracket.left_index == 0 && bracket.right_index == 1, "bracket indices");
  Require(std::fabs(bracket.blend - 0.5F) < 1.0e-6F, "bracket blend");

  auto truncated = bytes;
  truncated.resize(50);
  bool threw = false;
  try
  {
    (void)sonitude::dsp::LoadArrayProfileFromBytes(truncated);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "truncated profile must fail");

  auto bad_rate = loaded;
  bool rate_threw = false;
  try
  {
    sonitude::dsp::ValidateArrayProfile(bad_rate, 48000, 128, 32);
  }
  catch (const std::exception&)
  {
    rate_threw = true;
  }
  Require(rate_threw, "sample-rate mismatch must fail");

  auto cursed = bytes;
  cursed[80] ^= 0xff;
  bool hash_threw = false;
  try
  {
    (void)sonitude::dsp::LoadArrayProfileFromBytes(cursed);
  }
  catch (const std::exception&)
  {
    hash_threw = true;
  }
  Require(hash_threw, "tampered hash must fail");

  auto sparse = loaded;
  sparse.identity.synthetic = false;
  sparse.direction_count = 2;
  sparse.azimuth_deg = {-20.0F, 20.0F};
  const std::size_t bins = sparse.bin_count;
  const std::size_t mics = sparse.mic_count;
  sparse.weights_ri.assign(static_cast<std::size_t>(sparse.direction_count) * bins * mics * 2U, 0.0F);
  sparse.steering_ri.assign(sparse.weights_ri.size(), 0.0F);
  sparse.valid.assign(static_cast<std::size_t>(sparse.direction_count) * bins, 1);
  bool gap_threw = false;
  try
  {
    sonitude::dsp::ValidateArrayProfile(sparse, 44100, 128, 32);
  }
  catch (const std::exception&)
  {
    gap_threw = true;
  }
  Require(gap_threw, "large azimuth coverage gap must fail");
}
