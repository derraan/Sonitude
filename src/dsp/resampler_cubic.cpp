#include "dsp/resampler_cubic.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sonitude::dsp
{
namespace
{
class CubicResampler final : public IStereoResampler
{
 public:
  void reset() override
  {
    phase_ = 0.0;
  }

  ResamplerResult process(const StereoSample* input,
                          const std::size_t input_samples,
                          StereoSample* output,
                          const std::size_t max_output_samples,
                          const double ratio) override
  {
    if (input_samples < 2 || max_output_samples == 0 || ratio <= 0.0)
    {
      return {};
    }
    std::size_t produced = 0;
    while (produced < max_output_samples)
    {
      const std::size_t idx = static_cast<std::size_t>(phase_);
      if (idx + 1 >= input_samples)
      {
        break;
      }
      const double frac = phase_ - static_cast<double>(idx);
      output[produced].left =
          static_cast<float>((1.0 - frac) * input[idx].left + frac * input[idx + 1].left);
      output[produced].right =
          static_cast<float>((1.0 - frac) * input[idx].right + frac * input[idx + 1].right);
      ++produced;
      phase_ += ratio;
    }
    const std::size_t consumed = std::min(input_samples, static_cast<std::size_t>(phase_));
    phase_ -= static_cast<double>(consumed);
    return {.consumed = consumed, .produced = produced};
  }

 private:
  double phase_ = 0.0;
};
}  // namespace

std::unique_ptr<IStereoResampler> CreateCubicResampler()
{
  return std::make_unique<CubicResampler>();
}
}  // namespace sonitude::dsp
