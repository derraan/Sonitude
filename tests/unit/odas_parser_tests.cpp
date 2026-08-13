#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "spatial/mock_doa_provider.hpp"
#include "spatial/odas_message_parser.hpp"

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

#if defined(__linux__)
void TestOdasParserSocketpairFailureInjection()
{
  sonitude::spatial::OdasMessageParser parser(512);
  int fds[2] = {-1, -1};
  Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should be created");
  const std::string truncated = R"({"timeStamp":1.0,"src":[{"id":9,"x":1.0)";
  (void)::write(fds[0], truncated.data(), truncated.size());
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
  (void)::write(fds[0], garbage.data(), garbage.size());
  const std::string valid =
      R"({"timeStamp":3.0,"src":[{"id":8,"x":0.0,"y":1.0,"z":0.0,"activity":0.7}]})";
  (void)::write(fds[0], valid.data(), valid.size());
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
  TestMockDoaProviderScript();
#if defined(__linux__)
  TestOdasParserSocketpairFailureInjection();
#endif
}
