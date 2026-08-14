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

void TestWrapAwareAzimuthSmoothingBoundaryEquivalent()
{
  sonitude::spatial::SourceTracker tracker(2'000'000'000ULL);
  tracker.ingest({{1, -180.0F, 0.0F, 0.9F, 0}}, 10);
  tracker.ingest({{1, 180.0F, 0.0F, 0.9F, 1}}, 20);
  const auto boundary = tracker.best(20);
  Require(boundary.has_value(), "boundary track should remain available");
  Require(std::fabs(boundary->azimuth_deg - 180.0F) < 0.5F,
          "-180 and +180 should converge as the same direction");

  sonitude::spatial::SourceTracker tracker_359(2'000'000'000ULL);
  tracker_359.ingest({{2, 359.0F, 0.0F, 0.9F, 0}}, 10);
  tracker_359.ingest({{2, 1.0F, 0.0F, 0.9F, 1}}, 20);
  const auto wrapped = tracker_359.best(20);
  Require(wrapped.has_value(), "359/1 equivalent wrap should remain available");
  Require(std::fabs(wrapped->azimuth_deg) < 2.0F, "359/1 smoothing should stay near 0 degrees");
}

void TestUnderflowSafeStaleness()
{
  sonitude::spatial::SourceTracker tracker(10);
  tracker.ingest({{1, 10.0F, 0.0F, 0.8F, 0}}, 100);
  const auto best = tracker.best(90);
  Require(best.has_value(), "time regression should not stale valid track through underflow");
}

void TestDeterministicBestTieBreak()
{
  sonitude::spatial::SourceTracker tracker(1'000'000'000ULL);
  tracker.ingest({{200, 10.0F, 0.0F, 0.75F, 1}}, 100);
  tracker.ingest({{100, -10.0F, 0.0F, 0.75F, 2}}, 100);
  const auto best = tracker.best(100);
  Require(best.has_value(), "equal-confidence tracks should still resolve deterministically");
  Require(best->source_id == 100, "tie-break should pick lower source id for equal confidence/freshness");
}

void TestStrongestDistractorDeterministicTieBreak()
{
  sonitude::spatial::SourceTracker tracker(1'000'000'000ULL);
  tracker.ingest({{1, 0.0F, 0.0F, 0.95F, 1}}, 100);
  tracker.ingest({{20, 30.0F, 0.0F, 0.70F, 2}}, 100);
  tracker.ingest({{10, -30.0F, 0.0F, 0.70F, 3}}, 100);

  const auto distractor = tracker.strongestDistractor(100, 1);
  Require(distractor.has_value(), "expected a distractor when non-focus tracks are present");
  Require(distractor->source_id == 10,
          "distractor tie-break should be deterministic and independent of hash iteration order");
}
}  // namespace

void RunSourceTrackerTests()
{
  TestWrapAwareAzimuthSmoothing();
  TestWrapAwareAzimuthSmoothingBoundaryEquivalent();
  TestUnderflowSafeStaleness();
  TestDeterministicBestTieBreak();
  TestStrongestDistractorDeterministicTieBreak();
}
