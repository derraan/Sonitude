#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace sonitude::dsp
{
struct StereoSample
{
  float left = 0.0F;
  float right = 0.0F;
};

struct ResamplerResult
{
  std::size_t consumed = 0;
  std::size_t produced = 0;
};

class IStereoResampler
{
 public:
  virtual ~IStereoResampler() = default;
  virtual void reset() = 0;
  // Ratio convention: output_samples / input_samples. Values above 1.0 speed up drain.
  virtual ResamplerResult process(const StereoSample* input,
                                  std::size_t input_samples,
                                  StereoSample* output,
                                  std::size_t max_output_samples,
                                  double ratio) = 0;
};

std::unique_ptr<IStereoResampler> CreateLinearResampler();
std::unique_ptr<IStereoResampler> CreateCubicResampler();
std::unique_ptr<IStereoResampler> CreateSrcResampler();
}  // namespace sonitude::dsp
