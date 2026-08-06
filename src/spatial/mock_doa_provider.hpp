#pragma once

#include <cstdint>
#include <vector>

#include "spatial/doa_provider.hpp"

namespace sonitude::spatial
{
struct MockDoaEvent
{
  std::uint64_t timestamp_ns = 0;
  SourceObservation observation{};
};

class MockDoaProvider final : public IDoaProvider
{
 public:
  explicit MockDoaProvider(std::vector<MockDoaEvent> events);

  void setNowNs(std::uint64_t now_ns);
  void setFrozen(bool frozen);

  bool poll(std::vector<SourceObservation>& out) override;
  bool isHealthy() const override;

 private:
  std::vector<MockDoaEvent> events_;
  std::size_t cursor_ = 0;
  std::uint64_t now_ns_ = 0;
  bool frozen_ = false;
};
}  // namespace sonitude::spatial
