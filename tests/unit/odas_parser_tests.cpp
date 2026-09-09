#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_message_parser.hpp"
#include "spatial/odas_provider.hpp"

#if defined(__linux__)
#include <sys/socket.h>
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
  const std::string oversized(300, 'x');
  const auto none = parser.feed(oversized);
  Require(none.empty(), "oversized non-JSON buffer should not emit observations");
  Require(parser.overflowResyncCount() > 0, "oversized input should trigger buffer resync counter");

  const std::string payload =
      R"({"timeStamp":2.5,"src":[{"id":4,"x":1.0,"y":0.0,"z":0.0,"activity":0.6}]})";
  const auto recovered = parser.feed(payload);
  Require(recovered.size() == 1, "parser should recover after overflow cap resync");
  Require(recovered[0].source_id == 4, "recovered parser source id mismatch");
}

void TestOdasParserBoundedEdgeCases()
{
  {
    sonitude::spatial::OdasMessageParser parser(32);
    const auto out = parser.feed(std::string(64, 'x'));
    Require(out.empty(), "endless non-JSON bytes should not emit observations");
    Require(parser.overflowResyncCount() > 0, "endless non-JSON bytes should trigger bounded resync");
  }

  {
    sonitude::spatial::OdasMessageParser parser(64);
    const std::string oversized_partial = std::string("{\"src\":[") + std::string(200, '{');
    const auto out = parser.feed(oversized_partial);
    Require(out.empty(), "oversized malformed partial should not emit observations");
    Require(parser.overflowResyncCount() > 0, "oversized malformed partial should trigger bounded resync");
  }

  {
    sonitude::spatial::OdasMessageParser parser(32);
    const auto none = parser.feed(std::string(128, '{'));
    Require(none.empty(), "repeated opening braces without closure should not emit observations");
    Require(parser.overflowResyncCount() > 0,
            "repeated opening braces must remain bounded and trigger overflow resync");
  }

  {
    sonitude::spatial::OdasMessageParser parser(128);
    const std::string malformed =
        R"({"timeStamp":1.0,"src":[{"id":"bad","x":"a","y":"b","z":"c","activity":"d"}]})";
    const auto out = parser.feed(malformed);
    Require(out.empty(), "deeply malformed object should not emit observations");
    const std::string payload =
        R"({"timeStamp":6.0,"src":[{"id":6,"x":0.0,"y":1.0,"z":0.0,"activity":0.4}]})";
    const auto recovered = parser.feed(payload);
    Require(recovered.size() == 1, "parser should recover from malformed object to next valid object");
  }

  {
    constexpr std::size_t kLimit = 72;
    sonitude::spatial::OdasMessageParser parser(kLimit);
    const auto none = parser.feed(std::string(kLimit, 'x'));
    Require(none.empty(), "buffer exactly at limit should remain bounded without output");
    const auto none2 = parser.feed("x");
    Require(none2.empty(), "buffer over limit by one should still remain bounded without output");
  }

  {
    sonitude::spatial::OdasMessageParser parser(0);
    const auto none = parser.feed(std::string(32, '{'));
    Require(none.empty(), "zero-sized configured buffer should stay bounded and not emit malformed objects");
    const auto none2 = parser.feed(std::string(32, 'x'));
    Require(none2.empty(), "zero-sized configured buffer should remain bounded on non-JSON bytes");
  }
}

void TestOdasParserRejectsNonFiniteOrMalformedNumbers()
{
  {
    sonitude::spatial::OdasMessageParser parser;
    const std::string non_finite =
        R"({"timeStamp":1.0,"src":[{"id":1,"x":NaN,"y":0.0,"z":0.0,"activity":0.8}]})";
    const auto out = parser.feed(non_finite);
    Require(out.empty(), "NaN coordinate should be rejected");
  }

  {
    sonitude::spatial::OdasMessageParser parser;
    const std::string infinities =
        R"({"timeStamp":1.0,"src":[{"id":1,"x":inf,"y":-inf,"z":0.0,"activity":0.8}]})";
    const auto out = parser.feed(infinities);
    Require(out.empty(), "infinite coordinates should be rejected");
  }

  {
    sonitude::spatial::OdasMessageParser parser;
    const std::string trailing_junk =
        R"({"timeStamp":1.0,"src":[{"id":1,"x":1.0junk,"y":0.0,"z":0.0,"activity":0.8}]})";
    const auto out = parser.feed(trailing_junk);
    Require(out.empty(), "numbers with trailing junk should be rejected");
  }

  {
    sonitude::spatial::OdasMessageParser parser;
    const std::string incomplete_exponent =
        R"({"timeStamp":1.0,"src":[{"id":1,"x":1e,"y":0.0,"z":0.0,"activity":0.8}]})";
    const auto out = parser.feed(incomplete_exponent);
    Require(out.empty(), "incomplete exponent should be rejected");
  }

  {
    sonitude::spatial::OdasMessageParser parser;
    const std::string overflow =
        R"({"timeStamp":1e999,"src":[{"id":18446744073709551616,"x":1.0,"y":0.0,"z":0.0,"activity":0.8}]})";
    const auto out = parser.feed(overflow);
    Require(out.empty(), "overflowing timestamp and id should be rejected");
  }
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

void TestOdasProviderUnreachableEndpointFailsClosed()
{
#if defined(__linux__)
  auto provider = sonitude::spatial::CreateOdasProvider("unix:///tmp/sonitude-missing-odas.sock");
  std::vector<sonitude::spatial::SourceObservation> out;
  const bool got_data = provider->poll(out);
  Require(!got_data, "unreachable ODAS endpoint should not emit data");
  Require(!provider->isHealthy(), "unreachable ODAS endpoint should report unhealthy");
#endif
}

#if defined(__linux__)
void TestOdasParserSocketpairFailureInjection()
{
  sonitude::spatial::OdasMessageParser parser(512);
  int fds[2] = {-1, -1};
  Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should be created");
  const std::string truncated = R"({"timeStamp":1.0,"src":[{"id":9,"x":1.0)";
  const ssize_t truncated_written = ::write(fds[0], truncated.data(), truncated.size());
  Require(truncated_written == static_cast<ssize_t>(truncated.size()),
          "expected truncated payload write to complete");
  ::close(fds[0]);

  std::vector<sonitude::spatial::SourceObservation> out;
  char buf[64];
  for (;;)
  {
    const ssize_t n = ::read(fds[1], buf, sizeof(buf));
    if (n <= 0)
    {
      break;
    }
    auto parsed = parser.feed(std::string_view(buf, static_cast<std::size_t>(n)));
    out.insert(out.end(), parsed.begin(), parsed.end());
  }
  ::close(fds[1]);
  Require(out.empty(), "truncated socket payload should not emit observations");

  Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "second socketpair should be created");
  const std::string garbage(600, 'g');
  const ssize_t garbage_written = ::write(fds[0], garbage.data(), garbage.size());
  Require(garbage_written == static_cast<ssize_t>(garbage.size()),
          "expected garbage payload write to complete");
  const std::string valid =
      R"({"timeStamp":3.0,"src":[{"id":8,"x":0.0,"y":1.0,"z":0.0,"activity":0.7}]})";
  const ssize_t valid_written = ::write(fds[0], valid.data(), valid.size());
  Require(valid_written == static_cast<ssize_t>(valid.size()),
          "expected valid payload write to complete");
  ::close(fds[0]);

  out.clear();
  for (;;)
  {
    const ssize_t n = ::read(fds[1], buf, sizeof(buf));
    if (n <= 0)
    {
      break;
    }
    auto parsed = parser.feed(std::string_view(buf, static_cast<std::size_t>(n)));
    out.insert(out.end(), parsed.begin(), parsed.end());
  }
  ::close(fds[1]);

  Require(parser.overflowResyncCount() > 0, "oversized socket garbage should trigger resync");
  Require(!out.empty(), "parser should recover after socket-injected garbage");
}
#endif
}  // namespace

void RunOdasParserTests()
{
  TestOdasParserSplitFrames();
  TestOdasParserGarbageResync();
  TestOdasParserOverflowCapResync();
  TestOdasParserBoundedEdgeCases();
  TestOdasParserRejectsNonFiniteOrMalformedNumbers();
  TestMockDoaProviderScript();
  TestOdasProviderUnreachableEndpointFailsClosed();
#if defined(__linux__)
  TestOdasParserSocketpairFailureInjection();
#endif
}
