#include "dsp/hrtf_table.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace sonitude::dsp
{
namespace
{
constexpr std::array<char, 4> kMagic = {'S', 'N', 'H', 'R'};
constexpr std::uint32_t kVersion = 1;

std::uint32_t Crc32Update(std::uint32_t crc, const std::uint8_t byte)
{
  std::uint32_t x = crc ^ byte;
  for (int i = 0; i < 8; ++i)
  {
    x = (x & 1U) ? ((x >> 1U) ^ 0xEDB88320U) : (x >> 1U);
  }
  return x;
}

std::uint32_t Crc32(const std::vector<std::uint8_t>& payload)
{
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::uint8_t byte : payload)
  {
    crc = Crc32Update(crc, byte);
  }
  return ~crc;
}

template <typename T>
T ReadScalar(std::ifstream& in, std::vector<std::uint8_t>* crc_payload)
{
  T value{};
  in.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
  if (!in)
  {
    throw std::runtime_error("Failed to read HRTF table scalar");
  }
  if (crc_payload != nullptr)
  {
    const auto* ptr = reinterpret_cast<const std::uint8_t*>(&value);
    crc_payload->insert(crc_payload->end(), ptr, ptr + sizeof(T));
  }
  return value;
}
}  // namespace

HrtfTable LoadHrtfTableFromFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("Unable to open HRTF table: " + path);
  }

  std::array<char, 4> magic{};
  in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if (!in || magic != kMagic)
  {
    throw std::runtime_error("Invalid HRTF table magic in: " + path);
  }

  std::vector<std::uint8_t> crc_payload;
  const std::uint32_t version = ReadScalar<std::uint32_t>(in, &crc_payload);
  if (version != kVersion)
  {
    throw std::runtime_error("Unsupported HRTF table version: " + std::to_string(version));
  }

  const std::uint32_t sample_rate_hz = ReadScalar<std::uint32_t>(in, &crc_payload);
  const std::uint32_t direction_count = ReadScalar<std::uint32_t>(in, &crc_payload);
  const std::uint32_t taps_per_ear = ReadScalar<std::uint32_t>(in, &crc_payload);
  const std::uint32_t ear_count = ReadScalar<std::uint32_t>(in, &crc_payload);

  if (sample_rate_hz == 0 || direction_count == 0 || taps_per_ear == 0 || ear_count != 2)
  {
    throw std::runtime_error("HRTF table header values are invalid");
  }

  const std::uint64_t coeff_count_u64 =
      static_cast<std::uint64_t>(direction_count) * static_cast<std::uint64_t>(ear_count) *
      static_cast<std::uint64_t>(taps_per_ear);
  if (coeff_count_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
  {
    throw std::runtime_error("HRTF table coefficient payload is too large");
  }
  const std::size_t coeff_count = static_cast<std::size_t>(coeff_count_u64);

  HrtfTable table{};
  table.sample_rate_hz = sample_rate_hz;
  table.taps_per_ear = taps_per_ear;
  table.directions.resize(direction_count);
  table.fir.resize(coeff_count, 0.0F);

  for (std::uint32_t i = 0; i < direction_count; ++i)
  {
    HrtfDirection direction{};
    direction.azimuth_deg = ReadScalar<float>(in, &crc_payload);
    direction.elevation_deg = ReadScalar<float>(in, &crc_payload);
    direction.delay_left_samples = ReadScalar<float>(in, &crc_payload);
    direction.delay_right_samples = ReadScalar<float>(in, &crc_payload);
    table.directions[i] = direction;
  }

  in.read(reinterpret_cast<char*>(table.fir.data()),
          static_cast<std::streamsize>(table.fir.size() * sizeof(float)));
  if (!in)
  {
    throw std::runtime_error("Failed to read HRTF coefficient payload");
  }
  const auto* fir_bytes = reinterpret_cast<const std::uint8_t*>(table.fir.data());
  crc_payload.insert(
      crc_payload.end(), fir_bytes, fir_bytes + (table.fir.size() * sizeof(float)));

  const std::uint32_t declared_crc = ReadScalar<std::uint32_t>(in, nullptr);
  if (Crc32(crc_payload) != declared_crc)
  {
    throw std::runtime_error("HRTF table CRC mismatch");
  }
  return table;
}
}  // namespace sonitude::dsp
