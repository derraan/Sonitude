#include "tools/wav_replay_support.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace sonitude::tools::wav_replay
{
namespace
{
std::string Trim(const std::string& value)
{
  std::size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
  {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
  {
    --last;
  }
  return value.substr(first, last - first);
}

std::vector<std::string> SplitCsvLine(std::string line)
{
  std::replace(line.begin(), line.end(), '\t', ',');
  std::stringstream ss(line);
  std::vector<std::string> fields;
  std::string field;
  while (std::getline(ss, field, ','))
  {
    fields.push_back(Trim(field));
  }
  if (!line.empty() && line.back() == ',')
  {
    fields.emplace_back("");
  }
  return fields;
}

double ParseDoubleExact(const std::string& value, const std::size_t line_number, const char* name)
{
  std::size_t consumed = 0;
  try
  {
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size())
    {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": invalid trailing characters in " + name + " field");
    }
    return parsed;
  }
  catch (const std::exception&)
  {
    throw std::runtime_error("line " + std::to_string(line_number) + ": invalid " + name + " field");
  }
}

float ParseFloatExact(const std::string& value, const std::size_t line_number, const char* name)
{
  std::size_t consumed = 0;
  try
  {
    const float parsed = std::stof(value, &consumed);
    if (consumed != value.size())
    {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": invalid trailing characters in " + name + " field");
    }
    return parsed;
  }
  catch (const std::exception&)
  {
    throw std::runtime_error("line " + std::to_string(line_number) + ": invalid " + name + " field");
  }
}

bool LooksLikeHeaderTimeField(const std::string& value)
{
  std::string lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower == "t" || lower == "time" || lower == "time_s" || lower == "seconds";
}
}  // namespace

std::vector<SteeringEvent> LoadSteeringScript(const std::string& path,
                                              const std::uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0)
  {
    throw std::runtime_error("sample_rate_hz must be non-zero");
  }

  std::ifstream in(path);
  if (!in)
  {
    throw std::runtime_error("Unable to open steering script: " + path);
  }

  std::vector<SteeringEvent> events;
  std::string line;
  std::size_t line_number = 0;
  bool parsed_any_row = false;
  while (std::getline(in, line))
  {
    ++line_number;
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#')
    {
      continue;
    }

    const auto fields = SplitCsvLine(trimmed);
    if (fields.size() != 3U)
    {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": expected exactly 3 CSV columns (time_s,azimuth_deg,elevation_deg)");
    }
    if (fields[0].empty() || fields[1].empty() || fields[2].empty())
    {
      throw std::runtime_error("line " + std::to_string(line_number) + ": empty CSV field");
    }

    if (!parsed_any_row && LooksLikeHeaderTimeField(fields[0]))
    {
      parsed_any_row = true;
      continue;
    }
    parsed_any_row = true;

    const double time_s = ParseDoubleExact(fields[0], line_number, "time_s");
    if (!std::isfinite(time_s) || time_s < 0.0)
    {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": time_s must be finite and non-negative");
    }
    const float azimuth_deg = ParseFloatExact(fields[1], line_number, "azimuth_deg");
    const float elevation_deg = ParseFloatExact(fields[2], line_number, "elevation_deg");
    if (!std::isfinite(azimuth_deg) || !std::isfinite(elevation_deg))
    {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": steering angles must be finite");
    }

    const double frame_index_d = time_s * static_cast<double>(sample_rate_hz);
    if (frame_index_d > static_cast<double>(std::numeric_limits<std::size_t>::max()))
    {
      throw std::runtime_error("line " + std::to_string(line_number) +
                               ": frame index overflow from time/sample_rate");
    }
    events.push_back({static_cast<std::size_t>(frame_index_d), {azimuth_deg, elevation_deg}});
  }

  std::sort(events.begin(), events.end(), [](const SteeringEvent& a, const SteeringEvent& b) {
    return a.frame_index < b.frame_index;
  });
  return events;
}

std::vector<audio::MicFrame> ExtractMappedMicFrames(const audio::WavData& wav,
                                                    const std::vector<std::size_t>& channel_map)
{
  if (wav.channels == 0)
  {
    throw std::runtime_error("input WAV must have at least one channel");
  }
  if ((wav.interleaved.size() % wav.channels) != 0)
  {
    throw std::runtime_error("input WAV interleaved sample count is not channel-aligned");
  }
  if (channel_map.size() != audio::kMicChannels)
  {
    throw std::runtime_error("active_channel_map must contain exactly six channels");
  }

  for (std::size_t logical_channel = 0; logical_channel < channel_map.size(); ++logical_channel)
  {
    if (channel_map[logical_channel] >= wav.channels)
    {
      throw std::runtime_error("active_channel_map index " + std::to_string(channel_map[logical_channel]) +
                               " is out of range for input WAV with " +
                               std::to_string(wav.channels) + " channels");
    }
  }

  const std::size_t frames = wav.interleaved.size() / wav.channels;
  std::vector<audio::MicFrame> out(frames);
  for (std::size_t frame_index = 0; frame_index < frames; ++frame_index)
  {
    audio::MicFrame frame{};
    for (std::size_t mic_channel = 0; mic_channel < audio::kMicChannels; ++mic_channel)
    {
      frame[mic_channel] =
          wav.interleaved[(frame_index * wav.channels) + channel_map[mic_channel]];
    }
    out[frame_index] = frame;
  }
  return out;
}

std::vector<ReplaySegment> BuildReplaySegments(const std::size_t total_frames,
                                               const std::size_t block_frames,
                                               const std::vector<SteeringEvent>& events,
                                               const audio::BeamformerSteering neutral_target)
{
  if (block_frames == 0)
  {
    throw std::runtime_error("block_frames must be non-zero");
  }

  std::vector<ReplaySegment> segments;
  segments.reserve((total_frames / block_frames) + events.size() + 1U);

  audio::BeamformerSteering current_target = neutral_target;
  std::size_t event_index = 0;
  if (!events.empty() && events.front().frame_index == 0)
  {
    current_target = events.front().target;
    event_index = 1;
  }

  for (std::size_t block_start = 0; block_start < total_frames; block_start += block_frames)
  {
    const std::size_t block_end = std::min(block_start + block_frames, total_frames);
    while (event_index < events.size() && events[event_index].frame_index <= block_start)
    {
      current_target = events[event_index].target;
      ++event_index;
    }

    std::size_t segment_start = block_start;
    while (event_index < events.size() && events[event_index].frame_index < block_end)
    {
      const std::size_t event_frame = events[event_index].frame_index;
      if (event_frame > segment_start)
      {
        segments.push_back({segment_start, event_frame - segment_start, current_target});
      }
      current_target = events[event_index].target;
      segment_start = event_frame;
      ++event_index;
    }

    if (segment_start < block_end)
    {
      segments.push_back({segment_start, block_end - segment_start, current_target});
    }
  }

  return segments;
}
}  // namespace sonitude::tools::wav_replay
