#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
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
#include "dsp/biquad_cascade.hpp"
#include "dsp/calibration_applier.hpp"
#include "dsp/hrtf_table.hpp"
#include "dsp/limiter.hpp"
#include "dsp/suppression_stage.hpp"

namespace
{
struct SteeringEvent
{
  std::size_t frame_index = 0;
  sonitude::audio::BeamformerSteering target{};
  float width_deg = 0.0F;
};

// Beam "width" has no native meaning in MvdrBeamformer. This tool
// defines width as a directivity blend: the beamformed mono output is
// linearly blended toward a simple omnidirectional average of the six
// calibrated mic channels, in proportion to width_deg / kMaxWidthDeg.
// 0 deg = fully directional (pure beamformer output);
// kMaxWidthDeg = fully omnidirectional. Implemented in this tool around
// MvdrBeamformer output. See testbench/README.md, "Steering width definition".
constexpr float kMaxWidthDeg = 180.0F;

enum class SuppressionMode
{
  Auto,
  On,
  Off
};

SuppressionMode ParseSuppressionMode(const std::string& value)
{
  if (value == "on" || value == "enable" || value == "enabled")
  {
    return SuppressionMode::On;
  }
  if (value == "off" || value == "disable" || value == "disabled")
  {
    return SuppressionMode::Off;
  }
  return SuppressionMode::Auto;
}

bool ResolveSuppression(const SuppressionMode mode, const bool yaml_enabled)
{
  if (mode == SuppressionMode::On)
  {
    return true;
  }
  if (mode == SuppressionMode::Off)
  {
    return false;
  }
  return yaml_enabled;
}

sonitude::dsp::BinauralBackend ParseBinauralBackend(const std::string& backend)
{
  if (backend.empty() || backend == "mono_reference")
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
  if (backend == "array_downmix")
  {
    return sonitude::dsp::BinauralBackend::ArrayDownmix;
  }
  throw std::runtime_error("Unknown binaural backend: " + backend);
}

std::string SiblingFile(const std::string& path, const std::string& filename)
{
  const auto pos = path.find_last_of("/\\");
  if (pos == std::string::npos)
  {
    return filename;
  }
  return path.substr(0, pos + 1U) + filename;
}

sonitude::dsp::ArrayDownmixWeights ArrayDownmixForGeometry(
    const sonitude::app::GeometryConfig& geometry)
{
  std::vector<double> mic_x;
  mic_x.reserve(geometry.microphones.size());
  for (const auto& mic : geometry.microphones)
  {
    mic_x.push_back(mic.x);
  }
  return sonitude::dsp::MakeArrayDownmixWeights(mic_x);
}

std::string TablePathForBackend(const sonitude::app::BinauralConfig& binaural,
                                const sonitude::dsp::BinauralBackend backend)
{
  if (backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
  {
    if (binaural.profile.table_path.empty())
    {
      return {};
    }
    return SiblingFile(binaural.profile.table_path, "reference.shrf");
  }
  if (backend == sonitude::dsp::BinauralBackend::CompactHrtf)
  {
    return binaural.profile.table_path;
  }
  return {};
}

std::vector<sonitude::dsp::BiquadSectionSpec> BuildEqSpecs(
    const std::vector<sonitude::app::CalibrationChannel::EqSection>& sections)
{
  std::vector<sonitude::dsp::BiquadSectionSpec> out;
  out.reserve(sections.size());
  for (const auto& sec : sections)
  {
    out.push_back({
        .type = sonitude::dsp::BiquadCascade::ParseType(sec.type),
        .freq_hz = sec.freq_hz,
        .gain_db = sec.gain_db,
        .q = sec.q,
        .enabled = true,
    });
  }
  return out;
}

std::vector<sonitude::dsp::BiquadSectionSpec> BuildEqSpecs(
    const std::vector<sonitude::app::EqSectionConfig>& sections)
{
  std::vector<sonitude::dsp::BiquadSectionSpec> out;
  out.reserve(sections.size());
  for (const auto& sec : sections)
  {
    out.push_back({
        .type = sonitude::dsp::BiquadCascade::ParseType(sec.type),
        .freq_hz = sec.freq_hz,
        .gain_db = sec.gain_db,
        .q = sec.q,
        .enabled = true,
    });
  }
  return out;
}

void PrintCapabilities()
{
  std::cout << "{"
            << "\"protocol_version\":2,"
            << "\"suppression\":{\"modes\":[\"auto\",\"on\",\"off\"],"
               "\"backends\":[\"conservative\",\"spectral\"]},"
            << "\"taps\":[\"beamformed\",\"suppressed\",\"processed\",\"binaural\"],"
            << "\"binaural\":{"
            << "\"available\":true,"
            << "\"backends\":[\"array_downmix\",\"mono_reference\",\"itd_ild\",\"compact_hrtf\",\"full_hrtf_reference\"],"
            << "\"unavailable_backends\":[],"
            << "\"note\":\"array_downmix folds calibrated 6-mic capture to stereo using "
               "geometry-weighted ear hemispheres. itd_ild/HRTF backends virtualize "
               "beamformed mono instead. compact_16/32/64 are experimental raw-HRIR prefix "
               "candidates. mono_reference remains L=R of processed mono; --output stays 1ch.\""
            << "}"
            << "}\n";
}

void PrintUsage()
{
  std::cout << "Usage:\n"
            << "  sonitude_wav_replay --input <six_channel_wav> --config <runtime_yaml>\n"
            << "                      --script <steering_csv> --output <mono_wav>\n"
            << "                      [--suppression auto|on|off]\n"
            << "                      [--suppression-backend conservative|spectral]\n"
            << "                      [--enable-suppression] [--disable-suppression] [--disable-limiter]\n"
            << "                      [--output-beamformed <mono_wav>]\n"
            << "                      [--output-suppressed <mono_wav>]\n"
            << "                      [--output-mono-pre-binaural <mono_wav>]\n"
            << "                      [--output-binaural <stereo_wav>] [--binaural-backend <name>]\n"
            << "                      [--binaural-follow-steering] [--binaural-fixed-direction]\n"
            << "                      [--binaural-azimuth <deg>] [--binaural-elevation <deg>]\n"
            << "                      [--capabilities]\n"
            << "\n"
            << "  --output-beamformed writes the mono signal immediately after the\n"
            << "  beamformer (including the directional/omni blend), before suppression\n"
            << "  or limiting are applied.\n"
            << "  --output-suppressed writes the mono signal after suppression (if\n"
            << "  enabled) but before limiting. --output-mono-pre-binaural is the same tap.\n"
            << "  Both are diagnostic taps only; they do not change the final --output render.\n"
            << "  --output-binaural writes stereo via the requested backend. --output stays mono.\n"
            << "  --binaural-follow-steering / --binaural-fixed-direction override YAML\n"
            << "  binaural.direction.follow_steering. Fixed azimuth/elevation override YAML\n"
            << "  only when follow-steering is off (CLI flag or YAML).\n"
            << "\n"
            << "  Steering script columns: time_s,azimuth_deg,elevation_deg[,directivity_blend_deg]\n"
            << "  directivity_blend_deg (0-" << kMaxWidthDeg
            << ", default 0) mixes beamformer output toward\n"
            << "  a six-microphone average. This is NOT measured physical beamwidth.\n";
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
  std::string binaural_backend = "mono_reference";
  std::optional<bool> binaural_follow_override;
  std::optional<float> binaural_azimuth_override;
  std::optional<float> binaural_elevation_override;
  SuppressionMode suppression_mode = SuppressionMode::Auto;
  std::optional<std::string> suppression_backend_override;
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
    else if (arg == "--output-mono-pre-binaural" && i + 1 < argc)
    {
      output_mono_pre_binaural_path = argv[++i];
    }
    else if (arg == "--output-binaural" && i + 1 < argc)
    {
      output_binaural_path = argv[++i];
    }
    else if (arg == "--binaural-backend" && i + 1 < argc)
    {
      binaural_backend = argv[++i];
    }
    else if (arg == "--binaural-follow-steering")
    {
      binaural_follow_override = true;
    }
    else if (arg == "--binaural-fixed-direction")
    {
      binaural_follow_override = false;
    }
    else if (arg == "--binaural-azimuth" && i + 1 < argc)
    {
      binaural_azimuth_override = std::stof(argv[++i]);
    }
    else if (arg == "--binaural-elevation" && i + 1 < argc)
    {
      binaural_elevation_override = std::stof(argv[++i]);
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else if (arg == "--capabilities")
    {
      PrintCapabilities();
      return 0;
    }
    else if (arg == "--suppression" && i + 1 < argc)
    {
      suppression_mode = ParseSuppressionMode(argv[++i]);
    }
    else if (arg == "--suppression-backend" && i + 1 < argc)
    {
      suppression_backend_override = argv[++i];
    }
    else if (arg == "--enable-suppression")
    {
      suppression_mode = SuppressionMode::On;
    }
    else if (arg == "--disable-suppression")
    {
      suppression_mode = SuppressionMode::Off;
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
    for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
    {
      if (runtime.active_channel_map[ch] >= input_wav.channels)
      {
        throw std::runtime_error("active_channel_map index exceeds input WAV channel count");
      }
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
    std::array<sonitude::dsp::BiquadCascade, sonitude::audio::kMicChannels> per_mic_eq{};
    std::array<bool, sonitude::audio::kMicChannels> per_mic_eq_enabled{};
    for (std::size_t i = 0; i < sonitude::audio::kMicChannels; ++i)
    {
      const auto specs = BuildEqSpecs(calibration.channels[i].eq.sections);
      per_mic_eq[i].configure(
          runtime.capture.sample_rate_hz, specs, 1U, calibration.channels[i].eq.enabled && !specs.empty());
      per_mic_eq_enabled[i] = calibration.channels[i].eq.enabled && !specs.empty();
    }
    sonitude::dsp::BiquadCascade common_eq;
    const auto common_eq_specs = BuildEqSpecs(runtime.common_eq.sections);
    common_eq.configure(runtime.capture.sample_rate_hz,
                        common_eq_specs,
                        1U,
                        runtime.common_eq.enabled && !common_eq_specs.empty());
    calibration_applier.processBlock(
        std::span<const sonitude::audio::MicFrame>(mic.data(), mic.size()),
        std::span<sonitude::audio::MicFrame>(calibrated.data(), calibrated.size()));
    for (auto& frame : calibrated)
    {
      for (std::size_t ch = 0; ch < sonitude::audio::kMicChannels; ++ch)
      {
        if (per_mic_eq_enabled[ch])
        {
          frame[ch] = per_mic_eq[ch].processSample(0U, frame[ch]);
        }
      }
    }

    const auto events = LoadSteeringScript(script_path, input_wav.sample_rate_hz);
    sonitude::dsp::MvdrBeamformer beamformer;
    beamformer.configure(
        geometry, runtime.steering, calibration, input_wav.sample_rate_hz, runtime.capture.period_frames);
    beamformer.setTarget(events.front().target);
    float current_width_deg = events.front().width_deg;
    const bool suppression_enabled = ResolveSuppression(suppression_mode, runtime.suppression.enabled);
    const char* requested = suppression_mode == SuppressionMode::On
                                ? "on"
                                : (suppression_mode == SuppressionMode::Off ? "off" : "auto");
    const std::string backend_name =
        suppression_backend_override.value_or(runtime.suppression.backend);
    const auto suppression_backend =
        sonitude::dsp::ParseSuppressionBackend(backend_name.empty() ? "conservative" : backend_name);
    const bool binaural_enabled = !output_binaural_path.empty();
    const bool binaural_follow =
        binaural_follow_override.value_or(runtime.binaural.direction.follow_steering);
    const float binaural_az =
        binaural_azimuth_override.value_or(runtime.binaural.direction.azimuth_deg);
    const float binaural_el =
        binaural_elevation_override.value_or(runtime.binaural.direction.elevation_deg);
    sonitude::dsp::BinauralBackend backend = sonitude::dsp::BinauralBackend::MonoReference;
    constexpr std::size_t kBlock = 256;

    std::unique_ptr<sonitude::dsp::HrtfTable> hrtf_table;
    sonitude::dsp::BinauralRenderer binaural;
    if (binaural_enabled)
    {
      backend = ParseBinauralBackend(binaural_backend);
      const std::string table_path = TablePathForBackend(runtime.binaural, backend);
      if (backend == sonitude::dsp::BinauralBackend::CompactHrtf ||
          backend == sonitude::dsp::BinauralBackend::FullHrtfReference)
      {
        if (table_path.empty())
        {
          throw std::runtime_error("binaural.profile.table_path is required for HRTF backends");
        }
        hrtf_table = std::make_unique<sonitude::dsp::HrtfTable>(
            sonitude::dsp::LoadHrtfTableFromFile(table_path));
      }
      binaural.configure({.sample_rate_hz = input_wav.sample_rate_hz,
                          .backend = backend,
                          .transition_ms = runtime.binaural.transition.duration_ms,
                          .max_block_frames = kBlock,
                          .itd_ild = {.head_radius_m = runtime.binaural.model.head_radius_m,
                                      .max_ild_db = runtime.binaural.model.max_ild_db},
                          .table = hrtf_table.get(),
                          .array_downmix = ArrayDownmixForGeometry(geometry)});
      if (backend != sonitude::dsp::BinauralBackend::ArrayDownmix)
      {
        binaural.setDirection(binaural_follow
                                  ? events.front().target
                                  : sonitude::audio::BeamformerSteering{binaural_az, binaural_el});
      }
    }

    sonitude::dsp::SuppressionStage suppressor;
    suppressor.configure({.enabled = suppression_enabled,
                          .backend = suppression_backend,
                          .sample_rate_hz = input_wav.sample_rate_hz,
                          .maximum_block_frames = kBlock,
                          .conservative = {.ambient_floor_linear = runtime.steering.ambient_floor_linear,
                                           .fade_ms = runtime.suppression.fade_ms,
                                           .activity_threshold = runtime.suppression.activity_threshold,
                                           .confidence_threshold = runtime.suppression.confidence_threshold},
                          .spectral = {.enabled = true,
                                       .fft_size = runtime.suppression.spectral.fft_size,
                                       .hop_size = runtime.suppression.spectral.hop_size,
                                       .gain_floor_db = runtime.suppression.spectral.gain_floor_db,
                                       .confidence_threshold = runtime.suppression.confidence_threshold}});
    beamformer.setSpectralPostfilter(suppressor.spectralFilter());
    std::cerr << "sonitude_resolved {\"protocol_version\":2,\"suppression_requested\":\"" << requested
              << "\",\"suppression_resolved\":" << (suppression_enabled ? "true" : "false")
              << ",\"suppression_backend_requested\":\"" << backend_name << "\""
              << ",\"suppression_backend_resolved\":\""
              << sonitude::dsp::SuppressionBackendName(suppression_backend) << "\""
              << ",\"suppression_fft_size\":" << runtime.suppression.spectral.fft_size
              << ",\"suppression_hop_size\":" << runtime.suppression.spectral.hop_size
              << ",\"suppression_gain_floor_db\":" << runtime.suppression.spectral.gain_floor_db
              << ",\"suppression_algorithmic_delay_samples\":0"
              << ",\"limiter_disabled\":" << (disable_limiter ? "true" : "false")
              << ",\"binaural_backend\":\"" << binaural_backend << "\",\"binaural_available\":true"
              << ",\"binaural_follow_steering\":" << (binaural_follow ? "true" : "false")
              << ",\"binaural_azimuth_deg\":" << binaural_az
              << ",\"binaural_elevation_deg\":" << binaural_el << "}\n";
    sonitude::dsp::PeakLimiter limiter;
    limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, input_wav.sample_rate_hz);
    sonitude::dsp::StereoPeakLimiter stereo_limiter;
    if (binaural_enabled)
    {
      stereo_limiter.configure({.ceiling_linear = 0.95F, .release_ms = 80.0F}, input_wav.sample_rate_hz);
    }

    std::vector<float> mono(frames, 0.0F);
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> beamformed_tap;
    std::vector<float> suppressed_tap;
    std::vector<float> binaural_tap;
    if (binaural_enabled)
    {
      left.assign(frames, 0.0F);
      right.assign(frames, 0.0F);
    }
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
    constexpr std::size_t kProcessBlock = kBlock;
    for (std::size_t start = 0; start < frames; start += kProcessBlock)
    {
      while (event_index < events.size() && events[event_index].frame_index <= start)
      {
        beamformer.setTarget(events[event_index].target);
        if (binaural_enabled && binaural_follow)
        {
          binaural.setDirection(events[event_index].target);
        }
        current_width_deg = events[event_index].width_deg;
        ++event_index;
      }
      const std::size_t count = std::min(kProcessBlock, frames - start);
      if (suppression_enabled)
      {
        suppressor.setControl(true, 1.0F);
      }
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
      suppressor.process(std::span<float>(mono.data() + start, count));
      if (!suppressed_tap.empty())
      {
        std::copy(mono.begin() + static_cast<std::ptrdiff_t>(start),
                  mono.begin() + static_cast<std::ptrdiff_t>(start + count),
                  suppressed_tap.begin() + static_cast<std::ptrdiff_t>(start));
      }
      if (common_eq.enabled())
      {
        common_eq.processMono(std::span<float>(mono.data() + start, count));
      }
      if (!disable_limiter)
      {
        limiter.process(std::span<float>(mono.data() + start, count));
      }
      if (binaural_enabled)
      {
        if (backend == sonitude::dsp::BinauralBackend::ArrayDownmix)
        {
          binaural.processArray(std::span<const sonitude::audio::MicFrame>(calibrated.data() + start, count),
                                std::span<float>(left.data() + start, count),
                                std::span<float>(right.data() + start, count));
        }
        else
        {
          binaural.process(std::span<const float>(mono.data() + start, count),
                           std::span<float>(left.data() + start, count),
                           std::span<float>(right.data() + start, count));
        }
        if (!disable_limiter)
        {
          stereo_limiter.process(std::span<float>(left.data() + start, count),
                                 std::span<float>(right.data() + start, count));
        }
        if (!binaural_tap.empty())
        {
          for (std::size_t i = 0; i < count; ++i)
          {
            const std::size_t out_index = (start + i) * 2U;
            binaural_tap[out_index] = left[start + i];
            binaural_tap[out_index + 1U] = right[start + i];
          }
        }
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
      tap.interleaved = suppressed_tap;
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
      std::cout << "Wrote binaural stereo (" << binaural_backend << ") to " << output_binaural_path << '\n';
    }
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "wav_replay failed: " << ex.what() << '\n';
    return 1;
  }
}
