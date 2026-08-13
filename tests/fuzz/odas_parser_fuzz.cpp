#include <cstddef>
#include <cstdint>
#include <string_view>

#include "spatial/odas_message_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  static sonitude::spatial::OdasMessageParser parser(256 * 1024);
  if (data == nullptr || size == 0)
  {
    return 0;
  }
  (void)parser.feed(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
