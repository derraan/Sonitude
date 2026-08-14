#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_message_parser.hpp"
#include "spatial/odas_provider.hpp"

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace
{
void Require(bool condition, const std::string& message)
{
  if (!condition)
  {
    throw std::runtime_error(message);
  }
}

void TestOdasParserSplitFrames()
{
  sonitude::spatial::OdasMessageParser parser;
  const std::string payload =
      R"({"timeStamp":1.25,"src":[{"id":7,"x":1.0,"y":0.0,"z":0.0,"activity":0.80}]})";
  const auto first = parser.feed(std::string_view(payload.data(), 22));
  Require(first.empty(), "partial JSON chunk should not emit yet");
  const auto second = parser.feed(std::string_view(payload.data() + 22, payload.size() - 22));
  Require(second.size() == 1, "expected one parsed source from split frame");
  Require(second[0].source_id == 7, "source id mismatch");
  Require(std::fabs(second[0].confidence - 0.8F) < 1e-6F, "confidence parse mismatch");
  Require(std::fabs(second[0].azimuth_deg) < 1e-3F, "azimuth parse mismatch");
}

void TestOdasParserGarbageResync()
{
  sonitude::spatial::OdasMessageParser parser;
  const std::string garbage = "xxx-not-json-xxx";
  const auto none = parser.feed(garbage);
  Require(none.empty(), "garbage should not emit observations");

  const std::string payload =
      R"({"timeStamp":2.0,"src":[{"id":3,"x":0.0,"y":1.0,"z":0.0,"activity":0.5}]})";
  const auto out = parser.feed(payload);
  Require(out.size() == 1, "parser failed to resync after garbage");
  Require(std::fabs(out[0].azimuth_deg - 90.0F) < 0.5F, "resynced azimuth mismatch");
}

void TestOdasParserOverflowCapResync()
{
  sonitude::spatial::OdasMessageParser parser(256);
  const auto none = parser.feed(std::string(300, 'x'));
  Require(none.empty(), "oversized non-JSON input must not emit observations");
  Require(parser.overflowResyncCount() > 0, "oversized input must trigger bounded resync");

  const std::string payload =
      R"({"timeStamp":2.5,"src":[{"id":4,"x":1.0,"y":0.0,"z":0.0,"activity":0.6}]})";
  const auto recovered = parser.feed(payload);
  Require(recovered.size() == 1, "parser must recover after bounded overflow resync");
}

std::string NumericPayload(const std::string& id,
                           const std::string& x,
                           const std::string& timestamp = "1.0",
                           const std::string& activity = "0.8")
{
  return "{\"timeStamp\":" + timestamp + ",\"src\":[{\"id\":" + id + ",\"x\":" + x +
         ",\"y\":0.0,\"z\":0.0,\"activity\":" + activity + "}]}";
}

void RequireNumericRejected(const std::string& payload, const std::string& description)
{
  sonitude::spatial::OdasMessageParser parser;
  Require(parser.feed(payload).empty(), description);
}

void TestOdasParserRejectsUnsafeNumbers()
{
  RequireNumericRejected(NumericPayload("1", "NaN"), "NaN must be rejected");
  RequireNumericRejected(NumericPayload("1", "inf"), "positive infinity must be rejected");
  RequireNumericRejected(NumericPayload("1", "-inf"), "negative infinity must be rejected");
  RequireNumericRejected(NumericPayload("1", "1e999"), "overflowing exponent must be rejected");
  RequireNumericRejected(NumericPayload("18446744073709551616", "1.0"),
                         "IDs above uint64_t max must be rejected");
  RequireNumericRejected(NumericPayload("-1", "1.0"), "negative IDs must be rejected");
  RequireNumericRejected(NumericPayload("1.5", "1.0"), "fractional IDs must be rejected");
  RequireNumericRejected(NumericPayload("1", "1e"), "incomplete exponents must be rejected");
  RequireNumericRejected(NumericPayload("1", "1.0junk"), "trailing numeric junk must be rejected");
  RequireNumericRejected(NumericPayload("1", "1.0", "1e20"),
                         "timestamps that overflow nanoseconds must be rejected");
}

void TestOdasParserAcceptsUnsignedBoundaries()
{
  sonitude::spatial::OdasMessageParser parser;
  const auto zero = parser.feed(NumericPayload("0", "1.0", "0"));
  Require(zero.size() == 1 && zero[0].source_id == 0,
          "zero must be accepted as the lower unsigned ID boundary");

  const auto maximum =
      parser.feed(NumericPayload("18446744073709551615", "1.0", "18446744073"));
  Require(maximum.size() == 1 &&
              maximum[0].source_id == std::numeric_limits<std::uint64_t>::max(),
          "uint64_t max must be accepted without a floating-point cast");
}

void TestMockDoaProviderScript()
{
  std::vector<sonitude::spatial::MockDoaEvent> events;
  events.push_back({100, {1, 10.0F, 0.0F, 0.7F, 100}});
  events.push_back({200, {2, 15.0F, 0.0F, 0.9F, 200}});
  sonitude::spatial::MockDoaProvider provider(std::move(events));

  std::vector<sonitude::spatial::SourceObservation> out;
  provider.setNowNs(150);
  Require(provider.poll(out), "mock provider should emit first event");
  Require(out.size() == 1 && out[0].source_id == 1, "mock first event mismatch");

  provider.setFrozen(true);
  provider.setNowNs(400);
  Require(!provider.poll(out), "frozen provider should not emit");
  Require(!provider.isHealthy(), "frozen provider must report unhealthy");
}

void TestOdasProviderReconnectsAfterBackoff()
{
#if defined(__linux__)
  const std::string path =
      "/tmp/sonitude-odas-reconnect-" + std::to_string(static_cast<long long>(::getpid())) +
      ".sock";
  (void)::unlink(path.c_str());
  auto provider = sonitude::spatial::CreateOdasProvider("unix://" + path);
  std::vector<sonitude::spatial::SourceObservation> observations;
  Require(!provider->poll(observations), "missing ODAS socket must fail closed");
  Require(!provider->isHealthy(), "missing ODAS socket must be unhealthy");

  const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
  Require(server >= 0, "ODAS reconnect test socket must be created");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
  Require(::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "ODAS reconnect test socket must bind");
  Require(::listen(server, 1) == 0, "ODAS reconnect test socket must listen");

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  Require(!provider->poll(observations), "reconnect without data must not emit observations");
  const int peer = ::accept(server, nullptr, nullptr);
  Require(peer >= 0, "ODAS provider must reconnect after its initial backoff");
  const std::string payload =
      R"({"timeStamp":3.0,"src":[{"id":9,"x":1.0,"y":0.0,"z":0.0,"activity":0.9}]})";
  Require(::write(peer, payload.data(), payload.size()) ==
              static_cast<ssize_t>(payload.size()),
          "ODAS reconnect test payload must be written");
  Require(provider->poll(observations) && observations.size() == 1,
          "ODAS provider must parse data after reconnect");
  Require(provider->isHealthy(), "ODAS provider must become healthy after valid data");

  ::close(peer);
  ::close(server);
  (void)::unlink(path.c_str());
#endif
}
}  // namespace

void RunOdasParserTests()
{
  TestOdasParserSplitFrames();
  TestOdasParserGarbageResync();
  TestOdasParserOverflowCapResync();
  TestOdasParserRejectsUnsafeNumbers();
  TestOdasParserAcceptsUnsignedBoundaries();
  TestMockDoaProviderScript();
  TestOdasProviderReconnectsAfterBackoff();
}
