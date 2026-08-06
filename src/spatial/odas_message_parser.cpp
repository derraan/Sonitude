#include "spatial/odas_message_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

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
}  // namespace

bool OdasMessageParser::extractNumber(const std::string& json, const std::string& key, double& out)
{
  const auto key_pos = json.find("\"" + key + "\"");
  if (key_pos == std::string::npos)
  {
    return false;
  }
  const auto colon = json.find(':', key_pos);
  if (colon == std::string::npos)
  {
    return false;
  }
  std::size_t begin = colon + 1;
  while (begin < json.size() && std::isspace(static_cast<unsigned char>(json[begin])) != 0)
  {
    ++begin;
  }
  std::size_t end = begin;
  while (end < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[end])) != 0 || json[end] == '-' ||
          json[end] == '+' || json[end] == '.' || json[end] == 'e' || json[end] == 'E'))
  {
    ++end;
  }
  if (end == begin)
  {
    return false;
  }
  out = std::strtod(json.c_str() + static_cast<std::ptrdiff_t>(begin), nullptr);
  return true;
}

bool OdasMessageParser::extractUnsigned(const std::string& json,
                                        const std::string& key,
                                        std::uint64_t& out)
{
  double value = 0.0;
  if (!extractNumber(json, key, value))
  {
    return false;
  }
  if (value < 0.0)
  {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

std::size_t OdasMessageParser::findMatchingBrace(const std::string& json, const std::size_t open_pos)
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

std::vector<SourceObservation> OdasMessageParser::parseOneObject(const std::string& json_object) const
{
  std::vector<SourceObservation> out;
  double ts_sec = 0.0;
  if (!extractNumber(json_object, "timeStamp", ts_sec))
  {
    ts_sec = 0.0;
  }
  const std::uint64_t ts_ns = static_cast<std::uint64_t>(std::max(0.0, ts_sec) * 1'000'000'000.0);

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
      const double xy = std::sqrt((x * x) + (y * y));
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
  buffer_.append(bytes.data(), bytes.size());
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
}  // namespace sonitude::spatial
