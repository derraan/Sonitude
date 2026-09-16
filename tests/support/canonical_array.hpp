#pragma once

#include <string>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/config.hpp"
#include "dsp/beamformer.hpp"

#ifndef SONITUDE_SOURCE_DIR
#error "SONITUDE_SOURCE_DIR must be defined for tests"
#endif

namespace sonitude::tests::support
{
inline std::string SourcePath(const char* rel)
{
  return std::string(SONITUDE_SOURCE_DIR) + "/" + rel;
}

inline app::GeometryConfig LoadCanonicalGeometry()
{
  return app::LoadGeometryFromFile(SourcePath("config/geometry_soundbubble_initial.yaml"));
}

inline dsp::MvdrTuningParams LoadCanonicalMvdrTuning()
{
  const auto runtime = app::LoadRuntimeConfigFromFile(SourcePath("config/default.yaml"));
  return dsp::TuningFromRuntime(runtime.spatial.mvdr);
}

inline app::CalibrationConfig IdentityCalibration(const app::GeometryConfig& geometry,
                                                  const std::uint32_t sample_rate_hz,
                                                  const std::vector<float>& delays = {})
{
  app::CalibrationConfig c;
  c.sample_rate_hz = sample_rate_hz;
  c.channels.resize(geometry.microphones.size());
  for (std::size_t i = 0; i < c.channels.size(); ++i)
  {
    c.channels[i].id = geometry.microphones[i].id;
    c.channels[i].polarity = 1;
    c.channels[i].gain_linear = 1.0F;
    c.channels[i].delay_samples = delays.empty() ? 0.0F : delays[i];
  }
  return c;
}
}  // namespace sonitude::tests::support
