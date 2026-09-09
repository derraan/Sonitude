#include "app/calibration_estimator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace sonitude::app
{
namespace
{
float Mean(const std::vector<float>& v)
{
  if (v.empty())
  {
    return 0.0F;
  }
  double sum = 0.0;
  for (const float x : v)
  {
    sum += x;
  }
  return static_cast<float>(sum / static_cast<double>(v.size()));
}

float ComputeRms(const std::vector<float>& v)
{
  if (v.empty())
  {
    return 0.0F;
  }
  double sum = 0.0;
  for (const float x : v)
  {
    sum += static_cast<double>(x) * static_cast<double>(x);
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(v.size())));
}

bool DetectClipping(const std::vector<float>& v, const float threshold)
{
  for (const float x : v)
  {
    if (std::fabs(x) >= threshold)
    {
      return true;
    }
  }
  return false;
}

std::vector<float> DeinterleaveChannel(const std::span<const float> interleaved,
                                       const std::size_t channel_count,
                                       const std::size_t channel,
                                       const std::size_t start_frame,
                                       const std::size_t frame_count)
{
  std::vector<float> out;
  out.reserve(frame_count);
  const std::size_t total_frames = interleaved.size() / channel_count;
  const std::size_t end_frame = std::min(start_frame + frame_count, total_frames);
  for (std::size_t f = start_frame; f < end_frame; ++f)
  {
    out.push_back(interleaved[f * channel_count + channel]);
  }
  return out;
}

void RemoveMeanInPlace(std::vector<float>& v)
{
  const float m = Mean(v);
  for (float& x : v)
  {
    x -= m;
  }
}

struct NormalizedLagResult
{
  double lag_samples = 0.0;
  float peak = 0.0F;
  float confidence = 0.0F;
};

float ReadFractional(const std::vector<float>& signal, const double index)
{
  if (signal.empty())
  {
    return 0.0F;
  }
  if (index <= 0.0)
  {
    return signal.front();
  }
  const std::size_t i0 = static_cast<std::size_t>(index);
  if (i0 + 1U >= signal.size())
  {
    return signal.back();
  }
  const double frac = index - static_cast<double>(i0);
  return static_cast<float>((1.0 - frac) * static_cast<double>(signal[i0]) +
                            frac * static_cast<double>(signal[i0 + 1U]));
}

double NormalizedCorrelation(const std::vector<float>& reference,
                             const std::vector<float>& channel,
                             const double lag_samples)
{
  if (reference.empty() || channel.empty())
  {
    return 0.0;
  }
  double sum = 0.0;
  double ref_energy = 0.0;
  double ch_energy = 0.0;
  for (std::size_t i = 0; i < reference.size(); ++i)
  {
    const double ref_s = reference[i];
    const double ch_s = ReadFractional(channel, static_cast<double>(i) + lag_samples);
    sum += ref_s * ch_s;
    ref_energy += ref_s * ref_s;
    ch_energy += ch_s * ch_s;
  }
  const double denom = std::sqrt(ref_energy * ch_energy);
  if (denom < 1.0e-12)
  {
    return 0.0;
  }
  return sum / denom;
}

NormalizedLagResult EstimateNormalizedLag(const std::vector<float>& reference,
                                          const std::vector<float>& channel,
                                          const std::size_t max_lag_samples)
{
  NormalizedLagResult out{};
  if (reference.empty() || channel.empty())
  {
    return out;
  }

  constexpr std::size_t kAnalysisSamples = 4096U;
  const std::size_t available = std::min(reference.size(), channel.size());
  const std::size_t start =
      available > kAnalysisSamples ? (available - kAnalysisSamples) / 2U : 0U;
  const std::size_t count = std::min(kAnalysisSamples, available - start);

  std::vector<float> ref_seg(reference.begin() + static_cast<std::ptrdiff_t>(start),
                             reference.begin() + static_cast<std::ptrdiff_t>(start + count));
  std::vector<float> ch_seg(channel.begin() + static_cast<std::ptrdiff_t>(start),
                            channel.begin() + static_cast<std::ptrdiff_t>(start + count));

  const std::size_t max_lag = std::min<std::size_t>(max_lag_samples, 64U);
  double best_lag = 0.0;
  double best_abs_corr = 0.0;
  double best_signed_corr = 0.0;
  for (std::ptrdiff_t lag = -static_cast<std::ptrdiff_t>(max_lag);
       lag <= static_cast<std::ptrdiff_t>(max_lag);
       ++lag)
  {
    const double corr = NormalizedCorrelation(ref_seg, ch_seg, static_cast<double>(lag));
    const double abs_corr = std::fabs(corr);
    if (abs_corr > best_abs_corr)
    {
      best_abs_corr = abs_corr;
      best_signed_corr = corr;
      best_lag = static_cast<double>(lag);
    }
  }

  const double lag0 = best_lag - 1.0;
  const double lag1 = best_lag;
  const double lag2 = best_lag + 1.0;
  const double c0 = std::fabs(NormalizedCorrelation(ref_seg, ch_seg, lag0));
  const double c1 = std::fabs(NormalizedCorrelation(ref_seg, ch_seg, lag1));
  const double c2 = std::fabs(NormalizedCorrelation(ref_seg, ch_seg, lag2));
  double refined_lag = best_lag;
  const double denom = c0 - (2.0 * c1) + c2;
  if (std::fabs(denom) > 1.0e-12)
  {
    refined_lag += 0.5 * (c0 - c2) / denom;
  }

  out.lag_samples = refined_lag;
  out.peak = static_cast<float>(best_abs_corr);
  out.confidence = static_cast<float>(best_abs_corr / std::max(best_abs_corr + 0.05, 1.0e-6));
  (void)best_signed_corr;
  return out;
}

std::string UtcNowIso8601()
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

CalibrationQualityStatus WorstStatus(const CalibrationQualityStatus a,
                                     const CalibrationQualityStatus b)
{
  if (a == CalibrationQualityStatus::Invalid || b == CalibrationQualityStatus::Invalid)
  {
    return CalibrationQualityStatus::Invalid;
  }
  if (a == CalibrationQualityStatus::Unresolved || b == CalibrationQualityStatus::Unresolved)
  {
    return CalibrationQualityStatus::Unresolved;
  }
  if (a == CalibrationQualityStatus::Warning || b == CalibrationQualityStatus::Warning)
  {
    return CalibrationQualityStatus::Warning;
  }
  return CalibrationQualityStatus::Pass;
}
}  // namespace

CalibrationEstimateOutput EstimateCalibrationFromCapture(
    const std::span<const float> interleaved,
    const std::size_t channel_count,
    const CalibrationEstimateOptions& options)
{
  if (channel_count != 6)
  {
    throw std::runtime_error("EstimateCalibrationFromCapture expects six channels");
  }
  if (options.channel_ids.size() != channel_count)
  {
    throw std::runtime_error("EstimateCalibrationFromCapture channel_ids must match channel_count");
  }
  if (options.reference_channel_index >= channel_count)
  {
    throw std::runtime_error("reference_channel_index out of range");
  }
  if (interleaved.size() % channel_count != 0)
  {
    throw std::runtime_error("interleaved capture length is not an integer number of frames");
  }

  const std::size_t total_frames = interleaved.size() / channel_count;
  const std::size_t silence_start = options.silence_start_frame;
  const std::size_t silence_count =
      options.silence_frame_count != 0
          ? options.silence_frame_count
          : std::min<std::size_t>(total_frames / 5U, static_cast<std::size_t>(options.sample_rate_hz / 2U));
  const std::size_t signal_start =
      options.signal_start_frame != 0 ? options.signal_start_frame : silence_start + silence_count;
  const std::size_t signal_count =
      options.signal_frame_count != 0 ? options.signal_frame_count : total_frames - signal_start;

  if (signal_count < 256)
  {
    throw std::runtime_error("signal region too short for calibration estimation");
  }

  CalibrationEstimateOutput out{};
  out.report.sample_rate_hz = options.sample_rate_hz;
  out.report.reference_channel_index = options.reference_channel_index;
  out.report.reference_microphone_id = options.channel_ids[options.reference_channel_index];
  out.report.silence_frame_count = silence_count;
  out.report.signal_frame_count = signal_count;
  out.report.hardware_evidence = options.hardware_evidence;
  out.report.overall_status = CalibrationQualityStatus::Pass;

  out.calibration.schema_version = kCalibrationSchemaVersion;
  out.calibration.sample_rate_hz = options.sample_rate_hz;
  out.calibration.reference.microphone_id = out.report.reference_microphone_id;
  out.calibration.identity.geometry_id = options.geometry_id;
  out.calibration.identity.created_utc = UtcNowIso8601();
  out.calibration.capture.sample_rate_hz = options.sample_rate_hz;
  out.calibration.capture.channel_count = channel_count;
  out.calibration.quality.hardware_evidence = options.hardware_evidence;
  out.calibration.channels.resize(channel_count);

  std::vector<std::vector<float>> signal_segments(channel_count);
  std::vector<std::vector<float>> silence_segments(channel_count);
  for (std::size_t ch = 0; ch < channel_count; ++ch)
  {
    silence_segments[ch] =
        DeinterleaveChannel(interleaved, channel_count, ch, silence_start, silence_count);
    signal_segments[ch] =
        DeinterleaveChannel(interleaved, channel_count, ch, signal_start, signal_count);
  }

  const std::vector<float> ref_signal = signal_segments[options.reference_channel_index];
  std::vector<float> ref_work = ref_signal;
  RemoveMeanInPlace(ref_work);
  const float ref_rms = ComputeRms(ref_work);

  for (std::size_t ch = 0; ch < channel_count; ++ch)
  {
    CalibrationChannelReport ch_report;
    ch_report.id = options.channel_ids[ch];
    ch_report.dc_offset = Mean(silence_segments[ch]);
    ch_report.rms_level = ComputeRms(signal_segments[ch]);
    ch_report.clipping = DetectClipping(signal_segments[ch], options.clipping_threshold);
    ch_report.status = CalibrationQualityStatus::Pass;

    if (ch_report.clipping)
    {
      ch_report.warnings.push_back("signal region appears clipped");
      ch_report.status = CalibrationQualityStatus::Warning;
    }
    if (ch_report.rms_level < options.min_signal_rms)
    {
      ch_report.warnings.push_back("signal RMS below minimum useful level");
      ch_report.status = WorstStatus(ch_report.status, CalibrationQualityStatus::Unresolved);
    }

    std::vector<float> ch_work = signal_segments[ch];
    for (float& x : ch_work)
    {
      x -= ch_report.dc_offset;
    }
    RemoveMeanInPlace(ch_work);

    if (ch == options.reference_channel_index)
    {
      ch_report.polarity = 1;
      ch_report.gain_linear = 1.0F;
      ch_report.relative_gain_db = 0.0F;
      ch_report.delay_samples = 0.0F;
      ch_report.delay_us = 0.0F;
      ch_report.correlation_peak = 1.0F;
      ch_report.delay_confidence = 1.0F;
    }
    else
    {
      const NormalizedLagResult delay =
          EstimateNormalizedLag(ref_work, ch_work, static_cast<std::size_t>(64U));
      // Positive lag means channel is late relative to reference; store negative correction.
      ch_report.delay_samples = static_cast<float>(-delay.lag_samples);
      ch_report.delay_us =
          ch_report.delay_samples * 1.0e6F / static_cast<float>(options.sample_rate_hz);
      ch_report.correlation_peak = delay.peak;
      ch_report.delay_confidence = delay.confidence;

      if (delay.confidence < options.delay_confidence_threshold)
      {
        ch_report.warnings.push_back("delay estimate confidence below threshold");
        ch_report.status = WorstStatus(ch_report.status, CalibrationQualityStatus::Unresolved);
      }

      const double aligned_corr = NormalizedCorrelation(ref_work, ch_work, delay.lag_samples);
      if (std::fabs(aligned_corr) >= static_cast<double>(options.polarity_correlation_threshold))
      {
        ch_report.polarity = (aligned_corr >= 0.0) ? 1 : -1;
      }
      else
      {
        ch_report.polarity = 1;
        ch_report.polarity_unresolved = true;
        ch_report.warnings.push_back("polarity unresolved from available signal");
        ch_report.status = WorstStatus(ch_report.status, CalibrationQualityStatus::Unresolved);
      }

      const float ch_rms = ComputeRms(ch_work);
      if (ch_rms > options.min_signal_rms && ref_rms > options.min_signal_rms)
      {
        ch_report.gain_linear = ref_rms / ch_rms;
        ch_report.relative_gain_db =
            20.0F * std::log10(std::max(ch_report.gain_linear, 1.0e-12F));
      }
      else
      {
        ch_report.gain_linear = 1.0F;
        ch_report.relative_gain_db = 0.0F;
        ch_report.warnings.push_back("gain normalization skipped due to low signal level");
        ch_report.status = WorstStatus(ch_report.status, CalibrationQualityStatus::Unresolved);
      }
    }

    if (ch_report.gain_linear <= 0.0F || ch_report.gain_linear > 8.0F || !std::isfinite(ch_report.gain_linear))
    {
      ch_report.warnings.push_back("estimated gain out of supported runtime range");
      ch_report.status = CalibrationQualityStatus::Invalid;
    }

    out.report.channels.push_back(ch_report);
    out.report.overall_status = WorstStatus(out.report.overall_status, ch_report.status);

    auto& cal_ch = out.calibration.channels[ch];
    cal_ch.id = ch_report.id;
    cal_ch.polarity = ch_report.polarity_unresolved ? 1 : ch_report.polarity;
    cal_ch.gain_linear = ch_report.gain_linear;
    cal_ch.delay_samples = ch_report.delay_samples;
    cal_ch.dc_offset = ch_report.dc_offset;
  }

  out.calibration.quality.valid =
      out.report.overall_status == CalibrationQualityStatus::Pass && options.hardware_evidence;
  if (!options.hardware_evidence)
  {
    out.calibration.quality.warnings.push_back(
        "offline/synthetic capture; hardware evidence not established");
    out.report.warnings.push_back("hardware_evidence=false");
  }
  for (const auto& ch : out.report.channels)
  {
    out.calibration.quality.warnings.insert(out.calibration.quality.warnings.end(),
                                            ch.warnings.begin(), ch.warnings.end());
  }

  return out;
}
}  // namespace sonitude::app
