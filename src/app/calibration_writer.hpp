#pragma once

#include <string>

#include "app/calibration_config.hpp"

namespace sonitude::app
{
void WriteCalibrationYamlBackupSafe(const std::string& path,
                                    const CalibrationConfig& calibration,
                                    bool force_overwrite);
}  // namespace sonitude::app
