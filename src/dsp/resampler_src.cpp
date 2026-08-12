#include "dsp/resampler.hpp"

#if SONITUDE_WITH_LIBSAMPLERATE_ENABLED && __has_include(<samplerate.h>)
#include <samplerate.h>
#define SONITUDE_HAS_LIBSAMPLERATE 1
#else
#define SONITUDE_HAS_LIBSAMPLERATE 0
#endif

#include <memory>
#include <vector>

#include "dsp/resampler_linear.hpp"

namespace sonitude::dsp
{
namespace
{
#if SONITUDE_HAS_LIBSAMPLERATE
class SrcResampler final : public IStereoResampler
{
 public:
  SrcResampler()
  {
    int error = 0;
    state_ = src_new(SRC_SINC_FASTEST, 2, &error);
  }

  ~SrcResampler() override
  {
    if (state_ != nullptr)
    {
      src_delete(state_);
      state_ = nullptr;
    }
  }

  void reset() override
  {
    if (state_ != nullptr)
    {
      src_reset(state_);
    }
  }

  ResamplerResult process(const StereoSample* input,
                          const std::size_t input_samples,
                          StereoSample* output,
                          const std::size_t max_output_samples,
                          const double ratio) override
  {
    if (state_ == nullptr || input_samples == 0 || max_output_samples == 0 || ratio <= 0.0)
    {
      return {};
    }
    SRC_DATA data{};
    data.data_in = reinterpret_cast<const float*>(input);
    data.input_frames = static_cast<long>(input_samples);
    data.data_out = reinterpret_cast<float*>(output);
    data.output_frames = static_cast<long>(max_output_samples);
    data.src_ratio = ratio;
    data.end_of_input = 0;
    const int rc = src_process(state_, &data);
    if (rc != 0)
    {
      return {};
    }
    return {.consumed = static_cast<std::size_t>(data.input_frames_used),
            .produced = static_cast<std::size_t>(data.output_frames_gen)};
  }

 private:
  SRC_STATE* state_ = nullptr;
};
#endif
}  // namespace

std::unique_ptr<IStereoResampler> CreateSrcResampler()
{
#if SONITUDE_HAS_LIBSAMPLERATE
  return std::make_unique<SrcResampler>();
#else
  return CreateLinearResampler();
#endif
}
}  // namespace sonitude::dsp
