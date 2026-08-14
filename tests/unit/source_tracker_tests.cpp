#include <cmath>
#include <stdexcept>
#include <string>

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
          "azimuth smoothing must follow the shortest path across wrap");
}

void TestUnderflowSafeStaleness()
{
  sonitude::spatial::SourceTracker tracker(10);
  tracker.ingest({{1, 10.0F, 0.0F, 0.8F, 0}}, 100);
  Require(tracker.best(90).has_value(),
          "a clock regression must not stale a track through unsigned underflow");
}

void TestDeterministicSelectionAndDistractor()
{
  sonitude::spatial::SourceTracker tracker(1'000'000'000ULL);
  tracker.ingest({{200, 10.0F, 0.0F, 0.75F, 1}}, 100);
  tracker.ingest({{100, -10.0F, 0.0F, 0.75F, 2}}, 100);
  tracker.ingest({{1, 0.0F, 0.0F, 0.95F, 3}}, 100);

  const auto best = tracker.best(100);
  Require(best.has_value() && best->source_id == 1,
          "highest-confidence source must be selected deterministically");
  const auto distractor = tracker.strongestDistractor(100, 1);
  Require(distractor.has_value() && distractor->source_id == 100,
          "equal distractors must use the lower source ID tie-break");
}
} // namespace

void RunSourceTrackerTests()
{
  TestWrapAwareAzimuthSmoothing();
  TestUnderflowSafeStaleness();
  TestDeterministicSelectionAndDistractor();
}
