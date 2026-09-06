#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/fixed_binaural_mvdr.hpp"
#include "dsp/array_profile.hpp"

namespace
{
sonitude::app::GeometryConfig Geometry()
{
  sonitude::app::GeometryConfig g;
  g.profile_name = "bench";
  g.microphones = {
      {"M0", -0.038, 0.168, 0.0}, {"M1", 0.038, 0.168, 0.0}, {"M2", -0.090, 0.050, 0.0},
      {"M3", 0.090, 0.050, 0.0},  {"M4", -0.060, 0.000, 0.0}, {"M5", 0.060, 0.000, 0.0},
  };
  return g;
}

sonitude::app::CalibrationConfig Calibration()
{
  sonitude::app::CalibrationConfig c;
  c.sample_rate_hz = 44100;
  c.channels.resize(6);
  for (std::size_t i = 0; i < 6; ++i)
  {
    c.channels[i].id = "M" + std::to_string(i);
    c.channels[i].polarity = 1;
    c.channels[i].gain_linear = 1.0F;
  }
  return c;
}

std::vector<sonitude::audio::MicFrame> Noise(const std::size_t frames)
{
  std::vector<sonitude::audio::MicFrame> out(frames);
  std::uint32_t s = 1;
  for (std::size_t i = 0; i < frames; ++i)
  {
    for (std::size_t ch = 0; ch < 6; ++ch)
    {
      s = (1664525U * s) + 1013904223U;
      out[i][ch] = (static_cast<float>(s >> 8) / static_cast<float>(1U << 24)) - 0.5F;
    }
  }
  return out;
}

void Percentiles(const std::vector<double>& us, double& med, double& p95, double& p99, double& mx)
{
  auto s = us;
  std::sort(s.begin(), s.end());
  med = s[s.size() / 2];
  p95 = s[(s.size() * 95) / 100];
  p99 = s[(s.size() * 99) / 100];
  mx = s.back();
}
}  // namespace

int main()
{
  constexpr std::uint32_t kFs = 44100;
  constexpr std::size_t kBlock = 64;
  constexpr std::size_t kBlocks = 400;
  auto mic = Noise(kBlock * kBlocks);
  sonitude::app::SteeringConfig steering;
  steering.model = "near_field";
  steering.reference_mic_index = 2;

  sonitude::dsp::MvdrBeamformer adaptive;
  adaptive.configure(Geometry(), steering, Calibration(), kFs, kBlock);
  adaptive.setTarget({0.0F, 0.0F});
  adaptive.resetCovarianceDiagnosticsForTest();
  std::vector<float> mono(kBlock, 0.0F);
  std::vector<double> hop_us;
  hop_us.reserve(kBlocks);
  for (std::size_t b = 0; b < 20; ++b)
  {
    adaptive.process(std::span<const sonitude::audio::MicFrame>(mic.data() + (b * kBlock), kBlock),
                     mono);
  }
  adaptive.resetCovarianceDiagnosticsForTest();
  for (std::size_t b = 20; b < kBlocks; ++b)
  {
    const auto t0 = std::chrono::steady_clock::now();
    adaptive.process(std::span<const sonitude::audio::MicFrame>(mic.data() + (b * kBlock), kBlock),
                     mono);
    const auto t1 = std::chrono::steady_clock::now();
    hop_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  double med = 0;
  double p95 = 0;
  double p99 = 0;
  double mx = 0;
  Percentiles(hop_us, med, p95, p99, mx);
  std::cout << "backend=adaptive_geometric commit=local "
            << "block=" << kBlock << " fft=128 hop=32 "
            << "factors=" << adaptive.factorizationCountForTest()
            << " solves=" << adaptive.solveCountForTest()
            << " looks=" << adaptive.lookCountForTest()
            << " cov_hops=" << adaptive.covarianceUpdateHopsForTest()
            << " median_us=" << med << " p95_us=" << p95 << " p99_us=" << p99
            << " max_us=" << mx << '\n';
  std::cout << "Host wall time is informative, not a WCET proof.\n";
  return 0;
}
