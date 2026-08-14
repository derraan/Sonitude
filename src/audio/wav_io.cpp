#include "audio/wav_io.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
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

std::uint64_t CurrentOffset(std::ifstream& in)
{
  const std::streampos pos = in.tellg();
  if (pos < 0)
  {
    throw std::runtime_error("Invalid WAV stream position");
  }
  return static_cast<std::uint64_t>(pos);
}

std::uint64_t RemainingBytes(std::ifstream& in, const std::uint64_t file_size)
{
  const std::uint64_t offset = CurrentOffset(in);
  if (offset > file_size)
  {
    throw std::runtime_error("WAV stream advanced past file end");
  }
  return file_size - offset;
}

void SkipBytes(std::ifstream& in, const std::uint64_t count)
{
  if (count == 0)
  {
    return;
  }
  if (count > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
  {
    throw std::runtime_error("WAV chunk skip exceeds stream seek range");
  }
  in.seekg(static_cast<std::streamoff>(count), std::ios::cur);
  if (!in)
  {
    throw std::runtime_error("Failed to skip WAV chunk bytes");
  }
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
  in.seekg(0, std::ios::end);
  const std::streampos file_end = in.tellg();
  if (file_end < 0)
  {
    throw std::runtime_error("Unable to inspect WAV file length");
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(file_end);
  if (file_size < 12U)
  {
    throw std::runtime_error("WAV file too small for RIFF header");
  }
  in.seekg(0, std::ios::beg);

  std::array<char, 4> riff{};
  in.read(riff.data(), 4);
  if (!in)
  {
    throw std::runtime_error("Unexpected EOF while reading RIFF header");
  }
  if (std::memcmp(riff.data(), "RIFF", 4) != 0)
  {
    throw std::runtime_error("WAV RIFF header missing");
  }
  (void)ReadLe32(in);
  std::array<char, 4> wave{};
  in.read(wave.data(), 4);
  if (!in)
  {
    throw std::runtime_error("Unexpected EOF while reading WAVE header");
  }
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
    if (RemainingBytes(in, file_size) < 8U)
    {
      break;
    }
    std::array<char, 4> chunk_id{};
    in.read(chunk_id.data(), 4);
    if (!in)
    {
      break;
    }
    const std::uint32_t chunk_size = ReadLe32(in);
    const std::uint64_t payload_bytes_available = RemainingBytes(in, file_size);
    if (static_cast<std::uint64_t>(chunk_size) > payload_bytes_available)
    {
      throw std::runtime_error("WAV chunk size exceeds remaining file bytes");
    }
    const bool has_pad_byte = (chunk_size & 1U) != 0U;
    const std::uint64_t total_chunk_footprint =
        static_cast<std::uint64_t>(chunk_size) + (has_pad_byte ? 1U : 0U);
    if (total_chunk_footprint > payload_bytes_available)
    {
      throw std::runtime_error("WAV chunk padding exceeds remaining file bytes");
    }
    if (std::memcmp(chunk_id.data(), "fmt ", 4) == 0)
    {
      if (chunk_size < 16U)
      {
        throw std::runtime_error("WAV fmt chunk is shorter than 16 bytes");
      }
      wav_format_tag = ReadLe16(in);
      channels = ReadLe16(in);
      sample_rate = ReadLe32(in);
      (void)ReadLe32(in);
      (void)ReadLe16(in);
      bits_per_sample = ReadLe16(in);
      if (chunk_size > 16)
      {
        SkipBytes(in, static_cast<std::uint64_t>(chunk_size - 16U));
      }
    }
    else if (std::memcmp(chunk_id.data(), "data", 4) == 0)
    {
      pcm_bytes.resize(static_cast<std::size_t>(chunk_size));
      in.read(reinterpret_cast<char*>(pcm_bytes.data()), static_cast<std::streamsize>(chunk_size));
      if (!in)
      {
        throw std::runtime_error("Unexpected EOF in WAV data chunk");
      }
    }
    else
    {
      SkipBytes(in, static_cast<std::uint64_t>(chunk_size));
    }

    if (has_pad_byte)
    {
      SkipBytes(in, 1U);
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
