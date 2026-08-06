#include <cmath>
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

void RunPpmCase(const double ppm)
{
  auto resampler = sonitude::dsp::CreateSrcResampler();
  sonitude::dsp::AsrcController ctl({
      .min_ratio = 0.995,
      .max_ratio = 1.005,
      .kp = 0.0002,
      .ki = 0.00001,
      .target_buffer_frames = 256.0,
      .max_ratio_step = 0.00002,
  });
  double occupancy = 256.0;
  for (int i = 0; i < 20000; ++i)
  {
    const double producer = 64.0 * (1.0 + (ppm / 1'000'000.0));
    occupancy += producer;
    std::vector<sonitude::dsp::StereoSample> in(128);
    std::vector<sonitude::dsp::StereoSample> out(256);
    const double ratio = ctl.update(occupancy);
    const auto rr = resampler->process(in.data(), in.size(), out.data(), out.size(), ratio);
    occupancy -= static_cast<double>(rr.produced);
    if (occupancy < 0.0)
    {
      occupancy = 0.0;
    }
    if (occupancy > 1024.0)
    {
      occupancy = 1024.0;
    }
  }
  Require(occupancy >= 0.0 && occupancy <= 1024.0, "occupancy diverged in ASRC simulation");
}
}  // namespace

void RunAsrcSimulationTests()
{
  RunPpmCase(-150.0);
  RunPpmCase(-55.0);
  RunPpmCase(-20.0);
  RunPpmCase(20.0);
  RunPpmCase(55.0);
  RunPpmCase(150.0);
}
