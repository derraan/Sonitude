#include "dsp/steering_lut.hpp"

#include <cmath>
#include <stdexcept>

#include "spatial/angles.hpp"

namespace sonitude::dsp
{
namespace
{
constexpr double kMinDistanceM = 1.0e-6;

double Distance(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

double GuardedDistance(const double distance_m)
{
  if (!std::isfinite(distance_m) || distance_m < kMinDistanceM)
  {
    return kMinDistanceM;
  }
  return distance_m;
}
}  // namespace

void GeometricSteeringLut::configure(const app::GeometryConfig& geometry,
                                     const app::SteeringConfig& steering,
                                     const std::uint32_t sample_rate_hz)
{
  sample_rate_hz_ = sample_rate_hz;
  speed_of_sound_mps_ = steering.speed_of_sound_mps;
  source_distance_m_ = steering.source_distance_m;
  if (geometry.microphones.size() != audio::kMicChannels)
  {
    throw std::runtime_error("GeometricSteeringLut requires six microphones");
  }
  if (steering.reference_mic_index >= audio::kMicChannels)
  {
    throw std::runtime_error("GeometricSteeringLut reference_mic_index out of range");
  }
  if (source_distance_m_ <= 0.0F)
  {
    throw std::runtime_error("GeometricSteeringLut source_distance_m must be positive");
  }
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const auto& mic = geometry.microphones[i];
    mic_positions_[i] = {mic.x, mic.y, mic.z};
  }
}

GeometricSteeringLut::DelayArray GeometricSteeringLut::computeNearFieldDelays(
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
  const double ref_dist = GuardedDistance(Distance(source, mic_positions_[reference_mic_index]));
  DelayArray out{};
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const double dist_m = GuardedDistance(Distance(source, mic_positions_[i]));
    const double tau_sec = (dist_m - ref_dist) / static_cast<double>(speed_of_sound_mps_);
    out[i] = tau_sec * static_cast<double>(sample_rate_hz_);
  }
  return out;
}

GeometricSteeringLut::AmplitudeArray GeometricSteeringLut::computeNearFieldAmplitudes(
    const audio::BeamformerSteering target,
    const std::size_t reference_mic_index) const
{
  if (reference_mic_index >= audio::kMicChannels)
  {
    throw std::runtime_error("computeNearFieldAmplitudes reference_mic_index out of range");
  }
  const auto u = spatial::UnitVectorFromAzElDeg(target.azimuth_deg, target.elevation_deg);
  const std::array<double, 3> source = {u[0] * static_cast<double>(source_distance_m_),
                                        u[1] * static_cast<double>(source_distance_m_),
                                        u[2] * static_cast<double>(source_distance_m_)};
  const double ref_dist = GuardedDistance(Distance(source, mic_positions_[reference_mic_index]));
  AmplitudeArray out{};
  for (std::size_t i = 0; i < audio::kMicChannels; ++i)
  {
    const double dist_m = GuardedDistance(Distance(source, mic_positions_[i]));
    const double ratio = ref_dist / dist_m;
    out[i] = std::isfinite(ratio) ? static_cast<float>(ratio) : 1.0F;
  }
  out[reference_mic_index] = 1.0F;
  return out;
}
}  // namespace sonitude::dsp
