#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sonitude::dsp
{
constexpr double kPi = 3.14159265358979323846;

class FractionalDelayLine
{
 public:
  static constexpr std::size_t kTaps = 8;
  static constexpr std::size_t kPhases = 64;

  void configure(double max_delay_samples)
  {
    if (max_delay_samples < 0.0)
    {
      throw std::runtime_error("max_delay_samples must be non-negative");
    }
    max_delay_samples_ = max_delay_samples;
    const std::size_t size =
        static_cast<std::size_t>(std::ceil(max_delay_samples_)) + kTaps + 8U;
    buffer_.assign(size, 0.0F);
    write_index_ = 0;
  }

  void reset()
  {
    std::fill(buffer_.begin(), buffer_.end(), 0.0F);
    write_index_ = 0;
  }

  float process(float input, double delay_samples)
  {
    if (buffer_.empty())
    {
      throw std::runtime_error("FractionalDelayLine used before configure");
    }
    if (delay_samples < 0.0 || delay_samples > max_delay_samples_)
    {
      throw std::runtime_error("delay_samples out of configured range");
    }

    buffer_[write_index_] = input;
    write_index_ = (write_index_ + 1U) % buffer_.size();

    const auto& table = CoeffTable();
    const std::size_t int_delay = static_cast<std::size_t>(std::floor(delay_samples));
    const double frac = delay_samples - static_cast<double>(int_delay);
    const double phase_pos = frac * static_cast<double>(kPhases);
    std::size_t phase0 = static_cast<std::size_t>(phase_pos);
    if (phase0 >= kPhases)
    {
      phase0 = kPhases - 1U;
    }
    const std::size_t phase1 = (phase0 + 1U <= kPhases) ? (phase0 + 1U) : kPhases;
    const float t = static_cast<float>(phase_pos - static_cast<double>(phase0));

    float out = 0.0F;
    for (std::size_t tap = 0; tap < kTaps; ++tap)
    {
      const std::int64_t delayed_index = static_cast<std::int64_t>(write_index_) - 1 -
                                         static_cast<std::int64_t>(int_delay) -
                                         static_cast<std::int64_t>(tap);
      const std::size_t idx = WrapIndex(delayed_index);
      const float c0 = table[phase0][tap];
      const float c1 = table[phase1][tap];
      const float coeff = c0 + (c1 - c0) * t;
      out += buffer_[idx] * coeff;
    }
    return out;
  }

 private:
  static std::size_t WrapIndex(std::int64_t index, std::size_t size)
  {
    const std::int64_t s = static_cast<std::int64_t>(size);
    std::int64_t out = index % s;
    if (out < 0)
    {
      out += s;
    }
    return static_cast<std::size_t>(out);
  }

  std::size_t WrapIndex(std::int64_t index) const
  {
    return WrapIndex(index, buffer_.size());
  }

  static std::array<std::array<float, kTaps>, kPhases + 1U> BuildCoeffTable()
  {
    std::array<std::array<float, kTaps>, kPhases + 1U> out{};
    for (std::size_t phase = 0; phase <= kPhases; ++phase)
    {
      const double frac = static_cast<double>(phase) / static_cast<double>(kPhases);
      double sum = 0.0;
      for (std::size_t tap = 0; tap < kTaps; ++tap)
      {
        const double n = static_cast<double>(tap) - (static_cast<double>(kTaps - 1U) * 0.5) - frac;
        const double x = n;
        const double sinc = (std::fabs(x) < 1e-12) ? 1.0 : (std::sin(kPi * x) / (kPi * x));
        const double window = 0.5 - 0.5 *
                                        std::cos((2.0 * kPi * static_cast<double>(tap)) /
                                                 static_cast<double>(kTaps - 1U));
        const double coeff = sinc * window;
        out[phase][tap] = static_cast<float>(coeff);
        sum += coeff;
      }
      if (std::fabs(sum) > 1e-12)
      {
        for (std::size_t tap = 0; tap < kTaps; ++tap)
        {
          out[phase][tap] = static_cast<float>(out[phase][tap] / sum);
        }
      }
    }
    return out;
  }

  static const std::array<std::array<float, kTaps>, kPhases + 1U>& CoeffTable()
  {
    static const auto table = BuildCoeffTable();
    return table;
  }

  std::vector<float> buffer_;
  std::size_t write_index_ = 0;
  double max_delay_samples_ = 0.0;
};
}  // namespace sonitude::dsp
