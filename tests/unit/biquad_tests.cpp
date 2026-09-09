#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/biquad_cascade.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestBypassIsExact()
{
  sonitude::dsp::BiquadCascade eq;
  eq.configure(44100, {}, 1U, false);
  std::vector<float> x = {0.0F, 0.25F, -0.5F, 0.75F, -1.0F, 0.4F};
  const std::vector<float> in = x;
  eq.processMono(x);
  Require(x == in, "disabled biquad must be exact bypass");
}

void TestFiniteOutput()
{
  sonitude::dsp::BiquadCascade eq;
  std::vector<sonitude::dsp::BiquadSectionSpec> specs = {
      {.type = sonitude::dsp::BiquadType::Peak, .freq_hz = 163.5F, .gain_db = 2.8F, .q = 4.776F, .enabled = true},
      {.type = sonitude::dsp::BiquadType::Peak, .freq_hz = 756.0F, .gain_db = 6.0F, .q = 4.0F, .enabled = true},
  };
  eq.configure(44100, specs, 1U, true);
  std::vector<float> x(8192, 0.0F);
  x[0] = 1.0F;
  eq.processMono(x);
  const bool finite = std::all_of(x.begin(), x.end(), [](const float v) { return std::isfinite(v); });
  Require(finite, "biquad impulse response must remain finite");
}

void TestTypeParsing()
{
  Require(sonitude::dsp::BiquadCascade::ParseType("PK") == sonitude::dsp::BiquadType::Peak,
          "PK parse failed");
  Require(sonitude::dsp::BiquadCascade::ParseType("LS") == sonitude::dsp::BiquadType::LowShelf,
          "LS parse failed");
  bool threw = false;
  try
  {
    (void)sonitude::dsp::BiquadCascade::ParseType("nope");
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "unknown type must throw");
}

void TestNoAlgorithmicDelayForBypass()
{
  sonitude::dsp::BiquadCascade eq;
  eq.configure(44100, {}, 1U, false);
  std::vector<float> x(32, 0.0F);
  x[0] = 1.0F;
  eq.processMono(x);
  Require(std::fabs(x[0] - 1.0F) < 1.0e-7F, "bypass must not shift impulse");
}

void TestPeakCenterGainMatchesDesign()
{
  constexpr std::uint32_t kFs = 44100;
  sonitude::dsp::BiquadCascade eq;
  std::vector<sonitude::dsp::BiquadSectionSpec> specs = {
      {.type = sonitude::dsp::BiquadType::Peak, .freq_hz = 1000.0F, .gain_db = 6.0F, .q = 1.2F, .enabled = true},
  };
  eq.configure(kFs, specs, 1U, true);

  std::vector<float> x(44100, 0.0F);
  for (std::size_t i = 0; i < x.size(); ++i)
  {
    x[i] = std::sin(static_cast<float>(2.0 * 3.14159265358979323846 * 1000.0 *
                                       static_cast<double>(i) / static_cast<double>(kFs)));
  }
  std::vector<float> y = x;
  eq.processMono(y);

  double in_rms = 0.0;
  double out_rms = 0.0;
  const std::size_t settle = 1024;
  for (std::size_t i = settle; i < x.size(); ++i)
  {
    in_rms += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    out_rms += static_cast<double>(y[i]) * static_cast<double>(y[i]);
  }
  in_rms = std::sqrt(in_rms / static_cast<double>(x.size() - settle));
  out_rms = std::sqrt(out_rms / static_cast<double>(x.size() - settle));
  const double gain_db = 20.0 * std::log10(out_rms / std::max(in_rms, 1.0e-12));
  Require(std::fabs(gain_db - 6.0) < 0.4, "peak filter center gain should match design within tolerance");
}

void TestEnabledCascadeHasNoBlockLatency()
{
  sonitude::dsp::BiquadCascade eq;
  std::vector<sonitude::dsp::BiquadSectionSpec> specs = {
      {.type = sonitude::dsp::BiquadType::Peak, .freq_hz = 2000.0F, .gain_db = 3.0F, .q = 1.0F, .enabled = true},
  };
  eq.configure(44100, specs, 1U, true);
  std::vector<float> x(64, 0.0F);
  x[0] = 1.0F;
  eq.processMono(x);
  Require(std::fabs(x[0]) > 1.0e-6F, "biquad should respond at sample zero (no block delay)");
}
}  // namespace

void RunBiquadTests()
{
  TestBypassIsExact();
  TestFiniteOutput();
  TestTypeParsing();
  TestNoAlgorithmicDelayForBypass();
  TestPeakCenterGainMatchesDesign();
  TestEnabledCascadeHasNoBlockLatency();
}
