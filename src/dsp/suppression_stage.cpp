#include "dsp/suppression_stage.hpp"

#include <stdexcept>

namespace sonitude::dsp
{
SuppressionBackend ParseSuppressionBackend(const std::string& name)
{
  if (name.empty() || name == "conservative")
  {
    return SuppressionBackend::Conservative;
  }
  if (name == "off")
  {
    return SuppressionBackend::Off;
  }
  if (name == "spectral")
  {
    return SuppressionBackend::Spectral;
  }
  throw std::runtime_error("Unknown suppression backend: " + name);
}

SuppressionBackend ResolveEnabledBackend(const bool enabled, const std::string& backend_name)
{
  if (!enabled)
  {
    return SuppressionBackend::Off;
  }
  return ParseSuppressionBackend(backend_name.empty() ? "conservative" : backend_name);
}

const char* SuppressionBackendName(const SuppressionBackend backend) noexcept
{
  switch (backend)
  {
    case SuppressionBackend::Off:
      return "off";
    case SuppressionBackend::Spectral:
      return "spectral";
    case SuppressionBackend::Conservative:
    default:
      return "conservative";
  }
}

const char* SuppressionImplementationStatus(const SuppressionBackend backend) noexcept
{
  return backend == SuppressionBackend::Spectral ? "EXPERIMENTAL" : "";
}

void SuppressionStage::configure(const SuppressionStageConfig& config)
{
  ready_ = false;
  backend_ = config.backend;
  if (config.sample_rate_hz == 0 || config.maximum_block_frames == 0)
  {
    throw std::runtime_error("SuppressionStage sample_rate_hz and maximum_block_frames must be non-zero");
  }

  if (backend_ == SuppressionBackend::Conservative)
  {
    conservative_.configure(config.conservative, config.sample_rate_hz);
  }
  else if (backend_ == SuppressionBackend::Spectral)
  {
    SpectralPostfilterConfig spectral = config.spectral;
    spectral.enabled = true;
    if (!spectral_.prepare(static_cast<double>(config.sample_rate_hz),
                           config.maximum_block_frames,
                           spectral))
    {
      throw std::runtime_error("Spectral postfilter prepare failed");
    }
  }

  ready_ = true;
}

void SuppressionStage::reset() noexcept
{
  if (backend_ == SuppressionBackend::Spectral)
  {
    spectral_.reset();
  }
}

void SuppressionStage::setControl(const bool focus_active, const float confidence) noexcept
{
  if (backend_ == SuppressionBackend::Conservative)
  {
    conservative_.setControl(focus_active, confidence);
  }
  else if (backend_ == SuppressionBackend::Spectral)
  {
    spectral_.setControl(focus_active, confidence);
  }
}

void SuppressionStage::setConfidenceThreshold(const float threshold) noexcept
{
  if (backend_ == SuppressionBackend::Spectral)
  {
    spectral_.setConfidenceThreshold(threshold);
  }
}

void SuppressionStage::setEstimatorHold(const bool hold) noexcept
{
  if (backend_ == SuppressionBackend::Spectral)
  {
    spectral_.setEstimatorHold(hold);
  }
}

void SuppressionStage::process(const std::span<float> mono)
{
  if (!ready_ || backend_ != SuppressionBackend::Conservative || mono.empty())
  {
    return;
  }
  conservative_.process(mono);
}

float SuppressionStage::currentGain() const noexcept
{
  if (!ready_ || backend_ == SuppressionBackend::Off)
  {
    return 1.0F;
  }
  if (backend_ == SuppressionBackend::Conservative)
  {
    return conservative_.currentGain();
  }
  return spectral_.currentGain();
}
}  // namespace sonitude::dsp
