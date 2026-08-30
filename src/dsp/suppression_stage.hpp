#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "dsp/spectral_postfilter.hpp"
#include "dsp/suppressor.hpp"

namespace sonitude::dsp
{
enum class SuppressionBackend
{
  Conservative,
  Spectral,
};

SuppressionBackend ParseSuppressionBackend(const std::string& name);
const char* SuppressionBackendName(SuppressionBackend backend) noexcept;

struct SuppressionStageConfig
{
  bool enabled = false;
  SuppressionBackend backend = SuppressionBackend::Conservative;
  std::uint32_t sample_rate_hz = 0;
  std::size_t maximum_block_frames = 64;
  SuppressorConfig conservative{};
  SpectralPostfilterConfig spectral{};
};

class SuppressionStage
{
 public:
  void configure(const SuppressionStageConfig& config);
  void reset() noexcept;
  void setControl(bool focus_active, float confidence) noexcept;
  void setConfidenceThreshold(float threshold) noexcept;
  void setEstimatorHold(bool hold) noexcept;
  // Conservative: in-place PCM. Spectral: no-op; gains run inside the MVDR hop.
  void process(std::span<float> mono);

  [[nodiscard]] float currentGain() const noexcept;
  // Fusion wiring: nullptr unless enabled spectral backend is prepared.
  [[nodiscard]] SpectralPostfilter* spectralFilter() noexcept;

 private:
  bool enabled_ = false;
  bool ready_ = false;
  SuppressionBackend backend_ = SuppressionBackend::Conservative;
  ConservativeSuppressor conservative_{};
  SpectralPostfilter spectral_{};
};
}  // namespace sonitude::dsp
