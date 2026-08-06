#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "audio/wav_io.hpp"
#include "dsp/beamformer.hpp"

namespace
{
struct SteeringEvent
{
  std::size_t frame_index = 0;
  sonitude::audio::BeamformerSteering target{};
};

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_wav_replay --input <six_channel_wav> --config <runtime_yaml>\n"
            << "                      --script <steering_csv> --output <mono_wav>\n";
}

std::vector<SteeringEvent> LoadSteeringScript(const std::string& path, const std::uint32_t sample_rate_hz)
{
  std::ifstream in(path);
  if (!in)
  {
    throw std::runtime_error("Unable to open steering script: " + path);
  }

  std::vector<SteeringEvent> events;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#')
    {
      continue;
    }
    std::replace(line.begin(), line.end(), '\t', ',');
    std::stringstream ss(line);
    std::string t_s;
    std::string az_s;
    std::string el_s;
    if (!std::getline(ss, t_s, ',') || !std::getline(ss, az_s, ',') || !std::getline(ss, el_s, ','))
    {
      // allow optional header line
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(t_s[0])) && t_s[0] != '-' && t_s[0] != '+')
    {
      continue;
    }
    const double t = std::stod(t_s);
    const float az = std::stof(az_s);
    const float el = std::stof(el_s);
    events.push_back(
        {static_cast<std::size_t>(std::max(0.0, t) * static_cast<double>(sample_rate_hz)), {az, el}});
  }
  std::sort(events.begin(), events.end(), [](const SteeringEvent& a, const SteeringEvent& b) {
    return a.frame_index < b.frame_index;
  });
  if (events.empty())
  {
    events.push_back({0, {0.0F, 0.0F}});
  }
  return events;
}
}  // namespace

int main(int argc, char** argv)
{
  std::string input_path;
  std::string config_path = "config/default.yaml";
  std::string script_path;
  std::string output_path;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--input" && i + 1 < argc)
    {
      input_path = argv[++i];
    }
    else if (arg == "--config" && i + 1 < argc)
    {
      config_path = argv[++i];
    }
    else if (arg == "--script" && i + 1 < argc)
    {
      script_path = argv[++i];
    }
    else if (arg == "--output" && i + 1 < argc)
    {
      output_path = argv[++i];
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else
    {
      std::cerr << "Unknown or incomplete argument: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }

  if (input_path.empty() || script_path.empty() || output_path.empty())
  {
    PrintUsage();
    return 2;
  }

  try
  {
    const auto runtime = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    const auto geometry = sonitude::app::LoadGeometryFromFile(runtime.geometry_path);
    const auto calibration = sonitude::app::LoadCalibrationFromFile(runtime.calibration_path);
    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }
    sonitude::app::ValidateCalibrationConfig(calibration, geometry_ids, runtime.capture.sample_rate_hz);

    const auto input_wav = sonitude::audio::ReadWavFile(input_path);
    if (input_wav.channels < sonitude::audio::kMicChannels)
    {
      throw std::runtime_error("Input WAV must contain at least six channels");
    }
    if (input_wav.sample_rate_hz != runtime.capture.sample_rate_hz)
    {
      throw std::runtime_error("Input WAV sample rate must match runtime capture sample_rate_hz");
    }

    const std::size_t frames = input_wav.interleaved.size() / input_wav.channels;
    std::vector<sonitude::audio::MicFrame> mic(frames);
    for (std::size_t i = 0; i < frames; ++i)
    {
      sonitude::audio::MicFrame frame{};
      for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
      {
        frame[ch] = input_wav.interleaved[i * input_wav.channels + runtime.active_channel_map[ch]];
      }
      mic[i] = frame;
    }

    const auto events = LoadSteeringScript(script_path, input_wav.sample_rate_hz);
    sonitude::dsp::DelaySumBeamformer beamformer;
    beamformer.configure(
        geometry, runtime.steering, calibration, input_wav.sample_rate_hz, runtime.capture.period_frames);
    beamformer.setTarget(events.front().target);

    std::vector<float> mono(frames, 0.0F);
    std::size_t event_index = 1;
    constexpr std::size_t kBlock = 256;
    for (std::size_t start = 0; start < frames; start += kBlock)
    {
      while (event_index < events.size() && events[event_index].frame_index <= start)
      {
        beamformer.setTarget(events[event_index].target);
        ++event_index;
      }
      const std::size_t count = std::min(kBlock, frames - start);
      beamformer.process(std::span<const sonitude::audio::MicFrame>(mic.data() + start, count),
                         std::span<float>(mono.data() + start, count));
    }

    sonitude::audio::WavData out;
    out.sample_rate_hz = input_wav.sample_rate_hz;
    out.channels = 1;
    out.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    out.interleaved = std::move(mono);
    sonitude::audio::WriteWavFile(output_path, out);
    std::cout << "Rendered beamformed WAV to " << output_path << '\n';
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "wav_replay failed: " << ex.what() << '\n';
    return 1;
  }
}
