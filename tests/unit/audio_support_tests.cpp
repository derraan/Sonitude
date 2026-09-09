#include <cmath>
#include <cstring>
#include <functional>
#include <cstdio>
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

void RequireThrows(const std::function<void()>& fn, const std::string& message)
{
  bool threw = false;
  try
  {
    fn();
  }
  catch (const std::runtime_error&)
  {
    threw = true;
  }
  Require(threw, message);
}

int HexNibble(const char value)
{
  if (value >= '0' && value <= '9')
  {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f')
  {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F')
  {
    return value - 'A' + 10;
  }
  return -1;
}

std::vector<std::uint8_t> DecodeHexSeed(const std::string& seed)
{
  constexpr const char* kPrefix = "hex:";
  Require(seed.rfind(kPrefix, 0) == 0, "Hex seed must start with hex:");
  const std::size_t encoded_size = seed.size() - 4U;
  Require((encoded_size % 2U) == 0U, "Hex seed must contain full bytes");

  std::vector<std::uint8_t> out;
  out.reserve(encoded_size / 2U);
  for (std::size_t i = 4U; i < seed.size(); i += 2U)
  {
    const int hi = HexNibble(seed[i]);
    const int lo = HexNibble(seed[i + 1U]);
    Require(hi >= 0 && lo >= 0, "Hex seed contains invalid character");
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
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

void TestWavRejectsShortFmtChunk()
{
  const std::vector<std::uint8_t> bytes =
      DecodeHexSeed("hex:524946461800000057415645666d74200c00000001000100803e0000007d0000");
  RequireThrows(
      [&]()
      {
        (void)sonitude::audio::ReadWavBytes(bytes.data(), bytes.size());
      },
      "WAV parser should reject fmt chunks shorter than 16 bytes");
}

void TestWavOddChunkPaddingIsHandled()
{
  const std::vector<std::uint8_t> bytes =
      DecodeHexSeed("hex:5249464630000000574156454a554e4b010000007800666d74201000000001000100803e0000007d00000200100064617461020000000000");
  const auto wav = sonitude::audio::ReadWavBytes(bytes.data(), bytes.size());
  Require(wav.channels == 1, "odd-padding WAV should decode channels");
  Require(wav.sample_rate_hz == 16000U, "odd-padding WAV should decode sample rate");
  Require(wav.interleaved.size() == 1, "odd-padding WAV should decode one sample");
}

void TestWavRejectsOversizedChunkDeclaration()
{
  const std::vector<std::uint8_t> bytes =
      DecodeHexSeed("hex:524946462400000057415645666d74201000000001000100803e0000007d00000200100064617461ffffffff");
  RequireThrows(
      [&]()
      {
        (void)sonitude::audio::ReadWavBytes(bytes.data(), bytes.size());
      },
      "WAV parser should reject data chunk declarations beyond available bytes");
}

void TestWavRejectsTruncatedDataChunk()
{
  const std::vector<std::uint8_t> bytes =
      DecodeHexSeed("hex:524946462600000057415645666d74201000000001000100803e0000007d000002001000646174610200000000");
  RequireThrows(
      [&]()
      {
        (void)sonitude::audio::ReadWavBytes(bytes.data(), bytes.size());
      },
      "WAV parser should reject truncated data chunk payload");
}
}  // namespace

void RunAudioSupportTests()
{
  TestS16RoundTrip();
  TestChannelExtract();
  TestWavRoundTrip();
  TestWavRejectsShortFmtChunk();
  TestWavOddChunkPaddingIsHandled();
  TestWavRejectsOversizedChunkDeclaration();
  TestWavRejectsTruncatedDataChunk();
}
