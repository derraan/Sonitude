#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "app/calibration_writer.hpp"
#include "audio/wav_io.hpp"

namespace
{
float Mean(const std::vector<float>& v)
{
  if (v.empty())
  {
    return 0.0F;
  }
  double sum = 0.0;
  for (const float x : v)
  {
    sum += x;
  }
  return static_cast<float>(sum / static_cast<double>(v.size()));
}
}  // namespace

int main(int argc, char** argv)
{
  std::string in_path = "build/calibration_capture.wav";
  std::string out_path = "build/calibration_estimate.yaml";
  if (argc > 1)
  {
    in_path = argv[1];
  }
  if (argc > 2)
  {
    out_path = argv[2];
  }

  try
  {
    const auto wav = sonitude::audio::ReadWavFile(in_path);
    if (wav.channels != 6)
    {
      throw std::runtime_error("calibration_estimate expects a 6-channel WAV");
    }
    sonitude::app::CalibrationConfig cal;
    cal.sample_rate_hz = wav.sample_rate_hz;
    cal.channels.resize(6);
    for (std::size_t ch = 0; ch < 6; ++ch)
    {
      std::vector<float> sig;
      sig.reserve(wav.interleaved.size() / 6);
      for (std::size_t i = ch; i < wav.interleaved.size(); i += 6)
      {
        sig.push_back(wav.interleaved[i]);
      }
      const float dc = Mean(sig);
      double rms_sum = 0.0;
      double max_abs = 0.0;
      for (const float s : sig)
      {
        const float c = s - dc;
        rms_sum += static_cast<double>(c) * static_cast<double>(c);
        max_abs = std::max(max_abs, static_cast<double>(std::fabs(c)));
      }
      const float rms = static_cast<float>(std::sqrt(rms_sum / static_cast<double>(sig.size())));
      auto& out = cal.channels[ch];
      out.id = "M" + std::to_string(ch);
      out.polarity = 1;
      out.gain_linear = (rms > 1e-6F) ? (0.1F / rms) : 1.0F;
      out.delay_samples = 0.0F;
      out.dc_offset = dc;
      std::cout << "ch" << ch << " dc=" << dc << " rms=" << rms << " peak=" << max_abs << "\n";
    }
    sonitude::app::WriteCalibrationYamlBackupSafe(out_path, cal, true);
    std::cout << "Wrote calibration YAML: " << out_path << "\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "calibration_estimate failed: " << ex.what() << "\n";
    return 1;
  }
}
