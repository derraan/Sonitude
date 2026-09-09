#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "spatial/spatial_types.hpp"

namespace sonitude::spatial
{
class OdasMessageParser
{
 public:
  explicit OdasMessageParser(std::size_t max_buffer_bytes = 256 * 1024)
      : max_buffer_bytes_(max_buffer_bytes)
  {
  }
  std::vector<SourceObservation> feed(std::string_view bytes);
  std::uint64_t overflowResyncCount() const { return overflow_resync_count_; }

 private:
  std::vector<SourceObservation> parseOneObject(const std::string& json_object) const;
  static bool extractNumber(const std::string& json, const std::string& key, double& out);
  static bool extractUnsigned(const std::string& json, const std::string& key, std::uint64_t& out);
  static std::size_t findMatchingBrace(const std::string& json, std::size_t open_pos);

  std::string buffer_;
  std::size_t max_buffer_bytes_ = 256 * 1024;
  std::uint64_t overflow_resync_count_ = 0;
};
}  // namespace sonitude::spatial
