#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "audio/wav_io.hpp"
#include "dsp/beamformer.hpp"
#include "dsp/binaural_renderer.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/hrtf_table.hpp"
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

sonitude::dsp::BinauralBackend ParseBinauralBackend(const std::string& backend)
{
  if (backend == "mono_reference")
  {
    return sonitude::dsp::BinauralBackend::MonoReference;
  }
  if (backend == "itd_ild")
  {
    return sonitude::dsp::BinauralBackend::ItdIld;
  }
  if (backend == "compact_hrtf")
  {
    return sonitude::dsp::BinauralBackend::CompactHrtf;
  }
  if (backend == "full_hrtf_reference")
  {
    return sonitude::dsp::BinauralBackend::FullHrtfReference;
  }
  throw std::runtime_error("Unknown binaural backend: " + backend);
}

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_wav_replay --input <six_channel_wav> --config <runtime_yaml>\n"
            << "                      --script <steering_csv> --output <mono_wav>\n"
            << "                      [--enable-suppression] [--disable-limiter]\n"
            << "                      [--output-beamformed <mono_wav>]\n"
            << "                      [--output-suppressed <mono_wav>]\n"
            << "                      [--binaural]\n"
            << "                      [--binaural-backend <mono_reference|itd_ild|compact_hrtf|full_hrtf_reference>]\n"
            << "                      [--binaural-profile <id>]\n"
            << "                      [--output-mono-pre-binaural <mono_wav>]\n"
            << "                      [--output-binaural <stereo_wav>]\n"
            << "\n"
            << "  --output-beamformed writes the mono signal immediately after the\n"
            << "  beamformer (including the width blend, see below), before suppression\n"
            << "  or limiting are applied.\n"
            << "  --output-suppressed writes the mono signal after suppression (if\n"
            << "  enabled) but before limiting. Both are diagnostic taps only; they do\n"
            << "  not change the final --output render.\n"
            << "  --output-mono-pre-binaural is an explicit alias for the same tap.\n"
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
  std::string output_mono_pre_binaural_path;
  std::string output_binaural_path;
  std::string binaural_backend_override;
  std::string binaural_profile_override;
  bool enable_suppression = false;
  bool disable_limiter = false;
  bool force_binaural = false;

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
    else if (arg == "--output-mono-pre-binaural" && i + 1 < argc)
    {
      output_mono_pre_binaural_path = argv[++i];
    }
    else if (arg == "--output-binaural" && i + 1 < argc)
    {
      output_binaural_path = argv[++i];
    }
    else if (arg == "--binaural")
    {
      force_binaural = true;
    }
    else if (arg == "--binaural-backend" && i + 1 < argc)
    {
      binaural_backend_override = argv[++i];
    }
    else if (arg == "--binaural-profile" && i + 1 < argc)
    {
      binaural_profile_override = argv[++i];
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
    constexpr std::size_t kBlock = 256;
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
    const bool binaural_enabled = force_binaural || runtime.binaural.enabled;
    const std::string requested_backend =
        binaural_backend_override.empty() ? runtime.binaural.backend : binaural_backend_override;
    const std::string requested_profile =
        binaural_profile_override.empty() ? runtime.binaural.profile.id : binaural_profile_override;

    std::unique_ptr<sonitude::dsp::HrtfTable> hrtf_table;
    sonitude::dsp::BinauralRenderer binaural;
    if (binaural_enabled)
    {
      const auto backend = ParseBinauralBackend(requested_backend);
      if (backend == sonitude::dsp::BinauralBackend::CompactHrtf ||
          backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
      {
        if (runtime.binaural.profile.table_path.empty())
        {
          throw std::runtime_error("binaural.profile.table_path is required for HRTF backends");
        }
        hrtf_table = std::make_unique<sonitude::dsp::HrtfTable>(
            sonitude::dsp::LoadHrtfTableFromFile(runtime.binaural.profile.table_path));
      }

      binaural.configure({.sample_rate_hz = input_wav.sample_rate_hz,
                          .backend = backend,
                          .transition_ms = runtime.binaural.transition.duration_ms,
                          .max_block_frames = kBlock,
                          .itd_ild = {.head_radius_m = runtime.binaural.model.head_radius_m,
                                      .max_ild_db = runtime.binaural.model.max_ild_db},
                          .table = hrtf_table.get()});
      binaural.setDirection(runtime.binaural.direction.follow_steering
                                ? events.front().target
                                : sonitude::audio::BeamformerSteering{
                                      runtime.binaural.direction.azimuth_deg,
                                      runtime.binaural.direction.elevation_deg});
      std::cout << "Binaural renderer enabled: backend=" << requested_backend
                << " profile=" << requested_profile << '\n';
    }

    sonitude::dsp::PeakLimiter limiter;
    sonitude::dsp::StereoPeakLimiter stereo_limiter;
    if (binaural_enabled)
    {
      stereo_limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, input_wav.sample_rate_hz);
    }
    else
    {
      limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, input_wav.sample_rate_hz);
    }

    std::vector<float> mono(frames, 0.0F);
    std::vector<float> left(frames, 0.0F);
    std::vector<float> right(frames, 0.0F);
    std::vector<float> beamformed_tap;
    std::vector<float> suppressed_tap;
    std::vector<float> binaural_tap;
    if (!output_beamformed_path.empty())
    {
      beamformed_tap.resize(frames, 0.0F);
    }
    if (!output_suppressed_path.empty() || !output_mono_pre_binaural_path.empty())
    {
      suppressed_tap.resize(frames, 0.0F);
    }
    if (!output_binaural_path.empty())
    {
      binaural_tap.resize(frames * 2U, 0.0F);
    }

    std::size_t event_index = 1;
    for (std::size_t start = 0; start < frames; start += kBlock)
    {
      while (event_index < events.size() && events[event_index].frame_index <= start)
      {
        beamformer.setTarget(events[event_index].target);
        if (binaural_enabled && runtime.binaural.direction.follow_steering)
        {
          binaural.setDirection(events[event_index].target);
        }
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
        if (binaural_enabled)
        {
          binaural.process(std::span<const float>(mono.data() + start, count),
                           std::span<float>(left.data() + start, count),
                           std::span<float>(right.data() + start, count));
          if (!binaural_tap.empty())
          {
            for (std::size_t i = 0; i < count; ++i)
            {
              const std::size_t out_index = (start + i) * 2U;
              binaural_tap[out_index] = left[start + i];
              binaural_tap[out_index + 1U] = right[start + i];
            }
          }
          stereo_limiter.process(std::span<float>(left.data() + start, count),
                                 std::span<float>(right.data() + start, count));
        }
        else
        {
          limiter.process(std::span<float>(mono.data() + start, count));
        }
      }
      else if (binaural_enabled)
      {
        binaural.process(std::span<const float>(mono.data() + start, count),
                         std::span<float>(left.data() + start, count),
                         std::span<float>(right.data() + start, count));
      }
    }

    sonitude::audio::WavData out;
    out.sample_rate_hz = input_wav.sample_rate_hz;
    out.channels = binaural_enabled ? 2 : 1;
    out.format = sonitude::audio::PcmFormat::FLOAT32_LE;
    if (binaural_enabled)
    {
      out.interleaved.resize(frames * 2U, 0.0F);
      for (std::size_t i = 0; i < frames; ++i)
      {
        out.interleaved[(i * 2U)] = left[i];
        out.interleaved[(i * 2U) + 1U] = right[i];
      }
    }
    else
    {
      out.interleaved = std::move(mono);
    }
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
      if (!output_suppressed_path.empty())
      {
        sonitude::audio::WriteWavFile(output_suppressed_path, tap);
        std::cout << "Wrote pre-limiter tap to " << output_suppressed_path << '\n';
      }
      if (!output_mono_pre_binaural_path.empty())
      {
        sonitude::audio::WriteWavFile(output_mono_pre_binaural_path, tap);
        std::cout << "Wrote pre-binaural mono tap to " << output_mono_pre_binaural_path << '\n';
      }
    }
    if (!binaural_tap.empty())
    {
      sonitude::audio::WavData tap;
      tap.sample_rate_hz = input_wav.sample_rate_hz;
      tap.channels = 2;
      tap.format = sonitude::audio::PcmFormat::FLOAT32_LE;
      tap.interleaved = std::move(binaural_tap);
      sonitude::audio::WriteWavFile(output_binaural_path, tap);
      std::cout << "Wrote post-binaural stereo tap to " << output_binaural_path << '\n';
    }
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "wav_replay failed: " << ex.what() << '\n';
    return 1;
  }
}
