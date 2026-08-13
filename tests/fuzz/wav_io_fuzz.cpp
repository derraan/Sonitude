#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "audio/wav_io.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  static std::atomic<std::uint64_t> counter{0};
  if (data == nullptr || size < 12)
  {
    return 0;
  }

  const std::uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
  const std::filesystem::path path =
      std::filesystem::path(SONITUDE_SOURCE_DIR) / "build" / ("fuzz_wav_input_" + std::to_string(id) + ".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
      return 0;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  }

  try
  {
    (void)sonitude::audio::ReadWavFile(path.string());
  }
  catch (...)
  {
    // Fuzzer expects the parser to reject malformed inputs by throwing.
  }

  std::error_code ec;
  std::filesystem::remove(path, ec);
  return 0;
}
