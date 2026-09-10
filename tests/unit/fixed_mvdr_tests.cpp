#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/array_profile.hpp"
#include "dsp/fixed_binaural_mvdr.hpp"
#include "tests/support/synth_signals.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

sonitude::dsp::ArrayProfile MakeUnityProfile(const float gamma)
{
  sonitude::dsp::ArrayProfile p;
  p.sample_rate_hz = 16000;
  p.fft_size = 128;
  p.hop_size = 32;
  p.mic_count = 6;
  p.bin_count = 65;
  p.direction_count = 1;
  p.reference_mic = 0;
  p.left_ear_mic = 0;
  p.right_ear_mic = 5;
  p.reference_gain = gamma;
  p.identity.synthetic = true;
  p.identity.geometry_id = "synthetic";
  p.azimuth_deg = {0.0F};
  const std::size_t n = 1U * 65U * 6U * 2U;
  p.weights_ri.assign(n, 0.0F);
  p.steering_ri.assign(n, 0.0F);
  p.valid.assign(65, 1);
  p.dominance_scale.assign(65, 1.0F);
  for (std::size_t b = 0; b < 65; ++b)
  {
    for (std::size_t ch = 0; ch < 6; ++ch)
    {
      const std::size_t i = ((b * 6U) + ch) * 2U;
      p.steering_ri[i] = (ch == 0) ? 1.0F : 1.0F;
      p.weights_ri[i] = 1.0F / 6.0F;
    }
  }
  return p;
}

sonitude::dsp::ArrayProfile MakeDualAzimuthProfile()
{
  auto p = MakeUnityProfile(1.0F);
  p.direction_count = 2;
  p.azimuth_deg = {-30.0F, 30.0F};
  const std::size_t n = 2U * 65U * 6U * 2U;
  p.weights_ri.assign(n, 0.0F);
  p.steering_ri.assign(n, 0.0F);
  p.valid.assign(2U * 65U, 1);
  p.dominance_scale.assign(65, 1.0F);
  for (std::size_t b = 0; b < 65; ++b)
  {
    for (std::size_t ch = 0; ch < 6; ++ch)
    {
      const std::size_t i0 = ((((0U * 65U) + b) * 6U) + ch) * 2U;
      const std::size_t i1 = ((((1U * 65U) + b) * 6U) + ch) * 2U;
      p.steering_ri[i0] = 1.0F;
      p.steering_ri[i1] = 1.0F;
      p.weights_ri[i0] = 1.0F / 6.0F;
      p.weights_ri[i1] = (ch == 0U) ? 1.0F : 0.0F;
    }
  }
  return p;
}

std::vector<sonitude::audio::MicFrame> IdenticalMics(const std::vector<float>& src)
{
  std::vector<sonitude::audio::MicFrame> out(src.size());
  for (std::size_t i = 0; i < src.size(); ++i)
  {
    for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
    {
      out[i][ch] = src[i];
    }
  }
  return out;
}
}  // namespace

void RunFixedMvdrTests()
{
  constexpr std::uint32_t kFs = 16000;
  constexpr std::size_t kFrames = 4096;
  const auto src = sonitude::tests::support::GenerateSine(kFrames, kFs, 800.0);
  const auto mic = IdenticalMics(src);

  {
    auto profile = MakeUnityProfile(0.5F);
    sonitude::dsp::FixedBinauralMvdr bf;
    sonitude::dsp::FixedMvdrMaskParams mask;
    mask.constant_mask = 0.0F;
    bf.configure(profile, kFs, kFrames, mask);
    bf.setTarget({0.0F, 0.0F});
    std::vector<float> left(kFrames, 0.0F);
    std::vector<float> right(kFrames, 0.0F);
    bf.processStereo(mic, left, right);
    const double l_rms = sonitude::tests::support::ComputeRms(left, 512);
    const double r_rms = sonitude::tests::support::ComputeRms(right, 512);
    const double s_rms = sonitude::tests::support::ComputeRms(src, 512);
    Require(std::fabs(l_rms - (0.5 * s_rms)) < (0.08 * s_rms), "g=0 left equals gamma*reference");
    Require(std::fabs(r_rms - (0.5 * s_rms)) < (0.08 * s_rms), "g=0 right equals gamma*reference");
  }

  {
    auto profile = MakeUnityProfile(1.0F);
    sonitude::dsp::FixedBinauralMvdr bf;
    sonitude::dsp::FixedMvdrMaskParams mask;
    mask.constant_mask = 1.0F;
    bf.configure(profile, kFs, kFrames, mask);
    std::vector<float> left(kFrames, 0.0F);
    std::vector<float> right(kFrames, 0.0F);
    bf.processStereo(mic, left, right);
    const double l_rms = sonitude::tests::support::ComputeRms(left, 512);
    const double s_rms = sonitude::tests::support::ComputeRms(src, 512);
    Require(std::fabs(l_rms - s_rms) < (0.12 * s_rms), "g=1 DAS reconstruction near source");
    Require(bf.solveCountForTest() == 0, "fixed path must not solve");
    Require(bf.factorizationCountForTest() == 0, "fixed path must not factor");
  }

  {
    auto profile = MakeDualAzimuthProfile();
    sonitude::dsp::FixedBinauralMvdr bf;
    sonitude::dsp::FixedMvdrMaskParams mask;
    mask.constant_mask = 1.0F;
    bf.configure(profile, kFs, kFrames, mask, 150.0F, sonitude::dsp::AzimuthInterpolationMode::LinearBlend);
    bf.setTarget({0.0F, 0.0F});
    std::vector<sonitude::audio::MicFrame> sparse(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i)
    {
      sparse[i][0] = src[i];
    }
    std::vector<float> left(kFrames, 0.0F);
    std::vector<float> right(kFrames, 0.0F);
    bf.processStereo(sparse, left, right);
    const double l_rms = sonitude::tests::support::ComputeRms(left, 512);
    const double s_rms = sonitude::tests::support::ComputeRms(src, 512);
    Require(l_rms > (0.40 * s_rms) && l_rms < (0.75 * s_rms),
            "linear azimuth blending should interpolate between adjacent looks");
  }
}
