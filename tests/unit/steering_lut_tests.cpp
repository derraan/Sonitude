#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include "app/config.hpp"
#include "audio/audio_types.hpp"
#include "dsp/steering_lut.hpp"
#include "spatial/angles.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

sonitude::app::GeometryConfig BuildGeometry()
{
  sonitude::app::GeometryConfig geometry;
  geometry.profile_name = "steering_lut_unit_geometry";
  geometry.microphones = {
      {"M0", -0.038, 0.168, 0.0}, {"M1", 0.038, 0.168, 0.0}, {"M2", -0.090, 0.050, 0.0},
      {"M3", 0.090, 0.050, 0.0},  {"M4", -0.060, 0.000, 0.0}, {"M5", 0.060, 0.000, 0.0},
  };
  return geometry;
}

sonitude::app::SteeringConfig BuildSteering(const float source_distance_m)
{
  sonitude::app::SteeringConfig steering;
  steering.model = "near_field";
  steering.speed_of_sound_mps = 343.0F;
  steering.source_distance_m = source_distance_m;
  steering.reference_mic_index = 0;
  return steering;
}

double MicDistance(const std::array<double, 3>& source, const sonitude::app::GeometryMic& mic)
{
  const double dx = source[0] - mic.x;
  const double dy = source[1] - mic.y;
  const double dz = source[2] - mic.z;
  return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

sonitude::dsp::KemarSteeringLut::DelayArray ExpectedNearFieldDelays(
    const sonitude::app::GeometryConfig& geometry,
    const float azimuth_deg,
    const float elevation_deg,
    const float source_distance_m,
    const float speed_of_sound_mps,
    const std::uint32_t sample_rate_hz,
    const std::size_t reference_mic_index)
{
  const auto u = sonitude::spatial::UnitVectorFromAzElDeg(azimuth_deg, elevation_deg);
  const std::array<double, 3> source = {u[0] * static_cast<double>(source_distance_m),
                                        u[1] * static_cast<double>(source_distance_m),
                                        u[2] * static_cast<double>(source_distance_m)};
  const double ref_dist = MicDistance(source, geometry.microphones[reference_mic_index]);
  sonitude::dsp::KemarSteeringLut::DelayArray out{};
  for (std::size_t m = 0; m < sonitude::audio::kMicChannels; ++m)
  {
    const double dist_m = MicDistance(source, geometry.microphones[m]);
    const double tau_sec = (dist_m - ref_dist) / static_cast<double>(speed_of_sound_mps);
    out[m] = tau_sec * static_cast<double>(sample_rate_hz);
  }
  return out;
}

void CompareDelays(const sonitude::dsp::KemarSteeringLut::DelayArray& actual,
                   const sonitude::dsp::KemarSteeringLut::DelayArray& expected,
                   const std::string& label)
{
  for (std::size_t m = 0; m < sonitude::audio::kMicChannels; ++m)
  {
    Require(std::fabs(actual[m] - expected[m]) < 1.0e-9,
            label + " mismatch at mic " + std::to_string(m));
  }
}

void TestGeometricNearFieldDelays()
{
  constexpr std::uint32_t kFs = 16000;
  const auto geometry = BuildGeometry();
  struct Case
  {
    float azimuth_deg;
    float elevation_deg;
    float range_m;
    std::size_t reference_mic_index;
  };
  const Case cases[] = {{0.0F, 0.0F, 0.45F, 0},    {45.0F, 0.0F, 0.45F, 0},
                        {-90.0F, 0.0F, 0.30F, 0},  {30.0F, 17.5F, 0.45F, 0},
                        {120.0F, -20.0F, 1.00F, 0}, {0.0F, 10.0F, 0.30F, 4},
                        {-45.0F, 15.0F, 0.45F, 4},  {60.0F, -12.0F, 0.45F, 4}};
  for (const Case& test_case : cases)
  {
    auto steering = BuildSteering(test_case.range_m);
    steering.reference_mic_index = test_case.reference_mic_index;
    sonitude::dsp::KemarSteeringLut model;
    model.configure(geometry, steering, kFs);
    const sonitude::audio::BeamformerSteering target{test_case.azimuth_deg, test_case.elevation_deg};
    const auto actual = model.computeNearFieldDelays(target, test_case.reference_mic_index);
    const auto expected =
        ExpectedNearFieldDelays(geometry, test_case.azimuth_deg, test_case.elevation_deg,
                                test_case.range_m, steering.speed_of_sound_mps, kFs,
                                test_case.reference_mic_index);
    CompareDelays(actual, expected, "geometric delay case");
  }
}

void TestElevationNotSnappedToHorizontalRing()
{
  constexpr std::uint32_t kFs = 16000;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering(0.45F);
  sonitude::dsp::KemarSteeringLut model;
  model.configure(geometry, steering, kFs);
  const auto horizontal = model.computeNearFieldDelays({30.0F, 0.0F}, steering.reference_mic_index);
  const auto elevated = model.computeNearFieldDelays({30.0F, 17.5F}, steering.reference_mic_index);
  bool differs = false;
  for (std::size_t m = 0; m < sonitude::audio::kMicChannels; ++m)
  {
    if (std::fabs(horizontal[m] - elevated[m]) > 1.0e-6)
    {
      differs = true;
      break;
    }
  }
  Require(differs, "non-zero elevation must change near-field delay vector");
}

void TestReferenceMicDelayIsZero()
{
  constexpr std::uint32_t kFs = 16000;
  const auto geometry = BuildGeometry();
  auto steering = BuildSteering(0.45F);
  steering.reference_mic_index = 4;
  sonitude::dsp::KemarSteeringLut model;
  model.configure(geometry, steering, kFs);
  const auto delays = model.computeNearFieldDelays({-20.0F, 12.0F}, steering.reference_mic_index);
  Require(std::fabs(delays[steering.reference_mic_index]) < 1.0e-12, "reference mic delay must be zero");
}
}  // namespace

void RunSteeringLutTests()
{
  TestGeometricNearFieldDelays();
  TestElevationNotSnappedToHorizontalRing();
  TestReferenceMicDelayIsZero();
}
