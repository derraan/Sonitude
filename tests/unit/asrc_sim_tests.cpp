#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp/asrc_controller.hpp"
#include "dsp/resampler.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

struct SimMetrics
{
  double final_occupancy = 0.0;
  double mean_occupancy_last = 0.0;
  double mean_ratio_last = 0.0;
  std::size_t min_saturation_ticks = 0;
  std::size_t max_saturation_ticks = 0;
};

SimMetrics RunPpmCase(const double ppm, const int iterations)
{
  auto resampler = sonitude::dsp::CreateLinearResampler();
  sonitude::dsp::AsrcController ctl({
      .min_ratio = 0.995,
      .max_ratio = 1.005,
      .kp = 0.00015,
      .ki = 0.0000005,
      .target_buffer_frames = 256.0,
      .max_ratio_step = 0.0001,
  });
  std::vector<sonitude::dsp::StereoSample> in(65);
  std::vector<sonitude::dsp::StereoSample> out(256);
  std::vector<double> occupancy_log;
  std::vector<double> ratio_log;
  occupancy_log.reserve(static_cast<std::size_t>(iterations));
  ratio_log.reserve(static_cast<std::size_t>(iterations));
  double occupancy = 256.0;
  std::size_t min_hits = 0;
  std::size_t max_hits = 0;
  for (int i = 0; i < iterations; ++i)
  {
    const double producer = 64.0 * (1.0 + (ppm / 1'000'000.0));
    occupancy += producer;
    const double ratio = ctl.update(occupancy);
    const auto rr = resampler->process(in.data(), in.size(), out.data(), out.size(), ratio);
    occupancy -= static_cast<double>(rr.produced);
    if (std::fabs(ratio - 0.995) < 1e-9)
    {
      ++min_hits;
    }
    if (std::fabs(ratio - 1.005) < 1e-9)
    {
      ++max_hits;
    }
    if (!std::isfinite(occupancy) || occupancy < 0.0 || occupancy > 200000.0)
    {
      throw std::runtime_error("occupancy diverged in ASRC simulation");
    }
    occupancy_log.push_back(occupancy);
    ratio_log.push_back(ratio);
  }
  const std::size_t tail_count = std::min<std::size_t>(2000, occupancy_log.size());
  const std::size_t tail_start = occupancy_log.size() - tail_count;
  const double occ_sum = std::accumulate(occupancy_log.begin() + static_cast<std::ptrdiff_t>(tail_start),
                                         occupancy_log.end(),
                                         0.0);
  const double ratio_sum = std::accumulate(ratio_log.begin() + static_cast<std::ptrdiff_t>(tail_start),
                                           ratio_log.end(),
                                           0.0);
  return SimMetrics{
      .final_occupancy = occupancy,
      .mean_occupancy_last = occ_sum / static_cast<double>(tail_count),
      .mean_ratio_last = ratio_sum / static_cast<double>(tail_count),
      .min_saturation_ticks = min_hits,
      .max_saturation_ticks = max_hits,
  };
}

void TestRatioDirection()
{
  auto resampler = sonitude::dsp::CreateLinearResampler();
  std::vector<sonitude::dsp::StereoSample> in(65);
  std::vector<sonitude::dsp::StereoSample> out_fast(256);
  std::vector<sonitude::dsp::StereoSample> out_slow(256);
  const auto fast = resampler->process(in.data(), in.size(), out_fast.data(), out_fast.size(), 1.005);
  resampler->reset();
  const auto slow = resampler->process(in.data(), in.size(), out_slow.data(), out_slow.size(), 0.995);
  Require(fast.produced > slow.produced, "higher ratio must produce more output frames");
}

void TestControllerSign()
{
  sonitude::dsp::AsrcController ctl({
      .min_ratio = 0.995,
      .max_ratio = 1.005,
      .kp = 0.0002,
      .ki = 0.00001,
      .target_buffer_frames = 256.0,
      .max_ratio_step = 0.0001,
  });
  const double high_occ = ctl.update(400.0);
  ctl.reset();
  const double low_occ = ctl.update(120.0);
  Require(high_occ > 1.0, "high occupancy should increase ratio");
  Require(low_occ < 1.0, "low occupancy should decrease ratio");
}

void TestRecoveryAfterStep()
{
  auto resampler = sonitude::dsp::CreateLinearResampler();
  sonitude::dsp::AsrcController ctl({
      .min_ratio = 0.995,
      .max_ratio = 1.005,
      .kp = 0.00015,
      .ki = 0.0000005,
      .target_buffer_frames = 256.0,
      .max_ratio_step = 0.0001,
  });
  std::vector<sonitude::dsp::StereoSample> in(65);
  std::vector<sonitude::dsp::StereoSample> out(256);
  double occupancy = 256.0;
  for (int i = 0; i < 10000; ++i)
  {
    const double ppm = (i < 5000) ? 0.0 : 100.0;
    occupancy += 64.0 * (1.0 + (ppm / 1'000'000.0));
    const double ratio = ctl.update(occupancy);
    const auto rr = resampler->process(in.data(), in.size(), out.data(), out.size(), ratio);
    occupancy -= static_cast<double>(rr.produced);
  }
  Require(std::fabs(occupancy - 256.0) < 80.0, "controller should recover after ppm step");
}
}  // namespace

void RunAsrcSimulationTests()
{
  TestRatioDirection();
  TestControllerSign();
  for (const double ppm : std::vector<double>{-150.0, -55.0, -20.0, 20.0, 55.0, 150.0})
  {
    const SimMetrics m = RunPpmCase(ppm, 20000);
    const double expected_ratio = 1.0 + (ppm / 1'000'000.0);
    Require(std::fabs(m.mean_occupancy_last - 256.0) < 5000.0, "occupancy should remain bounded");
    Require(std::fabs(m.mean_ratio_last - expected_ratio) < 0.01, "ratio mean should track ppm drift");
    Require((m.min_saturation_ticks + m.max_saturation_ticks) < 18000U,
            "controller saturates for too much of the simulation");
  }
  TestRecoveryAfterStep();
}
