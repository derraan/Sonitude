#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/channel_extractor.hpp"
#include "audio/format_convert.hpp"
#include "audio/wav_io.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestS16RoundTrip()
{
  const std::vector<float> input = {-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
  const std::vector<std::uint8_t> bytes =
      sonitude::audio::FloatToInterleaved(input, sonitude::audio::PcmFormat::S16_LE);
  const std::vector<float> decoded =
      sonitude::audio::InterleavedToFloat(bytes.data(), input.size(), 1, sonitude::audio::PcmFormat::S16_LE);
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    Require(std::fabs(decoded[i] - input[i]) < 0.001F, "S16 roundtrip error too large");
  }
}

void TestChannelExtract()
{
  // 2 frames, 8-channel container, S16. Values are frame0 ch0..7, frame1 ch0..7.
  const std::vector<std::int16_t> src = {100,  200,  300,  400,  500,  600,  700,  800,
                                         1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};
  std::vector<std::uint8_t> bytes(src.size() * sizeof(std::int16_t), 0);
  std::memcpy(bytes.data(), src.data(), bytes.size());
  const std::vector<std::size_t> active = {0, 1, 2, 3, 4, 5};
  const auto frames = sonitude::audio::ExtractActiveMicFrames(
      bytes.data(), 2, 8, active, sonitude::audio::PcmFormat::S16_LE);
  Require(frames.size() == 2, "Channel extract frame count mismatch");
  Require(std::fabs(frames[0][0] - (100.0F / 32768.0F)) < 1e-6F, "channel 0 sample mismatch");
  Require(std::fabs(frames[1][5] - (6000.0F / 32768.0F)) < 1e-6F, "channel 5 sample mismatch");
}

void TestWavRoundTrip()
{
  sonitude::audio::WavData input;
  input.sample_rate_hz = 44100;
  input.channels = 6;
  input.format = sonitude::audio::PcmFormat::S16_LE;
  input.interleaved.assign(6 * 16, 0.0F);
  for (std::size_t i = 0; i < input.interleaved.size(); ++i)
  {
    const int centered = static_cast<int>(i % 31U) - 15;
    input.interleaved[i] = static_cast<float>(centered) / 16.0F;
  }

  const std::string path = "unit_wav_roundtrip.wav";
  sonitude::audio::WriteWavFile(path, input);
  const auto output = sonitude::audio::ReadWavFile(path);
  Require(output.sample_rate_hz == input.sample_rate_hz, "WAV sample rate mismatch");
  Require(output.channels == input.channels, "WAV channel mismatch");
  Require(output.interleaved.size() == input.interleaved.size(), "WAV sample length mismatch");
  for (std::size_t i = 0; i < input.interleaved.size(); ++i)
  {
    Require(std::fabs(output.interleaved[i] - input.interleaved[i]) < 0.001F,
            "WAV sample mismatch after roundtrip");
  }
  (void)std::remove(path.c_str());
}

void WriteLe16(std::ofstream& out, const std::uint16_t value)
{
  out.put(static_cast<char>(value & 0xFFU));
  out.put(static_cast<char>((value >> 8) & 0xFFU));
}

void WriteLe32(std::ofstream& out, const std::uint32_t value)
{
  out.put(static_cast<char>(value & 0xFFU));
  out.put(static_cast<char>((value >> 8) & 0xFFU));
  out.put(static_cast<char>((value >> 16) & 0xFFU));
  out.put(static_cast<char>((value >> 24) & 0xFFU));
}

void TestWavRejectsMalformedPayloads()
{
  {
    const std::string path = "unit_wav_bad_header.wav";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("NOPE", 4);
    out.close();
    bool threw = false;
    try
    {
      (void)sonitude::audio::ReadWavFile(path);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    (void)std::remove(path.c_str());
    Require(threw, "invalid RIFF header should throw");
  }

  {
    const std::string path = "unit_wav_truncated_data.wav";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    WriteLe32(out, 60);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    WriteLe32(out, 16);
    WriteLe16(out, 1);
    WriteLe16(out, 1);
    WriteLe32(out, 44100);
    WriteLe32(out, 88200);
    WriteLe16(out, 2);
    WriteLe16(out, 16);
    out.write("data", 4);
    WriteLe32(out, 8);
    WriteLe16(out, 0);
    WriteLe16(out, 0);
    out.close();
    bool threw = false;
    try
    {
      (void)sonitude::audio::ReadWavFile(path);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    (void)std::remove(path.c_str());
    Require(threw, "truncated data payload should throw");
  }

  {
    const std::string path = "unit_wav_unaligned_data.wav";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    WriteLe32(out, 48);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    WriteLe32(out, 16);
    WriteLe16(out, 1);
    WriteLe16(out, 2);
    WriteLe32(out, 44100);
    WriteLe32(out, 176400);
    WriteLe16(out, 4);
    WriteLe16(out, 16);
    out.write("data", 4);
    WriteLe32(out, 3);
    out.put(static_cast<char>(0));
    out.put(static_cast<char>(0));
    out.put(static_cast<char>(0));
    out.close();
    bool threw = false;
    try
    {
      (void)sonitude::audio::ReadWavFile(path);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    (void)std::remove(path.c_str());
    Require(threw, "sample-unaligned payload should throw");
  }
}
}  // namespace

void RunAudioSupportTests()
{
  TestS16RoundTrip();
  TestChannelExtract();
  TestWavRoundTrip();
  TestWavRejectsMalformedPayloads();
}
