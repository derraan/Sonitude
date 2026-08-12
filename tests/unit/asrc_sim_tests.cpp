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
  std::vector<sonitude::dsp::StereoSample> in(64);
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
    const double ratio = ctl.update(occupancy);
    const auto rr = resampler->process(in.data(), in.size(), out.data(), out.size(), ratio);
    Require(rr.consumed == in.size(), "streaming resampler must accept each complete input block");
    occupancy += static_cast<double>(rr.produced);
    occupancy -= 64.0 * (1.0 + (ppm / 1'000'000.0));
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

void TestLinearStreamingContinuity(const double ratio)
{
  constexpr std::size_t kBlockFrames = 64;
  constexpr std::size_t kBlocks = 1000;
  auto resampler = sonitude::dsp::CreateLinearResampler();
  std::vector<sonitude::dsp::StereoSample> in(kBlockFrames);
  std::vector<sonitude::dsp::StereoSample> out(kBlockFrames * 2U);
  std::size_t total_output = 0;
  double previous = 0.0;
  bool have_previous = false;
  double max_step_error = 0.0;
  for (std::size_t block = 0; block < kBlocks; ++block)
  {
    for (std::size_t i = 0; i < kBlockFrames; ++i)
    {
      const float value = static_cast<float>((block * kBlockFrames) + i);
      in[i] = {.left = value, .right = value};
    }
    const auto rr = resampler->process(in.data(), in.size(), out.data(), out.size(), ratio);
    Require(rr.consumed == in.size(), "linear resampler must retain its block-boundary tail");
    for (std::size_t i = 0; i < rr.produced; ++i)
    {
      if (have_previous)
      {
        max_step_error = std::max(max_step_error, std::fabs(
            static_cast<double>(out[i].left) - previous - (1.0 / ratio)));
      }
      previous = out[i].left;
      have_previous = true;
    }
    total_output += rr.produced;
  }

  const double expected = static_cast<double>(kBlockFrames * kBlocks) * ratio;
  Require(std::fabs(static_cast<double>(total_output) - expected) <= 2.0,
          "streaming output count must track output/input ratio");
  Require(max_step_error < 0.01, "linear output must remain continuous across block boundaries");
  if (ratio == 1.0)
  {
    Require(total_output == kBlockFrames * kBlocks,
            "unity linear resampling must not lose one frame per block");
  }
}

void TestSelectedBackendStreaming(const double ratio)
{
  constexpr std::size_t kBlockFrames = 64;
  constexpr std::size_t kBlocks = 1000;
  auto resampler = sonitude::dsp::CreateSrcResampler();
  std::vector<sonitude::dsp::StereoSample> block(kBlockFrames);
  std::vector<sonitude::dsp::StereoSample> pending;
  std::vector<sonitude::dsp::StereoSample> out(kBlockFrames * 2U);
  std::size_t total_output = 0;
  for (std::size_t i = 0; i < kBlocks; ++i)
  {
    pending.insert(pending.end(), block.begin(), block.end());
    const auto rr =
        resampler->process(pending.data(), pending.size(), out.data(), out.size(), ratio);
    Require(rr.consumed <= pending.size(), "selected resampler reported invalid input consumption");
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(rr.consumed));
    Require(pending.size() <= kBlockFrames, "selected resampler failed to consume streaming input");
    total_output += rr.produced;
  }
  const double expected = static_cast<double>(kBlockFrames * kBlocks) * ratio;
  Require(std::fabs(static_cast<double>(total_output) - expected) < 256.0,
          "selected resampler output count must track its ratio after transport delay");
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
  Require(high_occ < 1.0, "high playback occupancy should reduce generated output frames");
  Require(low_occ > 1.0, "low playback occupancy should increase generated output frames");
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
  std::vector<sonitude::dsp::StereoSample> in(64);
  std::vector<sonitude::dsp::StereoSample> out(256);
  double occupancy = 256.0;
  for (int i = 0; i < 10000; ++i)
  {
    const double ppm = (i < 5000) ? 0.0 : 100.0;
    const double ratio = ctl.update(occupancy);
    const auto rr = resampler->process(in.data(), in.size(), out.data(), out.size(), ratio);
    occupancy += static_cast<double>(rr.produced);
    occupancy -= 64.0 * (1.0 + (ppm / 1'000'000.0));
  }
  Require(std::fabs(occupancy - 256.0) < 80.0, "controller should recover after ppm step");
}
}  // namespace

void RunAsrcSimulationTests()
{
  TestRatioDirection();
  TestLinearStreamingContinuity(0.995);
  TestLinearStreamingContinuity(1.0);
  TestLinearStreamingContinuity(1.005);
  TestSelectedBackendStreaming(0.995);
  TestSelectedBackendStreaming(1.0);
  TestSelectedBackendStreaming(1.005);
  TestControllerSign();
  for (const double ppm : std::vector<double>{-150.0, -55.0, -20.0, 20.0, 55.0, 150.0})
  {
    const SimMetrics m = RunPpmCase(ppm, 20000);
    const double expected_ratio = 1.0 + (ppm / 1'000'000.0);
    Require(std::fabs(m.mean_occupancy_last - 256.0) < 8.0, "playback occupancy should converge");
    Require(std::fabs(m.mean_ratio_last - expected_ratio) < 0.0001, "ratio mean should track ppm drift");
    Require((m.min_saturation_ticks + m.max_saturation_ticks) < 18000U,
            "controller saturates for too much of the simulation");
  }
  TestRecoveryAfterStep();
}
