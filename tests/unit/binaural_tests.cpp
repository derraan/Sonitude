#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "audio/audio_types.hpp"
#include "dsp/binaural_renderer.hpp"
#include "dsp/hrtf_table.hpp"
#include "tests/support/synth_signals.hpp"

namespace
{
namespace dsp = sonitude::dsp;

void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

dsp::HrtfTable BuildSyntheticTable(const std::uint32_t sample_rate_hz, const std::uint32_t taps)
{
  dsp::HrtfTable table{};
  table.sample_rate_hz = sample_rate_hz;
  table.taps_per_ear = taps;
  table.directions = {{0.0F, 0.0F, 0.0F, 0.0F}, {-90.0F, 0.0F, 2.0F, 0.0F}, {90.0F, 0.0F, 0.0F, 2.0F}};
  table.fir.assign(table.directionCount() * 2U * taps, 0.0F);
  for (std::size_t d = 0; d < table.directionCount(); ++d)
  {
    const std::size_t left_offset = d * 2U * taps;
    const std::size_t right_offset = left_offset + taps;
    table.fir[left_offset] = 1.0F;
    table.fir[right_offset] = 1.0F;
  }
  return table;
}

std::vector<float> GenerateImpulse(const std::size_t frames)
{
  std::vector<float> x(frames, 0.0F);
  if (!x.empty())
  {
    x[0] = 1.0F;
  }
  return x;
}

void TestMonoIdentity()
{
  dsp::BinauralRenderer renderer;
  renderer.configure({.sample_rate_hz = 48000,
                      .backend = dsp::BinauralBackend::MonoReference,
                      .transition_ms = 50.0F,
                      .max_block_frames = 512});
  const auto x = sonitude::tests::support::GenerateSine(512, 48000, 440.0);
  std::vector<float> left(x.size(), 0.0F);
  std::vector<float> right(x.size(), 0.0F);
  renderer.process(x, left, right);
  for (std::size_t i = 0; i < x.size(); ++i)
  {
    Require(std::fabs(left[i] - x[i]) < 1e-7F, "mono backend must copy input to left");
    Require(std::fabs(right[i] - x[i]) < 1e-7F, "mono backend must copy input to right");
  }
}

void TestItdCenterSymmetry()
{
  dsp::BinauralRenderer renderer;
  renderer.configure({.sample_rate_hz = 48000,
                      .backend = dsp::BinauralBackend::ItdIld,
                      .transition_ms = 50.0F,
                      .max_block_frames = 256});
  renderer.setDirection({0.0F, 0.0F});
  const auto x = sonitude::tests::support::GenerateSine(1024, 48000, 600.0);
  std::vector<float> left(x.size(), 0.0F);
  std::vector<float> right(x.size(), 0.0F);
  renderer.process(x, left, right);
  for (std::size_t i = 64; i < x.size(); ++i)
  {
    Require(std::fabs(left[i] - right[i]) < 1e-4F, "center steering should be symmetric");
  }
}

void TestMirrorProperty()
{
  const auto x = sonitude::tests::support::GenerateSine(1400, 48000, 850.0);

  dsp::BinauralRenderer plus;
  plus.configure({.sample_rate_hz = 48000,
                  .backend = dsp::BinauralBackend::ItdIld,
                  .transition_ms = 20.0F,
                  .max_block_frames = 1400});
  plus.setDirection({45.0F, 0.0F});
  std::vector<float> plus_l(x.size(), 0.0F);
  std::vector<float> plus_r(x.size(), 0.0F);
  plus.process(x, plus_l, plus_r);

  dsp::BinauralRenderer minus;
  minus.configure({.sample_rate_hz = 48000,
                   .backend = dsp::BinauralBackend::ItdIld,
                   .transition_ms = 20.0F,
                   .max_block_frames = 1400});
  minus.setDirection({-45.0F, 0.0F});
  std::vector<float> minus_l(x.size(), 0.0F);
  std::vector<float> minus_r(x.size(), 0.0F);
  minus.process(x, minus_l, minus_r);

  for (std::size_t i = 512; i < x.size(); ++i)
  {
    Require(std::fabs(plus_l[i] - minus_r[i]) < 1e-3F, "mirror property left/right mismatch");
    Require(std::fabs(plus_r[i] - minus_l[i]) < 1e-3F, "mirror property right/left mismatch");
  }
}

void TestAzimuthWrapFinite()
{
  dsp::BinauralRenderer renderer;
  renderer.configure({.sample_rate_hz = 48000,
                      .backend = dsp::BinauralBackend::ItdIld,
                      .transition_ms = 5.0F,
                      .max_block_frames = 512});
  const auto x = sonitude::tests::support::GenerateSine(512, 48000, 1000.0);
  std::vector<float> left(x.size(), 0.0F);
  std::vector<float> right(x.size(), 0.0F);
  for (const float az : {0.0F, 90.0F, -90.0F, 179.0F, -179.0F})
  {
    renderer.setDirection({az, 0.0F});
    renderer.process(x, left, right);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
      Require(std::isfinite(left[i]) && std::isfinite(right[i]), "wrapped direction output must stay finite");
    }
  }
}

void TestBlockContinuity()
{
  const auto x = sonitude::tests::support::GenerateSine(1024, 48000, 500.0);
  dsp::BinauralRenderer one;
  one.configure({.sample_rate_hz = 48000,
                 .backend = dsp::BinauralBackend::ItdIld,
                 .transition_ms = 20.0F,
                 .max_block_frames = 1024});
  one.setDirection({35.0F, 0.0F});
  std::vector<float> left_full(x.size(), 0.0F);
  std::vector<float> right_full(x.size(), 0.0F);
  one.process(x, left_full, right_full);

  dsp::BinauralRenderer chunked;
  chunked.configure({.sample_rate_hz = 48000,
                     .backend = dsp::BinauralBackend::ItdIld,
                     .transition_ms = 20.0F,
                     .max_block_frames = 256});
  chunked.setDirection({35.0F, 0.0F});
  std::vector<float> left_chunk(x.size(), 0.0F);
  std::vector<float> right_chunk(x.size(), 0.0F);
  for (std::size_t start = 0; start < x.size(); start += 128)
  {
    const std::size_t count = std::min<std::size_t>(128, x.size() - start);
    chunked.process(std::span<const float>(x.data() + start, count),
                    std::span<float>(left_chunk.data() + start, count),
                    std::span<float>(right_chunk.data() + start, count));
  }

  for (std::size_t i = 512; i < x.size(); ++i)
  {
    Require(std::fabs(left_full[i] - left_chunk[i]) < 2e-3F, "left continuity mismatch");
    Require(std::fabs(right_full[i] - right_chunk[i]) < 2e-3F, "right continuity mismatch");
  }
}

void TestResetDeterminism()
{
  const auto x = sonitude::tests::support::GenerateSine(800, 48000, 920.0);
  dsp::BinauralRenderer renderer;
  renderer.configure({.sample_rate_hz = 48000,
                      .backend = dsp::BinauralBackend::ItdIld,
                      .transition_ms = 20.0F,
                      .max_block_frames = 800});
  renderer.setDirection({-30.0F, 0.0F});
  std::vector<float> left_a(x.size(), 0.0F);
  std::vector<float> right_a(x.size(), 0.0F);
  renderer.process(x, left_a, right_a);
  renderer.reset();
  renderer.setDirection({-30.0F, 0.0F});
  std::vector<float> left_b(x.size(), 0.0F);
  std::vector<float> right_b(x.size(), 0.0F);
  renderer.process(x, left_b, right_b);
  for (std::size_t i = 0; i < x.size(); ++i)
  {
    Require(std::fabs(left_a[i] - left_b[i]) < 1e-6F, "left reset mismatch");
    Require(std::fabs(right_a[i] - right_b[i]) < 1e-6F, "right reset mismatch");
  }
}

void TestDirectionTransitionNoHardDiscontinuity()
{
  const auto x = sonitude::tests::support::GenerateSine(2048, 48000, 700.0);
  dsp::BinauralRenderer renderer;
  renderer.configure({.sample_rate_hz = 48000,
                      .backend = dsp::BinauralBackend::ItdIld,
                      .transition_ms = 100.0F,
                      .max_block_frames = 256});
  renderer.setDirection({-80.0F, 0.0F});
  std::vector<float> left(x.size(), 0.0F);
  std::vector<float> right(x.size(), 0.0F);
  for (std::size_t start = 0; start < x.size(); start += 128)
  {
    if (start == 768)
    {
      renderer.setDirection({80.0F, 0.0F});
    }
    const std::size_t count = std::min<std::size_t>(128, x.size() - start);
    renderer.process(std::span<const float>(x.data() + start, count),
                    std::span<float>(left.data() + start, count),
                    std::span<float>(right.data() + start, count));
  }
  Require(sonitude::tests::support::MaxSecondDifference(left) < 0.9, "transition click in left channel");
  Require(sonitude::tests::support::MaxSecondDifference(right) < 0.9, "transition click in right channel");
}

void TestInvalidConfigurationRejects()
{
  bool threw = false;
  try
  {
    dsp::BinauralRenderer renderer;
    renderer.configure({.sample_rate_hz = 0, .backend = dsp::BinauralBackend::MonoReference, .max_block_frames = 64});
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "zero sample rate must be rejected");

  threw = false;
  try
  {
    dsp::BinauralRenderer renderer;
    renderer.configure({.sample_rate_hz = 48000,
                        .backend = dsp::BinauralBackend::CompactHrtf,
                        .max_block_frames = 64,
                        .table = nullptr});
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "missing HRTF table must be rejected");
}

void TestHrtfTableUsage()
{
  auto table = BuildSyntheticTable(48000, 16);
  dsp::BinauralRenderer renderer;
  renderer.configure({.sample_rate_hz = 48000,
                      .backend = dsp::BinauralBackend::CompactHrtf,
                      .transition_ms = 10.0F,
                      .max_block_frames = 256,
                      .table = &table});
  renderer.setDirection({90.0F, 0.0F});
  const auto impulse = GenerateImpulse(256);
  std::vector<float> left(impulse.size(), 0.0F);
  std::vector<float> right(impulse.size(), 0.0F);
  renderer.process(impulse, left, right);
  double energy_l = 0.0;
  double energy_r = 0.0;
  for (std::size_t i = 0; i < impulse.size(); ++i)
  {
    energy_l += static_cast<double>(left[i]) * static_cast<double>(left[i]);
    energy_r += static_cast<double>(right[i]) * static_cast<double>(right[i]);
  }
  Require(energy_l > 0.0 && energy_r > 0.0, "HRTF path should emit non-zero output");
  Require(renderer.coefficientBytes() == table.fir.size() * sizeof(float), "coefficientBytes mismatch");
}

void TestHrtfTableIntegrity()
{
  const std::filesystem::path table_path =
      std::filesystem::path(SONITUDE_SOURCE_DIR) / "data" / "hrtf" / "generic_sadie2_d2" /
      "compact_16.shrf";
  const auto table = dsp::LoadHrtfTableFromFile(table_path.string());
  Require(table.directionCount() > 0, "loaded table must contain directions");
  Require(table.taps_per_ear == 16, "loaded compact_16 table should have 16 taps");

  const std::filesystem::path corrupt_path =
      std::filesystem::path(SONITUDE_SOURCE_DIR) / "build" / "tmp_corrupt.shrf";
  {
    std::ifstream in(table_path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Require(bytes.size() > 32, "table must be large enough to corrupt");
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x11);
    std::ofstream out(corrupt_path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  bool threw = false;
  try
  {
    (void)dsp::LoadHrtfTableFromFile(corrupt_path.string());
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, "corrupted table must fail CRC validation");
  std::error_code ec;
  std::filesystem::remove(corrupt_path, ec);
}
}  // namespace

void RunBinauralTests()
{
  TestMonoIdentity();
  TestItdCenterSymmetry();
  TestMirrorProperty();
  TestAzimuthWrapFinite();
  TestBlockContinuity();
  TestResetDeterminism();
  TestDirectionTransitionNoHardDiscontinuity();
  TestInvalidConfigurationRejects();
  TestHrtfTableUsage();
  TestHrtfTableIntegrity();
}
