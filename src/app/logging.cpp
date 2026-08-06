#include "app/logging.hpp"

#include <stdexcept>

#include <spdlog/spdlog.h>

namespace sonitude::app
{
void InitializeLogging(const std::string& level_name)
{
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  if (level_name == "debug")
  {
    spdlog::set_level(spdlog::level::debug);
  }
  else if (level_name == "info")
  {
    spdlog::set_level(spdlog::level::info);
  }
  else if (level_name == "warn")
  {
    spdlog::set_level(spdlog::level::warn);
  }
  else if (level_name == "error")
  {
    spdlog::set_level(spdlog::level::err);
  }
  else
  {
    throw std::runtime_error("Unknown log level: " + level_name);
  }
}
}  // namespace sonitude::app
