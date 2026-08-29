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
#include "dsp/calibration_applier.hpp"
#include "dsp/limiter.hpp"
#include "dsp/suppressor.hpp"

namespace
{
struct SteeringEvent
{
  std::size_t frame_index = 0;
  sonitude::audio::BeamformerSteering target{};
  float width_deg = 0.0F;
};

// Beam "width" has no native meaning in DelaySumBeamformer (a fixed
// delay-and-sum array has no adjustable spatial width parameter). This tool
// defines width as a directivity blend: the beamformed mono output is
// linearly blended toward a simple omnidirectional average of the six
// calibrated mic channels, in proportion to width_deg / kMaxWidthDeg.
// 0 deg = fully directional (pure beamformer output, current behavior);
// kMaxWidthDeg = fully omnidirectional. This is implemented entirely in this
// tool around the unmodified IBeamformer output; DelaySumBeamformer itself
// is untouched. See testbench/README.md, "Steering width definition".
constexpr float kMaxWidthDeg = 180.0F;

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_wav_replay --input <six_channel_wav> --config <runtime_yaml>\n"
            << "                      --script <steering_csv> --output <mono_wav>\n"
            << "                      [--enable-suppression] [--disable-limiter]\n"
            << "                      [--output-beamformed <mono_wav>]\n"
            << "                      [--output-suppressed <mono_wav>]\n"
            << "\n"
            << "  --output-beamformed writes the mono signal immediately after the\n"
            << "  beamformer (including the width blend, see below), before suppression\n"
            << "  or limiting are applied.\n"
            << "  --output-suppressed writes the mono signal after suppression (if\n"
            << "  enabled) but before limiting. Both are diagnostic taps only; they do\n"
            << "  not change the final --output render.\n"
            << "\n"
            << "  Steering script columns: time_s,azimuth_deg,elevation_deg[,width_deg]\n"
            << "  width_deg (0-" << kMaxWidthDeg << ", default 0) blends the beamformer\n"
            << "  output toward an omnidirectional average; see --help text above.\n";
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
    std::string width_s;
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
    // Optional 4th column, width_deg; defaults to 0 (fully directional) for
    // backward compatibility with existing 3-column scripts.
    float width = 0.0F;
    if (std::getline(ss, width_s, ',') && !width_s.empty())
    {
      width = std::clamp(std::stof(width_s), 0.0F, kMaxWidthDeg);
    }
    events.push_back(
        {static_cast<std::size_t>(std::max(0.0, t) * static_cast<double>(sample_rate_hz)), {az, el}, width});
  }
  std::sort(events.begin(), events.end(), [](const SteeringEvent& a, const SteeringEvent& b) {
    return a.frame_index < b.frame_index;
  });
  if (events.empty())
  {
    events.push_back({0, {0.0F, 0.0F}, 0.0F});
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
  std::string output_beamformed_path;
  std::string output_suppressed_path;
  bool enable_suppression = false;
  bool disable_limiter = false;

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
    else if (arg == "--output-beamformed" && i + 1 < argc)
    {
      output_beamformed_path = argv[++i];
    }
    else if (arg == "--output-suppressed" && i + 1 < argc)
    {
      output_suppressed_path = argv[++i];
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else if (arg == "--enable-suppression")
    {
      enable_suppression = true;
    }
    else if (arg == "--disable-limiter")
    {
      disable_limiter = true;
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
    std::vector<sonitude::audio::MicFrame> calibrated(frames);
    for (std::size_t i = 0; i < frames; ++i)
    {
      sonitude::audio::MicFrame frame{};
      for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
      {
        frame[ch] = input_wav.interleaved[i * input_wav.channels + runtime.active_channel_map[ch]];
      }
      mic[i] = frame;
    }
    sonitude::dsp::CalibrationApplier calibration_applier(
        calibration.channels, geometry_ids, runtime.capture.sample_rate_hz, runtime.calibration_dc_block_hz);
    calibration_applier.processBlock(
        std::span<const sonitude::audio::MicFrame>(mic.data(), mic.size()),
        std::span<sonitude::audio::MicFrame>(calibrated.data(), calibrated.size()));

    const auto events = LoadSteeringScript(script_path, input_wav.sample_rate_hz);
    sonitude::dsp::DelaySumBeamformer beamformer;
    beamformer.configure(
        geometry, runtime.steering, calibration, input_wav.sample_rate_hz, runtime.capture.period_frames);
    beamformer.setTarget(events.front().target);
    float current_width_deg = events.front().width_deg;
    // --enable-suppression forces suppression on regardless of config; when
    // not passed, fall back to the config's own suppression.enabled, the
    // same precedence sonitude_stream_process and sonitude_realtime use.
    const bool suppression_enabled = enable_suppression || runtime.suppression.enabled;
    sonitude::dsp::ConservativeSuppressor suppressor;
    suppressor.configure(
        {.ambient_floor_linear = runtime.steering.ambient_floor_linear,
         .fade_ms = runtime.suppression.fade_ms,
         .activity_threshold = runtime.suppression.activity_threshold,
         .confidence_threshold = runtime.suppression.confidence_threshold},
        input_wav.sample_rate_hz);
    sonitude::dsp::PeakLimiter limiter;
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, input_wav.sample_rate_hz);

    std::vector<float> mono(frames, 0.0F);
    std::vector<float> beamformed_tap;
    std::vector<float> suppressed_tap;
    if (!output_beamformed_path.empty())
    {
      beamformed_tap.resize(frames, 0.0F);
    }
    if (!output_suppressed_path.empty())
    {
      suppressed_tap.resize(frames, 0.0F);
    }

    std::size_t event_index = 1;
    constexpr std::size_t kBlock = 256;
    for (std::size_t start = 0; start < frames; start += kBlock)
    {
      while (event_index < events.size() && events[event_index].frame_index <= start)
      {
        beamformer.setTarget(events[event_index].target);
        current_width_deg = events[event_index].width_deg;
        ++event_index;
      }
      const std::size_t count = std::min(kBlock, frames - start);
      beamformer.process(std::span<const sonitude::audio::MicFrame>(calibrated.data() + start, count),
                         std::span<float>(mono.data() + start, count));
      if (current_width_deg > 0.0F)
      {
        const float blend = std::clamp(current_width_deg / kMaxWidthDeg, 0.0F, 1.0F);
        for (std::size_t i = start; i < start + count; ++i)
        {
          float omni = 0.0F;
          for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
          {
            omni += calibrated[i][ch];
          }
          omni /= static_cast<float>(sonitude::audio::kMicChannels);
          mono[i] = ((1.0F - blend) * mono[i]) + (blend * omni);
        }
      }
      if (!beamformed_tap.empty())
      {
        std::copy(mono.begin() + static_cast<std::ptrdiff_t>(start),
                  mono.begin() + static_cast<std::ptrdiff_t>(start + count),
                  beamformed_tap.begin() + static_cast<std::ptrdiff_t>(start));
      }
      if (suppression_enabled)
      {
        suppressor.setControl(true, 1.0F);
        suppressor.process(std::span<float>(mono.data() + start, count));
      }
      if (!suppressed_tap.empty())
      {
        std::copy(mono.begin() + static_cast<std::ptrdiff_t>(start),
                  mono.begin() + static_cast<std::ptrdiff_t>(start + count),
                  suppressed_tap.begin() + static_cast<std::ptrdiff_t>(start));
      }
      if (!disable_limiter)
      {
        limiter.process(std::span<float>(mono.data() + start, count));
      }
    }

    sonitude::audio::WavData out;
    out.sample_rate_hz = input_wav.sample_rate_hz;
    out.channels = 1;
    out.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    out.interleaved = std::move(mono);
    sonitude::audio::WriteWavFile(output_path, out);
    std::cout << "Rendered beamformed WAV to " << output_path << '\n';

    if (!beamformed_tap.empty())
    {
      sonitude::audio::WavData tap;
      tap.sample_rate_hz = input_wav.sample_rate_hz;
      tap.channels = 1;
      tap.format = sonitude::audio::PcmFormat::FLOAT32_LE;
      tap.interleaved = std::move(beamformed_tap);
      sonitude::audio::WriteWavFile(output_beamformed_path, tap);
      std::cout << "Wrote pre-suppression beamformed tap to " << output_beamformed_path << '\n';
    }
    if (!suppressed_tap.empty())
    {
      sonitude::audio::WavData tap;
      tap.sample_rate_hz = input_wav.sample_rate_hz;
      tap.channels = 1;
      tap.format = sonitude::audio::PcmFormat::FLOAT32_LE;
      tap.interleaved = std::move(suppressed_tap);
      sonitude::audio::WriteWavFile(output_suppressed_path, tap);
      std::cout << "Wrote pre-limiter tap to " << output_suppressed_path << '\n';
    }
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "wav_replay failed: " << ex.what() << '\n';
    return 1;
  }
}
