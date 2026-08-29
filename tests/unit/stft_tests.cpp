#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/fft_backend.hpp"
#include "dsp/streaming_stft.hpp"
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

constexpr double kReconAbsTol = 2.0e-4;
constexpr std::uint32_t kFs = 44100;

std::vector<float> Sine(const std::size_t frames, const double freq_hz, const std::uint32_t fs)
{
  std::vector<float> out(frames, 0.0F);
  const double w = 2.0 * 3.14159265358979323846 * freq_hz / static_cast<double>(fs);
  for (std::size_t i = 0; i < frames; ++i)
  {
    out[i] = static_cast<float>(std::sin(w * static_cast<double>(i)));
  }
  return out;
}

std::vector<float> LcgNoise(const std::size_t frames, const std::uint32_t seed)
{
  std::vector<float> out(frames, 0.0F);
  std::uint32_t s = seed;
  for (std::size_t i = 0; i < frames; ++i)
  {
    s = (1664525U * s) + 1013904223U;
    const float u = static_cast<float>(s >> 8) / static_cast<float>(1U << 24);
    out[i] = (2.0F * u) - 1.0F;
  }
  return out;
}

std::size_t MeasureImpulseDelay(sonitude::dsp::StreamingStft& stft,
                                const std::size_t impulse_at,
                                const std::size_t length)
{
  std::vector<float> in(length, 0.0F);
  std::vector<float> out(length, 0.0F);
  in[impulse_at] = 1.0F;
  stft.reset();
  stft.process(in, out);
  std::size_t peak = impulse_at;
  float best = 0.0F;
  for (std::size_t i = 0; i < length; ++i)
  {
    const float a = std::fabs(out[i]);
    if (a > best)
    {
      best = a;
      peak = i;
    }
  }
  Require(best > 0.5F, "impulse reconstruction peak too small");
  Require(peak >= impulse_at, "impulse peak arrived before the input sample");
  return peak - impulse_at;
}

double MaxAbsErr(const std::vector<float>& a,
                 const std::vector<float>& b,
                 const std::size_t skip,
                 const std::size_t tail)
{
  const std::size_t end = a.size() - tail;
  double m = 0.0;
  for (std::size_t i = skip; i < end; ++i)
  {
    m = std::max(m, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
  }
  return m;
}

bool AllFinite(const std::vector<float>& x)
{
  return std::all_of(x.begin(), x.end(), [](const float v) { return std::isfinite(v); });
}

void ProcessIrregular(sonitude::dsp::StreamingStft& stft,
                      const std::vector<float>& in,
                      std::vector<float>& out,
                      const std::vector<std::size_t>& chunks)
{
  out.assign(in.size(), 0.0F);
  std::size_t pos = 0;
  std::size_t ci = 0;
  while (pos < in.size())
  {
    const std::size_t n = std::min(chunks[ci % chunks.size()], in.size() - pos);
    stft.process(std::span<const float>(in.data() + pos, n), std::span<float>(out.data() + pos, n));
    pos += n;
    ++ci;
  }
}

void TestFftRoundtrip()
{
  sonitude::dsp::Radix2Fft fft;
  Require(fft.prepare(128), "128-point FFT should prepare");
  std::vector<float> time(128, 0.0F);
  std::vector<float> re(128, 0.0F);
  std::vector<float> im(128, 0.0F);
  std::vector<float> back(128, 0.0F);
  for (std::size_t i = 0; i < 128; ++i)
  {
    time[i] = static_cast<float>(i % 17) * 0.01F;
  }
  fft.forward(time.data(), re.data(), im.data());
  fft.inverse(re.data(), im.data(), back.data());
  for (std::size_t i = 0; i < 128; ++i)
  {
    Require(std::fabs(back[i] - time[i]) < 2.0e-5F, "FFT round-trip must be identity at 128");
  }
}

void TestPrepareRejects()
{
  sonitude::dsp::StreamingStft stft;
  Require(!stft.prepare(44100.0, 64, {.fft_size = 512, .hop_size = 128}),
          "512/128 must be rejected for the low-latency preset");
  Require(!stft.prepare(44100.0, 64, {.fft_size = 128, .hop_size = 64}),
          "non-75% hop must be rejected");
  Require(!stft.prepare(0.0, 64, {.fft_size = 128, .hop_size = 32}), "zero sample rate must fail");
  Require(!stft.prepare(44100.0, 0, {.fft_size = 128, .hop_size = 32}), "zero max block must fail");
  Require(stft.prepare(48000.0, 64, {.fft_size = 128, .hop_size = 32}),
          "runtime 48 kHz with 128/32 must be accepted");
  Require(stft.prepare(44100.0, 256, {.fft_size = 256, .hop_size = 64}),
          "256/64 must be available as an optional config pair");
}

void TestUnityReconstruction(const sonitude::dsp::StreamingStftConfig& cfg, const std::uint32_t fs)
{
  sonitude::dsp::StreamingStft stft;
  Require(stft.prepare(static_cast<double>(fs), 256, cfg), "supported STFT config must prepare");
  const std::size_t n = 4096;
  const std::size_t delay = MeasureImpulseDelay(stft, 200, n);
  Require(delay > 0, "streaming STFT must have positive first-arrival delay");

  const auto check = [&](const std::vector<float>& in, const char* label) {
    std::vector<float> out(in.size(), 0.0F);
    stft.reset();
    stft.process(in, out);
    Require(AllFinite(out), std::string(label) + " produced non-finite samples");
    std::vector<float> aligned(in.size(), 0.0F);
    for (std::size_t i = delay; i < in.size(); ++i)
    {
      aligned[i] = in[i - delay];
    }
    const std::size_t skip = delay + cfg.fft_size;
    const std::size_t tail = cfg.fft_size;
    const double err = MaxAbsErr(out, aligned, skip, tail);
    Require(err < kReconAbsTol, std::string(label) + " reconstruction error " + std::to_string(err));
  };

  std::vector<float> impulse(n, 0.0F);
  impulse[200] = 1.0F;
  check(impulse, "impulse@200");
  impulse.assign(n, 0.0F);
  impulse[201] = 1.0F;
  check(impulse, "impulse@201");
  impulse.assign(n, 0.0F);
  impulse[333] = 1.0F;
  check(impulse, "impulse@333");

  std::vector<float> dc(n, 0.25F);
  check(dc, "dc");

  std::vector<float> nyquist(n, 0.0F);
  for (std::size_t i = 0; i < n; ++i)
  {
    nyquist[i] = ((i % 2U) == 0) ? 0.5F : -0.5F;
  }
  check(nyquist, "nyquist");

  const double bin_hz = static_cast<double>(fs) / static_cast<double>(cfg.fft_size);
  check(Sine(n, 4.0 * bin_hz, fs), "bin-centred");
  check(Sine(n, 5.3 * bin_hz, fs), "off-bin");
  check(std::vector<float>(n, 0.0F), "silence");
  check(LcgNoise(n, 0xC0FFEEu), "broadband");
}

void TestChunkInvariance()
{
  sonitude::dsp::StreamingStft a;
  sonitude::dsp::StreamingStft b;
  const sonitude::dsp::StreamingStftConfig cfg{.fft_size = 128, .hop_size = 32};
  Require(a.prepare(44100.0, 256, cfg), "chunk-A prepare");
  Require(b.prepare(44100.0, 256, cfg), "chunk-B prepare");
  const auto in = LcgNoise(3000, 7);
  std::vector<float> one(in.size(), 0.0F);
  std::vector<float> many;
  a.process(in, one);
  ProcessIrregular(b, in, many, {1, 3, 17, 64, 7, 128, 2});
  const double err = MaxAbsErr(one, many, 0, 0);
  Require(err < 1.0e-6, "chunk invariance failed: " + std::to_string(err));
}

void TestResetAndPrepareCycles()
{
  sonitude::dsp::StreamingStft stft;
  Require(stft.prepare(44100.0, 64, {.fft_size = 128, .hop_size = 32}), "prepare cycle 1");
  std::vector<float> in(512, 0.1F);
  std::vector<float> out1(512, 0.0F);
  std::vector<float> out2(512, 0.0F);
  stft.process(in, out1);
  stft.reset();
  stft.process(in, out2);
  Require(MaxAbsErr(out1, out2, 0, 0) == 0.0, "reset must restore startup state exactly");
  Require(stft.prepare(48000.0, 128, {.fft_size = 256, .hop_size = 64}), "re-prepare 256/64");
  stft.reset();
  Require(stft.prepare(44100.0, 64, {.fft_size = 128, .hop_size = 32}), "re-prepare 128/32");
}

void TestNoAllocAfterPrepare()
{
  sonitude::dsp::StreamingStft stft;
  Require(stft.prepare(44100.0, 256, {.fft_size = 128, .hop_size = 32}), "alloc-test prepare");
  const auto in = LcgNoise(2048, 99);
  std::vector<float> out(in.size(), 0.0F);
  const auto before = sonitude::tests::support::AllocationCount();
  stft.process(in, out);
  stft.process(std::span<const float>(in.data(), 17), std::span<float>(out.data(), 17));
  stft.reset();
  stft.process(in, out);
  const auto after = sonitude::tests::support::AllocationCount();
  Require(after == before, "StreamingStft::process/reset must not allocate after prepare");
}

void TestReportedLookahead()
{
  sonitude::dsp::StreamingStft stft;
  Require(stft.prepare(44100.0, 64, {.fft_size = 128, .hop_size = 32}), "lookahead prepare");
  Require(stft.calculatedLookaheadSamples() == 96, "N-H for 128/32 is 96 samples (calculated)");
  const std::size_t measured = MeasureImpulseDelay(stft, 80, 2048);
  Require(MeasureImpulseDelay(stft, 240, 2048) == measured,
          "impulse delay must be independent of impulse position");
  Require(measured >= stft.calculatedLookaheadSamples(),
          "measured first-arrival should be at least the calculated N-H lookahead");
  // Locked from this fixture: periodic Hann, hop-synchronous OLA, host radix-2.
  Require(measured == 127,
          "128/32 first-arrival delay must stay 127 samples, got " + std::to_string(measured));

  sonitude::dsp::StreamingStft stft256;
  Require(stft256.prepare(44100.0, 64, {.fft_size = 256, .hop_size = 64}), "256 lookahead prepare");
  Require(stft256.calculatedLookaheadSamples() == 192, "N-H for 256/64 is 192 samples (calculated)");
  const std::size_t measured256 = MeasureImpulseDelay(stft256, 80, 2048);
  Require(measured256 == 255,
          "256/64 first-arrival delay must stay 255 samples, got " + std::to_string(measured256));
}
}  // namespace

void RunStftTests()
{
  TestFftRoundtrip();
  TestPrepareRejects();
  TestUnityReconstruction({.fft_size = 128, .hop_size = 32}, kFs);
  TestUnityReconstruction({.fft_size = 256, .hop_size = 64}, kFs);
  TestUnityReconstruction({.fft_size = 128, .hop_size = 32}, 48000);
  TestChunkInvariance();
  TestResetAndPrepareCycles();
  TestNoAllocAfterPrepare();
  TestReportedLookahead();
}
