#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "audio/wav_io.hpp"

namespace
{
constexpr std::size_t kChannelCount = 6;

std::vector<std::string> DefaultChannelIds()
{
  return {"M0_left_ear", "M1_left_arc", "M2_left_top", "M3_right_top", "M4_right_arc", "M5_right_ear"};
}

float DelayReadLinear(const std::vector<float>& signal, const std::size_t index, const double delay)
{
  const double pos = static_cast<double>(index) - delay;
  const std::ptrdiff_t i0 = static_cast<std::ptrdiff_t>(std::floor(pos));
  const std::ptrdiff_t i1 = i0 + 1;
  const double frac = pos - static_cast<double>(i0);
  const auto sample_at = [&](const std::ptrdiff_t i) -> float {
    if (i < 0 || static_cast<std::size_t>(i) >= signal.size())
    {
      return 0.0F;
    }
    return signal[static_cast<std::size_t>(i)];
  };
  const float s0 = sample_at(i0);
  const float s1 = sample_at(i1);
  return static_cast<float>((1.0 - frac) * static_cast<double>(s0) + frac * static_cast<double>(s1));
}

void PrintUsage()
{
  std::cout << "Usage: sonitude_calibration_capture [--output path] [--sample-rate hz]\n"
            << "       [--synthetic-test] [--reference-index N]\n"
            << "\n"
            << "Produces a six-channel WAV with:\n"
            << "  1. silence region (DC estimation)\n"
            << "  2. common-source tone region (gain/delay/polarity estimation)\n"
            << "\n"
            << "Channel order matches config/geometry_soundbubble_initial.yaml (USB 0..5).\n"
            << "Reference microphone defaults to index 2 (M2_left_top).\n";
}
}  // namespace

int main(int argc, char** argv)
{
  std::string out_path = "build/calibration_capture.wav";
  std::uint32_t sample_rate_hz = 44100;
  bool synthetic_test = false;
  std::size_t reference_index = 2;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--output" && i + 1 < argc)
    {
      out_path = argv[++i];
    }
    else if (arg == "--sample-rate" && i + 1 < argc)
    {
      sample_rate_hz = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--synthetic-test")
    {
      synthetic_test = true;
    }
    else if (arg == "--reference-index" && i + 1 < argc)
    {
      reference_index = static_cast<std::size_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--help" || arg == "-h")
    {
      PrintUsage();
      return 0;
    }
    else
    {
      std::cerr << "Unknown argument: " << arg << "\n";
      PrintUsage();
      return 1;
    }
  }

  if (reference_index >= kChannelCount)
  {
    std::cerr << "reference-index must be in [0,5]\n";
    return 1;
  }

  try
  {
    const std::filesystem::path out_dir = std::filesystem::path(out_path).parent_path();
    if (!out_dir.empty())
    {
      std::filesystem::create_directories(out_dir);
    }

    const std::size_t silence_frames = sample_rate_hz / 2U;
    const std::size_t signal_frames = sample_rate_hz * 2U;
    const std::size_t total_frames = silence_frames + signal_frames;

    std::vector<float> mono(signal_frames, 0.0F);
    for (std::size_t i = 0; i < signal_frames; ++i)
    {
      const float t = static_cast<float>(i) / static_cast<float>(sample_rate_hz);
      mono[i] = 0.35F * std::sin(2.0F * 3.1415926535F * 700.0F * t) +
                0.25F * std::sin(2.0F * 3.1415926535F * 1200.0F * t) +
                0.15F * std::sin(2.0F * 3.1415926535F * 2100.0F * t);
    }

    const std::array<double, kChannelCount> synthetic_delays =
        synthetic_test ? std::array<double, kChannelCount>{0.0, 1.25, -0.8, 0.5, -1.1, 0.7}
                       : std::array<double, kChannelCount>{};
    const std::array<float, kChannelCount> synthetic_gains =
        synthetic_test ? std::array<float, kChannelCount>{1.0F, 0.85F, 1.0F, 1.12F, 0.93F, 1.05F}
                       : std::array<float, kChannelCount>{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    const std::array<int, kChannelCount> synthetic_polarity =
        synthetic_test ? std::array<int, kChannelCount>{1, -1, 1, 1, 1, 1} : std::array<int, kChannelCount>{};

    sonitude::audio::WavData wav;
    wav.sample_rate_hz = sample_rate_hz;
    wav.channels = static_cast<std::uint16_t>(kChannelCount);
    wav.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    wav.interleaved.assign(total_frames * kChannelCount, 0.0F);

    const auto ids = DefaultChannelIds();
    for (std::size_t f = 0; f < total_frames; ++f)
    {
      for (std::size_t ch = 0; ch < kChannelCount; ++ch)
      {
        float sample = 0.0F;
        if (f < silence_frames)
        {
          sample = 0.002F * static_cast<float>(ch) - 0.001F;
        }
        else
        {
          const std::size_t sig_i = f - silence_frames;
          sample = synthetic_polarity[ch] * synthetic_gains[ch] *
                   DelayReadLinear(mono, sig_i, synthetic_delays[ch]);
        }
        wav.interleaved[f * kChannelCount + ch] = sample;
      }
    }

    sonitude::audio::WriteWavFile(out_path, wav);

    std::cout << "Wrote " << out_path << "\n";
    std::cout << "sample_rate_hz: " << sample_rate_hz << "\n";
    std::cout << "silence_frames: " << silence_frames << "\n";
    std::cout << "signal_frames: " << signal_frames << "\n";
    std::cout << "reference_index: " << reference_index << " (" << ids[reference_index] << ")\n";
    std::cout << "channel_order:";
    for (const auto& id : ids)
    {
      std::cout << " " << id;
    }
    std::cout << "\n";
    if (synthetic_test)
    {
      std::cout << "synthetic_test: true (known delay/gain/polarity injected)\n";
    }
    else
    {
      std::cout << "synthetic_test: false (common-source portable fixture)\n";
    }
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "calibration_capture failed: " << ex.what() << "\n";
    return 1;
  }
}
