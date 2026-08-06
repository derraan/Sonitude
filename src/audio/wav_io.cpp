#include "audio/wav_io.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace sonitude::audio
{
namespace
{
constexpr std::uint16_t kWavFormatPcm = 0x0001;
constexpr std::uint16_t kWavFormatIeeeFloat = 0x0003;

void WriteLe16(std::ofstream& out, const std::uint16_t value)
{
  const std::array<std::uint8_t, 2> bytes = {
      static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>((value >> 8) & 0xFF)};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteLe32(std::ofstream& out, const std::uint32_t value)
{
  const std::array<std::uint8_t, 4> bytes = {static_cast<std::uint8_t>(value & 0xFF),
                                             static_cast<std::uint8_t>((value >> 8) & 0xFF),
                                             static_cast<std::uint8_t>((value >> 16) & 0xFF),
                                             static_cast<std::uint8_t>((value >> 24) & 0xFF)};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::uint16_t ReadLe16(std::ifstream& in)
{
  std::array<std::uint8_t, 2> bytes{};
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!in)
  {
    throw std::runtime_error("Unexpected EOF while reading WAV u16");
  }
  return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadLe32(std::ifstream& in)
{
  std::array<std::uint8_t, 4> bytes{};
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!in)
  {
    throw std::runtime_error("Unexpected EOF while reading WAV u32");
  }
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

PcmFormat ResolveFormat(const std::uint16_t wav_format_tag, const std::uint16_t bits_per_sample)
{
  if (wav_format_tag == kWavFormatIeeeFloat && bits_per_sample == 32)
  {
    return PcmFormat::FLOAT32_LE;
  }
  if (wav_format_tag != kWavFormatPcm)
  {
    throw std::runtime_error("Unsupported WAV format tag");
  }
  if (bits_per_sample == 16)
  {
    return PcmFormat::S16_LE;
  }
  if (bits_per_sample == 24)
  {
    return PcmFormat::S24_3LE;
  }
  if (bits_per_sample == 32)
  {
    return PcmFormat::S32_LE;
  }
  throw std::runtime_error("Unsupported WAV bit depth");
}
}  // namespace

void WriteWavFile(const std::string& path, const WavData& data)
{
  if (data.sample_rate_hz == 0 || data.channels == 0)
  {
    throw std::runtime_error("Invalid WAV metadata");
  }
  if (data.interleaved.empty() || (data.interleaved.size() % data.channels) != 0)
  {
    throw std::runtime_error("Invalid interleaved sample length for WAV write");
  }

  const std::vector<std::uint8_t> pcm = FloatToInterleaved(data.interleaved, data.format);
  const std::uint16_t bits_per_sample = static_cast<std::uint16_t>(BytesPerSample(data.format) * 8U);
  const std::uint16_t block_align =
      static_cast<std::uint16_t>(data.channels * BytesPerSample(data.format));
  const std::uint32_t byte_rate = data.sample_rate_hz * block_align;

  const std::uint32_t fmt_chunk_size = 16;
  const std::uint32_t data_chunk_size = static_cast<std::uint32_t>(pcm.size());
  const std::uint32_t riff_chunk_size = 4 + (8 + fmt_chunk_size) + (8 + data_chunk_size);
  const std::uint16_t wav_format =
      (data.format == PcmFormat::FLOAT32_LE) ? kWavFormatIeeeFloat : kWavFormatPcm;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
  {
    throw std::runtime_error("Unable to open WAV for writing: " + path);
  }

  out.write("RIFF", 4);
  WriteLe32(out, riff_chunk_size);
  out.write("WAVE", 4);

  out.write("fmt ", 4);
  WriteLe32(out, fmt_chunk_size);
  WriteLe16(out, wav_format);
  WriteLe16(out, data.channels);
  WriteLe32(out, data.sample_rate_hz);
  WriteLe32(out, byte_rate);
  WriteLe16(out, block_align);
  WriteLe16(out, bits_per_sample);

  out.write("data", 4);
  WriteLe32(out, data_chunk_size);
  out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
}

WavData ReadWavFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("Unable to open WAV for reading: " + path);
  }

  std::array<char, 4> riff{};
  in.read(riff.data(), 4);
  if (std::memcmp(riff.data(), "RIFF", 4) != 0)
  {
    throw std::runtime_error("WAV RIFF header missing");
  }
  (void)ReadLe32(in);
  std::array<char, 4> wave{};
  in.read(wave.data(), 4);
  if (std::memcmp(wave.data(), "WAVE", 4) != 0)
  {
    throw std::runtime_error("WAV WAVE header missing");
  }

  std::uint16_t wav_format_tag = 0;
  std::uint16_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint16_t bits_per_sample = 0;
  std::vector<std::uint8_t> pcm_bytes;

  while (in)
  {
    std::array<char, 4> chunk_id{};
    in.read(chunk_id.data(), 4);
    if (!in)
    {
      break;
    }
    const std::uint32_t chunk_size = ReadLe32(in);
    if (std::memcmp(chunk_id.data(), "fmt ", 4) == 0)
    {
      wav_format_tag = ReadLe16(in);
      channels = ReadLe16(in);
      sample_rate = ReadLe32(in);
      (void)ReadLe32(in);
      (void)ReadLe16(in);
      bits_per_sample = ReadLe16(in);
      if (chunk_size > 16)
      {
        in.seekg(static_cast<std::streamoff>(chunk_size - 16), std::ios::cur);
      }
    }
    else if (std::memcmp(chunk_id.data(), "data", 4) == 0)
    {
      pcm_bytes.resize(chunk_size);
      in.read(reinterpret_cast<char*>(pcm_bytes.data()), static_cast<std::streamsize>(chunk_size));
      if (!in)
      {
        throw std::runtime_error("Unexpected EOF in WAV data chunk");
      }
    }
    else
    {
      in.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
    }
  }

  if (channels == 0 || sample_rate == 0 || bits_per_sample == 0 || pcm_bytes.empty())
  {
    throw std::runtime_error("Incomplete WAV file");
  }

  const PcmFormat format = ResolveFormat(wav_format_tag, bits_per_sample);
  const std::size_t bps = BytesPerSample(format);
  if ((pcm_bytes.size() % bps) != 0)
  {
    throw std::runtime_error("WAV payload byte size is not sample-aligned");
  }
  const std::size_t total_samples = pcm_bytes.size() / bps;
  if ((total_samples % channels) != 0)
  {
    throw std::runtime_error("WAV payload sample count is not channel-aligned");
  }

  WavData out;
  out.sample_rate_hz = sample_rate;
  out.channels = channels;
  out.format = format;
  out.interleaved = InterleavedToFloat(pcm_bytes.data(), total_samples / channels, channels, format);
  return out;
}
}  // namespace sonitude::audio
