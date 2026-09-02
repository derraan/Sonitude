#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
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

void ExpectThrows(const std::function<void()>& fn, const std::string& message)
{
  bool threw = false;
  try
  {
    fn();
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  Require(threw, message);
}

void AppendLe16(std::vector<std::uint8_t>& bytes, const std::uint16_t value)
{
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void AppendLe32(std::vector<std::uint8_t>& bytes, const std::uint32_t value)
{
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFU));
}

void AppendTag(std::vector<std::uint8_t>& bytes, const std::array<char, 4>& tag)
{
  for (const char ch : tag)
  {
    bytes.push_back(static_cast<std::uint8_t>(ch));
  }
}

void FinalizeRiffSize(std::vector<std::uint8_t>& bytes)
{
  const std::uint32_t riff_size = static_cast<std::uint32_t>(bytes.size() - 8U);
  bytes[4] = static_cast<std::uint8_t>(riff_size & 0xFFU);
  bytes[5] = static_cast<std::uint8_t>((riff_size >> 8) & 0xFFU);
  bytes[6] = static_cast<std::uint8_t>((riff_size >> 16) & 0xFFU);
  bytes[7] = static_cast<std::uint8_t>((riff_size >> 24) & 0xFFU);
}

void WriteBytesToFile(const std::string& path, const std::vector<std::uint8_t>& bytes)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.good(), "failed to create WAV fixture");
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  Require(out.good(), "failed to write WAV fixture");
}

void TestS16RoundTrip()
{
  const std::vector<float> input = {-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
  const std::vector<std::uint8_t> bytes =
      sonitude::audio::FloatToInterleaved(input, sonitude::audio::PcmFormat::S16_LE);
  const std::vector<float> decoded = sonitude::audio::InterleavedToFloat(
      bytes.data(), input.size(), 1, sonitude::audio::PcmFormat::S16_LE);
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
  const auto frames = sonitude::audio::ExtractActiveMicFrames(bytes.data(), 2, 8, active,
                                                              sonitude::audio::PcmFormat::S16_LE);
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
  std::vector<std::uint8_t> bytes;
  AppendTag(bytes, {'R', 'I', 'F', 'F'});
  AppendLe32(bytes, 0);
  AppendTag(bytes, {'W', 'A', 'V', 'E'});
  AppendTag(bytes, {'f', 'm', 't', ' '});
  AppendLe32(bytes, 12);
  AppendLe16(bytes, 1);
  AppendLe16(bytes, 1);
  AppendLe32(bytes, 16000);
  AppendLe32(bytes, 32000);
  AppendLe16(bytes, 2);
  AppendTag(bytes, {'d', 'a', 't', 'a'});
  AppendLe32(bytes, 2);
  AppendLe16(bytes, 0);
  FinalizeRiffSize(bytes);

  const std::string path = "unit_wav_short_fmt.wav";
  WriteBytesToFile(path, bytes);
  ExpectThrows([&path]() { (void)sonitude::audio::ReadWavFile(path); },
               "WAV reader must reject fmt chunks smaller than 16 bytes");
  (void)std::remove(path.c_str());
}

void TestWavOddChunkPaddingIsHandled()
{
  std::vector<std::uint8_t> bytes;
  AppendTag(bytes, {'R', 'I', 'F', 'F'});
  AppendLe32(bytes, 0);
  AppendTag(bytes, {'W', 'A', 'V', 'E'});

  AppendTag(bytes, {'J', 'U', 'N', 'K'});
  AppendLe32(bytes, 1);
  bytes.push_back(static_cast<std::uint8_t>('x'));
  bytes.push_back(0); // RIFF pad byte for odd-sized chunk.

  AppendTag(bytes, {'f', 'm', 't', ' '});
  AppendLe32(bytes, 16);
  AppendLe16(bytes, 1);
  AppendLe16(bytes, 1);
  AppendLe32(bytes, 16000);
  AppendLe32(bytes, 32000);
  AppendLe16(bytes, 2);
  AppendLe16(bytes, 16);

  AppendTag(bytes, {'d', 'a', 't', 'a'});
  AppendLe32(bytes, 2);
  AppendLe16(bytes, 1200);

  FinalizeRiffSize(bytes);

  const std::string path = "unit_wav_with_odd_chunk.wav";
  WriteBytesToFile(path, bytes);
  const auto wav = sonitude::audio::ReadWavFile(path);
  Require(wav.sample_rate_hz == 16000,
          "WAV reader should parse sample rate after odd chunk padding");
  Require(wav.channels == 1, "WAV reader should parse channel count after odd chunk padding");
  Require(wav.interleaved.size() == 1,
          "WAV reader should decode one sample after odd chunk padding");
  (void)std::remove(path.c_str());
}

std::vector<std::uint8_t> MakeMinimalPcmWav()
{
  std::vector<std::uint8_t> bytes;
  AppendTag(bytes, {'R', 'I', 'F', 'F'});
  AppendLe32(bytes, 0);
  AppendTag(bytes, {'W', 'A', 'V', 'E'});
  AppendTag(bytes, {'f', 'm', 't', ' '});
  AppendLe32(bytes, 16);
  AppendLe16(bytes, 1);
  AppendLe16(bytes, 1);
  AppendLe32(bytes, 16000);
  AppendLe32(bytes, 32000);
  AppendLe16(bytes, 2);
  AppendLe16(bytes, 16);
  AppendTag(bytes, {'d', 'a', 't', 'a'});
  AppendLe32(bytes, 2);
  AppendLe16(bytes, 0);
  FinalizeRiffSize(bytes);
  return bytes;
}

void TestWavMemoryApiAcceptsExactValidBoundary()
{
  const std::vector<std::uint8_t> bytes = MakeMinimalPcmWav();
  const auto wav = sonitude::audio::ReadWavBytes(bytes.data(), bytes.size());
  Require(wav.sample_rate_hz == 16000, "memory WAV reader should preserve sample rate");
  Require(wav.channels == 1, "memory WAV reader should preserve channel count");
  Require(wav.interleaved.size() == 1, "exact RIFF boundary should decode one frame");
}

void TestWavRejectsInvalidRiffSizes()
{
  {
    std::vector<std::uint8_t> bytes = {'R', 'I', 'F', 'F'};
    AppendLe32(bytes, 0xFFFFFFFFU);
    AppendTag(bytes, {'W', 'A', 'V', 'E'});
    ExpectThrows([&bytes]() { (void)sonitude::audio::ReadWavBytes(bytes.data(), bytes.size()); },
                 "0xFFFFFFFF RIFF size must be rejected before chunk parsing");
  }

  {
    std::vector<std::uint8_t> bytes = MakeMinimalPcmWav();
    bytes.pop_back();
    ExpectThrows([&bytes]() { (void)sonitude::audio::ReadWavBytes(bytes.data(), bytes.size()); },
                 "RIFF size larger than available input must be rejected");
  }
}

void TestWavRejectsOverflowingMetadata()
{
  std::vector<std::uint8_t> bytes;
  AppendTag(bytes, {'R', 'I', 'F', 'F'});
  AppendLe32(bytes, 0);
  AppendTag(bytes, {'W', 'A', 'V', 'E'});
  AppendTag(bytes, {'f', 'm', 't', ' '});
  AppendLe32(bytes, 16);
  AppendLe16(bytes, 1);
  AppendLe16(bytes, 0xFFFFU);
  AppendLe32(bytes, 0xFFFFFFFFU);
  AppendLe32(bytes, 0);
  AppendLe16(bytes, 0);
  AppendLe16(bytes, 32);
  AppendTag(bytes, {'d', 'a', 't', 'a'});
  AppendLe32(bytes, 4);
  AppendLe32(bytes, 0);
  FinalizeRiffSize(bytes);

  ExpectThrows([&bytes]() { (void)sonitude::audio::ReadWavBytes(bytes.data(), bytes.size()); },
               "overflowing channel/sample/frame metadata must be rejected");
}

void TestWavRejectsOversizedChunkDeclaration()
{
  std::vector<std::uint8_t> bytes;
  AppendTag(bytes, {'R', 'I', 'F', 'F'});
  AppendLe32(bytes, 0);
  AppendTag(bytes, {'W', 'A', 'V', 'E'});

  AppendTag(bytes, {'f', 'm', 't', ' '});
  AppendLe32(bytes, 16);
  AppendLe16(bytes, 1);
  AppendLe16(bytes, 1);
  AppendLe32(bytes, 16000);
  AppendLe32(bytes, 32000);
  AppendLe16(bytes, 2);
  AppendLe16(bytes, 16);

  AppendTag(bytes, {'d', 'a', 't', 'a'});
  AppendLe32(bytes, 0xFFFFFFFFU);
  AppendLe16(bytes, 0);

  FinalizeRiffSize(bytes);

  const std::string path = "unit_wav_oversized_chunk.wav";
  WriteBytesToFile(path, bytes);
  ExpectThrows([&path]() { (void)sonitude::audio::ReadWavFile(path); },
               "WAV reader must reject chunk sizes that exceed file bounds");
  (void)std::remove(path.c_str());
}

void TestWavRejectsMalformedPayloads()
{
  {
    const std::string path = "unit_wav_bad_header.wav";
    WriteBytesToFile(path, {'N', 'O', 'P', 'E'});
    ExpectThrows([&path]() { (void)sonitude::audio::ReadWavFile(path); },
                 "invalid RIFF header should throw");
    (void)std::remove(path.c_str());
  }

  {
    std::vector<std::uint8_t> bytes;
    AppendTag(bytes, {'R', 'I', 'F', 'F'});
    AppendLe32(bytes, 0);
    AppendTag(bytes, {'W', 'A', 'V', 'E'});
    AppendTag(bytes, {'f', 'm', 't', ' '});
    AppendLe32(bytes, 16);
    AppendLe16(bytes, 1);
    AppendLe16(bytes, 1);
    AppendLe32(bytes, 44100);
    AppendLe32(bytes, 88200);
    AppendLe16(bytes, 2);
    AppendLe16(bytes, 16);
    AppendTag(bytes, {'d', 'a', 't', 'a'});
    AppendLe32(bytes, 8);
    AppendLe16(bytes, 0);
    AppendLe16(bytes, 0);
    FinalizeRiffSize(bytes);

    const std::string path = "unit_wav_truncated_data.wav";
    WriteBytesToFile(path, bytes);
    ExpectThrows([&path]() { (void)sonitude::audio::ReadWavFile(path); },
                 "truncated data payload should throw");
    (void)std::remove(path.c_str());
  }

  {
    std::vector<std::uint8_t> bytes;
    AppendTag(bytes, {'R', 'I', 'F', 'F'});
    AppendLe32(bytes, 0);
    AppendTag(bytes, {'W', 'A', 'V', 'E'});
    AppendTag(bytes, {'f', 'm', 't', ' '});
    AppendLe32(bytes, 16);
    AppendLe16(bytes, 1);
    AppendLe16(bytes, 2);
    AppendLe32(bytes, 44100);
    AppendLe32(bytes, 176400);
    AppendLe16(bytes, 4);
    AppendLe16(bytes, 16);
    AppendTag(bytes, {'d', 'a', 't', 'a'});
    AppendLe32(bytes, 3);
    bytes.insert(bytes.end(), 3, 0);
    FinalizeRiffSize(bytes);

    const std::string path = "unit_wav_unaligned_data.wav";
    WriteBytesToFile(path, bytes);
    ExpectThrows([&path]() { (void)sonitude::audio::ReadWavFile(path); },
                 "sample-unaligned payload should throw");
    (void)std::remove(path.c_str());
  }
}
} // namespace

void RunAudioSupportTests()
{
  TestS16RoundTrip();
  TestChannelExtract();
  TestWavRoundTrip();
  TestWavRejectsShortFmtChunk();
  TestWavOddChunkPaddingIsHandled();
  TestWavMemoryApiAcceptsExactValidBoundary();
  TestWavRejectsInvalidRiffSizes();
  TestWavRejectsOverflowingMetadata();
  TestWavRejectsOversizedChunkDeclaration();
  TestWavRejectsMalformedPayloads();
}
