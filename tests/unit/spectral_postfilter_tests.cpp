#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/spectral_postfilter.hpp"
#include "tests/support/alloc_counter.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

std::vector<float> Sine(const std::size_t n, const double freq, const double fs, const float amp)
{
  std::vector<float> out(n, 0.0F);
  const double w = 2.0 * 3.14159265358979323846 * freq / fs;
  for (std::size_t i = 0; i < n; ++i)
  {
    out[i] = amp * static_cast<float>(std::sin(w * static_cast<double>(i)));
  }
  return out;
}

std::vector<float> Lcg(const std::size_t n, std::uint32_t seed, const float amp)
{
  std::vector<float> out(n, 0.0F);
  for (std::size_t i = 0; i < n; ++i)
  {
    seed = (1664525U * seed) + 1013904223U;
    const float u = static_cast<float>(seed >> 8) / static_cast<float>(1U << 24);
    out[i] = amp * ((2.0F * u) - 1.0F);
  }
  return out;
}

double Rms(const std::vector<float>& x, const std::size_t skip)
{
  if (skip >= x.size())
  {
    return 0.0;
  }
  double s = 0.0;
  for (std::size_t i = skip; i < x.size(); ++i)
  {
    s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  }
  return std::sqrt(s / static_cast<double>(x.size() - skip));
}

bool AllFinite(const std::vector<float>& x)
{
  return std::all_of(x.begin(), x.end(), [](const float v) { return std::isfinite(v); });
}

void TestTrackerIndependent()
{
  sonitude::dsp::MinimaNoiseTracker tracker;
  Require(tracker.prepare(8, 1378.125), "tracker prepare");
  std::vector<float> p(8, 0.04F);
  tracker.update(p, true);
  for (int hop = 0; hop < 200; ++hop)
  {
    tracker.update(p, true);
  }
  for (const float n : tracker.noisePower())
  {
    Require(std::fabs(n - 0.04F) < 0.01F, "tracker should sit on stationary power");
  }
  std::vector<float> high(8, 0.4F);
  const float before = tracker.noisePower()[0];
  tracker.update(high, false);
  Require(tracker.noisePower()[0] <= before + 1.0e-6F, "frozen tracker must not rise");
  for (int hop = 0; hop < 8; ++hop)
  {
    tracker.update(high, true);
  }
  Require(tracker.noisePower()[0] < 0.2F, "allowed rise must still be slow versus an 8-hop step");
  tracker.reset();
  std::vector<float> mid(8, 0.04F);
  tracker.update(mid, true);
  std::vector<float> low(8, 0.001F);
  for (int hop = 0; hop < 200; ++hop)
  {
    tracker.update(low, true);
  }
  Require(tracker.noisePower()[0] < 0.008F, "minima must follow a sustained downward step");
}

void TestWienerIndependent()
{
  sonitude::dsp::BoundedWienerGain wiener;
  Require(wiener.prepare(8, 1378.125, 0.25F), "wiener prepare");
  std::vector<float> p(8, 1.0e-4F);
  std::vector<float> n(8, 1.0e-2F);
  std::vector<float> g(8, 0.0F);
  for (int hop = 0; hop < 40; ++hop)
  {
    wiener.compute(p, n, g);
  }
  for (const float gk : g)
  {
    Require(gk >= 0.25F && gk <= 1.0F, "gain must stay in [floor, 1]");
    Require(gk < 0.35F, "noise-dominated bins should sit near the floor");
  }
  std::fill(p.begin(), p.end(), 1.0F);
  std::fill(n.begin(), n.end(), 1.0e-4F);
  for (int hop = 0; hop < 80; ++hop)
  {
    wiener.compute(p, n, g);
  }
  for (const float gk : g)
  {
    Require(gk > 0.9F, "high a-posteriori SNR should open the Wiener gain");
    Require(gk <= 1.0F, "no amplification");
  }
}

void TestMalformedPrepare()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(!pf.prepare(44100.0, 64, {.enabled = false, .fft_size = 128, .hop_size = 32}),
          "disabled config must fail prepare");
  Require(!pf.prepare(44100.0, 64, {.enabled = true, .fft_size = 512, .hop_size = 128}),
          "512/128 must fail");
  Require(!pf.prepare(44100.0, 64, {.enabled = true, .fft_size = 128, .hop_size = 32, .gain_floor_db = 3.0F}),
          "positive gain floor (amplification) must fail");
  Require(pf.prepare(44100.0, 64, {.enabled = true, .fft_size = 128, .hop_size = 32, .gain_floor_db = -12.0F}),
          "default experimental config must prepare");
}

void TestFiniteAndFloor()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "prepare");
  std::vector<float> in(2048, 0.0F);
  std::vector<float> out(2048, 0.0F);
  pf.process(in, out);
  Require(AllFinite(out), "silence must stay finite");

  in[10] = std::numeric_limits<float>::quiet_NaN();
  in[11] = std::numeric_limits<float>::infinity();
  in[12] = std::numeric_limits<float>::denorm_min();
  pf.reset();
  pf.process(in, out);
  Require(AllFinite(out), "NaN/Inf/denormal input must not leak non-finite output");

  in = std::vector<float>(4096, 0.99F);
  out.assign(in.size(), 0.0F);
  pf.reset();
  pf.setControl(true, 1.0F);
  pf.process(in, out);
  Require(AllFinite(out), "full-scale DC must stay finite");
}

void TestNoiseOnlyDoesNotOpen()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "prepare");
  const auto in = Lcg(44100, 42, 0.2F);
  std::vector<float> out(in.size(), 0.0F);
  pf.setControl(true, 1.0F);
  pf.process(in, out);
  const std::size_t skip = pf.algorithmicDelaySamples() + 6000;
  const double in_rms = Rms(in, skip);
  const double out_rms = Rms(out, skip);
  const double ratio = out_rms / std::max(in_rms, 1.0e-20);
  Require(ratio < 0.6,
          "noise-only output RMS should drop versus input (fixture), ratio=" + std::to_string(ratio) +
              " gain=" + std::to_string(pf.currentGain()));
  Require(pf.currentGain() < 0.55F,
          "noise-only estimator must not open toward unity, gain=" + std::to_string(pf.currentGain()));
}

void TestCleanSineNegativeControl()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "prepare");
  const double bin = 44100.0 / 128.0;
  std::vector<float> in(44100, 0.0F);
  const auto tone = Sine(38000, 4.0 * bin, 44100.0, 0.3F);
  std::copy(tone.begin(), tone.end(), in.begin() + 4000);
  std::vector<float> out(in.size(), 0.0F);
  pf.setControl(true, 1.0F);
  pf.process(in, out);
  const std::size_t delay = pf.algorithmicDelaySamples();
  const std::size_t a = 8000 + delay;
  const std::size_t b = 30000;
  double in_e = 0.0;
  double out_e = 0.0;
  for (std::size_t i = a; i < b; ++i)
  {
    in_e += static_cast<double>(in[i - delay]) * static_cast<double>(in[i - delay]);
    out_e += static_cast<double>(out[i]) * static_cast<double>(out[i]);
  }
  const double level_err_db = 10.0 * std::log10(out_e / std::max(in_e, 1.0e-20));
  Require(level_err_db > -6.0, "clean-tone negative control: attenuation exceeded 6 dB fixture bound");
  Require(level_err_db < 1.0, "clean-tone negative control: unexpected amplification");
}

void TestChunkResetAllocDelay()
{
  sonitude::dsp::SpectralPostfilter a;
  sonitude::dsp::SpectralPostfilter b;
  const sonitude::dsp::SpectralPostfilterConfig cfg{.enabled = true, .gain_floor_db = -12.0F};
  Require(a.prepare(44100.0, 256, cfg) && b.prepare(44100.0, 256, cfg), "prepare pair");
  Require(a.algorithmicDelaySamples() == 127, "postfilter delay must match STFT first-arrival");
  const auto in = Lcg(5000, 9, 0.15F);
  std::vector<float> one(in.size(), 0.0F);
  std::vector<float> many(in.size(), 0.0F);
  a.setControl(true, 1.0F);
  b.setControl(true, 1.0F);
  a.process(in, one);
  std::size_t pos = 0;
  const std::size_t chunks[] = {1, 5, 64, 13, 128, 7};
  std::size_t ci = 0;
  while (pos < in.size())
  {
    const std::size_t n = std::min(chunks[ci % 6], in.size() - pos);
    b.process(std::span<const float>(in.data() + pos, n), std::span<float>(many.data() + pos, n));
    pos += n;
    ++ci;
  }
  double err = 0.0;
  for (std::size_t i = 0; i < in.size(); ++i)
  {
    err = std::max(err, std::fabs(static_cast<double>(one[i]) - static_cast<double>(many[i])));
  }
  Require(err < 2.0e-5, "spectral postfilter chunk invariance failed");

  std::vector<float> r1(2048, 0.0F);
  std::vector<float> r2(2048, 0.0F);
  std::vector<float> x(2048, 0.05F);
  a.reset();
  a.process(x, r1);
  a.reset();
  a.process(x, r2);
  Require(AllFinite(r1) && AllFinite(r2), "reset outputs finite");
  for (std::size_t i = 0; i < x.size(); ++i)
  {
    Require(r1[i] == r2[i], "reset must be deterministic");
  }

  const auto before = sonitude::tests::support::AllocationCount();
  a.process(in, one);
  a.setEstimatorHold(true);
  a.process(std::span<const float>(in.data(), 64), std::span<float>(one.data(), 64));
  const auto after = sonitude::tests::support::AllocationCount();
  Require(after == before,
          "process must not allocate, delta=" + std::to_string(after - before));
}

void TestHoldFreezesNoise()
{
  sonitude::dsp::MinimaNoiseTracker tracker;
  Require(tracker.prepare(4, 1000.0), "hold tracker");
  std::vector<float> p(4, 0.02F);
  tracker.update(p, true);
  const float frozen = tracker.noisePower()[0];
  std::fill(p.begin(), p.end(), 0.5F);
  tracker.update(p, false);
  Require(std::fabs(tracker.noisePower()[0] - frozen) < 1.0e-6F, "hold must freeze upward noise updates");
}
}  // namespace

void RunSpectralPostfilterTests()
{
  TestTrackerIndependent();
  TestWienerIndependent();
  TestMalformedPrepare();
  TestFiniteAndFloor();
  TestNoiseOnlyDoesNotOpen();
  TestCleanSineNegativeControl();
  TestChunkResetAllocDelay();
  TestHoldFreezesNoise();
}
