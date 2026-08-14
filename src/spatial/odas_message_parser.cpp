#include "spatial/odas_message_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <system_error>

#include "spatial/angles.hpp"

namespace sonitude::spatial
{
namespace
{
double Clamp01(const double v)
{
  if (v < 0.0)
  {
    return 0.0;
  }
  if (v > 1.0)
  {
    return 1.0;
  }
  return v;
}

bool IsJsonNumberDelimiter(const char value)
{
  return value == ',' || value == '}' || value == ']';
}

struct ParsedNumber
{
  double value = 0.0;
  std::size_t token_begin = 0;
  std::size_t token_end = 0;
};

bool ParseNumberForKey(const std::string& json, const std::string& key, ParsedNumber& parsed)
{
  const std::size_t key_pos = json.find("\"" + key + "\"");
  if (key_pos == std::string::npos)
  {
    return false;
  }
  const std::size_t colon = json.find(':', key_pos);
  if (colon == std::string::npos)
  {
    return false;
  }

  std::size_t begin = colon + 1;
  while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin])) != 0)
  {
    ++begin;
  }
  if (begin == json.size())
  {
    return false;
  }

  errno = 0;
  char* end_ptr = nullptr;
  const char* const token_ptr = json.c_str() + static_cast<std::ptrdiff_t>(begin);
  const double value = std::strtod(token_ptr, &end_ptr);
  if (end_ptr == token_ptr || errno == ERANGE || !std::isfinite(value))
  {
    return false;
  }

  const std::size_t token_end = static_cast<std::size_t>(end_ptr - json.c_str());
  std::size_t delimiter = token_end;
  while (delimiter < json.size() && std::isspace(static_cast<unsigned char>(json[delimiter])) != 0)
  {
    ++delimiter;
  }
  if (delimiter >= json.size() || !IsJsonNumberDelimiter(json[delimiter]))
  {
    return false;
  }

  parsed = {.value = value, .token_begin = begin, .token_end = token_end};
  return true;
}
} // namespace

bool OdasMessageParser::extractNumber(const std::string& json, const std::string& key, double& out)
{
  ParsedNumber parsed;
  if (!ParseNumberForKey(json, key, parsed))
  {
    return false;
  }
  out = parsed.value;
  return true;
}

bool OdasMessageParser::extractUnsigned(const std::string& json, const std::string& key,
                                        std::uint64_t& out)
{
  ParsedNumber parsed;
  if (!ParseNumberForKey(json, key, parsed))
  {
    return false;
  }
  const std::string_view token(json.data() + parsed.token_begin,
                               parsed.token_end - parsed.token_begin);
  if (token.empty() || token.front() == '-' || token.front() == '+')
  {
    return false;
  }
  if (!std::all_of(token.begin(), token.end(),
                   [](const char value) { return value >= '0' && value <= '9'; }))
  {
    return false;
  }

  std::uint64_t parsed_id = 0;
  const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed_id, 10);
  if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
  {
    return false;
  }
  out = parsed_id;
  return true;
}

std::size_t OdasMessageParser::findMatchingBrace(const std::string& json,
                                                 const std::size_t open_pos)
{
  std::size_t depth = 0;
  for (std::size_t i = open_pos; i < json.size(); ++i)
  {
    if (json[i] == '{')
    {
      ++depth;
    }
    else if (json[i] == '}')
    {
      if (depth == 0)
      {
        return std::string::npos;
      }
      --depth;
      if (depth == 0)
      {
        return i;
      }
    }
  }
  return std::string::npos;
}

std::vector<SourceObservation>
OdasMessageParser::parseOneObject(const std::string& json_object) const
{
  std::vector<SourceObservation> out;
  double ts_sec = 0.0;
  const bool has_timestamp = json_object.find("\"timeStamp\"") != std::string::npos;
  if (!extractNumber(json_object, "timeStamp", ts_sec))
  {
    if (has_timestamp)
    {
      return out;
    }
    ts_sec = 0.0;
  }
  constexpr long double kNanosPerSecond = 1'000'000'000.0L;
  const long double timestamp_ns = static_cast<long double>(ts_sec) * kNanosPerSecond;
  if (timestamp_ns < 0.0L ||
      timestamp_ns > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
  {
    return out;
  }
  const std::uint64_t ts_ns = static_cast<std::uint64_t>(timestamp_ns);

  const auto src_pos = json_object.find("\"src\"");
  if (src_pos == std::string::npos)
  {
    return out;
  }
  const auto open = json_object.find('[', src_pos);
  const auto close = (open == std::string::npos) ? std::string::npos : json_object.find(']', open);
  if (open == std::string::npos || close == std::string::npos || close <= open)
  {
    return out;
  }
  const std::string src_body = json_object.substr(open + 1, close - open - 1);

  std::size_t cursor = 0;
  while (cursor < src_body.size())
  {
    const auto obj_open = src_body.find('{', cursor);
    if (obj_open == std::string::npos)
    {
      break;
    }
    const auto obj_close = findMatchingBrace(src_body, obj_open);
    if (obj_close == std::string::npos)
    {
      break;
    }
    const std::string src_obj = src_body.substr(obj_open, obj_close - obj_open + 1);

    SourceObservation obs{};
    std::uint64_t id = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double activity = 0.0;
    if (extractUnsigned(src_obj, "id", id) && extractNumber(src_obj, "x", x) &&
        extractNumber(src_obj, "y", y) && extractNumber(src_obj, "z", z) &&
        extractNumber(src_obj, "activity", activity))
    {
      const double az = std::atan2(y, x) * (180.0 / kPi);
      const double xy = std::hypot(x, y);
      const double el = std::atan2(z, xy) * (180.0 / kPi);
      obs.source_id = id;
      obs.azimuth_deg = static_cast<float>(NormalizeAzimuthDeg(az));
      obs.elevation_deg = static_cast<float>(el);
      obs.confidence = static_cast<float>(Clamp01(activity));
      obs.timestamp_ns = ts_ns;
      out.push_back(obs);
    }
    cursor = obj_close + 1;
  }
  return out;
}

std::vector<SourceObservation> OdasMessageParser::feed(const std::string_view bytes)
{
  const std::size_t max_buffer_bytes = std::max<std::size_t>(1, max_buffer_bytes_);
  if (bytes.size() >= max_buffer_bytes)
  {
    buffer_.assign(bytes.data() + (bytes.size() - max_buffer_bytes), max_buffer_bytes);
    ++overflow_resync_count_;
  }
  else
  {
    const std::size_t available = max_buffer_bytes - bytes.size();
    if (buffer_.size() > available)
    {
      buffer_.erase(0, buffer_.size() - available);
      ++overflow_resync_count_;
    }
    buffer_.append(bytes.data(), bytes.size());
  }
  std::vector<SourceObservation> out;

  std::size_t start = 0;
  while (start < buffer_.size())
  {
    const auto open = buffer_.find('{', start);
    if (open == std::string::npos)
    {
      buffer_.clear();
      break;
    }
    const auto close = findMatchingBrace(buffer_, open);
    if (close == std::string::npos)
    {
      if (open > 0)
      {
        buffer_.erase(0, open);
      }
      else if (buffer_.size() >= max_buffer_bytes)
      {
        buffer_.erase(0, 1);
        ++overflow_resync_count_;
      }
      break;
    }
    const std::string one = buffer_.substr(open, close - open + 1);
    auto parsed = parseOneObject(one);
    out.insert(out.end(), parsed.begin(), parsed.end());
    start = close + 1;
  }

  if (start > 0 && start <= buffer_.size())
  {
    buffer_.erase(0, start);
  }
  return out;
}
} // namespace sonitude::spatial
