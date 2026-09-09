#include "dsp/biquad_cascade.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>

namespace sonitude::dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

std::string Lower(const std::string& s)
{
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}
}  // namespace

BiquadType BiquadCascade::ParseType(const std::string& value)
{
  const std::string v = Lower(value);
  if (v == "pk" || v == "peak")
  {
    return BiquadType::Peak;
  }
  if (v == "ls" || v == "lowshelf" || v == "low_shelf")
  {
    return BiquadType::LowShelf;
  }
  if (v == "hs" || v == "highshelf" || v == "high_shelf")
  {
    return BiquadType::HighShelf;
  }
  if (v == "lp" || v == "lowpass" || v == "low_pass")
  {
    return BiquadType::LowPass;
  }
  if (v == "hp" || v == "highpass" || v == "high_pass")
  {
    return BiquadType::HighPass;
  }
  throw std::runtime_error("Unknown biquad type: " + value);
}

BiquadCascade::Coeff BiquadCascade::Design(const BiquadSectionSpec& spec,
                                           const std::uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0U)
  {
    throw std::runtime_error("Biquad sample_rate_hz must be non-zero");
  }
  if (!(spec.freq_hz > 0.0F && spec.freq_hz < (0.5F * static_cast<float>(sample_rate_hz))))
  {
    throw std::runtime_error("Biquad freq_hz out of range");
  }
  if (!(spec.q > 0.0F))
  {
    throw std::runtime_error("Biquad q must be > 0");
  }

  const double omega = (2.0 * kPi * static_cast<double>(spec.freq_hz)) /
                       static_cast<double>(sample_rate_hz);
  const double sn = std::sin(omega);
  const double cs = std::cos(omega);
  const double q = static_cast<double>(spec.q);
  const double a = std::pow(10.0, static_cast<double>(spec.gain_db) / 40.0);
  const double alpha = sn / (2.0 * q);
  const double sqrt_a = std::sqrt(a);

  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a0 = 1.0;
  double a1 = 0.0;
  double a2 = 0.0;

  switch (spec.type)
  {
    case BiquadType::Peak:
      b0 = 1.0 + (alpha * a);
      b1 = -2.0 * cs;
      b2 = 1.0 - (alpha * a);
      a0 = 1.0 + (alpha / a);
      a1 = -2.0 * cs;
      a2 = 1.0 - (alpha / a);
      break;
    case BiquadType::LowShelf:
      b0 = a * ((a + 1.0) - ((a - 1.0) * cs) + (2.0 * sqrt_a * alpha));
      b1 = 2.0 * a * ((a - 1.0) - ((a + 1.0) * cs));
      b2 = a * ((a + 1.0) - ((a - 1.0) * cs) - (2.0 * sqrt_a * alpha));
      a0 = (a + 1.0) + ((a - 1.0) * cs) + (2.0 * sqrt_a * alpha);
      a1 = -2.0 * ((a - 1.0) + ((a + 1.0) * cs));
      a2 = (a + 1.0) + ((a - 1.0) * cs) - (2.0 * sqrt_a * alpha);
      break;
    case BiquadType::HighShelf:
      b0 = a * ((a + 1.0) + ((a - 1.0) * cs) + (2.0 * sqrt_a * alpha));
      b1 = -2.0 * a * ((a - 1.0) + ((a + 1.0) * cs));
      b2 = a * ((a + 1.0) + ((a - 1.0) * cs) - (2.0 * sqrt_a * alpha));
      a0 = (a + 1.0) - ((a - 1.0) * cs) + (2.0 * sqrt_a * alpha);
      a1 = 2.0 * ((a - 1.0) - ((a + 1.0) * cs));
      a2 = (a + 1.0) - ((a - 1.0) * cs) - (2.0 * sqrt_a * alpha);
      break;
    case BiquadType::LowPass:
      b0 = (1.0 - cs) * 0.5;
      b1 = 1.0 - cs;
      b2 = (1.0 - cs) * 0.5;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    case BiquadType::HighPass:
      b0 = (1.0 + cs) * 0.5;
      b1 = -(1.0 + cs);
      b2 = (1.0 + cs) * 0.5;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
  }

  if (std::fabs(a0) < 1.0e-12)
  {
    throw std::runtime_error("Biquad design produced invalid a0");
  }
  return {
      .b0 = b0 / a0,
      .b1 = b1 / a0,
      .b2 = b2 / a0,
      .a1 = a1 / a0,
      .a2 = a2 / a0,
  };
}

void BiquadCascade::configure(const std::uint32_t sample_rate_hz,
                              const std::span<const BiquadSectionSpec> sections,
                              const std::size_t channels,
                              const bool enabled)
{
  if (channels == 0U || channels > kMaxChannels)
  {
    throw std::runtime_error("Biquad channels out of range");
  }
  if (sections.size() > kMaxSections)
  {
    throw std::runtime_error("Biquad section count exceeds kMaxSections");
  }

  channels_ = channels;
  enabled_ = enabled;
  section_count_ = 0U;
  for (const auto& sec : sections)
  {
    if (!sec.enabled)
    {
      continue;
    }
    coeffs_[section_count_] = Design(sec, sample_rate_hz);
    ++section_count_;
  }
  configured_ = true;
  reset();
}

void BiquadCascade::reset()
{
  for (std::size_t s = 0; s < kMaxSections; ++s)
  {
    for (std::size_t ch = 0; ch < kMaxChannels; ++ch)
    {
      states_[s][ch] = {};
    }
  }
}

float BiquadCascade::processSample(const std::size_t channel, const float x)
{
  if (!configured_)
  {
    throw std::runtime_error("Biquad used before configure");
  }
  if (channel >= channels_)
  {
    throw std::runtime_error("Biquad channel index out of range");
  }
  if (!enabled_)
  {
    return x;
  }

  double y = static_cast<double>(x);
  for (std::size_t s = 0; s < section_count_; ++s)
  {
    auto& st = states_[s][channel];
    const auto& c = coeffs_[s];
    const double out = (c.b0 * y) + st.z1;
    st.z1 = (c.b1 * y) - (c.a1 * out) + st.z2;
    st.z2 = (c.b2 * y) - (c.a2 * out);
    y = out;
  }
  return static_cast<float>(y);
}

void BiquadCascade::processMono(const std::span<float> mono)
{
  for (float& sample : mono)
  {
    sample = processSample(0U, sample);
  }
}

void BiquadCascade::processMicFrame(audio::MicFrame& frame)
{
  for (std::size_t ch = 0; ch < channels_; ++ch)
  {
    frame[ch] = processSample(ch, frame[ch]);
  }
}
}  // namespace sonitude::dsp
