#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "spatial/spatial_types.hpp"

namespace sonitude::spatial
{
class OdasMessageParser
{
 public:
  std::vector<SourceObservation> feed(std::string_view bytes);

 private:
  std::vector<SourceObservation> parseOneObject(const std::string& json_object) const;
  static bool extractNumber(const std::string& json, const std::string& key, double& out);
  static bool extractUnsigned(const std::string& json, const std::string& key, std::uint64_t& out);
  static std::size_t findMatchingBrace(const std::string& json, std::size_t open_pos);

  std::string buffer_;
};
}  // namespace sonitude::spatial
