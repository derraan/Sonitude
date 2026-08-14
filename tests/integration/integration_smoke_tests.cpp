#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "app/config.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}
} // namespace

int main()
{
  try
  {
    const auto runtime = sonitude::app::LoadRuntimeConfigFromFile(
        std::string(SONITUDE_SOURCE_DIR) + "/tests/fixtures/runtime_valid.yaml");
    Require(runtime.capture.sample_rate_hz > 0,
            "runtime fixture should load for integration smoke");

    const std::filesystem::path source_root = SONITUDE_SOURCE_DIR;
    Require(std::filesystem::exists(source_root / "scripts/openmha_golden_render.sh"),
            "openMHA integration harness script should be present");
    Require(std::filesystem::exists(source_root / "tests/integration/openmha_m4_validation.md"),
            "openMHA integration instructions should be present");

    std::cout << "Integration smoke tests passed.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Integration smoke test failure: " << ex.what() << '\n';
    return 1;
  }
}
