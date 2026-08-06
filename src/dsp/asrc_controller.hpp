#pragma once

#include <algorithm>

namespace sonitude::dsp
{
class AsrcController
{
 public:
  struct Config
  {
    double min_ratio = 0.995;
    double max_ratio = 1.005;
    double kp = 0.0002;
    double ki = 0.00001;
    double target_buffer_frames = 256.0;
    double max_ratio_step = 0.0001;
  };

  explicit AsrcController(Config cfg) : cfg_(cfg) {}

  double update(const double occupancy_frames)
  {
    const double error = occupancy_frames - cfg_.target_buffer_frames;
    integral_ += error;
    double ratio = 1.0 + (cfg_.kp * error) + (cfg_.ki * integral_);
    ratio = std::clamp(ratio, cfg_.min_ratio, cfg_.max_ratio);
    const double min_step = last_ratio_ - cfg_.max_ratio_step;
    const double max_step = last_ratio_ + cfg_.max_ratio_step;
    ratio = std::clamp(ratio, min_step, max_step);
    last_ratio_ = ratio;
    return ratio;
  }

  void reset()
  {
    integral_ = 0.0;
    last_ratio_ = 1.0;
  }

 private:
  Config cfg_;
  double integral_ = 0.0;
  double last_ratio_ = 1.0;
};
}  // namespace sonitude::dsp
