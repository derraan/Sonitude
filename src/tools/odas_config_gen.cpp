#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "app/config.hpp"

namespace
{
void PrintUsage()
{
  std::cout << "Usage: sonitude_odas_config_gen --config <runtime_yaml> --output <odas_cfg>\n";
}
}  // namespace

int main(int argc, char** argv)
{
  std::string config_path = "config/default.yaml";
  std::string output_path;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg == "--config" && i + 1 < argc)
    {
      config_path = argv[++i];
    }
    else if (arg == "--output" && i + 1 < argc)
    {
      output_path = argv[++i];
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
  if (output_path.empty())
  {
    PrintUsage();
    return 2;
  }

  try
  {
    const auto runtime = sonitude::app::LoadRuntimeConfigFromFile(config_path);
    const auto geometry = sonitude::app::LoadGeometryFromFile(runtime.geometry_path);

    std::ofstream out(output_path, std::ios::trunc);
    if (!out)
    {
      throw std::runtime_error("Unable to open output file");
    }

    out << "# Auto-generated Sonitude ODAS config\n";
    out << "sample_rate_hz: " << runtime.capture.sample_rate_hz << "\n";
    out << "input_channels: " << runtime.active_channel_map.size() << "\n";
    out << "endpoint: " << runtime.odas.endpoint << "\n";
    out << "microphones:\n";
    for (std::size_t i = 0; i < runtime.active_channel_map.size(); ++i)
    {
      const auto map_idx = runtime.active_channel_map[i];
      const auto& mic = geometry.microphones[i];
      out << "  - active_channel: " << map_idx << "\n";
      out << "    id: " << mic.id << "\n";
      out << "    x: " << mic.x << "\n";
      out << "    y: " << mic.y << "\n";
      out << "    z: " << mic.z << "\n";
    }
    std::cout << "Generated ODAS config: " << output_path << '\n';
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "odas_config_gen failed: " << ex.what() << '\n';
    return 1;
  }
}
