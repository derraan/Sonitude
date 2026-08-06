#pragma once

#include <memory>
#include <string>

#include "spatial/doa_provider.hpp"

namespace sonitude::spatial
{
std::unique_ptr<IDoaProvider> CreateOdasProvider(const std::string& endpoint);
}  // namespace sonitude::spatial
