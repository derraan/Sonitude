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
  Off,
  Conservative,
  Spectral,
};

SuppressionBackend ParseSuppressionBackend(const std::string& name);
SuppressionBackend ResolveEnabledBackend(bool enabled, const std::string& backend_name);
const char* SuppressionBackendName(SuppressionBackend backend) noexcept;
const char* SuppressionImplementationStatus(SuppressionBackend backend) noexcept;

struct SuppressionStageConfig
{
  SuppressionBackend backend = SuppressionBackend::Off;
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

  [[nodiscard]] SuppressionBackend backend() const noexcept { return backend_; }
  [[nodiscard]] std::size_t algorithmicDelaySamples() const noexcept { return 0; }
  [[nodiscard]] float currentGain() const noexcept;
  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] SpectralPostfilter* spectralFilter() noexcept
  {
    return (backend_ == SuppressionBackend::Spectral && ready_) ? &spectral_ : nullptr;
  }

 private:
  bool ready_ = false;
  SuppressionBackend backend_ = SuppressionBackend::Off;
  ConservativeSuppressor conservative_{};
  SpectralPostfilter spectral_{};
};
}  // namespace sonitude::dsp
