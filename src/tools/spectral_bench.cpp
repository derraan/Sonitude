#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "dsp/spectral_postfilter.hpp"

namespace
{
std::vector<float> Lcg(const std::size_t n, std::uint32_t seed)
{
  std::vector<float> out(n, 0.0F);
  for (std::size_t i = 0; i < n; ++i)
  {
    seed = (1664525U * seed) + 1013904223U;
    const float u = static_cast<float>(seed >> 8) / static_cast<float>(1U << 24);
    out[i] = (2.0F * u) - 1.0F;
  }
  return out;
}

double PercentileUs(std::vector<double> samples, const double p)
{
  if (samples.empty())
  {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double idx = p * static_cast<double>(samples.size() - 1U);
  const std::size_t i = static_cast<std::size_t>(idx);
  const double frac = idx - static_cast<double>(i);
  if (i + 1 >= samples.size())
  {
    return samples.back();
  }
  return samples[i] + (frac * (samples[i + 1] - samples[i]));
}

void RunConfig(const std::string& label,
               const sonitude::dsp::SpectralPostfilterConfig& cfg,
               const std::uint32_t fs,
               const std::size_t block,
               const std::size_t blocks)
{
  sonitude::dsp::SpectralPostfilter pf;
  if (!pf.prepare(static_cast<double>(fs), block, cfg))
  {
    std::cout << label << ": PREPARE_FAILED\n";
    return;
  }
  auto in = Lcg(block, 123);
  std::vector<float> out(block, 0.0F);
  pf.setControl(true, 1.0F);
  pf.process(in, out);  // warmup

  std::vector<double> times_us;
  times_us.reserve(blocks);
  for (std::size_t b = 0; b < blocks; ++b)
  {
    const auto t0 = std::chrono::steady_clock::now();
    pf.process(in, out);
    const auto t1 = std::chrono::steady_clock::now();
    times_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }

  const double block_us = 1.0e6 * static_cast<double>(block) / static_cast<double>(fs);
  const double med = PercentileUs(times_us, 0.50);
  const double p95 = PercentileUs(times_us, 0.95);
  const double p99 = PercentileUs(times_us, 0.99);
  const double mx = *std::max_element(times_us.begin(), times_us.end());
  const double mean = std::accumulate(times_us.begin(), times_us.end(), 0.0) / static_cast<double>(times_us.size());

  std::cout << label
            << " compiler=GNU/MinGW"
            << " target=host"
            << " sample_rate_hz=" << fs
            << " block_frames=" << block
            << " fft=" << cfg.fft_size
            << " hop=" << cfg.hop_size
            << " channels_at_integration=1"
            << " persistent_bytes=" << pf.persistentBytes()
            << " delay_samples=" << pf.algorithmicDelaySamples()
            << " mean_us=" << mean
            << " median_us=" << med
            << " p95_us=" << p95
            << " p99_us=" << p99
            << " max_us=" << mx
            << " block_deadline_us=" << block_us
            << " median_util=" << (med / block_us)
            << " max_util=" << (mx / block_us)
            << " overruns=NOT_MEASURED"
            << " end_to_end_latency=NOT_MEASURED"
            << " pico2w=NOT_MEASURED"
            << " stm32h7=NOT_MEASURED"
            << '\n';
}
}  // namespace

int main()
{
  std::cout << "Host-only spectral postfilter benchmark. Not MCU evidence.\n";
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kBlock = 64;
  constexpr std::size_t kBlocks = 4000;
  RunConfig("spectral_128_32",
            {.enabled = true, .fft_size = 128, .hop_size = 32, .gain_floor_db = -12.0F},
            kFs,
            kBlock,
            kBlocks);
  RunConfig("spectral_256_64",
            {.enabled = true, .fft_size = 256, .hop_size = 64, .gain_floor_db = -12.0F},
            kFs,
            kBlock,
            kBlocks);
  return 0;
}
