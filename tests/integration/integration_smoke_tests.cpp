#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/calibration_config.hpp"
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

    const auto production = sonitude::app::LoadRuntimeConfigFromFile(
        std::string(SONITUDE_SOURCE_DIR) + "/config/production_pi.yaml");
    Require(production.capture.alsa_device == "hw:active,0",
            "production capture device should match the tested Pi profile");
    Require(production.playback.alsa_device == "hw:X1,0",
            "production playback device should match the tested Pi profile");
    Require(production.active_channel_map == std::vector<std::size_t>({5, 4, 3, 2, 1, 0}),
            "production map should match the tested reverse channel map");
    Require(production.geometry_path.find("geometry_soundbubble_xyz_v1.yaml") != std::string::npos,
            "production profile should use the corrected geometry");
    Require(production.calibration_path.find("calibration_example.yaml") != std::string::npos,
            "production profile should use unity calibration");
    Require(!production.odas.enabled && !production.suppression.enabled,
            "production baseline should keep ODAS and suppression disabled");
    Require(production.realtime.require_realtime &&
                production.realtime.enable_mlockall &&
                production.realtime.require_memory_lock,
            "production baseline should enforce realtime and memory lock startup requirements");

    const auto geometry = sonitude::app::LoadGeometryFromFile(production.geometry_path);
    Require(geometry.microphones.size() == 6U, "production geometry should load six microphones");
    const auto calibration = sonitude::app::LoadCalibrationFromFile(production.calibration_path);
    std::vector<std::string> geometry_ids;
    geometry_ids.reserve(geometry.microphones.size());
    for (const auto& mic : geometry.microphones)
    {
      geometry_ids.push_back(mic.id);
    }
    sonitude::app::ValidateCalibrationConfig(calibration, geometry_ids, production.capture.sample_rate_hz);

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
