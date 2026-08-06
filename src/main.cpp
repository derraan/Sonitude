#include <exception>
#include <iostream>
#include <string>

#include "app/config.hpp"
#include "app/logging.hpp"

namespace
{
void PrintUsage()
{
  std::cout
      << "sonitude_realtime Milestone 0 scaffold\n"
      << "Usage:\n"
      << "  sonitude_realtime [--config <path>] [--validate-config]\n"
      << "  sonitude_realtime --version\n"
      << "  sonitude_realtime --help\n";
}
}  // namespace

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  bool validate_only = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--config")
    {
      if (i + 1 >= argc)
      {
        std::cerr << "Missing value for --config\n";
        return 2;
      }
      config_path = argv[++i];
    }
    else if (arg == "--validate-config")
    {
      validate_only = true;
    }
    else if (arg == "--version")
    {
      std::cout << SONITUDE_VERSION << '\n';
      return 0;
    }
    else if (arg == "--help")
    {
      PrintUsage();
      return 0;
    }
    else
    {
      std::cerr << "Unknown argument: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }

  try
  {
    sonitude::app::InitializeLogging("info");
    const auto runtime_config = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    const auto geometry = sonitude::app::LoadGeometryFromFile(runtime_config.geometry_path);
    (void)geometry;

    std::cout << "Config validation passed: " << config_path << '\n';
    if (validate_only)
    {
      return 0;
    }

    std::cout << "Milestone 0 scaffold only: realtime audio pipeline is not implemented yet.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Startup failed: " << ex.what() << '\n';
    return 1;
  }
}
