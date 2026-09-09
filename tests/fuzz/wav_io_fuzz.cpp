#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "audio/wav_io.hpp"

namespace
{
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

std::vector<std::uint8_t> DecodeSeedIfHex(const std::uint8_t* data, const std::size_t size)
{
  constexpr std::string_view prefix = "hex:";
  std::size_t encoded_size = size;
  while (encoded_size > prefix.size() &&
         (data[encoded_size - 1U] == '\n' || data[encoded_size - 1U] == '\r'))
  {
    --encoded_size;
  }
  if (encoded_size < prefix.size() ||
      std::string_view(reinterpret_cast<const char*>(data), prefix.size()) != prefix ||
      ((encoded_size - prefix.size()) & 1U) != 0U)
  {
    return {};
  }

  std::vector<std::uint8_t> decoded;
  decoded.reserve((encoded_size - prefix.size()) / 2U);
  for (std::size_t i = prefix.size(); i < encoded_size; i += 2U)
  {
    const int high = HexNibble(static_cast<char>(data[i]));
    const int low = HexNibble(static_cast<char>(data[i + 1U]));
    if (high < 0 || low < 0)
    {
      return {};
    }
    decoded.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return decoded;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
  if (data == nullptr || size == 0U)
  {
    return 0;
  }

  const std::vector<std::uint8_t> decoded = DecodeSeedIfHex(data, size);
  const std::uint8_t* input = decoded.empty() ? data : decoded.data();
  const std::size_t input_size = decoded.empty() ? size : decoded.size();

  try
  {
    (void)sonitude::audio::ReadWavBytes(input, input_size);
  }
  catch (const std::runtime_error&)
  {
    // Malformed WAV rejection is expected. Allocation and other resource
    // failures intentionally escape so the fuzzer reports them.
  }
  return 0;
}
