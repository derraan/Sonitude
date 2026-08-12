#include "spatial/mock_doa_provider.hpp"

#include <algorithm>
#include <utility>

namespace sonitude::spatial
{
MockDoaProvider::MockDoaProvider(std::vector<MockDoaEvent> events) : events_(std::move(events))
{
  std::sort(events_.begin(), events_.end(), [](const MockDoaEvent& a, const MockDoaEvent& b) {
    return a.timestamp_ns < b.timestamp_ns;
  });
}

void MockDoaProvider::setNowNs(const std::uint64_t now_ns)
{
  now_ns_ = now_ns;
}

void MockDoaProvider::setFrozen(const bool frozen)
{
  frozen_ = frozen;
}

bool MockDoaProvider::poll(std::vector<SourceObservation>& out)
{
  out.clear();
  if (frozen_)
  {
    return false;
  }
  while (cursor_ < events_.size() && events_[cursor_].timestamp_ns <= now_ns_)
  {
    out.push_back(events_[cursor_].observation);
    ++cursor_;
  }
  return !out.empty();
}

bool MockDoaProvider::isHealthy() const
{
  return !frozen_;
}
}  // namespace sonitude::spatial
