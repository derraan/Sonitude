#include <cmath>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>

#include "app/calibration_config.hpp"
#include "app/calibration_writer.hpp"
#include "audio/audio_types.hpp"
#include "dsp/calibration_applier.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestCalibrationApply()
{
  std::vector<sonitude::app::CalibrationChannel> channels(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < channels.size(); ++i)
  {
    channels[i].id = "M" + std::to_string(i);
    channels[i].polarity = (i == 1) ? -1 : 1;
    channels[i].gain_linear = 1.1F;
    channels[i].dc_offset = 0.01F;
  }
  sonitude::dsp::CalibrationApplier applier(channels, 44100, 20.0F);
  sonitude::audio::MicFrame in{};
  in.fill(0.1F);
  const auto out = applier.process(in);
  Require(out[0] > 0.08F, "calibration gain/polarity failed");
  Require(out[1] < -0.08F, "calibration polarity inversion failed");
}

void TestDcBlockerCutoff()
{
  std::vector<sonitude::app::CalibrationChannel> channels(sonitude::audio::kMicChannels);
  for (std::size_t i = 0; i < channels.size(); ++i)
  {
    channels[i].id = "M" + std::to_string(i);
    channels[i].polarity = 1;
    channels[i].gain_linear = 1.0F;
    channels[i].dc_offset = 0.0F;
  }
  sonitude::dsp::CalibrationApplier applier(channels, 44100, 20.0F);
  std::vector<sonitude::audio::MicFrame> in(4410);
  std::vector<sonitude::audio::MicFrame> out(4410);
  for (auto& frame : in)
  {
    frame.fill(1.0F);
  }
  applier.processBlock(std::span<const sonitude::audio::MicFrame>(in.data(), in.size()),
                       std::span<sonitude::audio::MicFrame>(out.data(), out.size()));
  Require(std::fabs(out.back()[0]) < 0.1F, "dc blocker should attenuate sustained DC");
}

void TestWriter()
{
  sonitude::app::CalibrationConfig cal;
  cal.sample_rate_hz = 44100;
  for (std::size_t i = 0; i < sonitude::audio::kMicChannels; ++i)
  {
    sonitude::app::CalibrationChannel c;
    c.id = "M" + std::to_string(i);
    cal.channels.push_back(c);
  }
  sonitude::app::WriteCalibrationYamlBackupSafe("calibration_writer_test.yaml", cal, true);
}
}  // namespace

void RunCalibrationTests()
{
  TestCalibrationApply();
  TestDcBlockerCutoff();
  TestWriter();
}
