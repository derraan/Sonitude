#include "audio/wav_io.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <streambuf>

namespace sonitude::audio
{
namespace
{
constexpr std::uint16_t kWavFormatPcm = 0x0001;
constexpr std::uint16_t kWavFormatIeeeFloat = 0x0003;
constexpr std::uint32_t kCanonicalFmtSize = 16;
constexpr std::uint64_t kRiffHeaderBytes = 8;

void WriteLe16(std::ostream& out, const std::uint16_t value)
{
  const std::array<std::uint8_t, 2> bytes = {static_cast<std::uint8_t>(value & 0xFF),
                                             static_cast<std::uint8_t>((value >> 8) & 0xFF)};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteLe32(std::ostream& out, const std::uint32_t value)
{
  const std::array<std::uint8_t, 4> bytes = {static_cast<std::uint8_t>(value & 0xFF),
                                             static_cast<std::uint8_t>((value >> 8) & 0xFF),
                                             static_cast<std::uint8_t>((value >> 16) & 0xFF),
                                             static_cast<std::uint8_t>((value >> 24) & 0xFF)};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void ReadExact(std::istream& in, void* destination, std::uint64_t count, std::uint64_t& offset)
{
  auto* output = static_cast<char*>(destination);
  const auto stream_limit = static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max());
  while (count != 0U)
  {
    const std::uint64_t step = std::min(count, stream_limit);
    in.read(output, static_cast<std::streamsize>(step));
    if (!in)
    {
      throw std::runtime_error("Unexpected EOF while reading WAV data");
    }
    output += static_cast<std::size_t>(step);
    offset += step;
    count -= step;
  }
}

std::uint16_t ReadLe16(std::istream& in, std::uint64_t& offset)
{
  std::array<std::uint8_t, 2> bytes{};
  ReadExact(in, bytes.data(), bytes.size(), offset);
  return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadLe32(std::istream& in, std::uint64_t& offset)
{
  std::array<std::uint8_t, 4> bytes{};
  ReadExact(in, bytes.data(), bytes.size(), offset);
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

void SkipBytes(std::istream& in, std::uint64_t count, std::uint64_t& offset)
{
  std::array<char, 4096> discard{};
  while (count != 0U)
  {
    const std::uint64_t step = std::min<std::uint64_t>(count, discard.size());
    ReadExact(in, discard.data(), step, offset);
    count -= step;
  }
}

class MemoryStreamBuffer final : public std::streambuf
{
 public:
  MemoryStreamBuffer(const std::uint8_t* data, const std::size_t size)
  {
    auto* begin = const_cast<char*>(reinterpret_cast<const char*>(data));
    setg(begin, begin, begin + size);
  }
};

WavData ReadWavFromStream(std::istream& in, const std::uint64_t available_input)
{
  if (available_input < 12U)
  {
    throw std::runtime_error("WAV input is too small for a RIFF header");
  }

  std::uint64_t offset = 0;
  std::array<char, 4> riff{};
  ReadExact(in, riff.data(), riff.size(), offset);
  if (std::memcmp(riff.data(), "RIFF", 4) != 0)
  {
    throw std::runtime_error("WAV RIFF header missing");
  }
  const std::uint32_t riff_size = ReadLe32(in, offset);
  if (riff_size < 4U)
  {
    throw std::runtime_error("WAV RIFF size is smaller than the WAVE identifier");
  }
  const std::uint64_t riff_end = kRiffHeaderBytes + static_cast<std::uint64_t>(riff_size);
  if (riff_end > available_input)
  {
    throw std::runtime_error("WAV RIFF size exceeds available input");
  }

  std::array<char, 4> wave{};
  ReadExact(in, wave.data(), wave.size(), offset);
  if (std::memcmp(wave.data(), "WAVE", 4) != 0)
  {
    throw std::runtime_error("WAV WAVE header missing");
  }

  bool fmt_seen = false;
  bool data_seen = false;
  std::uint16_t wav_format_tag = 0;
  std::uint16_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t declared_byte_rate = 0;
  std::uint16_t declared_block_align = 0;
  std::uint16_t bits_per_sample = 0;
  std::vector<std::uint8_t> pcm_bytes;

  while (offset < riff_end)
  {
    const std::uint64_t header_bytes_available = riff_end - offset;
    if (header_bytes_available < 8U)
    {
      throw std::runtime_error("WAV RIFF ends with an incomplete chunk header");
    }

    std::array<char, 4> chunk_id{};
    ReadExact(in, chunk_id.data(), chunk_id.size(), offset);
    const std::uint32_t chunk_size = ReadLe32(in, offset);
    const bool has_pad_byte = (chunk_size & 1U) != 0U;
    const std::uint64_t chunk_footprint =
        static_cast<std::uint64_t>(chunk_size) + (has_pad_byte ? 1U : 0U);
    const std::uint64_t payload_bytes_available = riff_end - offset;
    if (chunk_footprint > payload_bytes_available)
    {
      throw std::runtime_error("WAV chunk size or padding exceeds remaining RIFF bytes");
    }

    if (std::memcmp(chunk_id.data(), "fmt ", 4) == 0)
    {
      if (fmt_seen)
      {
        throw std::runtime_error("WAV contains multiple fmt chunks");
      }
      if (chunk_size < kCanonicalFmtSize)
      {
        throw std::runtime_error("WAV fmt chunk is shorter than 16 bytes");
      }
      wav_format_tag = ReadLe16(in, offset);
      channels = ReadLe16(in, offset);
      sample_rate = ReadLe32(in, offset);
      declared_byte_rate = ReadLe32(in, offset);
      declared_block_align = ReadLe16(in, offset);
      bits_per_sample = ReadLe16(in, offset);
      SkipBytes(in, static_cast<std::uint64_t>(chunk_size - kCanonicalFmtSize), offset);
      fmt_seen = true;
    }
    else if (std::memcmp(chunk_id.data(), "data", 4) == 0)
    {
      if (data_seen)
      {
        throw std::runtime_error("WAV contains multiple data chunks");
      }
      if (static_cast<std::uint64_t>(chunk_size) > kMaxDecodedPcmBytes)
      {
        throw std::runtime_error("WAV data chunk exceeds the decoded payload limit");
      }
      if (static_cast<std::uint64_t>(chunk_size) >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
      {
        throw std::runtime_error("WAV data chunk exceeds the platform allocation range");
      }
      pcm_bytes.resize(static_cast<std::size_t>(chunk_size));
      ReadExact(in, pcm_bytes.data(), chunk_size, offset);
      data_seen = true;
    }
    else
    {
      SkipBytes(in, chunk_size, offset);
    }

    if (has_pad_byte)
    {
      SkipBytes(in, 1U, offset);
    }
  }

  if (!fmt_seen || !data_seen || channels == 0U || sample_rate == 0U || pcm_bytes.empty())
  {
    throw std::runtime_error("Incomplete WAV file");
  }

  const PcmFormat format = ResolveFormat(wav_format_tag, bits_per_sample);
  const std::size_t bytes_per_sample = BytesPerSample(format);
  if (channels > (std::numeric_limits<std::uint16_t>::max() / bytes_per_sample))
  {
    throw std::runtime_error("WAV channel and sample-width arithmetic overflows block alignment");
  }
  const std::size_t expected_block_align = static_cast<std::size_t>(channels) * bytes_per_sample;
  if (declared_block_align != expected_block_align)
  {
    throw std::runtime_error("WAV block alignment is inconsistent with channel metadata");
  }
  if (sample_rate >
      (std::numeric_limits<std::uint32_t>::max() / static_cast<std::uint32_t>(expected_block_align)))
  {
    throw std::runtime_error("WAV sample rate and block alignment overflow byte rate");
  }
  const std::uint32_t expected_byte_rate = sample_rate * static_cast<std::uint32_t>(expected_block_align);
  if (declared_byte_rate != expected_byte_rate)
  {
    throw std::runtime_error("WAV byte rate is inconsistent with format metadata");
  }
  if ((pcm_bytes.size() % expected_block_align) != 0U)
  {
    throw std::runtime_error("WAV payload byte size is not frame-aligned");
  }

  const std::size_t frames = pcm_bytes.size() / expected_block_align;
  if (frames > (std::numeric_limits<std::size_t>::max() / channels))
  {
    throw std::runtime_error("WAV frame and channel count overflow");
  }
  const std::size_t total_samples = frames * channels;
  if (total_samples > std::vector<float>().max_size())
  {
    throw std::runtime_error("WAV decoded sample count exceeds vector capacity");
  }

  WavData out;
  out.sample_rate_hz = sample_rate;
  out.channels = channels;
  out.format = format;
  out.interleaved = InterleavedToFloat(pcm_bytes.data(), frames, channels, format);
  return out;
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

  const std::size_t bytes_per_sample = BytesPerSample(data.format);
  if (data.channels > (std::numeric_limits<std::uint16_t>::max() / bytes_per_sample))
  {
    throw std::runtime_error("WAV channel and sample-width arithmetic overflows block alignment");
  }
  const std::size_t block_align_size = static_cast<std::size_t>(data.channels) * bytes_per_sample;
  if (data.sample_rate_hz >
      (std::numeric_limits<std::uint32_t>::max() / static_cast<std::uint32_t>(block_align_size)))
  {
    throw std::runtime_error("WAV sample rate and block alignment overflow byte rate");
  }
  if (data.interleaved.size() > (std::numeric_limits<std::uint32_t>::max() / bytes_per_sample))
  {
    throw std::runtime_error("WAV payload exceeds the RIFF data size field");
  }

  const std::size_t pcm_size = data.interleaved.size() * bytes_per_sample;
  constexpr std::uint32_t kRiffOverhead = 36;
  const std::uint32_t data_padding = (pcm_size & 1U) != 0U ? 1U : 0U;
  if (pcm_size > (std::numeric_limits<std::uint32_t>::max() - kRiffOverhead - data_padding))
  {
    throw std::runtime_error("WAV payload exceeds the RIFF chunk size field");
  }
  if (pcm_size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
  {
    throw std::runtime_error("WAV payload exceeds the stream write range");
  }

  const std::vector<std::uint8_t> pcm = FloatToInterleaved(data.interleaved, data.format);
  const std::uint16_t bits_per_sample = static_cast<std::uint16_t>(bytes_per_sample * 8U);
  const std::uint16_t block_align = static_cast<std::uint16_t>(block_align_size);
  const std::uint32_t byte_rate = data.sample_rate_hz * block_align;
  const std::uint32_t data_chunk_size = static_cast<std::uint32_t>(pcm_size);
  const std::uint32_t riff_chunk_size = kRiffOverhead + data_chunk_size + data_padding;
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
  WriteLe32(out, kCanonicalFmtSize);
  WriteLe16(out, wav_format);
  WriteLe16(out, data.channels);
  WriteLe32(out, data.sample_rate_hz);
  WriteLe32(out, byte_rate);
  WriteLe16(out, block_align);
  WriteLe16(out, bits_per_sample);

  out.write("data", 4);
  WriteLe32(out, data_chunk_size);
  out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
  if (data_padding != 0U)
  {
    out.put('\0');
  }
  if (!out)
  {
    throw std::runtime_error("Failed while writing WAV file: " + path);
  }
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
  in.seekg(0, std::ios::beg);
  return ReadWavFromStream(in, file_size);
}

WavData ReadWavBytes(const std::uint8_t* data, const std::size_t size)
{
  if (data == nullptr || size == 0U)
  {
    throw std::runtime_error("WAV byte buffer is empty");
  }
  if (size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
  {
    throw std::runtime_error("WAV byte buffer exceeds addressable stream range");
  }

  MemoryStreamBuffer buffer(data, size);
  std::istream in(&buffer);
  return ReadWavFromStream(in, size);
}
}  // namespace sonitude::audio
