#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "spatial/source_tracker.hpp"

namespace
{
void Require(const bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestWrapAwareAzimuthSmoothing()
{
  sonitude::spatial::SourceTracker tracker(2'000'000'000ULL);
  tracker.ingest({{1, 179.0F, 0.0F, 0.8F, 0}}, 10);
  tracker.ingest({{1, -179.0F, 0.0F, 0.8F, 1}}, 20);

  const auto best = tracker.best(20);
  Require(best.has_value(), "track should remain available");
  Require(std::fabs(std::fabs(best->azimuth_deg) - 179.5F) < 1.0F,
          "azimuth smoothing should follow shortest angular path near wrap");
}

void TestUnderflowSafeStaleness()
{
  sonitude::spatial::SourceTracker tracker(10);
  tracker.ingest({{1, 10.0F, 0.0F, 0.8F, 0}}, 100);
  const auto best = tracker.best(90);
  Require(best.has_value(), "time regression should not stale valid track through underflow");
}
}  // namespace

void RunSourceTrackerTests()
{
  TestWrapAwareAzimuthSmoothing();
  TestUnderflowSafeStaleness();
}
