#pragma once

#include <cstdint>
#include <vector>

#include "spatial/spatial_types.hpp"

namespace sonitude::spatial
{
class IDoaProvider
{
 public:
  virtual ~IDoaProvider() = default;
  virtual bool poll(std::vector<SourceObservation>& out) = 0;
  virtual bool isHealthy() const = 0;
};
}  // namespace sonitude::spatial
