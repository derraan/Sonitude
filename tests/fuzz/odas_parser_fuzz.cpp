#include <cstddef>
#include <cstdint>
#include <string_view>

#include "spatial/odas_message_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  if (data == nullptr || size == 0)
  {
    return 0;
  }
  sonitude::spatial::OdasMessageParser parser;
  (void)parser.feed(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
