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
  if (name == "spectral")
  {
    return SuppressionBackend::Spectral;
  }
  throw std::runtime_error("Unknown suppression backend: " + name);
}

const char* SuppressionBackendName(const SuppressionBackend backend) noexcept
{
  return backend == SuppressionBackend::Spectral ? "spectral" : "conservative";
}

void SuppressionStage::configure(const SuppressionStageConfig& config)
{
  ready_ = false;
  enabled_ = config.enabled;
  backend_ = config.backend;
  if (config.sample_rate_hz == 0 || config.maximum_block_frames == 0)
  {
    throw std::runtime_error("SuppressionStage sample_rate_hz and maximum_block_frames must be non-zero");
  }

  if (!enabled_)
  {
    ready_ = true;
    return;
  }

  if (backend_ == SuppressionBackend::Conservative)
  {
    conservative_.configure(config.conservative, config.sample_rate_hz);
  }
  else
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
  if (enabled_ && backend_ == SuppressionBackend::Spectral)
  {
    spectral_.reset();
  }
}

void SuppressionStage::setControl(const bool focus_active, const float confidence) noexcept
{
  if (!enabled_ || !ready_)
  {
    return;
  }
  if (backend_ == SuppressionBackend::Conservative)
  {
    conservative_.setControl(focus_active, confidence);
  }
  else
  {
    spectral_.setControl(focus_active, confidence);
  }
}

void SuppressionStage::setConfidenceThreshold(const float threshold) noexcept
{
  if (enabled_ && backend_ == SuppressionBackend::Spectral)
  {
    spectral_.setConfidenceThreshold(threshold);
  }
}

void SuppressionStage::setEstimatorHold(const bool hold) noexcept
{
  if (enabled_ && backend_ == SuppressionBackend::Spectral)
  {
    spectral_.setEstimatorHold(hold);
  }
}

void SuppressionStage::process(const std::span<float> mono)
{
  if (!ready_ || !enabled_ || backend_ != SuppressionBackend::Conservative || mono.empty())
  {
    return;
  }
  conservative_.process(mono);
}

float SuppressionStage::currentGain() const noexcept
{
  if (!ready_ || !enabled_)
  {
    return 1.0F;
  }
  if (backend_ == SuppressionBackend::Conservative)
  {
    return conservative_.currentGain();
  }
  return spectral_.currentGain();
}

SpectralPostfilter* SuppressionStage::spectralFilter() noexcept
{
  if (!ready_ || !enabled_ || backend_ != SuppressionBackend::Spectral)
  {
    return nullptr;
  }
  return &spectral_;
}
}  // namespace sonitude::dsp
