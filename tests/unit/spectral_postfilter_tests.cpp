#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/beamformer.hpp"
#include "dsp/spectral_postfilter.hpp"
#include "dsp/streaming_stft.hpp"
#include "tests/support/alloc_counter.hpp"
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

constexpr std::size_t kSharedFftDelay = 127;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> Sine(const std::size_t n, const double freq, const double fs, const float amp)
{
  std::vector<float> out(n, 0.0F);
  const double w = 2.0 * kPi * freq / fs;
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

struct ToneProjection
{
  double target_rms = 0.0;
  double residual_rms = 0.0;
};

ToneProjection ProjectTone(const std::vector<float>& x,
                           const double freq,
                           const double fs,
                           const std::size_t begin,
                           const std::size_t end,
                           const double time_origin)
{
  ToneProjection out;
  if (end <= begin || end > x.size())
  {
    return out;
  }
  const double n = static_cast<double>(end - begin);
  const double w = 2.0 * kPi * freq / fs;
  double c = 0.0;
  double s = 0.0;
  for (std::size_t i = begin; i < end; ++i)
  {
    const double t = w * (static_cast<double>(i) + time_origin);
    const double xi = static_cast<double>(x[i]);
    c += xi * std::cos(t);
    s += xi * std::sin(t);
  }
  const double ac = 2.0 * c / n;
  const double as = 2.0 * s / n;
  out.target_rms = std::hypot(ac, as) / std::sqrt(2.0);
  double resid = 0.0;
  for (std::size_t i = begin; i < end; ++i)
  {
    const double t = w * (static_cast<double>(i) + time_origin);
    const double pred = (ac * std::cos(t)) + (as * std::sin(t));
    const double e = static_cast<double>(x[i]) - pred;
    resid += e * e;
  }
  out.residual_rms = std::sqrt(resid / n);
  return out;
}

double DbRatio(const double num, const double den)
{
  return 20.0 * std::log10(std::max(num, 1.0e-20) / std::max(den, 1.0e-20));
}

bool AllFinite(const std::vector<float>& x)
{
  return std::all_of(x.begin(), x.end(), [](const float v) { return std::isfinite(v); });
}

struct FilterHop
{
  sonitude::dsp::SpectralPostfilter* pf = nullptr;
};

void OnFilterHop(void* context, float* re, float* im, const std::size_t fft_size) noexcept
{
  auto* ctx = static_cast<FilterHop*>(context);
  if (ctx == nullptr || ctx->pf == nullptr || re == nullptr || im == nullptr)
  {
    return;
  }
  ctx->pf->processSpectrum(std::span<float>(re, fft_size), std::span<float>(im, fft_size));
}

struct SharedStftFilter
{
  sonitude::dsp::StreamingStft stft;
  FilterHop ctx{};

  bool prepare(const double fs, sonitude::dsp::SpectralPostfilter& pf)
  {
    ctx.pf = &pf;
    return stft.prepare(fs, 256, {.fft_size = 128, .hop_size = 32, .synthesize = true});
  }

  void process(const std::span<const float> input, const std::span<float> output)
  {
    stft.process(input, output, &OnFilterHop, &ctx);
  }

  void reset() { stft.reset(); }
};

void FilterPcm(sonitude::dsp::SpectralPostfilter& pf,
               const std::span<const float> input,
               const std::span<float> output)
{
  SharedStftFilter host;
  Require(host.prepare(44100.0, pf), "shared STFT host must prepare");
  host.process(input, output);
}

sonitude::app::GeometryConfig TestGeometry()
{
  sonitude::app::GeometryConfig g;
  g.profile_name = "unit_test_geometry";
  g.microphones = {
      {"M0", -0.038, 0.168, 0.0}, {"M1", 0.038, 0.168, 0.0}, {"M2", -0.090, 0.050, 0.0},
      {"M3", 0.090, 0.050, 0.0},  {"M4", -0.060, 0.000, 0.0}, {"M5", 0.060, 0.000, 0.0},
  };
  return g;
}

sonitude::app::CalibrationConfig TestCalibration()
{
  sonitude::app::CalibrationConfig c;
  c.sample_rate_hz = 44100;
  c.channels.resize(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < c.channels.size(); ++i)
  {
    c.channels[i].id = "M" + std::to_string(i);
    c.channels[i].polarity = 1;
    c.channels[i].gain_linear = 1.0F;
    c.channels[i].delay_samples = 0.0F;
  }
  return c;
}

void TestTrackerIndependent()
{
  sonitude::dsp::AsymmetricNoisePowerTracker tracker;
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
  FilterPcm(pf, in, out);
  Require(AllFinite(out), "silence must stay finite");

  in[10] = std::numeric_limits<float>::quiet_NaN();
  in[11] = std::numeric_limits<float>::infinity();
  in[12] = std::numeric_limits<float>::denorm_min();
  pf.reset();
  FilterPcm(pf, in, out);
  Require(AllFinite(out), "NaN/Inf/denormal input must not leak non-finite output");

  in = std::vector<float>(4096, 0.99F);
  out.assign(in.size(), 0.0F);
  pf.reset();
  pf.setControl(true, 1.0F);
  FilterPcm(pf, in, out);
  Require(AllFinite(out), "full-scale DC must stay finite");
}

void TestNoiseOnlyDoesNotOpen()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "prepare");
  const auto in = Lcg(44100, 42, 0.2F);
  std::vector<float> out(in.size(), 0.0F);
  pf.setControl(true, 1.0F);
  FilterPcm(pf, in, out);
  const std::size_t skip = kSharedFftDelay + 6000;
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
  FilterPcm(pf, in, out);
  const std::size_t delay = kSharedFftDelay;
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
  SharedStftFilter host_a;
  SharedStftFilter host_b;
  Require(host_a.prepare(44100.0, a) && host_b.prepare(44100.0, b), "shared STFT hosts");
  const auto in = Lcg(5000, 9, 0.15F);
  std::vector<float> one(in.size(), 0.0F);
  std::vector<float> many(in.size(), 0.0F);
  a.setControl(true, 1.0F);
  b.setControl(true, 1.0F);
  host_a.process(in, one);
  std::size_t pos = 0;
  const std::size_t chunks[] = {1, 5, 64, 13, 128, 7};
  std::size_t ci = 0;
  while (pos < in.size())
  {
    const std::size_t n = std::min(chunks[ci % 6], in.size() - pos);
    host_b.process(std::span<const float>(in.data() + pos, n), std::span<float>(many.data() + pos, n));
    pos += n;
    ++ci;
  }
  double err = 0.0;
  for (std::size_t i = 0; i < in.size(); ++i)
  {
    err = std::max(err, std::fabs(static_cast<double>(one[i]) - static_cast<double>(many[i])));
  }
  Require(err < 2.0e-5, "shared-STFT spectral chunk invariance failed");

  std::vector<float> r1(2048, 0.0F);
  std::vector<float> r2(2048, 0.0F);
  std::vector<float> x(2048, 0.05F);
  a.reset();
  host_a.reset();
  host_a.process(x, r1);
  a.reset();
  host_a.reset();
  host_a.process(x, r2);
  Require(AllFinite(r1) && AllFinite(r2), "reset outputs finite");
  for (std::size_t i = 0; i < x.size(); ++i)
  {
    Require(r1[i] == r2[i], "reset must be deterministic");
  }

  const auto before = sonitude::tests::support::AllocationCount();
  host_a.process(in, one);
  a.setEstimatorHold(true);
  host_a.process(std::span<const float>(in.data(), 64), std::span<float>(one.data(), 64));
  const auto after = sonitude::tests::support::AllocationCount();
  Require(after == before,
          "shared-STFT process must not allocate, delta=" + std::to_string(after - before));
}

void TestHoldFreezesNoise()
{
  sonitude::dsp::AsymmetricNoisePowerTracker tracker;
  Require(tracker.prepare(4, 1000.0), "hold tracker");
  std::vector<float> p(4, 0.02F);
  tracker.update(p, true);
  const float frozen = tracker.noisePower()[0];
  std::fill(p.begin(), p.end(), 0.5F);
  tracker.update(p, false);
  Require(std::fabs(tracker.noisePower()[0] - frozen) < 1.0e-6F, "hold must freeze upward noise updates");
  std::fill(p.begin(), p.end(), 0.001F);
  tracker.update(p, false);
  Require(std::fabs(tracker.noisePower()[0] - frozen) < 1.0e-6F, "hold must freeze downward snaps too");
}

void TestMixtureSnrWithNoiseLeadIn()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "lead-in prepare");
  const double fs = 44100.0;
  const double freq = 4.0 * fs / 128.0;
  const std::size_t n = 44100;
  const std::size_t lead = 8000;
  const auto tone = Sine(n, freq, fs, 0.25F);
  const auto noise = Lcg(n, 99, 0.15F);
  std::vector<float> mix(n, 0.0F);
  for (std::size_t i = 0; i < n; ++i)
  {
    mix[i] = noise[i] + (i >= lead ? tone[i] : 0.0F);
  }
  std::vector<float> out(n, 0.0F);
  pf.setControl(true, 1.0F);
  FilterPcm(pf, mix, out);
  const std::size_t delay = kSharedFftDelay;
  const std::size_t a = lead + 4000;
  const std::size_t b = n - delay - 500;
  const auto in_t = ProjectTone(mix, freq, fs, a, b, 0.0);
  const auto out_t = ProjectTone(out, freq, fs, a + delay, b + delay, -static_cast<double>(delay));
  const double snr_in = DbRatio(in_t.target_rms, in_t.residual_rms);
  const double snr_out = DbRatio(out_t.target_rms, out_t.residual_rms);
  const double d_snr = snr_out - snr_in;
  const double target_att = DbRatio(out_t.target_rms, in_t.target_rms);
  Require(d_snr > 0.5, "noise-lead-in mixture must improve coherent SNR, dSNR=" + std::to_string(d_snr));
  Require(target_att > -6.0, "noise-lead-in must not floor the tone, att=" + std::to_string(target_att));
}

void TestMixtureSnrSimultaneousStart()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "mixture prepare");
  const double fs = 44100.0;
  const double freq = 4.0 * fs / 128.0;
  const auto tone = Sine(44100, freq, fs, 0.25F);
  const auto noise = Lcg(44100, 99, 0.15F);
  std::vector<float> mix(44100, 0.0F);
  for (std::size_t i = 0; i < mix.size(); ++i)
  {
    mix[i] = tone[i] + noise[i];
  }
  std::vector<float> out(mix.size(), 0.0F);
  pf.setControl(true, 1.0F);
  FilterPcm(pf, mix, out);
  const std::size_t delay = kSharedFftDelay;
  const std::size_t a = 8000;
  const std::size_t b = mix.size() - delay - 500;
  const auto in_t = ProjectTone(mix, freq, fs, a, b, 0.0);
  const auto out_t = ProjectTone(out, freq, fs, a + delay, b + delay, -static_cast<double>(delay));
  const double d_snr = DbRatio(out_t.target_rms, out_t.residual_rms) - DbRatio(in_t.target_rms, in_t.residual_rms);
  const double target_att = DbRatio(out_t.target_rms, in_t.target_rms);
  const double noise_att = DbRatio(out_t.residual_rms, in_t.residual_rms);
  Require(d_snr > 0.0, "simultaneous tone+noise must not worsen coherent SNR, dSNR=" + std::to_string(d_snr));
  Require(target_att > noise_att,
          "target must not be attenuated more than residual noise, target=" + std::to_string(target_att) +
              " noise=" + std::to_string(noise_att));
}

void TestTonePresentFromStartup()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "startup prepare");
  const double fs = 44100.0;
  const double freq = 4.0 * fs / 128.0;
  const auto in = Sine(44100, freq, fs, 0.3F);
  std::vector<float> out(in.size(), 0.0F);
  pf.setControl(true, 1.0F);
  FilterPcm(pf, in, out);
  const std::size_t delay = kSharedFftDelay;
  const std::size_t a = 4000;
  const std::size_t b = in.size() - delay - 500;
  const auto in_t = ProjectTone(in, freq, fs, a, b, 0.0);
  const auto out_t = ProjectTone(out, freq, fs, a + delay, b + delay, -static_cast<double>(delay));
  const double att = DbRatio(out_t.target_rms, in_t.target_rms);
  Require(att > -6.0, "tone present from startup must not be learned as noise, att=" + std::to_string(att));
}

void TestFocusTransitionDoesNotLearnBypassAsNoise()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256, {.enabled = true, .gain_floor_db = -12.0F}), "focus prepare");
  SharedStftFilter host;
  Require(host.prepare(44100.0, pf), "focus host STFT");
  const double fs = 44100.0;
  const double freq = 4.0 * fs / 128.0;
  const auto in = Sine(44100, freq, fs, 0.3F);
  std::vector<float> out(in.size(), 0.0F);
  pf.setControl(false, 1.0F);
  host.process(std::span<const float>(in.data(), 20000), std::span<float>(out.data(), 20000));
  pf.setControl(true, 1.0F);
  host.process(std::span<const float>(in.data() + 20000, 24100), std::span<float>(out.data() + 20000, 24100));
  const std::size_t delay = kSharedFftDelay;
  const std::size_t a = 28000;
  const std::size_t b = in.size() - delay - 500;
  const auto in_t = ProjectTone(in, freq, fs, a, b, 0.0);
  const auto out_t = ProjectTone(out, freq, fs, a + delay, b + delay, -static_cast<double>(delay));
  const double att = DbRatio(out_t.target_rms, in_t.target_rms);
  Require(att > -6.0, "focus-on after unfocused speech must not floor the target, att=" + std::to_string(att));
}

void TestConfidenceThresholdIsHonored()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 256,
                    {.enabled = true, .gain_floor_db = -12.0F, .confidence_threshold = 0.9F}),
          "threshold prepare");
  const auto in = Lcg(20000, 7, 0.2F);
  std::vector<float> out(in.size(), 0.0F);
  pf.setControl(true, 0.5F);
  FilterPcm(pf, in, out);
  const std::size_t skip = kSharedFftDelay + 2000;
  const double ratio = Rms(out, skip) / std::max(Rms(in, skip), 1.0e-20);
  Require(ratio > 0.85, "below-threshold confidence must stay near unity, ratio=" + std::to_string(ratio));
}

void TestPersistentBytesCountsEstimator()
{
  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(44100.0, 64, {.enabled = true, .fft_size = 128, .hop_size = 32}), "bytes prepare");
  const std::size_t n_bins = (128U / 2U) + 1U;
  Require(pf.persistentBytes() >= (8U * n_bins * sizeof(float)),
          "persistentBytes must include tracker and gain arrays");
}

void TestTwoTalkersGuardContrast()
{
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kFrames = 44100;
  const auto geometry = TestGeometry();
  const double f_target = 4.0 * static_cast<double>(kFs) / 128.0;
  const double f_off = 8.0 * static_cast<double>(kFs) / 128.0;
  const auto src_t = sonitude::tests::support::GenerateSine(kFrames, kFs, f_target);
  const auto src_o = sonitude::tests::support::GenerateSine(kFrames, kFs, f_off);
  auto mic_t = sonitude::tests::support::GeneratePlaneWave(src_t, geometry, kFs, 0, 0.0F, 0.0F, 343.0F);
  const auto mic_o = sonitude::tests::support::GeneratePlaneWave(src_o, geometry, kFs, 0, 90.0F, 0.0F, 343.0F);
  for (std::size_t i = 0; i < kFrames; ++i)
  {
    for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
    {
      mic_t[i][ch] = (0.35F * mic_t[i][ch]) + (0.35F * mic_o[i][ch]);
    }
  }

  sonitude::app::SteeringConfig steering{};
  steering.speed_of_sound_mps = 343.0F;
  steering.reference_mic_index = 0;
  steering.steering_ramp_ms = 1.0F;
  const auto calibration = TestCalibration();

  sonitude::dsp::DelaySumBeamformer raw;
  raw.configure(geometry, steering, calibration, kFs, 256);
  raw.setTarget({0.0F, 0.0F});
  std::vector<float> unfiltered(kFrames, 0.0F);
  raw.process(mic_t, unfiltered);

  sonitude::dsp::SpectralPostfilter pf;
  Require(pf.prepare(static_cast<double>(kFs), 256, {.enabled = true, .gain_floor_db = -12.0F}),
          "two-talker prepare");
  pf.setControl(true, 1.0F);
  sonitude::dsp::DelaySumBeamformer bf;
  bf.configure(geometry, steering, calibration, kFs, 256);
  bf.setTarget({0.0F, 0.0F});
  bf.setSpectralPostfilter(&pf);
  Require(bf.algorithmicDelaySamples() == kSharedFftDelay, "MVDR first-arrival remains one STFT");
  std::vector<float> out(kFrames, 0.0F);
  bf.process(mic_t, out);

  const std::size_t a = 8000;
  const std::size_t b = kFrames - 500;
  const auto in_t = ProjectTone(unfiltered, f_target, kFs, a, b, 0.0);
  const auto out_t = ProjectTone(out, f_target, kFs, a, b, 0.0);
  const auto in_o = ProjectTone(unfiltered, f_off, kFs, a, b, 0.0);
  const auto out_o = ProjectTone(out, f_off, kFs, a, b, 0.0);
  const double att_t = DbRatio(out_t.target_rms, in_t.target_rms);
  const double att_o = DbRatio(out_o.target_rms, in_o.target_rms);
  Require(AllFinite(out), "two-talker output finite");
  Require(att_t > att_o + 1.0,
          "off-axis talker should be attenuated more than the target, target=" + std::to_string(att_t) +
              " off=" + std::to_string(att_o));
  Require(att_t > -6.0, "desired talker should not be floored, att=" + std::to_string(att_t));
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
  TestMixtureSnrWithNoiseLeadIn();
  TestMixtureSnrSimultaneousStart();
  TestTonePresentFromStartup();
  TestFocusTransitionDoesNotLearnBypassAsNoise();
  TestConfidenceThresholdIsHonored();
  TestPersistentBytesCountsEstimator();
  TestTwoTalkersGuardContrast();
}
