#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/binaural_renderer.hpp"

namespace
{
sonitude::dsp::HrtfTable BuildBenchTable(const std::uint32_t sample_rate_hz, const std::uint32_t taps)
{
  sonitude::dsp::HrtfTable table{};
  table.sample_rate_hz = sample_rate_hz;
  table.taps_per_ear = taps;
  table.directions = {{0.0F, 0.0F, 0.0F, 0.0F}, {-90.0F, 0.0F, 2.0F, 0.0F}, {90.0F, 0.0F, 0.0F, 2.0F}};
  table.fir.assign(table.directionCount() * 2U * taps, 0.0F);
  for (std::size_t i = 0; i < table.directionCount(); ++i)
  {
    table.fir[(i * 2U * taps)] = 1.0F;
    table.fir[(i * 2U * taps) + taps] = 1.0F;
  }
  return table;
}

void RunBench(const std::string& label,
              const sonitude::dsp::BinauralConfig& cfg,
              const std::size_t block_frames,
              const std::size_t blocks)
{
  sonitude::dsp::BinauralRenderer renderer;
  renderer.configure(cfg);
  renderer.setDirection({45.0F, 0.0F});

  std::vector<float> mono(block_frames, 0.0F);
  std::vector<float> left(block_frames, 0.0F);
  std::vector<float> right(block_frames, 0.0F);
  for (std::size_t i = 0; i < block_frames; ++i)
  {
    mono[i] = static_cast<float>((i % 64) / 64.0);
  }

  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks; ++b)
  {
    renderer.process(mono, left, right);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  const double samples = static_cast<double>(block_frames * blocks);
  const double sps = samples / sec;
  const double block_us = (sec * 1'000'000.0) / static_cast<double>(blocks);
  const double rt_factor = sps / static_cast<double>(cfg.sample_rate_hz);

  std::cout << label << ": samples_per_sec=" << sps << " block_us=" << block_us
            << " realtime_factor=" << rt_factor
            << " state_bytes=" << renderer.stateBytes()
            << " (delay+FIR+working copies)"
            << " coeff_bytes=" << renderer.coefficientBytes()
            << " first_arrival_samples=" << renderer.algorithmicLatencySamples() << '\n';
}
}  // namespace

int main()
{
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kBlock = 256;
  constexpr std::size_t kBlocks = 800;

  std::cout << "Desktop/reference benchmark only; not edge-hardware evidence.\n";
  RunBench("mono_reference",
           {.sample_rate_hz = kFs, .backend = sonitude::dsp::BinauralBackend::MonoReference, .max_block_frames = kBlock},
           kBlock,
           kBlocks);
  RunBench("itd_ild",
           {.sample_rate_hz = kFs, .backend = sonitude::dsp::BinauralBackend::ItdIld, .max_block_frames = kBlock},
           kBlock,
           kBlocks);

  auto t16 = BuildBenchTable(kFs, 16);
  auto t32 = BuildBenchTable(kFs, 32);
  auto t64 = BuildBenchTable(kFs, 64);
  auto tref = BuildBenchTable(kFs, 256);
  RunBench("compact_16",
           {.sample_rate_hz = kFs,
            .backend = sonitude::dsp::BinauralBackend::CompactHrtf,
            .max_block_frames = kBlock,
            .table = &t16},
           kBlock,
           kBlocks);
  RunBench("compact_32",
           {.sample_rate_hz = kFs,
            .backend = sonitude::dsp::BinauralBackend::CompactHrtf,
            .max_block_frames = kBlock,
            .table = &t32},
           kBlock,
           kBlocks);
  RunBench("compact_64",
           {.sample_rate_hz = kFs,
            .backend = sonitude::dsp::BinauralBackend::CompactHrtf,
            .max_block_frames = kBlock,
            .table = &t64},
           kBlock,
           kBlocks);
  RunBench("full_reference_256",
           {.sample_rate_hz = kFs,
            .backend = sonitude::dsp::BinauralBackend::FullHrtfReference,
            .max_block_frames = kBlock,
            .table = &tref},
           kBlock,
           kBlocks);
  return 0;
}
