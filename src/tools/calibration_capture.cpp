#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "audio/wav_io.hpp"

int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;
  // Portable fallback in M3: emit deterministic 6ch synthetic capture block that downstream
  // calibration_estimate can consume on non-Linux hosts.
  sonitude::audio::WavData wav;
  wav.sample_rate_hz = 44100;
  wav.channels = 6;
  wav.format = sonitude::audio::PcmFormat::FLOAT32_LE;
  constexpr std::size_t frames = 44100;
  wav.interleaved.resize(frames * wav.channels, 0.0F);
  for (std::size_t i = 0; i < frames; ++i)
  {
    const float t = static_cast<float>(i) / 44100.0F;
    for (std::size_t ch = 0; ch < wav.channels; ++ch)
    {
      wav.interleaved[i * wav.channels + ch] =
          0.1F * std::sin((2.0F * 3.1415926535F * (500.0F + (100.0F * static_cast<float>(ch)))) * t);
    }
  }
  sonitude::audio::WriteWavFile("build/calibration_capture.wav", wav);
  std::cout << "Wrote build/calibration_capture.wav (placeholder capture source)\n";
  return 0;
}
