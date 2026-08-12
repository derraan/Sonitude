#include "dsp/resampler_linear.hpp"

#include <algorithm>
#include <memory>

namespace sonitude::dsp
{
namespace
{
class LinearResampler final : public IStereoResampler
{
 public:
  void reset() override
  {
    phase_ = 0.0;
    has_previous_ = false;
    previous_ = {};
  }

  ResamplerResult process(const StereoSample* input,
                          const std::size_t input_samples,
                          StereoSample* output,
                          const std::size_t max_output_samples,
                          const double ratio) override
  {
    if (input_samples == 0 || max_output_samples == 0 || ratio <= 0.0)
    {
      return {};
    }

    const std::size_t prefix = has_previous_ ? 1U : 0U;
    const std::size_t available = input_samples + prefix;
    const auto sample_at = [&](const std::size_t index) -> const StereoSample&
    {
      return (has_previous_ && index == 0U) ? previous_ : input[index - prefix];
    };

    std::size_t produced = 0;
    while (produced < max_output_samples)
    {
      const std::size_t idx = static_cast<std::size_t>(phase_);
      if (idx >= available)
      {
        break;
      }
      const double frac = phase_ - static_cast<double>(idx);
      if (idx + 1U >= available)
      {
        // An integer-position final sample needs no lookahead. A fractional
        // position is completed when the next block supplies its first sample.
        if (frac > 1e-12)
        {
          break;
        }
        output[produced] = sample_at(idx);
      }
      else
      {
        const auto& current = sample_at(idx);
        const auto& next = sample_at(idx + 1U);
        output[produced].left =
            static_cast<float>((1.0 - frac) * current.left + frac * next.left);
        output[produced].right =
            static_cast<float>((1.0 - frac) * current.right + frac * next.right);
      }
      ++produced;
      phase_ += (1.0 / ratio);
    }

    std::size_t consumed = input_samples;
    std::size_t retained_index = available - 1U;
    if (produced == max_output_samples && phase_ < static_cast<double>(available - 1U))
    {
      retained_index = static_cast<std::size_t>(phase_);
      consumed = has_previous_ ? retained_index : retained_index + 1U;
    }
    previous_ = sample_at(retained_index);
    has_previous_ = true;
    phase_ -= static_cast<double>(retained_index);
    return {.consumed = consumed, .produced = produced};
  }

 private:
  double phase_ = 0.0;
  bool has_previous_ = false;
  StereoSample previous_{};
};
}  // namespace

std::unique_ptr<IStereoResampler> CreateLinearResampler()
{
  return std::make_unique<LinearResampler>();
}
}  // namespace sonitude::dsp
