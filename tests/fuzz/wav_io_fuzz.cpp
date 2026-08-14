#include <cstddef>
#include <cstdint>

#include "audio/wav_io.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  if (data == nullptr || size < 12)
  {
    return 0;
  }

  try
  {
    (void)sonitude::audio::ReadWavBytes(data, size);
  }
  catch (...)
  {
    // Fuzzer expects the parser to reject malformed inputs by throwing.
  }
  return 0;
}
