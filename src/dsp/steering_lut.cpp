#include "dsp/steering_lut.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

#include "spatial/angles.hpp"

namespace sonitude::dsp
{
namespace
{
double Distance(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}
}  // namespace

void KemarSteeringLut::configureAnalytic(const app::GeometryConfig& geometry,
                                         const app::SteeringConfig& steering,
                                         const std::uint32_t sample_rate_hz)
{
  enabled_ = false;
  sample_rate_hz_ = sample_rate_hz;
  speed_of_sound_mps_ = steering.speed_of_sound_mps;
  source_distance_m_ = steering.source_distance_m;
  reference_mic_index_ = steering.reference_mic_index;

  if (geometry.microphones.size() != audio::kMicChannels)
  {
    throw std::runtime_error("KemarSteeringLut requires six microphones");
  }
  if (reference_mic_index_ >= audio::kMicChannels)
  {
    throw std::runtime_error("KemarSteeringLut reference_mic_index out of range");
  }
  if (source_distance_m_ <= 0.0F)
  {
    throw std::runtime_error("KemarSteeringLut source_distance_m must be positive");
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const auto& mic = geometry.microphones[i];
    mic_positions_[i] = {mic.x, mic.y, mic.z};
  }
  lut_delays_.clear();
  lut_directions_.clear();
  left_ear_offsets_.clear();
  right_ear_offsets_.clear();
}

void KemarSteeringLut::configure(const HrtfTable& table,
                                 const app::GeometryConfig& geometry,
                                 const app::SteeringConfig& steering,
                                 const std::uint32_t sample_rate_hz)
{
  enabled_ = steering.kemar_lut.enabled;
  sample_rate_hz_ = sample_rate_hz;
  speed_of_sound_mps_ = steering.speed_of_sound_mps;
  source_distance_m_ = steering.source_distance_m;
  reference_mic_index_ = steering.reference_mic_index;

  if (geometry.microphones.size() != audio::kMicChannels)
  {
    throw std::runtime_error("KemarSteeringLut requires six microphones");
  }
  if (reference_mic_index_ >= audio::kMicChannels)
  {
    throw std::runtime_error("KemarSteeringLut reference_mic_index out of range");
  }
  if (source_distance_m_ <= 0.0F)
  {
    throw std::runtime_error("KemarSteeringLut source_distance_m must be positive");
  }

  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const auto& mic = geometry.microphones[i];
    mic_positions_[i] = {mic.x, mic.y, mic.z};
  }

  lut_delays_.clear();
  lut_directions_.clear();
  left_ear_offsets_.clear();
  right_ear_offsets_.clear();

  if (!enabled_)
  {
    return;
  }
  if (table.empty())
  {
    throw std::runtime_error("KemarSteeringLut requires a non-empty HRTF table");
  }
  if (table.sample_rate_hz != sample_rate_hz)
  {
    throw std::runtime_error("KemarSteeringLut HRTF table sample rate mismatch");
  }

  lut_delays_.resize(table.directions.size());
  lut_directions_ = table.directions;
  left_ear_offsets_.resize(table.directions.size(), 0.0F);
  right_ear_offsets_.resize(table.directions.size(), 0.0F);

  for (std::size_t d = 0; d < table.directions.size(); ++d)
  {
    const auto& dir = table.directions[d];
    lut_delays_[d] = computeNearFieldDelays({dir.azimuth_deg, dir.elevation_deg}, reference_mic_index_);

    const float center_delay = 0.5F * (dir.delay_left_samples + dir.delay_right_samples);
    left_ear_offsets_[d] = dir.delay_left_samples - center_delay;
    right_ear_offsets_[d] = dir.delay_right_samples - center_delay;
  }
}

std::size_t KemarSteeringLut::lookupIndex(const float azimuth_deg, const float elevation_deg) const
{
  if (!enabled_ || lut_directions_.empty())
  {
    return 0;
  }
  std::size_t best = 0;
  double best_error = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < lut_directions_.size(); ++i)
  {
    const auto& d = lut_directions_[i];
    const double da = spatial::AngularDistanceAbsDeg(d.azimuth_deg, azimuth_deg);
    const double de = std::fabs(static_cast<double>(d.elevation_deg - elevation_deg));
    const double err = da + de;
    if (err < best_error)
    {
      best_error = err;
      best = i;
    }
  }
  return best;
}

audio::BeamformerSteering KemarSteeringLut::snappedDirection(const std::size_t index) const
{
  if (!enabled_ || index >= lut_directions_.size())
  {
    return {0.0F, 0.0F};
  }
  const auto& d = lut_directions_[index];
  return {d.azimuth_deg, d.elevation_deg};
}

const KemarSteeringLut::DelayArray& KemarSteeringLut::delaysForIndex(const std::size_t index) const
{
  static const DelayArray kZero{};
  if (!enabled_ || index >= lut_delays_.size())
  {
    return kZero;
  }
  return lut_delays_[index];
}

float KemarSteeringLut::leftEarOffsetSamples(const std::size_t index) const
{
  if (!enabled_ || index >= left_ear_offsets_.size())
  {
    return 0.0F;
  }
  return left_ear_offsets_[index];
}

float KemarSteeringLut::rightEarOffsetSamples(const std::size_t index) const
{
  if (!enabled_ || index >= right_ear_offsets_.size())
  {
    return 0.0F;
  }
  return right_ear_offsets_[index];
}

KemarSteeringLut::DelayArray KemarSteeringLut::computeNearFieldDelays(
    const audio::BeamformerSteering target,
    const std::size_t reference_mic_index) const
{
  if (reference_mic_index >= audio::kMicChannels)
  {
    throw std::runtime_error("computeNearFieldDelays reference_mic_index out of range");
  }

  const auto u = spatial::UnitVectorFromAzElDeg(target.azimuth_deg, target.elevation_deg);
  const std::array<double, 3> source = {u[0] * static_cast<double>(source_distance_m_),
                                        u[1] * static_cast<double>(source_distance_m_),
                                        u[2] * static_cast<double>(source_distance_m_)};
  const double ref_dist = Distance(source, mic_positions_[reference_mic_index]);

  DelayArray out{};
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const double dist_m = Distance(source, mic_positions_[i]);
    const double tau_sec = (dist_m - ref_dist) / static_cast<double>(speed_of_sound_mps_);
    out[i] = tau_sec * static_cast<double>(sample_rate_hz_);
  }
  return out;
}
}  // namespace sonitude::dsp
