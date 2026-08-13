#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/audio_types.hpp"
#include "audio/wav_io.hpp"

namespace sonitude::tools::wav_replay
{
struct SteeringEvent
{
  std::size_t frame_index = 0;
  audio::BeamformerSteering target{};
};

struct ReplaySegment
{
  std::size_t start_frame = 0;
  std::size_t frame_count = 0;
  audio::BeamformerSteering target{};
};

std::vector<SteeringEvent> LoadSteeringScript(const std::string& path, std::uint32_t sample_rate_hz);

std::vector<audio::MicFrame> ExtractMappedMicFrames(const audio::WavData& wav,
                                                    const std::vector<std::size_t>& channel_map);

std::vector<ReplaySegment> BuildReplaySegments(
    std::size_t total_frames,
    std::size_t block_frames,
    const std::vector<SteeringEvent>& events,
    audio::BeamformerSteering neutral_target = audio::BeamformerSteering{});
}  // namespace sonitude::tools::wav_replay
