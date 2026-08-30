#include "embedded_mvdr.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sonitude::embedded {
namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kOlaScale = 2.0F / 3.0F;  // periodic Hann squared, N/H = 4

bool finite(float x) noexcept { return std::isfinite(x); }
}  // namespace

bool FastPath::init(DoubleBufferMailbox<WeightSet>* weights,
                    DoubleBufferMailbox<Spectrum>* snapshots,
                    const FastOvdConfig ovd) noexcept {
  if (weights == nullptr || snapshots == nullptr ||
      arm_rfft_fast_init_f32(&fft_, kFftSize) != ARM_MATH_SUCCESS) return false;
  if (ovd.enabled && (ovd.front_channel >= kMicrophones || ovd.rear_channel >= kMicrophones ||
                      ovd.front_channel == ovd.rear_channel ||
                      !(ovd.energy_ratio_threshold > 0.0F) ||
                      !(ovd.active_gain > 0.0F && ovd.active_gain <= 1.0F) ||
                      !(ovd.attack_step > 0.0F && ovd.attack_step <= 1.0F) ||
                      !(ovd.release_step > 0.0F && ovd.release_step <= 1.0F))) return false;

  weights_mailbox_ = weights;
  snapshots_mailbox_ = snapshots;
  ovd_ = ovd;
  for (std::size_t i = 0; i < kFftSize; ++i)
    window_[i] = 0.5F * (1.0F - std::cos(2.0F * kPi * static_cast<float>(i) /
                                        static_cast<float>(kFftSize)));
  // Safe initial fallback: broadside delay-and-sum until calibrated weights arrive.
  for (auto& bin : weights_)
    for (auto& w : bin) w = {1.0F / static_cast<float>(kMicrophones), 0.0F};
  return true;
}

void FastPath::unpackSpectrum(const float packed[kFftSize], Complex out[kBins]) noexcept {
  out[0] = {packed[0], 0.0F};
  out[kBins - 1] = {packed[1], 0.0F};
  for (std::size_t k = 1; k + 1 < kBins; ++k) out[k] = {packed[2 * k], packed[2 * k + 1]};
}

void FastPath::packSpectrum(const Complex in[kBins], float packed[kFftSize]) noexcept {
  packed[0] = in[0].re;
  packed[1] = in[kBins - 1].re;
  for (std::size_t k = 1; k + 1 < kBins; ++k) {
    packed[2 * k] = in[k].re;
    packed[2 * k + 1] = in[k].im;
  }
}

void FastPath::processHop() noexcept {
  weights_mailbox_->readIfNew(weight_generation_, weights_);
  for (std::size_t m = 0; m < kMicrophones; ++m) {
    for (std::size_t i = 0; i < kFftSize; ++i)
      time_[m][i] = ring_[m][(ring_write_ + i) % kFftSize];
    arm_mult_f32(time_[m].data(), window_.data(), time_[m].data(), kFftSize);
    arm_rfft_fast_f32(&fft_, time_[m].data(), packed_[m].data(), 0);
    unpackSpectrum(packed_[m].data(), spectra_[m].data());
  }
  (void)snapshots_mailbox_->publish(spectra_);

  std::array<Complex, kBins> output_spectrum{};
  for (std::size_t k = 0; k < kBins; ++k) {
    float mic[2 * kMicrophones]{};
    float conjugate_weights[2 * kMicrophones]{};
    float products[2 * kMicrophones]{};
    for (std::size_t m = 0; m < kMicrophones; ++m) {
      mic[2 * m] = spectra_[m][k].re;
      mic[2 * m + 1] = spectra_[m][k].im;
      conjugate_weights[2 * m] = weights_[k][m].re;
      conjugate_weights[2 * m + 1] = -weights_[k][m].im;
    }
    arm_cmplx_mult_cmplx_f32(mic, conjugate_weights, products, kMicrophones);
    for (std::size_t m = 0; m < kMicrophones; ++m) {
      output_spectrum[k].re += products[2 * m];
      output_spectrum[k].im += products[2 * m + 1];
    }
  }

  packSpectrum(output_spectrum.data(), output_packed_.data());
  arm_rfft_fast_f32(&fft_, output_packed_.data(), inverse_.data(), 1);
  arm_mult_f32(inverse_.data(), window_.data(), inverse_.data(), kFftSize);
  for (std::size_t i = 0; i < kFftSize; ++i) ola_[i] += inverse_[i] * kOlaScale;
  for (std::size_t i = 0; i < kHopSize; ++i) {
    if (out_count_ == output_fifo_.size()) {
      out_read_ = (out_read_ + 1U) % output_fifo_.size();
      --out_count_;
    }
    output_fifo_[out_write_] = ola_[i];
    out_write_ = (out_write_ + 1U) % output_fifo_.size();
    ++out_count_;
  }
  std::memmove(ola_.data(), ola_.data() + kHopSize, (kFftSize - kHopSize) * sizeof(float));
  std::fill(ola_.begin() + (kFftSize - kHopSize), ola_.end(), 0.0F);
  ++counters_.processed_hops;
}

void FastPath::processBlock(const float input[kMicrophones][kBlockFrames],
                            float output[kBlockFrames]) noexcept {
  float front_energy = 0.0F, rear_energy = 0.0F;
  for (std::size_t i = 0; i < kBlockFrames; ++i) {
    for (std::size_t m = 0; m < kMicrophones; ++m) {
      const float x = finite(input[m][i]) ? input[m][i] : 0.0F;
      counters_.invalid_samples += finite(input[m][i]) ? 0U : 1U;
      ring_[m][ring_write_] = x;
    }
    if (ovd_.enabled) {
      const float front = ring_[ovd_.front_channel][ring_write_];
      const float rear = ring_[ovd_.rear_channel][ring_write_];
      front_energy += front * front;
      rear_energy += rear * rear;
    }
    ring_write_ = (ring_write_ + 1U) % kFftSize;
    if (++hop_filled_ == kHopSize) { hop_filled_ = 0; processHop(); }
    float y = 0.0F;
    if (out_count_ != 0) {
      y = output_fifo_[out_read_];
      out_read_ = (out_read_ + 1U) % output_fifo_.size();
      --out_count_;
    }
    output[i] = y;
  }
  if (ovd_.enabled && rear_energy > 1.0e-20F &&
      front_energy / rear_energy > ovd_.energy_ratio_threshold) ovd_latched_ = true;
  const float target = (ovd_.enabled && ovd_latched_) ? ovd_.active_gain : 1.0F;
  const float step = target < ovd_gain_ ? ovd_.attack_step : ovd_.release_step;
  for (std::size_t i = 0; i < kBlockFrames; ++i) {
    ovd_gain_ += std::clamp(target - ovd_gain_, -step, step);
    output[i] *= ovd_gain_;
  }
}

bool SlowPath::init(DoubleBufferMailbox<Spectrum>* snapshots,
                    DoubleBufferMailbox<WeightSet>* weights,
                    const SlowPathConfig config) noexcept {
  if (snapshots == nullptr || weights == nullptr) return false;
  if (config.adaptation_enabled && (!(config.covariance_alpha > 0.0F && config.covariance_alpha <= 1.0F) ||
                                    !(config.diagonal_load_relative > 0.0F) ||
                                    !(config.minimum_bin_power > 0.0F))) return false;
  snapshots_mailbox_ = snapshots;
  weights_mailbox_ = weights;
  config_ = config;
  for (auto& bin : steering_)
    for (auto& d : bin) d = {1.0F, 0.0F};
  for (auto& bin : weights_)
    for (auto& w : bin) w = {1.0F / static_cast<float>(kMicrophones), 0.0F};
  (void)weights_mailbox_->publish(weights_);
  return true;
}

void SlowPath::setSteering(const WeightSet& steering_vectors) noexcept { steering_ = steering_vectors; }

bool SlowPath::poll() noexcept {
  if (!snapshots_mailbox_->readIfNew(snapshot_generation_, snapshot_)) return false;
  if (!config_.adaptation_enabled) return true;
  const float a = config_.covariance_alpha;
  for (std::size_t k = 0; k < kBins; ++k)
    for (std::size_t i = 0; i < kMicrophones; ++i)
      for (std::size_t j = 0; j < kMicrophones; ++j) {
        const auto xi = snapshot_[i][k];
        const auto xj = snapshot_[j][k];
        const Complex outer{xi.re * xj.re + xi.im * xj.im,
                            xi.im * xj.re - xi.re * xj.im};
        auto& r = covariance_[k][i][j];
        r.re += a * (outer.re - r.re);
        r.im += a * (outer.im - r.im);
      }
  return updateWeights();
}

bool SlowPath::updateWeights() noexcept {
  constexpr std::size_t n = 2 * kMicrophones;
  bool any = false;
  for (std::size_t k = 0; k < kBins; ++k) {
    float augmented[n * n]{};
    float inverse[n * n]{};
    float rhs[n]{};
    float solution[n]{};
    float trace = 0.0F;
    for (std::size_t i = 0; i < kMicrophones; ++i) trace += covariance_[k][i][i].re;
    const float load = config_.diagonal_load_relative *
        std::max(trace / static_cast<float>(kMicrophones), config_.minimum_bin_power);
    for (std::size_t i = 0; i < kMicrophones; ++i) {
      rhs[i] = steering_[k][i].re;
      rhs[i + kMicrophones] = steering_[k][i].im;
      for (std::size_t j = 0; j < kMicrophones; ++j) {
        const auto r = covariance_[k][i][j];
        augmented[i * n + j] = r.re + (i == j ? load : 0.0F);
        augmented[i * n + j + kMicrophones] = -r.im;
        augmented[(i + kMicrophones) * n + j] = r.im;
        augmented[(i + kMicrophones) * n + j + kMicrophones] = r.re + (i == j ? load : 0.0F);
      }
    }
    arm_matrix_instance_f32 a{}, inv{};
    arm_mat_init_f32(&a, n, n, augmented);
    arm_mat_init_f32(&inv, n, n, inverse);
    if (arm_mat_inverse_f32(&a, &inv) != ARM_MATH_SUCCESS) { ++counters_.singular_bins; continue; }
    arm_matrix_instance_f32 b{}, x{};
    arm_mat_init_f32(&b, n, 1, rhs);
    arm_mat_init_f32(&x, n, 1, solution);
    if (arm_mat_mult_f32(&inv, &b, &x) != ARM_MATH_SUCCESS) { ++counters_.singular_bins; continue; }
    float denom = 0.0F;
    for (std::size_t i = 0; i < kMicrophones; ++i)
      denom += steering_[k][i].re * solution[i] + steering_[k][i].im * solution[i + kMicrophones];
    if (!(denom > config_.minimum_bin_power) || !finite(denom)) { ++counters_.singular_bins; continue; }
    for (std::size_t i = 0; i < kMicrophones; ++i)
      weights_[k][i] = {solution[i] / denom, solution[i + kMicrophones] / denom};
    any = true;
  }
  if (any) (void)weights_mailbox_->publish(weights_);
  return any;
}

}  // namespace sonitude::embedded
